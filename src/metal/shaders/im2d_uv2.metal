struct Im2DUV2In
{
	float4 pos [[attribute(ATTRIB_POS)]];
	float4 color [[attribute(ATTRIB_COLOR)]];
	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0)]];
	float2 tex1 [[attribute(ATTRIB_TEXCOORDS1)]];
};

struct VertexOutUV2
{
	float4 position [[position, invariant]];
	float4 color;
	float2 tex0;
	float2 tex1;
	float fog;
	float pointSize [[point_size]];
};

struct FragmentInUV2
{
	float4 position [[position]];
	float4 color;
	float2 tex0;
	float2 tex1;
	float fog;
};

vertex VertexOutUV2
im2dUV2VS(Im2DUV2In in [[stage_in]],
          constant Scene &scene [[buffer(BUFFER_SCENE)]],
          constant State &state [[buffer(BUFFER_STATE)]])
{
	VertexOutUV2 out;
	out.position = in.pos;
	out.position.xy = out.position.xy * scene.xform.xy + scene.xform.zw;
	out.fog = DoFog(out.position.w, state);
	out.position.xyz *= out.position.w;
	out.color = in.color;
	out.tex0 = in.tex0;
	out.tex1 = in.tex1;
	out.pointSize = 1.0;
	out.position = MetalDepth(out.position);
	return out;
}
