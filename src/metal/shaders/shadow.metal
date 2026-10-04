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

static bool
shadowRayStart(constant ShadowRayParams &p, depth2d<float, access::read> depth, texture2d<float, access::read> normals,
               uint2 gid, thread float3 &origin, thread float4 &facing)
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
	if(!shadowRayStart(p, depth, normals, gid, o, f)){
		raw.write(float4(1.0), gid);
		return;
	}
	if(shFacesAwayFromSun(f.w, dot(f.xyz, p.sun.xyz), p.sun.w)){
		raw.write(float4(0.0), gid);
		return;
	}
	SHVec3 sun = { p.sun.x, p.sun.y, p.sun.z };
	intersector<triangle_data, instancing> isect;
	isect.accept_any_intersection(true);
	isect.assume_geometry_type(geometry_type::triangle);
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
	if(!shadowRayStart(p, depth, normals, gid, o, f)){
		raw.write(float4(1.0), gid);
		return;
	}
	if(shFacesAwayFromSun(f.w, dot(f.xyz, p.sun.xyz), p.sun.w)){
		raw.write(float4(0.0), gid);
		return;
	}
	SHVec3 sun = { p.sun.x, p.sun.y, p.sun.z };
	intersector<triangle_data, instancing> isect;
	isect.accept_any_intersection(true);
	isect.assume_geometry_type(geometry_type::triangle);
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

static float
shadowUpsample(texture2d<float, access::read> raw, depth2d<float, access::read> depth, constant ShadowAccumParams &p, uint2 gid,
               float dist, uint2 off)
{
	int2 rmax = int2(p.rawSize) - 1;
	float sum = 0.0, wsum = 0.0;
	for(int j = 0; j < 2; j++)
		for(int i = 0; i < 2; i++){
			SHTap tx = shUpsampleTap(gid.x, off.x, i), ty = shUpsampleTap(gid.y, off.y, j);
			float bw = tx.weight*ty.weight;
			if(bw == 0.0)
				continue;
			int2 h = min(int2(int(tx.index), int(ty.index)), rmax);
			uint2 src = uint2(shRaySource(uint(h.x), off.x, p.size.x), shRaySource(uint(h.y), off.y, p.size.y));
			float td = length(shadowWorld(p.invViewProj, src, p.size, depth.read(src)) - p.eye.xyz);
			float w = bw*shUpsampleWeight(td, dist);
			sum += raw.read(uint2(h)).r*w;
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
	float cur = own ? raw.read(uint2(min(rp, rmax))).r : shadowUpsample(raw, depth, p, gid, dist, off);
	float lo = raw.read(uint2(min(rp, rmax))).r, hi = lo;
	for(int y = -1; y <= 1; y++)
		for(int x = -1; x <= 1; x++){
			float r = raw.read(uint2(clamp(rp + int2(x, y), int2(0), rmax))).r;
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
			float2 o = normals.read(gid).xy;
			PPVec3 nv = ppOctDecode(o.x, o.y);
			float3 N = normalize((p.viewToWorld * float4(nv.x, nv.y, nv.z, 0.0)).xyz);
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
