struct Im2DIn
{
	float4 pos [[attribute(ATTRIB_POS)]];
	float4 color [[attribute(ATTRIB_COLOR)]];
	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];
};

vertex VertexOut
im2dVS(Im2DIn in [[stage_in]],
       constant Scene &scene [[buffer(BUFFER_SCENE)]],
       constant State &state [[buffer(BUFFER_STATE)]])
{
	VertexOut out;
	out.position = in.pos;
	out.position.xy = out.position.xy * scene.xform.xy + scene.xform.zw;
	out.fog = DoFog(out.position.w, state);
	out.position.xyz *= out.position.w;
	out.color = in.color;
	out.tex0 = in.tex0;
	out.pointSize = 1.0;
	out.position = MetalDepth(out.position);
	return out;
}
