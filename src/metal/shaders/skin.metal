#define MAX_BONES 64

struct Skin
{
	float4x4 bones[MAX_BONES];
};

struct SkinIn
{
	float3 pos [[attribute(ATTRIB_POS)]];
	float3 normal [[attribute(ATTRIB_NORMAL)]];
	float4 color [[attribute(ATTRIB_COLOR)]];
	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];
	float4 weights [[attribute(ATTRIB_WEIGHTS)]];
	uchar4 indices [[attribute(ATTRIB_INDICES)]];
};

vertex VertexOut
skinVS(SkinIn in [[stage_in]],
       constant Scene &scene [[buffer(BUFFER_SCENE)]],
       constant Object &object [[buffer(BUFFER_OBJECT)]],
       constant Lights &lights [[buffer(BUFFER_LIGHTS)]],
       constant Material &material [[buffer(BUFFER_MATERIAL)]],
       constant State &state [[buffer(BUFFER_STATE)]],
       constant Skin &skin [[buffer(BUFFER_SKIN)]])
{
	VertexOut out;
	float3 SkinVertex = float3(0.0);
	float3 SkinNormal = float3(0.0);
	for(int i = 0; i < 4; i++){
		float4x4 bone = skin.bones[min(uint(in.indices[i]), uint(MAX_BONES-1))];
		SkinVertex += (bone * float4(in.pos, 1.0)).xyz * in.weights[i];
		SkinNormal += (bone * float4(in.normal, 0.0)).xyz * in.weights[i];
	}
	float4 V = object.world * float4(SkinVertex, 1.0);
	out.position = scene.proj * scene.view * V;
	float3 N = (object.world * float4(SkinNormal, 0.0)).xyz;
	out.tex0 = in.tex0;
	out.color = in.color;
	out.color.rgb += lights.ambLight.rgb*surfAmbient;
	out.color.rgb += DoDynamicLight(V.xyz, N, lights)*surfDiffuse;
	out.color = clamp(out.color, 0.0, 1.0);
	out.color *= material.matColor;
	out.fog = DoFog(out.position.z, state);
	out.pointSize = 1.0;
	out.position = MetalDepth(out.position);
	return out;
}
