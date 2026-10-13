#ifndef RW_METAL_METALLIGHTS_H
#define RW_METAL_METALLIGHTS_H

#ifdef __METAL_VERSION__
#define LT_SQRT sqrt
#define LT_POW pow
#define LT_LOG log
#define LT_EXP exp
#define LT_FLOOR floor
#define LT_MIN fmin
#define LT_MAX fmax
typedef float4 LTFloat4;
typedef uint4 LTUint4;
typedef float4x4 LTMatrix;
#else
#include <math.h>
#include <stdint.h>
#include <algorithm>
#include <simd/simd.h>
#define LT_SQRT sqrtf
#define LT_POW powf
#define LT_LOG logf
#define LT_EXP expf
#define LT_FLOOR floorf
#define LT_MIN fminf
#define LT_MAX fmaxf
typedef simd_float4 LTFloat4;
typedef simd_uint4 LTUint4;
typedef simd_float4x4 LTMatrix;
#endif

#define LT_MAX_LIGHTS 256
#define LT_CLUSTER_X 16
#define LT_CLUSTER_Y 9
#define LT_CLUSTER_Z 24
#define LT_PER_CLUSTER 32
#define LT_MAX_SHADOWED 8
#define LT_SPOT 1u

struct LTVec3 { float x, y, z; };

struct LTLight
{
	LTFloat4 posRadius, colorGloss, dirOuter, spot;
	LTUint4 ids;
};

struct LTFrame
{
	LTMatrix view, invProj;
	LTFloat4 eye, axis, grid, params;
};

typedef char LTCheckLight[sizeof(LTLight) == 80 ? 1 : -1];
typedef char LTCheckFrame[sizeof(LTFrame) == 192 ? 1 : -1];
typedef char LTCheckCluster[LT_PER_CLUSTER*2 == 64 ? 1 : -1];

static inline float
ltSaturate(float v)
{
	return LT_MIN(LT_MAX(v, 0.0f), 1.0f);
}

static inline float
ltFalloff(float d2, float radius)
{
	if(!(radius > 0.0f) || !(d2 >= 0.0f))
		return 0.0f;
	float q = d2/(radius*radius);
	float w = ltSaturate(1.0f - q*q);
	return w*w/(d2 + 0.01f);
}

static inline float
ltSpot(float cosAngle, float cosOuter, float cosInner)
{
	if(cosOuter <= -1.0f)
		return 1.0f;
	float t = ltSaturate((cosAngle - cosOuter)/(cosInner - cosOuter));
	return t*t*(3.0f - 2.0f*t);
}

static inline float
ltSpecular(float nh, float gloss)
{
	if(!(gloss > 0.0f))
		return 0.0f;
	return gloss*LT_POW(LT_MAX(nh, 0.0f), 8.0f + 120.0f*gloss);
}

static inline unsigned int
ltSlice(float viewZ, float zNear, float zFar)
{
	float s = LT_LOG(LT_MAX(viewZ, zNear)/zNear)/LT_LOG(zFar/zNear)*(float)LT_CLUSTER_Z;
	s = LT_MIN(LT_MAX(s, 0.0f), (float)(LT_CLUSTER_Z - 1));
	return (unsigned int)LT_FLOOR(s + 0.0001f);
}

static inline float
ltSliceNear(unsigned int k, float zNear, float zFar)
{
	return zNear*LT_EXP(LT_LOG(zFar/zNear)*(float)k/(float)LT_CLUSTER_Z);
}

static inline unsigned int
ltCluster(float px, float py, float viewZ, float width, float height, float zNear, float zFar)
{
	float fx = LT_FLOOR(px/width*(float)LT_CLUSTER_X);
	float fy = LT_FLOOR(py/height*(float)LT_CLUSTER_Y);
	fx = LT_MIN(LT_MAX(fx, 0.0f), (float)(LT_CLUSTER_X - 1));
	fy = LT_MIN(LT_MAX(fy, 0.0f), (float)(LT_CLUSTER_Y - 1));
	unsigned int z = ltSlice(viewZ, zNear, zFar);
	return (unsigned int)fx + LT_CLUSTER_X*((unsigned int)fy + LT_CLUSTER_Y*z);
}

static inline bool
ltSphereHitsBox(LTVec3 c, float r, LTVec3 lo, LTVec3 hi)
{
	float dx = c.x - LT_MIN(LT_MAX(c.x, lo.x), hi.x);
	float dy = c.y - LT_MIN(LT_MAX(c.y, lo.y), hi.y);
	float dz = c.z - LT_MIN(LT_MAX(c.z, lo.z), hi.z);
	return dx*dx + dy*dy + dz*dz <= r*r;
}

static inline bool
ltConeHitsSphere(LTVec3 apex, LTVec3 dir, float cosOuter, float radius, LTVec3 centre, float sphereR)
{
	float vx = centre.x - apex.x, vy = centre.y - apex.y, vz = centre.z - apex.z;
	float d2 = vx*vx + vy*vy + vz*vz;
	float reach = radius + sphereR;
	if(d2 > reach*reach)
		return false;
	if(d2 <= sphereR*sphereR || cosOuter <= -1.0f)
		return true;
	float d = LT_SQRT(d2);
	float cosA = (vx*dir.x + vy*dir.y + vz*dir.z)/d;
	if(cosA >= cosOuter)
		return true;
	float sinB = LT_MIN(sphereR/d, 1.0f);
	float cosB = LT_SQRT(1.0f - sinB*sinB);
	float sinO = LT_SQRT(LT_MAX(1.0f - cosOuter*cosOuter, 0.0f));
	if(sinO*cosB + cosOuter*sinB < 0.0f)
		return true;
	return cosA >= cosOuter*cosB - sinO*sinB;
}

static inline float
ltScore(float intensity, float d2)
{
	return intensity/LT_MAX(d2, 1.0f);
}

static inline int
ltBudget(int setting, int shadowQuality)
{
	if(shadowQuality <= 0)
		return 0;
	if(setting < 0){
		if(shadowQuality == 2) return 2;
		if(shadowQuality >= 3) return 4;
		return 0;
	}
	return setting > LT_MAX_SHADOWED ? LT_MAX_SHADOWED : setting;
}

#endif
