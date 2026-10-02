struct SkinJob
{
	uint stride;
	uint pos;
	uint weights;
	uint indices;
	uint count;
};

kernel void
skinPositions(device const uchar *verts [[buffer(0)]],
              constant SkinJob &j [[buffer(1)]],
              device const float4x4 *bones [[buffer(2)]],
              device packed_float3 *out [[buffer(3)]],
              uint i [[thread_position_in_grid]])
{
	if(i >= j.count)
		return;
	device const uchar *v = verts + i*j.stride;
	float3 p = float3(*(device const packed_float3*)(v + j.pos));
	float4 w = float4(*(device const packed_float4*)(v + j.weights));
	uchar4 b = *(device const uchar4*)(v + j.indices);
	float3 s = float3(0.0);
	for(int k = 0; k < 4; k++)
		s += (bones[min(uint(b[k]), 63u)] * float4(p, 1.0)).xyz * w[k];
	out[i] = packed_float3(s);
}

kernel void
traceOne(instance_acceleration_structure as [[buffer(0)]],
         constant float4 *r [[buffer(1)]],
         constant uint &mask [[buffer(2)]],
         device uint *hit [[buffer(3)]])
{
	intersector<triangle_data, instancing> isect;
	isect.accept_any_intersection(true);
	ray q(r[0].xyz, r[1].xyz, 0.0, r[0].w);
	hit[0] = isect.intersect(q, as, mask).type == intersection_type::none ? 0u : 1u;
}
