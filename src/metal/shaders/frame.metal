struct FrameVSOut
{
	float4 pos [[position]];
	float2 uv;
};

struct CopyRect
{
	float4 uv;
};

vertex FrameVSOut
frameVS(uint vid [[vertex_id]], constant CopyRect &rect [[buffer(0)]])
{
	FrameVSOut o;
	float2 t = float2((vid << 1) & 2, vid & 2);
	o.pos = float4(t.x*2.0 - 1.0, 1.0 - t.y*2.0, 0.0, 1.0);
	o.uv = rect.uv.xy + t*rect.uv.zw;
	return o;
}

static float3
frameColour(float2 uv, texture2d<float> scene, texture2d<float> overlay)
{
	constexpr sampler smp(filter::linear, address::clamp_to_edge);
	float3 s = scene.sample(smp, uv).rgb;
	float4 o = overlay.sample(smp, uv);
	return s*(1.0 - o.a) + o.rgb;
}

static float4
sdrPixel(float3 c)
{
	float m = max(c.r, max(c.g, c.b));
	return float4(sdrShoulder(c.r, m), sdrShoulder(c.g, m), sdrShoulder(c.b, m), 1.0);
}

fragment float4
frameCompositeFS(FrameVSOut in [[stage_in]],
                 texture2d<float> scene [[texture(0)]],
                 texture2d<float> overlay [[texture(1)]])
{
	return sdrPixel(frameColour(in.uv, scene, overlay));
}

fragment float4
frameCopySceneFS(FrameVSOut in [[stage_in]],
                 texture2d<float> scene [[texture(0)]])
{
	constexpr sampler smp(filter::linear, address::clamp_to_edge);
	return float4(saturate(scene.sample(smp, in.uv).rgb), 1.0);
}

fragment float4
frameCopyPostFS(FrameVSOut in [[stage_in]],
                texture2d<float> scene [[texture(0)]],
                texture2d<float> overlay [[texture(1)]])
{
	return sdrPixel(frameColour(in.uv, scene, overlay));
}

fragment float4
frameCompositeEdrFS(FrameVSOut in [[stage_in]],
                    texture2d<float> scene [[texture(0)]],
                    texture2d<float> overlay [[texture(1)]],
                    constant float4 &out [[buffer(0)]])
{
	float3 c = frameColour(in.uv, scene, overlay);
	return float4(edrChannel(c.r, out.x), edrChannel(c.g, out.x), edrChannel(c.b, out.x), 1.0);
}

fragment float4
frameClassicEdrFS(FrameVSOut in [[stage_in]],
                  texture2d<float> tex [[texture(0)]])
{
	constexpr sampler smp(filter::linear, address::clamp_to_edge);
	float3 c = tex.sample(smp, in.uv).rgb;
	return float4(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b), 1.0);
}
