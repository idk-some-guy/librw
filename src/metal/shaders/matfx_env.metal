struct MatFX
{
	float4x4 texMatrix;
	float4 fxParams;
	float4 colorClamp;
	float4 envColor;
};

struct EnvIn
{
	float3 pos [[attribute(ATTRIB_POS)]];
	float4 normal [[attribute(ATTRIB_NORMAL)]];
	float4 color [[attribute(ATTRIB_COLOR)]];
	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];
};

struct EnvVertexOut
{
	float4 position [[position, invariant]];
	float4 color;
	float4 envColor;
	float2 tex0;
	float2 tex1;
	float fog;
	float4 sun [[function_constant(relit)]];
	float4 classic1 [[function_constant(relitPrelit)]];
	float4 relitNormal [[function_constant(relitPrelit)]];
	float3 relitPos [[function_constant(relitPrelit)]];
	float pointSize [[point_size]];
};

struct EnvFragmentIn
{
	float4 position [[position]];
	float4 color;
	float4 envColor;
	float2 tex0;
	float2 tex1;
	float fog;
	float4 sun [[function_constant(relit)]];
	float4 classic1 [[function_constant(relitPrelit)]];
	float4 relitNormal [[function_constant(relitPrelit)]];
	float3 relitPos [[function_constant(relitPrelit)]];
};

vertex EnvVertexOut
matfxEnvVS(EnvIn in [[stage_in]],
           constant Scene &scene [[buffer(BUFFER_SCENE)]],
           constant Object &object [[buffer(BUFFER_OBJECT)]],
           constant Lights &lights [[buffer(BUFFER_LIGHTS)]],
           constant Material &material [[buffer(BUFFER_MATERIAL)]],
           constant State &state [[buffer(BUFFER_STATE)]],
           constant MatFX &matfx [[buffer(BUFFER_MATFX)]],
           constant Relit &relitBlock [[buffer(BUFFER_RELIT), function_constant(relitPrelit)]])
{
	EnvVertexOut out;
	float4 V = object.world * float4(in.pos, 1.0);
	out.position = scene.proj * scene.view * V;
	float3 N = (object.world * float4(in.normal.xyz, 0.0)).xyz;
	out.tex0 = in.tex0;
	out.tex1 = (matfx.texMatrix * float4(relitPrelit ? float3(0.0) : N, 1.0)).xy;
	if(relitPrelit){
		float4 c1 = RelitClassic(in.color, 1.0, lights, material);
		out.envColor = max(c1, matfx.colorClamp) * matfx.envColor;
		out.classic1 = c1*material.matColor;
		out.color = RelitClassic(in.color, relitBlock.params.x, lights, material)*material.matColor;
		out.relitNormal = float4(N, in.normal.w);
		out.relitPos = V.xyz;
		out.sun = float4(0.0);
	}else{
		out.color = in.color;
		out.color.rgb += lights.ambLight.rgb*surfAmbient;
		out.color.rgb += DoDynamicLight(V.xyz, N, lights)*surfDiffuse;
		out.color = clamp(out.color, 0.0, 1.0);
		out.envColor = max(out.color, matfx.colorClamp) * matfx.envColor;
		out.color *= material.matColor;
		if(relit)
			out.sun = RelitSunPart(out.color, in.color, V.xyz, N, surfDiffuse, lights, material);
	}
	out.fog = DoFog(out.position.w, state);
	out.pointSize = 1.0;
	out.position = MetalDepth(out.position);
	return out;
}

fragment float4
matfxEnvFS(EnvFragmentIn in [[stage_in]],
           constant State &state [[buffer(BUFFER_STATE)]],
           constant MatFX &matfx [[buffer(BUFFER_MATFX)]],
           texture2d<float> tex0 [[texture(0)]],
           sampler tex0Sampler [[sampler(0)]],
           texture2d<float> tex1 [[texture(1)]],
           sampler tex1Sampler [[sampler(1)]],
           uint relitPid [[primitive_id, function_constant(relitPrelit)]],
           const device ushort *faceIdx [[buffer(BUFFER_FACEINDICES), function_constant(relitPrelit)]],
           const device uchar *faceVerts [[buffer(BUFFER_FACEVERTICES), function_constant(relitPrelit)]],
           constant FaceFetch &face [[buffer(BUFFER_FACELAYOUT), function_constant(relitPrelit)]],
           constant Relit &relitBlock [[buffer(BUFFER_RELIT), function_constant(relitFragmentBlock)]],
           constant Material &material [[buffer(BUFFER_MATERIAL), function_constant(relitPrelit)]],
           texture2d<float> shadowTex [[texture(TEXTURE_SHADOW), function_constant(relitTexture)]])
{
	float4 base = relitPrelit ? RelitFragment(in.color, in.classic1, in.relitNormal, in.relitPos, relitPid, faceIdx, faceVerts,
		face, relitBlock, material, in.position, shadowTex) :
		relit ? RelitShadowSun(in.color, in.sun, in.position, shadowTex) :
		relitShadow ? RelitPlainShadow(in.color, in.position, shadowTex, relitBlock) : in.color;
	float4 pass1 = base*tex0.sample(tex0Sampler, in.tex0);
	float4 pass2 = in.envColor*matfx.fxParams.x*tex1.sample(tex1Sampler, in.tex1);
	pass1.rgb = mix(state.fogColor.rgb, pass1.rgb, in.fog);
	pass2.rgb = mix(float3(0.0), pass2.rgb, in.fog);
	float fba = max(pass1.a, matfx.fxParams.y);
	float4 color;
	color.rgb = pass1.rgb*pass1.a + pass2.rgb*fba;
	color.a = pass1.a;
	DoAlphaTest(color.a, state);
	return color;
}
