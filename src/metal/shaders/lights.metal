constant uint LIGHT_IS_SPOT = 1;

static float3
clusterRay(constant LTFrame &f, float nx, float ny)
{
	float4 p = f.invProj*float4(nx, ny, 0.5f, 1.0f);
	return float3(p.x/p.z, p.y/p.z, 1.0f);
}

kernel void
lightCluster(constant LTFrame &f [[buffer(0)]],
	const device LTLight *lights [[buffer(1)]],
	device ushort *indices [[buffer(2)]],
	device uint *counts [[buffer(3)]],
	device atomic_uint *stats [[buffer(4)]],
	uint3 gid [[thread_position_in_grid]])
{
	if(gid.x >= LT_CLUSTER_X || gid.y >= LT_CLUSTER_Y || gid.z >= LT_CLUSTER_Z)
		return;
	float zNear = f.axis.w, zFar = f.grid.z;
	float z0 = ltSliceNear(gid.z, zNear, zFar);
	float z1 = gid.z + 1 >= LT_CLUSTER_Z ? zFar : ltSliceNear(gid.z + 1, zNear, zFar);
	float x0 = -1.0f + 2.0f*(float)gid.x/(float)LT_CLUSTER_X, x1 = -1.0f + 2.0f*(float)(gid.x + 1)/(float)LT_CLUSTER_X;
	float y1 = 1.0f - 2.0f*(float)gid.y/(float)LT_CLUSTER_Y, y0 = 1.0f - 2.0f*(float)(gid.y + 1)/(float)LT_CLUSTER_Y;
	float3 lo = float3(INFINITY), hi = float3(-INFINITY);
	for(uint c = 0; c < 4; c++){
		float3 ray = clusterRay(f, (c & 1) ? x1 : x0, (c & 2) ? y1 : y0);
		float3 a = ray*z0, b = ray*z1;
		lo = min(lo, min(a, b));
		hi = max(hi, max(a, b));
	}
	float3 centre = 0.5f*(lo + hi);
	float reach = length(hi - lo)*0.5f;
	LTVec3 blo = { lo.x, lo.y, lo.z }, bhi = { hi.x, hi.y, hi.z };
	float3x3 rot = float3x3(f.view[0].xyz, f.view[1].xyz, f.view[2].xyz);
	uint count = (uint)f.grid.w;
	ushort keep[LT_PER_CLUSTER];
	float score[LT_PER_CLUSTER];
	uint kept = 0, hits = 0;
	for(uint i = 0; i < count; i++){
		LTLight l = lights[i];
		float3 p = rot*l.posRadius.xyz;
		LTVec3 pos = { p.x, p.y, p.z };
		bool hit;
		if((l.ids.y & LIGHT_IS_SPOT) != 0){
			float3 d = normalize(rot*l.dirOuter.xyz);
			LTVec3 dir = { d.x, d.y, d.z };
			LTVec3 mid = { centre.x, centre.y, centre.z };
			hit = ltConeHitsSphere(pos, dir, l.dirOuter.w, l.posRadius.w, mid, reach);
		}else
			hit = ltSphereHitsBox(pos, l.posRadius.w, blo, bhi);
		if(!hit)
			continue;
		hits++;
		float3 off = p - centre;
		float s = ltScore(fmax(l.colorGloss.x, fmax(l.colorGloss.y, l.colorGloss.z)), dot(off, off));
		if(kept < LT_PER_CLUSTER){
			keep[kept] = (ushort)i;
			score[kept] = s;
			kept++;
		}else{
			uint worst = 0;
			for(uint k = 1; k < LT_PER_CLUSTER; k++)
				if(score[k] < score[worst])
					worst = k;
			if(s > score[worst]){
				keep[worst] = (ushort)i;
				score[worst] = s;
			}
		}
	}
	uint cluster = gid.x + LT_CLUSTER_X*(gid.y + LT_CLUSTER_Y*gid.z);
	for(uint k = 0; k < kept; k++)
		indices[cluster*LT_PER_CLUSTER + k] = keep[k];
	counts[cluster] = kept;
	if(hits > kept)
		atomic_fetch_add_explicit(&stats[0], hits - kept, memory_order_relaxed);
	atomic_fetch_max_explicit(&stats[1], hits, memory_order_relaxed);
}
