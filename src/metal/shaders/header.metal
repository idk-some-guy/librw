#include <metal_stdlib>
using namespace metal;

#define ATTRIB_POS	0
#define ATTRIB_NORMAL	1
#define ATTRIB_COLOR	2
#define ATTRIB_WEIGHTS	3
#define ATTRIB_INDICES	4
#define ATTRIB_TEXCOORDS0	5
#define ATTRIB_TEXCOORDS1	6

#define BUFFER_VERTEX	0
#define BUFFER_SCENE	1
#define BUFFER_OBJECT	2
#define BUFFER_MATERIAL	3
#define BUFFER_STATE	4
#define BUFFER_SKIN	5
#define BUFFER_CUSTOM	6
#define BUFFER_LIGHTS	7
#define BUFFER_MATFX	8
#define BUFFER_EMISSIVE	9
#define BUFFER_NORMALSTREAM 13
#define BUFFER_FACEINDICES 14
#define BUFFER_FACEVERTICES 15
#define BUFFER_FACELAYOUT 16
#define BUFFER_RELIT 17
#define TEXTURE_SHADOW 8

constant bool alphaTest [[function_constant(0)]];
constant bool directionals [[function_constant(1)]];
constant bool pointLights [[function_constant(2)]];
constant bool spotLights [[function_constant(3)]];
constant bool relit [[function_constant(6)]];
constant bool relitPrelit [[function_constant(7)]];
constant bool relitShadow [[function_constant(8)]];
constant bool relitTexture = relit || relitShadow;
constant bool relitFragmentBlock = relitPrelit || relitShadow;
constant bool relitPlain = relitShadow && !relit && !relitPrelit;

struct State
{
	float4 alphaRef;
	float4 fogData;
	float4 fogColor;
};

#define u_fogStart (state.fogData.x)
#define u_fogEnd (state.fogData.y)
#define u_fogRange (state.fogData.z)
#define u_fogDisable (state.fogData.w)

struct Scene
{
	float4x4 proj;
	float4x4 view;
	float4 xform;
};

#define MAX_LIGHTS 8

struct Object
{
	float4x4 world;
};

struct Lights
{
	float4 ambLight;
	float4 lightParams[MAX_LIGHTS];
	float4 lightPosition[MAX_LIGHTS];
	float4 lightDirection[MAX_LIGHTS];
	float4 lightColor[MAX_LIGHTS];
};

struct Material
{
	float4 matColor;
	float4 surfProps;
};

#define surfAmbient (material.surfProps.x)
#define surfSpecular (material.surfProps.y)
#define surfDiffuse (material.surfProps.z)

struct Relit
{
	float4 sunDir;
	float4 sunColor;
	float4 skyTop;
	float4 skyBottom;
	float4 eye;
	float4 params;
	float4 shadow;
};

struct FaceFetch
{
	float4 col0;
	float4 col1;
	float4 col2;
	uint stride;
	uint posOffset;
	uint firstIndex;
	uint strip;
	uint numVertices;
	uint twoSided;
	uint skipFull;
	uint pad[1];
};

struct VertexOut
{
	float4 position [[position, invariant]];
	float4 color;
	float2 tex0;
	float fog;
	float4 sun [[function_constant(relit)]];
	float4 classic1 [[function_constant(relitPrelit)]];
	float4 relitNormal [[function_constant(relitPrelit)]];
	float3 relitPos [[function_constant(relitPrelit)]];
	float3 plainPos [[function_constant(relitPlain)]];
	float pointSize [[point_size]];
};

struct FragmentIn
{
	float4 position [[position]];
	float4 color;
	float2 tex0;
	float fog;
	float4 sun [[function_constant(relit)]];
	float4 classic1 [[function_constant(relitPrelit)]];
	float4 relitNormal [[function_constant(relitPrelit)]];
	float3 relitPos [[function_constant(relitPrelit)]];
	float3 plainPos [[function_constant(relitPlain)]];
};

static float3
DoDynamicLight(float3 V, float3 N, constant Lights &lights)
{
	float3 color = float3(0.0, 0.0, 0.0);
	for(int i = 0; i < MAX_LIGHTS; i++){
		if(lights.lightParams[i].x == 0.0)
			break;
		if(directionals && lights.lightParams[i].x == 1.0){
			float l = max(0.0, dot(N, -lights.lightDirection[i].xyz));
			color += l*lights.lightColor[i].rgb;
		}else if(pointLights && lights.lightParams[i].x == 2.0){
			float3 dir = V - lights.lightPosition[i].xyz;
			float dist = length(dir);
			float atten = max(0.0, (1.0 - dist/lights.lightParams[i].y));
			float l = max(0.0, dot(N, -normalize(dir)));
			color += l*lights.lightColor[i].rgb*atten;
		}else if(spotLights && lights.lightParams[i].x == 3.0){
			float3 dir = V - lights.lightPosition[i].xyz;
			float dist = length(dir);
			float atten = max(0.0, (1.0 - dist/lights.lightParams[i].y));
			dir /= dist;
			float l = max(0.0, dot(N, -dir));
			float pcos = dot(dir, lights.lightDirection[i].xyz);
			float ccos = -lights.lightParams[i].z;
			float falloff = (pcos-ccos)/(1.0-ccos);
			if(falloff < 0.0)
				l = 0.0;
			l *= max(falloff, lights.lightParams[i].w);
			return l*lights.lightColor[i].rgb*atten;
		}
	}
	return color;
}

static float3
DoSunLight(float3 N, constant Lights &lights)
{
	float3 c = float3(0.0);
	for(int i = 0; i < MAX_LIGHTS; i++){
		if(lights.lightParams[i].x == 0.0)
			break;
		if(directionals && lights.lightParams[i].x == 1.0){
			if(lights.lightParams[i].w == 1.0)
				c += max(0.0, dot(N, -lights.lightDirection[i].xyz))*lights.lightColor[i].rgb;
		}else if(spotLights && lights.lightParams[i].x == 3.0)
			return float3(0.0);
	}
	return c;
}

static float4
RelitClassic(float4 prelit, float w, constant Lights &lights, constant Material &material)
{
	float4 c = float4(relitWeighted(prelit.r, w), relitWeighted(prelit.g, w), relitWeighted(prelit.b, w), prelit.a);
	c.rgb += lights.ambLight.rgb*surfAmbient;
	return clamp(c, 0.0, 1.0);
}

static float4
RelitSunPart(float4 full, float4 prelit, float3 V, float3 N, float diffuse, constant Lights &lights,
             constant Material &material)
{
	float3 c = prelit.rgb + lights.ambLight.rgb*surfAmbient + (DoDynamicLight(V, N, lights) - DoSunLight(N, lights))*diffuse;
	return float4(full.rgb - clamp(c, 0.0, 1.0)*material.matColor.rgb, 0.0);
}

static float3
faceFetchPos(const device uchar *v, constant FaceFetch &f, uint i)
{
	return float3(*(const device packed_float3*)(v + i*f.stride + f.posOffset));
}

static bool
faceFetchNormal(uint pid, const device ushort *idx, const device uchar *v, constant FaceFetch &f, thread float3 &g)
{
	if(f.numVertices == 0)
		return false;
	uint k = f.firstIndex + (f.strip ? pid : pid*3);
	uint i0 = idx[k], i1 = idx[k+1], i2 = idx[k+2];
	if(max(i0, max(i1, i2)) >= f.numVertices)
		return false;
	float3 a = faceFetchPos(v, f, i0);
	float3 e = cross(faceFetchPos(v, f, i1) - a, faceFetchPos(v, f, i2) - a);
	g = f.col0.xyz*e.x + f.col1.xyz*e.y + f.col2.xyz*e.z;
	return true;
}

static float3
RelitFilledNormal(float3 n, float valid, float3 g, float3 toEye, float3 fallback)
{
	if(dot(g, toEye) < 0.0)
		g = -g;
	RLVec3 r = relitFillNormal(RLVec3{ n.x, n.y, n.z }, valid, RLVec3{ g.x, g.y, g.z }, RLVec3{ fallback.x, fallback.y, fallback.z });
	return float3(r.x, r.y, r.z);
}

static float3
RelitSurfaceNormal(float3 n, float valid, float3 g, float3 toEye, float3 fallback)
{
	RLVec3 r = relitSurfaceNormal(RLVec3{ n.x, n.y, n.z }, valid, RLVec3{ g.x, g.y, g.z }, RLVec3{ toEye.x, toEye.y, toEye.z },
		RLVec3{ fallback.x, fallback.y, fallback.z });
	return float3(r.x, r.y, r.z);
}

static float
RelitShadowFactor(float4 position, texture2d<float> shadowTex)
{
	uint2 p = min(uint2(position.xy), uint2(shadowTex.get_width() - 1, shadowTex.get_height() - 1));
	return shadowTex.read(p).r;
}

static float4
RelitShadowSun(float4 color, float4 sun, float4 position, texture2d<float> shadowTex)
{
	if(!relitShadow)
		return color;
	float s = RelitShadowFactor(position, shadowTex);
	return float4(relitShadowed(color.r, sun.r, s), relitShadowed(color.g, sun.g, s), relitShadowed(color.b, sun.b, s), color.a);
}

static float4
RelitPlainShadow(float4 color, float4 position, float3 pos, texture2d<float> shadowTex, constant Relit &r)
{
	float3 toEye = r.eye.xyz - pos, g = cross(dfdx(pos), dfdy(pos));
	float3 n = RelitSurfaceNormal(g, 0.0, g, toEye, normalize(toEye));
	float s = relitFacedShadow(RelitShadowFactor(position, shadowTex), dot(n, -r.sunDir.xyz), r.shadow.y), k = r.shadow.x;
	return float4(relitPlainShadowed(color.r, s, k), relitPlainShadowed(color.g, s, k), relitPlainShadowed(color.b, s, k), color.a);
}

static float4
RelitFragment(float4 color, float4 classic1, float4 nrm, float3 pos, uint pid, const device ushort *idx,
              const device uchar *verts, constant FaceFetch &face, constant Relit &r, constant Material &material,
              float4 position, texture2d<float> shadowTex)
{
	float3 toEye = r.eye.xyz - pos;
	float3 g, n;
	float v = 1.0;
	bool known = false;
	if(face.skipFull != 0 && face.numVertices != 0 && nrm.w >= 1.0 && dot(nrm.xyz, nrm.xyz) > 1e-30){
		float3 u = normalize(nrm.xyz), e = normalize(toEye);
		RLVec3 q = relitFacingNormal(RLVec3{ u.x, u.y, u.z }, RLVec3{ e.x, e.y, e.z });
		n = float3(q.x, q.y, q.z);
		known = dot(n, toEye) != 0.0;
	}
	if(!known){
		if(faceFetchNormal(pid, idx, verts, face, g))
			n = RelitSurfaceNormal(nrm.xyz, nrm.w, g, toEye, normalize(toEye));
		else{
			v = dot(nrm.xyz, nrm.xyz) > 1e-30 ? saturate(nrm.w) : 0.0;
			n = v > 0.0 ? normalize(nrm.xyz) : float3(0.0, 0.0, 1.0);
			if(dot(n, toEye) < 0.0)
				n = -n;
		}
	}
	float L = 1.0 - r.params.x;
	RLColor top = { r.skyTop.r, r.skyTop.g, r.skyTop.b };
	RLColor bottom = { r.skyBottom.r, r.skyBottom.g, r.skyBottom.b };
	RLColor h = relitDesaturate(relitHemisphere(n.z, top, bottom), r.params.z);
	float hs = L*r.params.y*surfAmbient;
	float ss = relitSunCos(dot(n, -r.sunDir.xyz), face.twoSided != 0)*r.sunDir.w*L*r.params.w*surfDiffuse;
	float3 m = material.matColor.rgb;
	RLSplit sr = relitSplitAdd(color.r, h.r*hs, r.sunColor.r*ss, m.r);
	RLSplit sg = relitSplitAdd(color.g, h.g*hs, r.sunColor.g*ss, m.g);
	RLSplit sb = relitSplitAdd(color.b, h.b*hs, r.sunColor.b*ss, m.b);
	float3 lit = float3(sr.full, sg.full, sb.full);
	float3 c1 = classic1.rgb;
	if(relitShadow){
		float s = RelitShadowFactor(position, shadowTex), k = r.shadow.x, f = relitFacedShadow(s, dot(n, -r.sunDir.xyz), r.shadow.y);
		float ls = face.twoSided != 0 ? f : s;
		lit = float3(relitShadowedPrelit(sr.full, sr.sun, color.r, ls, f, k), relitShadowedPrelit(sg.full, sg.sun, color.g, ls, f, k),
			relitShadowedPrelit(sb.full, sb.sun, color.b, ls, f, k));
		c1 = float3(relitPlainShadowed(c1.r, f, k), relitPlainShadowed(c1.g, f, k), relitPlainShadowed(c1.b, f, k));
	}
	if(v >= 1.0)
		return float4(lit, color.a);
	return float4(c1 + (lit - c1)*v, color.a);
}

static float
DoFog(float w, constant State &state)
{
	return clamp((w - u_fogEnd)*u_fogRange, u_fogDisable, 1.0);
}

static float4
MetalDepth(float4 pos)
{
	pos.z = 0.5*(pos.z + pos.w);
	return pos;
}

static void
DoAlphaTest(float a, constant State &state)
{
	if(alphaTest && (a < state.alphaRef.x || a >= state.alphaRef.y))
		discard_fragment();
}
