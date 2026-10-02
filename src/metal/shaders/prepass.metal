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
	float3 normal [[attribute(ATTRIB_NORMAL)]];
};

struct PrepassOut
{
	float4 position [[position, invariant]];
	float3 viewNormal;
	float3 viewPos;
	float4 curClip;
	float4 prevClip;
};

struct PrepassFragIn
{
	float4 position [[position]];
	float3 viewNormal;
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
	out.viewNormal = (scene.view * float4((object.world * float4(in.normal, 0.0)).xyz, 0.0)).xyz;
	out.viewPos = (scene.view * V).xyz;
	out.curClip = pf.viewProj * V;
	out.prevClip = pf.prevViewProj * (po.prevWorld * float4(in.pos, 1.0));
	out.position = MetalDepth(out.position);
	return out;
}

fragment PrepassTargets
prepassFS(PrepassFragIn in [[stage_in]])
{
	float3 n = in.viewNormal;
	if(dot(n, n) < 1e-12)
		n = cross(dfdx(in.viewPos), dfdy(in.viewPos));
	n = normalize(n);
	if(dot(n, in.viewPos) > 0.0)
		n = -n;
	PPVec2 o = ppOctEncode(n.x, n.y, n.z);
	PPVec2 m = ppMotion(in.curClip.x, in.curClip.y, in.curClip.w, in.prevClip.x, in.prevClip.y, in.prevClip.w);
	PrepassTargets t;
	t.normal = float2(o.x, o.y);
	t.motion = float2(m.x, m.y);
	return t;
}
