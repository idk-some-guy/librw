struct ShadowRayParams
{
	float4x4 invViewProj;
	float4x4 viewToWorld;
	float4 sun;
	float4 eye;
	uint2 size;
	uint2 depthSize;
	uint frame;
	uint raysPerPixel;
	uint halfRes;
	uint alphaEntries;
	uint terminatorOff;
};

struct ShadowAccumParams
{
	float4x4 invViewProj;
	float4x4 prevInvViewProj;
	float4x4 viewToWorld;
	float4 eye;
	float4 prevEye;
	uint2 size;
	uint2 rawSize;
	uint halfRes;
	uint valid;
	float maxHistory;
	float pad;
};

constant uint shadowPhase [[function_constant(0)]];

static float3
shadowWorld(float4x4 inv, uint2 px, uint2 dims, float d)
{
	float2 uv = (float2(px) + 0.5)/float2(dims);
	float4 w = inv * float4(uv.x*2.0 - 1.0, 1.0 - uv.y*2.0, d*2.0 - 1.0, 1.0);
	return w.xyz/w.w;
}

struct ShadowAlphaEntry
{
	texture2d<float> tex;
	float threshold;
	uint address;
};

struct ShadowAlphaTriangle
{
	packed_float2 uv0, uv1, uv2;
};

static bool
shadowAlphaBlocks(device const ShadowAlphaEntry *alpha, uint entries, uint base, uint geometry, const device void *data, float2 bary)
{
	if(!shAlphaIndexValid(base, geometry, entries))
		return false;
	device const ShadowAlphaEntry &e = alpha[base + geometry];
	device const ShadowAlphaTriangle &t = *(device const ShadowAlphaTriangle*)data;
	float2 uv = float2(t.uv0)*(1.0 - bary.x - bary.y) + float2(t.uv1)*bary.x + float2(t.uv2)*bary.y;
	uv = float2(shAddress(uv.x, e.address & 3u), shAddress(uv.y, e.address >> 2));
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	return e.tex.sample(s, uv, level(0)).a >= e.threshold;
}

static float
shadowTap(depth2d<float, access::read> depth, int2 at, int2 last)
{
	return depth.read(uint2(clamp(at, int2(0), last)));
}

static float3
shadowFacet(constant ShadowRayParams &p, depth2d<float, access::read> depth, uint2 src, float d, float3 P)
{
	int2 s = int2(src), last = int2(p.depthSize) - 1, dx = int2(1, 0), dy = int2(0, 1);
	bool nx = s.x > 0, px = s.x < last.x, ny = s.y > 0, py = s.y < last.y;
	if(!(nx || px) || !(ny || py))
		return float3(0.0);
	float xn = shadowTap(depth, s - dx, last), xp = shadowTap(depth, s + dx, last);
	float yn = shadowTap(depth, s - dy, last), yp = shadowTap(depth, s + dy, last);
	bool sx = shFacetPositive(d, xn, shadowTap(depth, s - 2*dx, last), xp, shadowTap(depth, s + 2*dx, last), nx, px);
	bool sy = shFacetPositive(d, yn, shadowTap(depth, s - 2*dy, last), yp, shadowTap(depth, s + 2*dy, last), ny, py);
	float tx = sx ? xp : xn, ty = sy ? yp : yn;
	if(tx >= 1.0 || ty >= 1.0)
		return float3(0.0);
	float3 X = shadowWorld(p.invViewProj, uint2(sx ? s + dx : s - dx), p.depthSize, tx) - P;
	float3 Y = shadowWorld(p.invViewProj, uint2(sy ? s + dy : s - dy), p.depthSize, ty) - P;
	return cross(sx ? X : -X, sy ? Y : -Y);
}

static bool
shadowRayStart(constant ShadowRayParams &p, depth2d<float, access::read> depth, texture2d<float, access::read> normals,
               uint2 gid, thread float3 &origin, thread float4 &facing, thread bool &cull, thread bool &dark)
{
	uint2 src = gid;
	if(p.halfRes != 0)
		src = uint2(shRaySource(gid.x, shPhaseX(p.frame), p.depthSize.x), shRaySource(gid.y, shPhaseY(p.frame), p.depthSize.y));
	float d = depth.read(src);
	if(d >= 1.0)
		return false;
	float3 P = shadowWorld(p.invViewProj, src, p.depthSize, d);
	float3 toEye = p.eye.xyz - P;
	float dist = length(toEye);
	float2 e = normals.read(src).xy;
	PPVec3 nv = ppOctDecode(e.x, e.y);
	float3 N = (p.viewToWorld * float4(nv.x, nv.y, nv.z, 0.0)).xyz;
	float nl = length(N);
	float3 v = toEye/dist;
	N = nl > 1e-6 ? N/nl : v;
	PPVec3 f = ppFacingNormal(PPVec3{ N.x, N.y, N.z }, PPVec3{ v.x, v.y, v.z });
	SHVec3 o = shRayOrigin(SHVec3{ P.x, P.y, P.z }, SHVec3{ f.x, f.y, f.z }, dist);
	origin = float3(o.x, o.y, o.z);
	facing = float4(f.x, f.y, f.z, nl > 1e-6 ? ppNormalStored(e.x, e.y) : 0.0);
	float ns = dot(facing.xyz, p.sun.xyz);
	cull = false;
	dark = shFacesAwayFromSun(facing.w, ns, p.sun.w);
	if(p.terminatorOff == 0u && facing.w > 0.0){
		float3 g = shadowFacet(p, depth, src, d, P);
		g = dot(g, toEye) < 0.0 ? -g : g;
		cull = shCullsBackFaces(facing.w, dot(g, p.sun.xyz));
		if(cull){
			dark = shClassDark(ns);
			g = normalize(g);
			SHVec3 b = shRayOrigin(SHVec3{ P.x, P.y, P.z }, SHVec3{ -g.x, -g.y, -g.z }, dist);
			origin = float3(b.x, b.y, b.z);
		}
	}
	return true;
}

struct ShadowAlphaTableParams
{
	uint entries;
	uint mode;
	uint pad[2];
};

[[intersection(triangle, triangle_data, instancing)]]
bool
shadowAlphaHit(uint user [[user_instance_id]],
               uint geometry [[geometry_id]],
               float2 bary [[barycentric_coord]],
               const device void *data [[primitive_data]],
               device const ShadowAlphaEntry *alpha [[buffer(0)]],
               constant ShadowAlphaTableParams &tp [[buffer(1)]])
{
	if(tp.mode != 0u)
		return tp.mode == 1u;
	return shadowAlphaBlocks(alpha, tp.entries, user, geometry, data, bary);
}

kernel void
shadowRays(instance_acceleration_structure tlas [[buffer(0)]],
           constant ShadowRayParams &p [[buffer(1)]],
           depth2d<float, access::read> depth [[texture(0)]],
           texture2d<float, access::write> raw [[texture(1)]],
           texture2d<float, access::read> normals [[texture(2)]],
           uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	float3 o;
	float4 f;
	bool cull, dark;
	if(!shadowRayStart(p, depth, normals, gid, o, f, cull, dark)){
		raw.write(float4(1.0), gid);
		return;
	}
	if(dark){
		raw.write(float4(0.0), gid);
		return;
	}
	SHVec3 sun = { p.sun.x, p.sun.y, p.sun.z };
	intersector<triangle_data, instancing> isect;
	isect.accept_any_intersection(true);
	isect.assume_geometry_type(geometry_type::triangle);
	isect.set_triangle_front_facing_winding(winding::clockwise);
	if(cull)
		isect.set_triangle_cull_mode(triangle_cull_mode::back);
	isect.force_opacity(forced_opacity::opaque);
	float vis = 0.0;
	for(uint r = 0; r < p.raysPerPixel; r++){
		SHVec3 dd = shDiscDirection(sun, p.sun.w, shHash(gid.x, gid.y, p.frame, 2*r), shHash(gid.x, gid.y, p.frame, 2*r + 1));
		ray q(o, float3(dd.x, dd.y, dd.z), 0.0, p.eye.w);
		vis += isect.intersect(q, tlas, 0xFF).type == intersection_type::none ? 1.0 : 0.0;
	}
	raw.write(float4(vis/float(p.raysPerPixel)), gid);
}

kernel void
shadowRaysAlpha(instance_acceleration_structure tlas [[buffer(0)]],
                constant ShadowRayParams &p [[buffer(1)]],
                intersection_function_table<triangle_data, instancing> ft [[buffer(2)]],
                depth2d<float, access::read> depth [[texture(0)]],
                texture2d<float, access::write> raw [[texture(1)]],
                texture2d<float, access::read> normals [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	float3 o;
	float4 f;
	bool cull, dark;
	if(!shadowRayStart(p, depth, normals, gid, o, f, cull, dark)){
		raw.write(float4(1.0), gid);
		return;
	}
	if(dark){
		raw.write(float4(0.0), gid);
		return;
	}
	SHVec3 sun = { p.sun.x, p.sun.y, p.sun.z };
	intersector<triangle_data, instancing> isect;
	isect.accept_any_intersection(true);
	isect.assume_geometry_type(geometry_type::triangle);
	isect.set_triangle_front_facing_winding(winding::clockwise);
	if(cull)
		isect.set_triangle_cull_mode(triangle_cull_mode::back);
	float vis = 0.0;
	for(uint r = 0; r < p.raysPerPixel; r++){
		SHVec3 dd = shDiscDirection(sun, p.sun.w, shHash(gid.x, gid.y, p.frame, 2*r), shHash(gid.x, gid.y, p.frame, 2*r + 1));
		ray q(o, float3(dd.x, dd.y, dd.z), 0.0, p.eye.w);
		vis += isect.intersect(q, tlas, 0xFF, ft).type == intersection_type::none ? 1.0 : 0.0;
	}
	raw.write(float4(vis/float(p.raysPerPixel)), gid);
}

static float3
shadowHistoryPoint(constant ShadowAccumParams &p, uint2 hp, float distance)
{
	float2 uv = (float2(hp) + 0.5)/float2(p.size);
	float4 f = p.prevInvViewProj * float4(uv.x*2.0 - 1.0, 1.0 - uv.y*2.0, 1.0, 1.0);
	float3 dir = normalize(f.xyz/f.w - p.prevEye.xyz);
	return p.prevEye.xyz + dir*distance;
}

static float4
shadowStoredNormal(texture2d<float, access::read> normals, uint2 px)
{
	float2 e = normals.read(px).xy;
	PPVec3 n = ppOctDecode(e.x, e.y);
	return float4(n.x, n.y, n.z, ppNormalStored(e.x, e.y));
}

static uint2
shadowRawSource(constant ShadowAccumParams &p, int2 h, uint2 off)
{
	if(p.halfRes == 0)
		return uint2(h);
	return uint2(shRaySource(uint(h.x), off.x, p.size.x), shRaySource(uint(h.y), off.y, p.size.y));
}

static float
shadowUpsample(texture2d<float, access::read> raw, depth2d<float, access::read> depth, texture2d<float, access::read> normals,
               constant ShadowAccumParams &p, uint2 gid, float dist, float4 own, uint2 off)
{
	int2 rmax = int2(p.rawSize) - 1, h[4];
	float bw[4], r[4];
	bool same = true;
	for(int k = 0; k < 4; k++){
		SHTap tx = shUpsampleTap(gid.x, off.x, k & 1), ty = shUpsampleTap(gid.y, off.y, k >> 1);
		bw[k] = tx.weight*ty.weight;
		h[k] = min(int2(int(tx.index), int(ty.index)), rmax);
		r[k] = k > 0 && bw[k] == 0.0 ? r[0] : raw.read(uint2(h[k])).r;
		same = same && r[k] == r[0];
	}
	if(same)
		return r[0];
	float sum = 0.0, wsum = 0.0;
	for(int k = 0; k < 4; k++){
		if(bw[k] == 0.0)
			continue;
		uint2 src = shadowRawSource(p, h[k], off);
		float td = length(shadowWorld(p.invViewProj, src, p.size, depth.read(src)) - p.eye.xyz);
		float4 n = shadowStoredNormal(normals, src);
		float w = bw[k]*shUpsampleWeight(td, dist)*shUpsampleNormalWeight(own.w, n.w, dot(own.xyz, n.xyz));
		sum += r[k]*w;
		wsum += w;
	}
	return sum/wsum;
}

kernel void
shadowAccumulate(texture2d<float, access::read> raw [[texture(0)]],
                 texture2d<float, access::read> hist [[texture(1)]],
                 texture2d<float, access::read> motion [[texture(2)]],
                 texture2d<float, access::read> normals [[texture(3)]],
                 depth2d<float, access::read> depth [[texture(4)]],
                 texture2d<float, access::write> out [[texture(5)]],
                 constant ShadowAccumParams &p [[buffer(0)]],
                 uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	float d = depth.read(gid);
	if(d >= 1.0){
		out.write(float4(1.0, 0.0, 0.0, 1.0), gid);
		return;
	}
	float3 P = shadowWorld(p.invViewProj, gid, p.size, d);
	float dist = length(P - p.eye.xyz);
	uint2 off = p.halfRes != 0 ? uint2(shPhaseX(shadowPhase), shPhaseY(shadowPhase)) : uint2(0);
	SHTap tx = shUpsampleTap(gid.x, off.x, 0), ty = shUpsampleTap(gid.y, off.y, 0);
	int2 rp = int2(p.halfRes != 0 ? uint2(tx.index, ty.index) : gid);
	int2 rmax = int2(p.rawSize) - 1;
	bool own = p.halfRes == 0 || tx.weight*ty.weight == 1.0;
	float4 on = shadowStoredNormal(normals, gid);
	float cur = own ? raw.read(uint2(min(rp, rmax))).r : shadowUpsample(raw, depth, normals, p, gid, dist, on, off);
	float lo = cur, hi = cur;
	for(int y = -1; y <= 1; y++)
		for(int x = -1; x <= 1; x++){
			int2 h = clamp(rp + int2(x, y), int2(0), rmax);
			float r = raw.read(uint2(h)).r;
			if(r >= lo && r <= hi)
				continue;
			float4 n = shadowStoredNormal(normals, shadowRawSource(p, h, off));
			if(!shClampTapAgrees(on.w, n.w, dot(on.xyz, n.xyz)))
				continue;
			lo = min(lo, r);
			hi = max(hi, r);
		}
	SHAccum a = { cur, 1.0 };
	float2 huv = (float2(gid) + 0.5)/float2(p.size) + motion.read(gid).xy;
	if(p.valid != 0 && all(huv >= 0.0) && all(huv < 1.0)){
		uint2 hp = min(uint2(huv*float2(p.size)), p.size - 1);
		float4 h = hist.read(hp);
		if(h.g > 0.0){
			float3 Q = shadowHistoryPoint(p, hp, h.b);
			float3 N = normalize((p.viewToWorld * float4(on.xyz, 0.0)).xyz);
			if(shHistoryMatches(SHVec3{ Q.x, Q.y, Q.z }, SHVec3{ P.x, P.y, P.z }, SHVec3{ N.x, N.y, N.z }, h.b, dist))
				a = shAccumulateClamped(h.r, h.g, cur, lo, hi, p.maxHistory);
		}
	}
	out.write(float4(a.value, a.length, dist, 1.0), gid);
}

kernel void
shadowClear(texture2d<float, access::write> t [[texture(0)]],
            uint2 gid [[thread_position_in_grid]])
{
	if(gid.x < t.get_width() && gid.y < t.get_height())
		t.write(float4(1.0, 0.0, 0.0, 1.0), gid);
}
