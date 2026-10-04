struct DebugViewParams
{
	uint view;
	uint2 outSize;
	uint pad;
};

kernel void
debugView(texture2d<float> src [[texture(0)]],
          texture2d<float, access::write> out [[texture(1)]],
          constant DebugViewParams &p [[buffer(0)]],
          uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= p.outSize.x || gid.y >= p.outSize.y)
		return;
	constexpr sampler pt(filter::nearest, address::clamp_to_edge);
	float4 s = src.sample(pt, (float2(gid) + 0.5)/float2(p.outSize));
	float3 c;
	if(p.view == 1 || p.view == 4)
		c = float3(s.r);
	else if(p.view == 2){
		if(s.r == 0.0 && s.g == 0.0)
			c = float3(0.0);
		else{
			PPVec3 n = ppOctDecode(s.r, s.g);
			c = 0.5 + 0.5*float3(n.x, n.y, n.z);
		}
	}else
		c = float3(0.5 + 20.0*s.r, 0.5 + 20.0*s.g, 0.5);
	out.write(float4(c, 1.0), gid);
}
