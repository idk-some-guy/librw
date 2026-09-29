fragment float4
simpleFS(FragmentIn in [[stage_in]],
         constant State &state [[buffer(BUFFER_STATE)]],
         texture2d<float> tex0 [[texture(0)]],
         sampler tex0Sampler [[sampler(0)]])
{
	float4 color = in.color*tex0.sample(tex0Sampler, in.tex0);
	color.rgb = mix(state.fogColor.rgb, color.rgb, in.fog);
	DoAlphaTest(color.a, state);
	return color;
}
