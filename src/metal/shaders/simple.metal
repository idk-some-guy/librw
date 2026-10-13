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
         constant Material &material [[buffer(BUFFER_MATERIAL), function_constant(lightsMaterial)]],
         texture2d<float> shadowTex [[texture(TEXTURE_SHADOW), function_constant(relitTexture)]],
         constant LTFrame &lightFrame [[buffer(BUFFER_LIGHTFRAME), function_constant(lights)]],
         const device LTLight *lightList [[buffer(BUFFER_LIGHTLIST), function_constant(lights)]],
         const device ushort *lightClusters [[buffer(BUFFER_LIGHTCLUSTERS), function_constant(lights)]],
         constant LTDraw &lightDraw [[buffer(BUFFER_LIGHTDRAW), function_constant(lights)]])
{
	float3 n = float3(0.0, 0.0, 1.0);
	float4 c = relitPrelit ? RelitFragment(in.color, in.classic1, in.relitNormal, in.relitPos, relitPid, faceIdx, faceVerts,
		face, relitBlock, material, in.position, shadowTex, n) :
		relit ? RelitShadowSun(in.color, in.sun, in.position, shadowTex) :
		relitShadow ? RelitPlainShadow(in.color, in.position, in.plainPos, shadowTex, relitBlock) : in.color;
	float3 lightAdd = float3(0.0);
	if(lights){
		LightSurface s = relitPrelit ? LightsSurface(in.relitPos, n, lightFrame, material) :
			lightsLit ? LightsSurface(in.lightPos, in.lightNormal, material) : LightsSurface(in.plainPos, lightFrame, material);
		lightAdd = LightsSum(s.rel, s.N, s.albedo, s.gloss, in.position, lightFrame, lightList, lightClusters);
	}
	float4 t0 = tex0.sample(tex0Sampler, in.tex0);
	float4 color = c*t0;
	if(lights)
		color.rgb = LightsEmissive(color.rgb, lightAdd*t0.rgb, lightDraw);
	color.rgb = mix(state.fogColor.rgb, color.rgb, in.fog);
	DoAlphaTest(color.a, state);
	return color;
}
