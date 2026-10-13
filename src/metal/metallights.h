#ifndef RW_METAL_METALLIGHTS_H
#define RW_METAL_METALLIGHTS_H

#ifdef __METAL_VERSION__
#define LT_SQRT sqrt
#define LT_POW pow
#define LT_LOG log
#define LT_EXP exp
#define LT_COS cos
#define LT_SIN sin
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
#define LT_COS cosf
#define LT_SIN sinf
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
#define LT_HOLD_FRAMES 6

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

struct LTDraw
{
	LTFloat4 emissive;
};

typedef char LTCheckLight[sizeof(LTLight) == 80 ? 1 : -1];
typedef char LTCheckDraw[sizeof(LTDraw) == 16 ? 1 : -1];
typedef char LTCheckFrame[sizeof(LTFrame) == 192 ? 1 : -1];
typedef char LTCheckCluster[LT_PER_CLUSTER*2 == 64 ? 1 : -1];

static inline float
ltSaturate(float v)
{
	return LT_MIN(LT_MAX(v, 0.0f), 1.0f);
}

static inline float
ltEmissivePeak(float luma, float scale)
{
	float t = ltSaturate((luma - 0.6f)/0.3f);
	return 1.0f + (scale - 1.0f)*t*t*(3.0f - 2.0f*t);
}

static inline float
ltEmissiveGate(float r, float g, float b, float scale)
{
	return ltEmissivePeak(LT_MAX(LT_MAX(r, g), b), scale);
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

#ifndef __METAL_VERSION__
#define LT_CAND_PINNED 1u

struct LTCand { float score; unsigned id; unsigned flags; unsigned group; int held; };

static inline int
ltPickShadowed(const LTCand *c, int n, int budget, int out[LT_MAX_SHADOWED])
{
	if(budget > LT_MAX_SHADOWED)
		budget = LT_MAX_SHADOWED;
	if(n > LT_MAX_LIGHTS)
		n = LT_MAX_LIGHTS;
	if(budget <= 0 || n <= 0)
		return 0;
	bool taken[LT_MAX_LIGHTS] = {};
	unsigned groups[LT_MAX_SHADOWED];
	int count = 0, ngroups = 0;
	for(int i = 0; i < n && count < budget; i++){
		if(!(c[i].flags & LT_CAND_PINNED))
			continue;
		bool dup = false;
		for(int g = 0; c[i].group && g < ngroups; g++)
			dup = dup || groups[g] == c[i].group;
		if(dup)
			continue;
		taken[i] = true;
		if(c[i].group)
			groups[ngroups++] = c[i].group;
		out[count++] = i;
	}
	float bestUnheld = 0.0f;
	for(int i = 0; i < n; i++)
		if(!taken[i] && c[i].held <= 0 && c[i].score > bestUnheld)
			bestUnheld = c[i].score;
	for(int pass = 0; pass < 2; pass++){
		while(count < budget){
			int best = -1;
			for(int i = 0; i < n; i++){
				if(taken[i] || !(c[i].score > 0.0f))
					continue;
				if(pass == 0 && !(c[i].held > 0 && c[i].score >= 0.5f*bestUnheld))
					continue;
				bool dup = false;
				for(int g = 0; c[i].group && g < ngroups; g++)
					dup = dup || groups[g] == c[i].group;
				if(dup)
					continue;
				if(best < 0 || c[i].score > c[best].score)
					best = i;
			}
			if(best < 0)
				break;
			taken[best] = true;
			if(c[best].group)
				groups[ngroups++] = c[best].group;
			out[count++] = best;
		}
	}
	return count;
}

static inline bool
ltInView(const float *m, LTVec3 c, float radius)
{
	for(int k = 0; k < 6; k++){
		int i = k >> 1;
		float s = (k & 1) ? -1.0f : 1.0f;
		float a = m[3] + s*m[i], b = m[7] + s*m[4 + i], cc = m[11] + s*m[8 + i], d = m[15] + s*m[12 + i];
		float len = LT_SQRT(a*a + b*b + cc*cc);
		if(!(a*c.x + b*c.y + cc*c.z + d >= -radius*len))
			return false;
	}
	return true;
}

static inline int
ltHoldNext(int held, bool picked, int holdFrames)
{
	if(picked)
		return holdFrames > 0 ? holdFrames : 0;
	return held > 0 ? held - 1 : 0;
}

static inline bool
ltSlotReset(bool hadPrev, unsigned int prevId, unsigned int id)
{
	return !hadPrev || prevId != id;
}

static inline int
ltSlotFor(const LTCand *c, int n, const int *picked, int count, int i)
{
	for(int k = 0; k < count; k++)
		if(picked[k] == i || (c[i].group != 0 && picked[k] < n && c[picked[k]].group == c[i].group))
			return k;
	return -1;
}

static inline int
ltEntrySlot(int slot, unsigned int slots, bool ranPass)
{
	return ranPass && slot >= 0 && (unsigned int)slot < slots ? slot : -1;
}

static inline bool
ltLightTextureBound(bool ranPass, unsigned int slots)
{
	return ranPass && slots > 0;
}
#endif

static inline bool
ltRayWanted(LTVec3 p, LTVec3 lightPos, float radius, LTVec3 dir, float cosOuter, bool spot)
{
	if(!(radius > 0.0f))
		return false;
	float vx = p.x - lightPos.x, vy = p.y - lightPos.y, vz = p.z - lightPos.z;
	float d2 = vx*vx + vy*vy + vz*vz;
	if(!(d2 <= radius*radius))
		return false;
	if(!spot || d2 == 0.0f)
		return true;
	return (vx*dir.x + vy*dir.y + vz*dir.z) >= cosOuter*LT_SQRT(d2);
}

static inline float
ltRayLength(float dist, float backoff)
{
	return LT_MAX(dist - backoff, 0.0f);
}

static inline LTVec3
ltDiscToward(LTVec3 toLight, float softness, float dist, float u1, float u2)
{
	if(!(softness > 0.0f) || !(dist > 0.0f))
		return toLight;
	LTVec3 a = toLight.x*toLight.x < 0.25f ? LTVec3{1.0f, 0.0f, 0.0f} : LTVec3{0.0f, 1.0f, 0.0f};
	LTVec3 t1 = {toLight.y*a.z - toLight.z*a.y, toLight.z*a.x - toLight.x*a.z, toLight.x*a.y - toLight.y*a.x};
	float inv1 = 1.0f/LT_SQRT(t1.x*t1.x + t1.y*t1.y + t1.z*t1.z);
	t1.x *= inv1; t1.y *= inv1; t1.z *= inv1;
	LTVec3 t2 = {toLight.y*t1.z - toLight.z*t1.y, toLight.z*t1.x - toLight.x*t1.z, toLight.x*t1.y - toLight.y*t1.x};
	float r = softness*LT_SQRT(u1), phi = 6.2831853f*u2;
	float s = r*LT_COS(phi), t = r*LT_SIN(phi);
	LTVec3 v = {toLight.x*dist + t1.x*s + t2.x*t, toLight.y*dist + t1.y*s + t2.y*t, toLight.z*dist + t1.z*s + t2.z*t};
	float inv = 1.0f/LT_SQRT(v.x*v.x + v.y*v.y + v.z*v.z);
	return LTVec3{v.x*inv, v.y*inv, v.z*inv};
}

#endif
