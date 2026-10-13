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
	uint ownerOff;
	uint listCap;
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
	uint phase;
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
	uv = float2(shAddress(uv.x, shAlphaAddressU(e.address)), shAddress(uv.y, shAlphaAddressV(e.address)));
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
               uint2 gid, thread float3 &origin, thread float4 &facing, thread bool &cull, thread bool &dark, thread float4 &back)
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
	back = float4(0.0);
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
			back = float4(g, shNormalOffset(dist));
		}
	}
	return true;
}

static void
shadowListAppend(device atomic_uint *listed, device uint2 *list, uint cap, uint2 gid, uint owner)
{
	uint i = atomic_fetch_add_explicit(listed, 1u, memory_order_relaxed);
	if(i < cap)
		list[i] = uint2(shListEntry(gid.x, gid.y), owner);
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
	return shadowAlphaBlocks(alpha, tp.entries, shAlphaBaseOf(user), geometry, data, bary);
}

kernel void
shadowRays(instance_acceleration_structure tlas [[buffer(0)]],
           constant ShadowRayParams &p [[buffer(1)]],
           depth2d<float, access::read> depth [[texture(0)]],
           texture2d<float, access::write> raw [[texture(1)]],
           texture2d<float, access::read> normals [[texture(2)]],
           device atomic_uint *listed [[buffer(3)]],
           device uint2 *list [[buffer(4)]],
           uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	float3 o;
	float4 f, back;
	bool cull, dark;
	if(!shadowRayStart(p, depth, normals, gid, o, f, cull, dark, back)){
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
	uint owned = 0u;
	for(uint r = 0; r < p.raysPerPixel; r++){
		SHVec3 dd = shDiscDirection(sun, p.sun.w, shHash(gid.x, gid.y, p.frame, 2*r), shHash(gid.x, gid.y, p.frame, 2*r + 1));
		ray q(o, float3(dd.x, dd.y, dd.z), 0.0, p.eye.w);
		auto h = isect.intersect(q, tlas, 0xFF);
		vis += h.type == intersection_type::none ? 1.0 : 0.0;
		if(h.type != intersection_type::none && shOwnerListed(cull, shOwnerOf(h.user_instance_id), p.ownerOff == 0u))
			owned = shOwnerOf(h.user_instance_id);
	}
	raw.write(float4(vis/float(p.raysPerPixel)), gid);
	if(owned != 0u)
		shadowListAppend(listed, list, p.listCap, gid, owned);
}

kernel void
shadowRaysAlpha(instance_acceleration_structure tlas [[buffer(0)]],
                constant ShadowRayParams &p [[buffer(1)]],
                intersection_function_table<triangle_data, instancing> ft [[buffer(2)]],
                depth2d<float, access::read> depth [[texture(0)]],
                texture2d<float, access::write> raw [[texture(1)]],
                texture2d<float, access::read> normals [[texture(2)]],
                device atomic_uint *listed [[buffer(3)]],
                device uint2 *list [[buffer(4)]],
                uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	float3 o;
	float4 f, back;
	bool cull, dark;
	if(!shadowRayStart(p, depth, normals, gid, o, f, cull, dark, back)){
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
	uint owned = 0u;
	for(uint r = 0; r < p.raysPerPixel; r++){
		SHVec3 dd = shDiscDirection(sun, p.sun.w, shHash(gid.x, gid.y, p.frame, 2*r), shHash(gid.x, gid.y, p.frame, 2*r + 1));
		ray q(o, float3(dd.x, dd.y, dd.z), 0.0, p.eye.w);
		auto h = isect.intersect(q, tlas, 0xFF, ft);
		vis += h.type == intersection_type::none ? 1.0 : 0.0;
		if(h.type != intersection_type::none && shOwnerListed(cull, shOwnerOf(h.user_instance_id), p.ownerOff == 0u))
			owned = shOwnerOf(h.user_instance_id);
	}
	raw.write(float4(vis/float(p.raysPerPixel)), gid);
	if(owned != 0u)
		shadowListAppend(listed, list, p.listCap, gid, owned);
}

kernel void
shadowOwnerReset(device uint *listed [[buffer(3)]])
{
	listed[0] = 0u;
}

static uint
shadowOwnOwner(instance_acceleration_structure tlas, float3 o, float4 back)
{
	intersector<triangle_data, instancing> isect;
	isect.assume_geometry_type(geometry_type::triangle);
	isect.force_opacity(forced_opacity::opaque);
	ray q(o + back.xyz*(2.0*back.w), -back.xyz, 0.0, 2.0*back.w);
	auto h = isect.intersect(q, tlas, 0xFF);
	return h.type == intersection_type::none ? 0u : shOwnerOf(h.user_instance_id);
}

static bool
shadowCandidateBlocks(device const ShadowAlphaEntry *alpha, uint entries, uint user, uint geometry, const device void *data, float2 bary)
{
	uint base = shAlphaBaseOf(user);
	if(!shAlphaIndexValid(base, 0u, entries) || !shAlphaCandidate(base, geometry, entries, shAlphaCountOf(alpha[base].address)))
		return true;
	return shadowAlphaBlocks(alpha, entries, base, geometry, data, bary);
}

kernel void
shadowOwnerFix(instance_acceleration_structure tlas [[buffer(0)]],
               constant ShadowRayParams &p [[buffer(1)]],
               device const uint *listed [[buffer(3)]],
               device const uint2 *list [[buffer(4)]],
               device const ShadowAlphaEntry *alpha [[buffer(5)]],
               device uint *taken [[buffer(6)]],
               depth2d<float, access::read> depth [[texture(0)]],
               texture2d<float, access::write> raw [[texture(1)]],
               texture2d<float, access::read> normals [[texture(2)]],
               uint tid [[thread_position_in_grid]])
{
	if(tid == 0u)
		taken[0] = listed[0];
	uint n = min(listed[0], p.listCap);
	SHVec3 sun = { p.sun.x, p.sun.y, p.sun.z };
	for(uint i = tid; i < n; i += SH_OWNER_THREADS){
		uint2 e = list[i];
		uint2 gid = uint2(shListX(e.x), shListY(e.x));
		float3 o;
		float4 f, back;
		bool cull, dark;
		if(!shadowRayStart(p, depth, normals, gid, o, f, cull, dark, back) || !cull || dark)
			continue;
		uint own = shadowOwnOwner(tlas, o, back);
		if(own != e.y)
			continue;
		intersection_params ip;
		ip.assume_geometry_type(geometry_type::triangle);
		ip.set_triangle_front_facing_winding(winding::clockwise);
		ip.set_triangle_cull_mode(triangle_cull_mode::back);
		ip.force_opacity(forced_opacity::non_opaque);
		ip.accept_any_intersection(true);
		float vis = 0.0;
		for(uint r = 0; r < p.raysPerPixel; r++){
			SHVec3 dd = shDiscDirection(sun, p.sun.w, shHash(gid.x, gid.y, p.frame, 2*r), shHash(gid.x, gid.y, p.frame, 2*r + 1));
			ray q(o, float3(dd.x, dd.y, dd.z), 0.0, p.eye.w);
			intersection_query<triangle_data, instancing> iq(q, tlas, 0xFF, ip);
			while(iq.next()){
				uint user = iq.get_candidate_user_instance_id();
				if(shOwnerSkips(own, shOwnerOf(user)))
					continue;
				if(!shadowCandidateBlocks(alpha, p.alphaEntries, user, iq.get_candidate_geometry_id(), iq.get_candidate_primitive_data(),
				                          iq.get_candidate_triangle_barycentric_coord()))
					continue;
				iq.commit_triangle_intersection();
			}
			vis += iq.get_committed_intersection_type() == intersection_type::none ? 1.0 : 0.0;
		}
		raw.write(float4(vis/float(p.raysPerPixel)), gid);
	}
}

struct LightRaySlot
{
	float4 posRadius;
	float4 dirOuter;
	float4 misc;
	uint4 ids;
};

static bool
lightRayWanted(constant LightRaySlot &L, float3 o)
{
	float3 toLight = L.posRadius.xyz - o;
	return dot(toLight, toLight) > 1e-8 &&
	       ltRayWanted(LTVec3{ o.x, o.y, o.z }, LTVec3{ L.posRadius.x, L.posRadius.y, L.posRadius.z }, L.posRadius.w,
	                   LTVec3{ L.dirOuter.x, L.dirOuter.y, L.dirOuter.z }, L.dirOuter.w, L.misc.y != 0.0);
}

static ray
lightRayToward(constant ShadowRayParams &p, constant LightRaySlot &L, uint3 gid, float3 o)
{
	float3 toLight = L.posRadius.xyz - o;
	float dist = length(toLight);
	float3 u = toLight/dist;
	LTVec3 d = ltDiscToward(LTVec3{ u.x, u.y, u.z }, L.misc.x, dist, shHash(gid.x, gid.y, p.frame, 2*gid.z), shHash(gid.x, gid.y, p.frame, 2*gid.z + 1));
	return ray(o, float3(d.x, d.y, d.z), 0.0, ltRayLength(dist, 0.1));
}

kernel void
lightShadowRays(instance_acceleration_structure tlas [[buffer(0)]],
                constant ShadowRayParams &p [[buffer(1)]],
                device atomic_uint *listed [[buffer(3)]],
                device uint2 *list [[buffer(4)]],
                constant LightRaySlot *slots [[buffer(7)]],
                device atomic_uint *counts [[buffer(8)]],
                depth2d<float, access::read> depth [[texture(0)]],
                texture2d_array<float, access::write> raw [[texture(1)]],
                texture2d<float, access::read> normals [[texture(2)]],
                uint3 gid [[thread_position_in_grid]])
{
	constant LightRaySlot &L = slots[gid.z];
	float3 o;
	float4 f, back;
	bool cull, dark;
	bool wanted = shadowRayStart(p, depth, normals, gid.xy, o, f, cull, dark, back) && lightRayWanted(L, o);
	uint n = simd_sum(wanted ? 1u : 0u);
	if(simd_is_first() && n > 0u)
		atomic_fetch_add_explicit(&counts[gid.z], n, memory_order_relaxed);
	if(!wanted){
		raw.write(float4(1.0), gid.xy, gid.z);
		return;
	}
	intersector<triangle_data, instancing> isect;
	isect.accept_any_intersection(true);
	isect.assume_geometry_type(geometry_type::triangle);
	isect.set_triangle_front_facing_winding(winding::clockwise);
	isect.force_opacity(forced_opacity::opaque);
	auto h = isect.intersect(lightRayToward(p, L, gid, o), tlas, 0xFF);
	bool hit = h.type != intersection_type::none;
	if(hit && p.ownerOff == 0u && L.ids.y != 0u && shOwnerOf(h.user_instance_id) == L.ids.y)
		shadowListAppend(listed, list, p.listCap, gid.xy, L.ids.y | gid.z << 8);
	raw.write(float4(hit ? 0.0 : 1.0), gid.xy, gid.z);
}

kernel void
lightShadowRaysAlpha(instance_acceleration_structure tlas [[buffer(0)]],
                     constant ShadowRayParams &p [[buffer(1)]],
                     intersection_function_table<triangle_data, instancing> ft [[buffer(2)]],
                     device atomic_uint *listed [[buffer(3)]],
                     device uint2 *list [[buffer(4)]],
                     constant LightRaySlot *slots [[buffer(7)]],
                     device atomic_uint *counts [[buffer(8)]],
                     depth2d<float, access::read> depth [[texture(0)]],
                     texture2d_array<float, access::write> raw [[texture(1)]],
                     texture2d<float, access::read> normals [[texture(2)]],
                     uint3 gid [[thread_position_in_grid]])
{
	constant LightRaySlot &L = slots[gid.z];
	float3 o;
	float4 f, back;
	bool cull, dark;
	bool wanted = shadowRayStart(p, depth, normals, gid.xy, o, f, cull, dark, back) && lightRayWanted(L, o);
	uint n = simd_sum(wanted ? 1u : 0u);
	if(simd_is_first() && n > 0u)
		atomic_fetch_add_explicit(&counts[gid.z], n, memory_order_relaxed);
	if(!wanted){
		raw.write(float4(1.0), gid.xy, gid.z);
		return;
	}
	intersector<triangle_data, instancing> isect;
	isect.accept_any_intersection(true);
	isect.assume_geometry_type(geometry_type::triangle);
	isect.set_triangle_front_facing_winding(winding::clockwise);
	auto h = isect.intersect(lightRayToward(p, L, gid, o), tlas, 0xFF, ft);
	bool hit = h.type != intersection_type::none;
	if(hit && p.ownerOff == 0u && L.ids.y != 0u && shOwnerOf(h.user_instance_id) == L.ids.y)
		shadowListAppend(listed, list, p.listCap, gid.xy, L.ids.y | gid.z << 8);
	raw.write(float4(hit ? 0.0 : 1.0), gid.xy, gid.z);
}

kernel void
lightShadowOwnerFix(instance_acceleration_structure tlas [[buffer(0)]],
                    constant ShadowRayParams &p [[buffer(1)]],
                    device const uint *listed [[buffer(3)]],
                    device const uint2 *list [[buffer(4)]],
                    device const ShadowAlphaEntry *alpha [[buffer(5)]],
                    constant LightRaySlot *slots [[buffer(7)]],
                    depth2d<float, access::read> depth [[texture(0)]],
                    texture2d_array<float, access::write> raw [[texture(1)]],
                    texture2d<float, access::read> normals [[texture(2)]],
                    uint tid [[thread_position_in_grid]])
{
	uint n = min(listed[0], p.listCap);
	for(uint i = tid; i < n; i += SH_OWNER_THREADS){
		uint2 e = list[i];
		uint3 gid = uint3(shListX(e.x), shListY(e.x), e.y >> 8);
		uint own = e.y & 0xFFu;
		float3 o;
		float4 f, back;
		bool cull, dark;
		if(!shadowRayStart(p, depth, normals, gid.xy, o, f, cull, dark, back))
			continue;
		intersection_params ip;
		ip.assume_geometry_type(geometry_type::triangle);
		ip.set_triangle_front_facing_winding(winding::clockwise);
		ip.force_opacity(forced_opacity::non_opaque);
		ip.accept_any_intersection(true);
		intersection_query<triangle_data, instancing> iq(lightRayToward(p, slots[gid.z], gid, o), tlas, 0xFF, ip);
		while(iq.next()){
			uint user = iq.get_candidate_user_instance_id();
			if(shOwnerSkips(own, shOwnerOf(user)))
				continue;
			if(!shadowCandidateBlocks(alpha, p.alphaEntries, user, iq.get_candidate_geometry_id(), iq.get_candidate_primitive_data(),
			                          iq.get_candidate_triangle_barycentric_coord()))
				continue;
			iq.commit_triangle_intersection();
		}
		raw.write(float4(iq.get_committed_intersection_type() == intersection_type::none ? 1.0 : 0.0), gid.xy, gid.z);
	}
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

// Every normal is read: a read behind a range test serialises the loop.
static float2
shadowClampBox(texture2d<float, access::read> raw, texture2d<float, access::read> normals, constant ShadowAccumParams &p, int2 rp, uint2 off,
               float4 own, float cur, int b)
{
	int2 rmax = int2(p.rawSize) - 1;
	float lo = cur, hi = cur;
	for(int y = b; y <= 1; y++)
		for(int x = b; x <= 1; x++){
			int2 h = clamp(rp + int2(x, y), int2(0), rmax);
			float r = raw.read(uint2(h)).r;
			float4 n = shadowStoredNormal(normals, shadowRawSource(p, h, off));
			float t = shClampTapAgrees(own.w, n.w, dot(own.xyz, n.xyz)) ? r : cur;
			lo = min(lo, t);
			hi = max(hi, t);
		}
	return float2(lo, hi);
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
	SHAccum a = { cur, 1.0 };
	float2 huv = (float2(gid) + 0.5)/float2(p.size) + motion.read(gid).xy;
	if(p.valid != 0 && all(huv >= 0.0) && all(huv < 1.0)){
		uint2 hp = min(uint2(huv*float2(p.size)), p.size - 1);
		float4 h = hist.read(hp);
		if(h.g > 0.0){
			float3 Q = shadowHistoryPoint(p, hp, h.b);
			float3 N = normalize((p.viewToWorld * float4(on.xyz, 0.0)).xyz);
			if(shHistoryMatches(SHVec3{ Q.x, Q.y, Q.z }, SHVec3{ P.x, P.y, P.z }, SHVec3{ N.x, N.y, N.z }, h.b, dist)){
				float2 box = shClampBoxNeeded(h.r, cur) ? shadowClampBox(raw, normals, p, rp, off, on, cur, shClampBoxStart(p.halfRes)) : float2(cur);
				a = shAccumulateClamped(h.r, h.g, cur, box.x, box.y, p.maxHistory);
			}
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

kernel void
lightShadowAccumulate(texture2d<float, access::read> raw [[texture(0)]],
                      texture2d<float, access::read> hist [[texture(1)]],
                      texture2d<float, access::read> motion [[texture(2)]],
                      texture2d<float, access::read> normals [[texture(3)]],
                      depth2d<float, access::read> depth [[texture(4)]],
                      texture2d<float, access::write> out [[texture(5)]],
                      constant ShadowAccumParams &p [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.rawSize.x || gid.y >= p.rawSize.y)
		return;
	uint2 off = uint2(shPhaseX(p.phase), shPhaseY(p.phase));
	uint2 src = shadowRawSource(p, int2(gid), off);
	float d = depth.read(src);
	if(d >= 1.0){
		out.write(float4(1.0, 0.0, 0.0, 1.0), gid);
		return;
	}
	float cur = raw.read(gid).r;
	SHAccum a = { cur, 1.0 };
	float2 huv = (float2(src) + 0.5)/float2(p.size) + motion.read(src).xy;
	if(p.valid != 0 && all(huv >= 0.0) && all(huv < 1.0)){
		uint2 hp = min(min(uint2(huv*float2(p.size)), p.size - 1)/2, p.rawSize - 1);
		float2 h = hist.read(hp).rg;
		if(h.g > 0.0){
			float4 on = shadowStoredNormal(normals, src);
			float2 box = shClampBoxNeeded(h.r, cur) ? shadowClampBox(raw, normals, p, int2(gid), off, on, cur, shClampBoxStart(0u)) : float2(cur);
			a = shAccumulateClamped(h.r, h.g, cur, box.x, box.y, p.maxHistory);
		}
	}
	out.write(float4(a.value, a.length, 0.0, 1.0), gid);
}

kernel void
lightShadowUpsample(texture2d<float, access::read> hist [[texture(0)]],
                    texture2d<float, access::read> normals [[texture(1)]],
                    depth2d<float, access::read> depth [[texture(2)]],
                    texture2d<float, access::write> out [[texture(3)]],
                    constant ShadowAccumParams &p [[buffer(0)]],
                    uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.size.x || gid.y >= p.size.y)
		return;
	float d = depth.read(gid);
	if(d >= 1.0){
		out.write(float4(1.0), gid);
		return;
	}
	float dist = length(shadowWorld(p.invViewProj, gid, p.size, d) - p.eye.xyz);
	uint2 off = uint2(shPhaseX(p.phase), shPhaseY(p.phase));
	SHTap tx = shUpsampleTap(gid.x, off.x, 0), ty = shUpsampleTap(gid.y, off.y, 0);
	bool own = tx.weight*ty.weight == 1.0;
	float v = own ? hist.read(min(uint2(tx.index, ty.index), p.rawSize - 1)).r :
		shadowUpsample(hist, depth, normals, p, gid, dist, shadowStoredNormal(normals, gid), off);
	out.write(float4(v), gid);
}
