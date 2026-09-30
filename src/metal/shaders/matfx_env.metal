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
	float3 normal [[attribute(ATTRIB_NORMAL)]];
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
};

vertex EnvVertexOut
matfxEnvVS(EnvIn in [[stage_in]],
           constant Scene &scene [[buffer(BUFFER_SCENE)]],
           constant Object &object [[buffer(BUFFER_OBJECT)]],
           constant Lights &lights [[buffer(BUFFER_LIGHTS)]],
           constant Material &material [[buffer(BUFFER_MATERIAL)]],
           constant State &state [[buffer(BUFFER_STATE)]],
           constant MatFX &matfx [[buffer(BUFFER_MATFX)]])
{
	EnvVertexOut out;
	float4 V = object.world * float4(in.pos, 1.0);
	out.position = scene.proj * scene.view * V;
	float3 N = (object.world * float4(in.normal, 0.0)).xyz;
	out.tex0 = in.tex0;
	out.tex1 = (matfx.texMatrix * float4(N, 1.0)).xy;
	out.color = in.color;
	out.color.rgb += lights.ambLight.rgb*surfAmbient;
	out.color.rgb += DoDynamicLight(V.xyz, N, lights)*surfDiffuse;
	out.color = clamp(out.color, 0.0, 1.0);
	out.envColor = max(out.color, matfx.colorClamp) * matfx.envColor;
	out.color *= material.matColor;
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
           sampler tex1Sampler [[sampler(1)]])
{
	float4 pass1 = in.color*tex0.sample(tex0Sampler, in.tex0);
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
