struct DefaultIn
{
	float3 pos [[attribute(ATTRIB_POS)]];
	float3 normal [[attribute(ATTRIB_NORMAL)]];
	float4 color [[attribute(ATTRIB_COLOR)]];
	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];
};

vertex VertexOut
defaultVS(DefaultIn in [[stage_in]],
          constant Scene &scene [[buffer(BUFFER_SCENE)]],
          constant Object &object [[buffer(BUFFER_OBJECT)]],
          constant Lights &lights [[buffer(BUFFER_LIGHTS)]],
          constant Material &material [[buffer(BUFFER_MATERIAL)]],
          constant State &state [[buffer(BUFFER_STATE)]])
{
	VertexOut out;
	float4 V = object.world * float4(in.pos, 1.0);
	out.position = scene.proj * scene.view * V;
	float3 N = (object.world * float4(in.normal, 0.0)).xyz;
	out.tex0 = in.tex0;
	out.color = in.color;
	out.color.rgb += lights.ambLight.rgb*surfAmbient;
	out.color.rgb += DoDynamicLight(V.xyz, N, lights)*surfDiffuse;
	out.color = clamp(out.color, 0.0, 1.0);
	out.color *= material.matColor;
	out.fog = DoFog(out.position.w, state);
	out.pointSize = 1.0;
	out.position = MetalDepth(out.position);
	return out;
}
