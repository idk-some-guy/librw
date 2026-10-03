#ifndef RW_METAL_METALSHADOW_H
#define RW_METAL_METALSHADOW_H

#ifdef __METAL_VERSION__
#define SH_SQRT sqrt
#define SH_SIN sin
#define SH_COS cos
#define SH_ABS fabs
#define SH_MIN fmin
#define SH_MAX fmax
#else
#include <math.h>
#include <stdint.h>
#include <algorithm>
#define SH_SQRT sqrtf
#define SH_SIN sinf
#define SH_COS cosf
#define SH_ABS fabsf
#define SH_MIN fminf
#define SH_MAX fmaxf
#endif

struct SHVec3 { float x, y, z; };
struct SHQuality { unsigned int raysPerPixel; unsigned int halfRes; float maxHistory; };
struct SHAccum { float value, length; };

static inline SHQuality
shQuality(int q)
{
	SHQuality r = { 0u, 0u, 0.0f };
	if(q == 1){ r.raysPerPixel = 1u; r.halfRes = 1u; r.maxHistory = 16.0f; }
	else if(q == 2){ r.raysPerPixel = 1u; r.halfRes = 0u; r.maxHistory = 12.0f; }
	else if(q == 3){ r.raysPerPixel = 2u; r.halfRes = 0u; r.maxHistory = 8.0f; }
	return r;
}

static inline float
shHash(unsigned int x, unsigned int y, unsigned int frame, unsigned int k)
{
	unsigned int h = x*1973u + y*9277u + frame*26699u + k*104729u + 1u;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return (float)(h & 0xffffffu)/16777216.0f;
}

static inline SHVec3
shNormalize(SHVec3 v)
{
	float l = SH_SQRT(v.x*v.x + v.y*v.y + v.z*v.z);
	SHVec3 r = { v.x/l, v.y/l, v.z/l };
	return r;
}

static inline SHVec3
shDiscDirection(SHVec3 sun, float radius, float u1, float u2)
{
	SHVec3 s = shNormalize(sun);
	SHVec3 helper = { 0.0f, 0.0f, 1.0f };
	if(SH_ABS(s.z) > 0.999f){
		helper.x = 1.0f;
		helper.z = 0.0f;
	}
	SHVec3 t = { helper.y*s.z - helper.z*s.y, helper.z*s.x - helper.x*s.z, helper.x*s.y - helper.y*s.x };
	t = shNormalize(t);
	SHVec3 b = { s.y*t.z - s.z*t.y, s.z*t.x - s.x*t.z, s.x*t.y - s.y*t.x };
	float r = SH_SIN(radius)*SH_SQRT(u1);
	float phi = 6.28318531f*u2;
	float c = SH_COS(phi)*r, d = SH_SIN(phi)*r;
	float k = SH_SQRT(1.0f - r*r);
	SHVec3 o = { s.x*k + t.x*c + b.x*d, s.y*k + t.y*c + b.y*d, s.z*k + t.z*c + b.z*d };
	return shNormalize(o);
}

static inline float
shBias(float distance)
{
	return 0.02f + 0.0015f*distance;
}

static inline SHVec3
shRayOrigin(SHVec3 p, SHVec3 toCamera, float distance)
{
	float b = shBias(distance);
	SHVec3 o = { p.x + toCamera.x*b, p.y + toCamera.y*b, p.z + toCamera.z*b };
	return o;
}

// The backend's latched sun direction points away from the sun: pass -sunDir.z.
static inline bool
shSunUp(float towardSunZ)
{
	return towardSunZ > 0.0f;
}

static inline bool
shSourceOn(int quality, bool rtSupported, bool modern)
{
	return shQuality(quality).raysPerPixel > 0u && rtSupported && modern;
}

static inline bool
shPassRuns(int quality, bool rtSupported, bool modern, bool prepassRan, float towardSunZ, float sunScale, float lightPresent)
{
	return shSourceOn(quality, rtSupported, modern) && prepassRan && shSunUp(towardSunZ) && sunScale > 0.0f && lightPresent > 0.0f;
}

static inline SHAccum
shAccumulate(float prev, float length, float raw, float maxHistory)
{
	SHAccum a;
	a.length = SH_MIN(length + 1.0f, maxHistory);
	a.value = prev + (raw - prev)/a.length;
	return a;
}

static inline float
shClampHistory(float hist, float lo, float hi)
{
	return SH_MIN(SH_MAX(hist, lo), hi);
}

static inline SHAccum
shAccumulateClamped(float prev, float length, float raw, float lo, float hi, float maxHistory)
{
	float c = shClampHistory(prev, lo, hi);
	return shAccumulate(c, SH_ABS(c - prev) > 0.5f ? 0.0f : length, raw, maxHistory);
}

static inline bool
shDisoccluded(float prevViewZ, float viewZ)
{
	return SH_ABS(prevViewZ - viewZ) > 0.05f*viewZ + 0.1f;
}

static inline bool
shSunMoved(SHVec3 a, SHVec3 b)
{
	SHVec3 u = shNormalize(a), v = shNormalize(b);
	return u.x*v.x + u.y*v.y + u.z*v.z < 0.99939083f;
}

static inline bool
shHistoryMatches(SHVec3 q, SHVec3 p, SHVec3 n, float prevDistance, float distance)
{
	float d = (q.x - p.x)*n.x + (q.y - p.y)*n.y + (q.z - p.z)*n.z;
	return SH_ABS(d) <= 0.02f + 0.003f*distance || !shDisoccluded(prevDistance, distance);
}

static inline float
shLinearDepth(float d, float n, float f)
{
	float z = d*2.0f - 1.0f;
	return 2.0f*n*f/((f + n) - z*(f - n));
}

#ifndef __METAL_VERSION__
inline uint32_t
shCapByDistance(const float *dist2, uint32_t n, uint32_t cap, uint32_t *order)
{
	for(uint32_t i = 0; i < n; i++)
		order[i] = i;
	uint32_t k = cap < n ? cap : n;
	std::partial_sort(order, order + k, order + n, [dist2](uint32_t a, uint32_t b){
		return dist2[a] < dist2[b] || (dist2[a] == dist2[b] && a < b);
	});
	return k;
}
#endif

#endif
