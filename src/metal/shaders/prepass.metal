#define BUFFER_PREPASSFRAME 10
#define BUFFER_PREPASSOBJECT 11
#define BUFFER_PREVSKIN 12

struct PrepassFrame
{
	float4x4 viewProj;
	float4x4 prevViewProj;
};

struct PrepassObject
{
	float4x4 prevWorld;
};

struct PrepassIn
{
	float3 pos [[attribute(ATTRIB_POS)]];
	float4 normal [[attribute(ATTRIB_NORMAL)]];
	float2 tex0 [[attribute(ATTRIB_TEXCOORDS0), function_constant(alphaTest)]];
};

struct PrepassOut
{
	float4 position [[position, invariant]];
	float2 tex0;
	float3 viewNormal;
	float valid;
	float3 viewPos;
	float4 curClip;
	float4 prevClip;
};

struct PrepassFragIn
{
	float4 position [[position]];
	float2 tex0;
	float3 viewNormal;
	float valid;
	float3 viewPos;
	float4 curClip;
	float4 prevClip;
};

struct PrepassTargets
{
	float2 normal [[color(0)]];
	float2 motion [[color(1)]];
};

vertex PrepassOut
prepassVS(PrepassIn in [[stage_in]],
          constant Scene &scene [[buffer(BUFFER_SCENE)]],
          constant Object &object [[buffer(BUFFER_OBJECT)]],
          constant PrepassFrame &pf [[buffer(BUFFER_PREPASSFRAME)]],
          constant PrepassObject &po [[buffer(BUFFER_PREPASSOBJECT)]])
{
	PrepassOut out;
	float4 V = object.world * float4(in.pos, 1.0);
	out.position = scene.proj * scene.view * V;
	out.viewNormal = (scene.view * float4((object.world * float4(in.normal.xyz, 0.0)).xyz, 0.0)).xyz;
	out.valid = in.normal.w;
	out.viewPos = (scene.view * V).xyz;
	out.curClip = pf.viewProj * V;
	out.prevClip = pf.prevViewProj * (po.prevWorld * float4(in.pos, 1.0));
	out.tex0 = float2(0.0);
	if(alphaTest)
		out.tex0 = in.tex0;
	out.position = MetalDepth(out.position);
	return out;
}

static void
prepassCutout(float2 uv, texture2d<float> tex, sampler smp, constant State &state, bool layer)
{
	float a = tex.sample(smp, uv).a;
	if(!(layer ? ppLayerKeep(a, state.alphaRef.x) : ppCutoutKeep(a, state.alphaRef.x)))
		discard_fragment();
}

static PrepassTargets
prepassWrite(PrepassFragIn in, float3 g)
{
	float3 f = RelitFilledNormal(in.viewNormal, in.valid, g, -in.viewPos, float3(0.0, 0.0, -1.0)), v = normalize(-in.viewPos);
	PPVec3 n = ppFacingNormal(PPVec3{ f.x, f.y, f.z }, PPVec3{ v.x, v.y, v.z });
	PPVec2 o = ppOctEncode(n.x, n.y, n.z);
	PPVec2 m = ppMotion(in.curClip.x, in.curClip.y, in.curClip.w, in.prevClip.x, in.prevClip.y, in.prevClip.w);
	PrepassTargets t;
	t.normal = float2(o.x, o.y);
	t.motion = float2(m.x, m.y);
	return t;
}

// A GPU quad can hold two triangles that share an edge, so dfdx there mixes their planes.
fragment PrepassTargets
prepassFS(PrepassFragIn in [[stage_in]],
          constant State &state [[buffer(BUFFER_STATE), function_constant(alphaTest)]],
          texture2d<float> tex0 [[texture(0), function_constant(alphaTest)]],
          sampler tex0Sampler [[sampler(0), function_constant(alphaTest)]])
{
	float3 g = cross(dfdx(in.viewPos), dfdy(in.viewPos));
	if(alphaTest)
		prepassCutout(in.tex0, tex0, tex0Sampler, state, false);
	return prepassWrite(in, g);
}

fragment PrepassTargets
prepassLayerFS(PrepassFragIn in [[stage_in]],
               constant State &state [[buffer(BUFFER_STATE), function_constant(alphaTest)]],
               texture2d<float> tex0 [[texture(0), function_constant(alphaTest)]],
               sampler tex0Sampler [[sampler(0), function_constant(alphaTest)]])
{
	float3 g = cross(dfdx(in.viewPos), dfdy(in.viewPos));
	if(alphaTest)
		prepassCutout(in.tex0, tex0, tex0Sampler, state, true);
	return prepassWrite(in, g);
}

fragment PrepassTargets
prepassFaceFS(PrepassFragIn in [[stage_in]], uint pid [[primitive_id]],
              const device ushort *idx [[buffer(BUFFER_FACEINDICES)]],
              const device uchar *v [[buffer(BUFFER_FACEVERTICES)]],
              constant FaceFetch &f [[buffer(BUFFER_FACELAYOUT)]],
              constant State &state [[buffer(BUFFER_STATE), function_constant(alphaTest)]],
              texture2d<float> tex0 [[texture(0), function_constant(alphaTest)]],
              sampler tex0Sampler [[sampler(0), function_constant(alphaTest)]])
{
	float3 g = cross(dfdx(in.viewPos), dfdy(in.viewPos));
	if(alphaTest)
		prepassCutout(in.tex0, tex0, tex0Sampler, state, false);
	faceFetchNormal(pid, idx, v, f, g);
	return prepassWrite(in, g);
}

fragment PrepassTargets
prepassLayerFaceFS(PrepassFragIn in [[stage_in]], uint pid [[primitive_id]],
                   const device ushort *idx [[buffer(BUFFER_FACEINDICES)]],
                   const device uchar *v [[buffer(BUFFER_FACEVERTICES)]],
                   constant FaceFetch &f [[buffer(BUFFER_FACELAYOUT)]],
                   constant State &state [[buffer(BUFFER_STATE), function_constant(alphaTest)]],
                   texture2d<float> tex0 [[texture(0), function_constant(alphaTest)]],
                   sampler tex0Sampler [[sampler(0), function_constant(alphaTest)]])
{
	float3 g = cross(dfdx(in.viewPos), dfdy(in.viewPos));
	if(alphaTest)
		prepassCutout(in.tex0, tex0, tex0Sampler, state, true);
	faceFetchNormal(pid, idx, v, f, g);
	return prepassWrite(in, g);
}
