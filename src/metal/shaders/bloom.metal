static float3
hdrAt(texture2d<float> scene, sampler smp, float2 uv)
{
	return scene.sample(smp, uv, level(0)).rgb;
}

kernel void
bloomDownFirst(texture2d<float> scene [[texture(0)]],
               texture2d<float, access::write> dst [[texture(2)]],
               constant uint &karis [[buffer(0)]],
               uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= dst.get_width() || gid.y >= dst.get_height())
		return;
	constexpr sampler smp(filter::linear, address::clamp_to_edge);
	float2 t = 1.0/float2(scene.get_width(), scene.get_height());
	float2 uv = (float2(gid) + 0.5)/float2(dst.get_width(), dst.get_height());
	float3 a = hdrAt(scene, smp, uv + t*float2(-2.0, -2.0));
	float3 b = hdrAt(scene, smp, uv + t*float2(0.0, -2.0));
	float3 c = hdrAt(scene, smp, uv + t*float2(2.0, -2.0));
	float3 d = hdrAt(scene, smp, uv + t*float2(-2.0, 0.0));
	float3 e = hdrAt(scene, smp, uv);
	float3 f = hdrAt(scene, smp, uv + t*float2(2.0, 0.0));
	float3 g = hdrAt(scene, smp, uv + t*float2(-2.0, 2.0));
	float3 h = hdrAt(scene, smp, uv + t*float2(0.0, 2.0));
	float3 i = hdrAt(scene, smp, uv + t*float2(2.0, 2.0));
	float3 j = hdrAt(scene, smp, uv + t*float2(-1.0, -1.0));
	float3 k = hdrAt(scene, smp, uv + t*float2(1.0, -1.0));
	float3 l = hdrAt(scene, smp, uv + t*float2(-1.0, 1.0));
	float3 m = hdrAt(scene, smp, uv + t*float2(1.0, 1.0));
	float3 o;
	if(karis != 0){
		float3 tap[13] = { a, b, c, d, e, f, g, h, i, j, k, l, m };
		float part[13] = { 0.03125, 0.0625, 0.03125, 0.0625, 0.125, 0.0625, 0.03125, 0.0625, 0.03125,
			0.125, 0.125, 0.125, 0.125 };
		float3 s = float3(0.0);
		float ws = 0.0;
		for(int n = 0; n < 13; n++){
			float w = part[n]*bloomKarisWeight(tap[n].r, tap[n].g, tap[n].b);
			s += tap[n]*w;
			ws += w;
		}
		o = s/ws;
	}else
		o = e*0.125 + (a + c + g + i)*0.03125 + (b + d + f + h)*0.0625 + (j + k + l + m)*0.125;
	dst.write(float4(o, 1.0), gid);
}

kernel void
bloomDown(texture2d<float> src [[texture(0)]],
          texture2d<float, access::write> dst [[texture(2)]],
          uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= dst.get_width() || gid.y >= dst.get_height())
		return;
	constexpr sampler smp(filter::linear, address::clamp_to_edge);
	float2 t = 1.0/float2(src.get_width(), src.get_height());
	float2 uv = (float2(gid) + 0.5)/float2(dst.get_width(), dst.get_height());
	float3 a = src.sample(smp, uv + t*float2(-2.0, -2.0), level(0)).rgb;
	float3 b = src.sample(smp, uv + t*float2(0.0, -2.0), level(0)).rgb;
	float3 c = src.sample(smp, uv + t*float2(2.0, -2.0), level(0)).rgb;
	float3 d = src.sample(smp, uv + t*float2(-2.0, 0.0), level(0)).rgb;
	float3 e = src.sample(smp, uv, level(0)).rgb;
	float3 f = src.sample(smp, uv + t*float2(2.0, 0.0), level(0)).rgb;
	float3 g = src.sample(smp, uv + t*float2(-2.0, 2.0), level(0)).rgb;
	float3 h = src.sample(smp, uv + t*float2(0.0, 2.0), level(0)).rgb;
	float3 i = src.sample(smp, uv + t*float2(2.0, 2.0), level(0)).rgb;
	float3 j = src.sample(smp, uv + t*float2(-1.0, -1.0), level(0)).rgb;
	float3 k = src.sample(smp, uv + t*float2(1.0, -1.0), level(0)).rgb;
	float3 l = src.sample(smp, uv + t*float2(-1.0, 1.0), level(0)).rgb;
	float3 m = src.sample(smp, uv + t*float2(1.0, 1.0), level(0)).rgb;
	float3 o = e*0.125 + (a + c + g + i)*0.03125 + (b + d + f + h)*0.0625 + (j + k + l + m)*0.125;
	dst.write(float4(o, 1.0), gid);
}

kernel void
bloomUp(texture2d<float> lower [[texture(0)]],
        texture2d<float, access::read> here [[texture(1)]],
        texture2d<float, access::write> dst [[texture(2)]],
        constant float &radius [[buffer(0)]],
        uint2 gid [[thread_position_in_grid]])
{
	if(gid.x >= dst.get_width() || gid.y >= dst.get_height())
		return;
	constexpr sampler smp(filter::linear, address::clamp_to_edge);
	float2 uv = (float2(gid) + 0.5)/float2(dst.get_width(), dst.get_height());
	float2 r = radius/float2(lower.get_width(), lower.get_height());
	float3 s = lower.sample(smp, uv + float2(-r.x, -r.y), level(0)).rgb
		+ 2.0*lower.sample(smp, uv + float2(0.0, -r.y), level(0)).rgb
		+ lower.sample(smp, uv + float2(r.x, -r.y), level(0)).rgb
		+ 2.0*lower.sample(smp, uv + float2(-r.x, 0.0), level(0)).rgb
		+ 4.0*lower.sample(smp, uv, level(0)).rgb
		+ 2.0*lower.sample(smp, uv + float2(r.x, 0.0), level(0)).rgb
		+ lower.sample(smp, uv + float2(-r.x, r.y), level(0)).rgb
		+ 2.0*lower.sample(smp, uv + float2(0.0, r.y), level(0)).rgb
		+ lower.sample(smp, uv + float2(r.x, r.y), level(0)).rgb;
	dst.write(float4(here.read(gid).rgb + s/16.0, 1.0), gid);
}
