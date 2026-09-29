struct Im3DIn
{
	float3 pos [[attribute(ATTRIB_POS)]];
	float4 color [[attribute(ATTRIB_COLOR)]];
	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];
};

vertex VertexOut
im3dVS(Im3DIn in [[stage_in]],
       constant Scene &scene [[buffer(BUFFER_SCENE)]],
       constant Object &object [[buffer(BUFFER_OBJECT)]],
       constant State &state [[buffer(BUFFER_STATE)]])
{
	VertexOut out;
	float4 V = object.world * float4(in.pos, 1.0);
	out.position = scene.proj * scene.view * V;
	out.color = in.color;
	out.tex0 = in.tex0;
	out.fog = DoFog(out.position.w, state);
	out.pointSize = 1.0;
	out.position = MetalDepth(out.position);
	return out;
}
