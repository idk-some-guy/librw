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

constant bool alphaTest [[function_constant(0)]];
constant bool directionals [[function_constant(1)]];
constant bool pointLights [[function_constant(2)]];
constant bool spotLights [[function_constant(3)]];

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

struct VertexOut
{
	float4 position [[position]];
	float4 color;
	float2 tex0;
	float fog;
	float pointSize [[point_size]];
};

struct FragmentIn
{
	float4 position [[position]];
	float4 color;
	float2 tex0;
	float fog;
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
