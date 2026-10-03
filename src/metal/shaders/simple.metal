fragment float4
simpleFS(FragmentIn in [[stage_in]],
         constant State &state [[buffer(BUFFER_STATE)]],
         texture2d<float> tex0 [[texture(0)]],
         sampler tex0Sampler [[sampler(0)]],
         uint relitPid [[primitive_id, function_constant(relitPrelit)]],
         const device ushort *faceIdx [[buffer(BUFFER_FACEINDICES), function_constant(relitPrelit)]],
         const device uchar *faceVerts [[buffer(BUFFER_FACEVERTICES), function_constant(relitPrelit)]],
         constant FaceFetch &face [[buffer(BUFFER_FACELAYOUT), function_constant(relitPrelit)]],
         constant Relit &relitBlock [[buffer(BUFFER_RELIT), function_constant(relitFragmentBlock)]],
         constant Material &material [[buffer(BUFFER_MATERIAL), function_constant(relitPrelit)]],
         texture2d<float> shadowTex [[texture(TEXTURE_SHADOW), function_constant(relitTexture)]])
{
	float4 c = relitPrelit ? RelitFragment(in.color, in.classic1, in.relitNormal, in.relitPos, relitPid, faceIdx, faceVerts,
		face, relitBlock, material, in.position, shadowTex) :
		relit ? RelitShadowSun(in.color, in.sun, in.position, shadowTex) :
		relitShadow ? RelitPlainShadow(in.color, in.position, shadowTex, relitBlock) : in.color;
	float4 color = c*tex0.sample(tex0Sampler, in.tex0);
	color.rgb = mix(state.fogColor.rgb, color.rgb, in.fog);
	DoAlphaTest(color.a, state);
	return color;
}
