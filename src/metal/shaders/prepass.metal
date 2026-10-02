#define BUFFER_PREPASSFRAME 10
#define BUFFER_PREPASSOBJECT 11
#define BUFFER_PREVSKIN 12
#define BUFFER_FACEINDICES 14
#define BUFFER_FACEVERTICES 15
#define BUFFER_FACELAYOUT 16

struct PrepassFrame
{
	float4x4 viewProj;
	float4x4 prevViewProj;
};

struct PrepassObject
{
	float4x4 prevWorld;
};

struct PrepassFace
{
	float4 col0;
	float4 col1;
	float4 col2;
	uint stride;
	uint posOffset;
	uint firstIndex;
	uint strip;
	uint numVertices;
	uint pad[3];
};

struct PrepassIn
{
	float3 pos [[attribute(ATTRIB_POS)]];
	float4 normal [[attribute(ATTRIB_NORMAL)]];
};

struct PrepassOut
{
	float4 position [[position, invariant]];
	float3 viewNormal;
	float valid;
	float3 viewPos;
	float4 curClip;
	float4 prevClip;
};

struct PrepassFragIn
{
	float4 position [[position]];
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
	out.position = MetalDepth(out.position);
	return out;
}

static PrepassTargets
prepassWrite(PrepassFragIn in, float3 g)
{
	if(dot(g, in.viewPos) > 0.0)
		g = -g;
	RLVec3 r = relitFillNormal(RLVec3{ in.viewNormal.x, in.viewNormal.y, in.viewNormal.z }, in.valid, RLVec3{ g.x, g.y, g.z },
		RLVec3{ 0.0, 0.0, -1.0 });
	float3 n = float3(r.x, r.y, r.z);
	if(dot(n, in.viewPos) > 0.0)
		n = -n;
	PPVec2 o = ppOctEncode(n.x, n.y, n.z);
	PPVec2 m = ppMotion(in.curClip.x, in.curClip.y, in.curClip.w, in.prevClip.x, in.prevClip.y, in.prevClip.w);
	PrepassTargets t;
	t.normal = float2(o.x, o.y);
	t.motion = float2(m.x, m.y);
	return t;
}

// A GPU quad can hold two triangles that share an edge, so dfdx there mixes their planes.
fragment PrepassTargets
prepassFS(PrepassFragIn in [[stage_in]])
{
	return prepassWrite(in, cross(dfdx(in.viewPos), dfdy(in.viewPos)));
}

static float3
prepassFacePos(const device uchar *v, constant PrepassFace &f, uint i)
{
	float3 p = float3(*(const device packed_float3*)(v + i*f.stride + f.posOffset));
	return f.col0.xyz*p.x + f.col1.xyz*p.y + f.col2.xyz*p.z;
}

fragment PrepassTargets
prepassFaceFS(PrepassFragIn in [[stage_in]], uint pid [[primitive_id]],
              const device ushort *idx [[buffer(BUFFER_FACEINDICES)]],
              const device uchar *v [[buffer(BUFFER_FACEVERTICES)]],
              constant PrepassFace &f [[buffer(BUFFER_FACELAYOUT)]])
{
	float3 g = cross(dfdx(in.viewPos), dfdy(in.viewPos));
	uint k = f.firstIndex + (f.strip ? pid : pid*3);
	uint i0 = idx[k], i1 = idx[k+1], i2 = idx[k+2];
	if(max(i0, max(i1, i2)) < f.numVertices){
		float3 a = prepassFacePos(v, f, i0);
		g = cross(prepassFacePos(v, f, i1) - a, prepassFacePos(v, f, i2) - a);
	}
	return prepassWrite(in, g);
}
