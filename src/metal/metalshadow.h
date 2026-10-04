#ifndef RW_METAL_METALSHADOW_H
#define RW_METAL_METALSHADOW_H

#ifdef __METAL_VERSION__
#define SH_SQRT sqrt
#define SH_SIN sin
#define SH_COS cos
#define SH_ABS fabs
#define SH_MIN fmin
#define SH_MAX fmax
#define SH_FLOOR floor
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
#define SH_FLOOR floorf
#endif

struct SHVec3 { float x, y, z; };
struct SHQuality { unsigned int raysPerPixel; unsigned int halfRes; float maxHistory; };
struct SHAccum { float value, length; };

static inline SHQuality
shQuality(int q)
{
	SHQuality r = { 0u, 0u, 0.0f };
	if(q == 1){ r.raysPerPixel = 1u; r.halfRes = 1u; r.maxHistory = 16.0f; }
	else if(q == 2){ r.raysPerPixel = 1u; r.halfRes = 1u; r.maxHistory = 12.0f; }
	else if(q == 3){ r.raysPerPixel = 1u; r.halfRes = 0u; r.maxHistory = 12.0f; }
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
shNormalOffset(float distance)
{
	return 0.005f + 0.0002f*distance;
}

static inline SHVec3
shRayOrigin(SHVec3 p, SHVec3 n, float distance)
{
	float b = shNormalOffset(distance);
	SHVec3 o = { p.x + n.x*b, p.y + n.y*b, p.z + n.z*b };
	return o;
}

static inline bool
shFacesAwayFromSun(float normalValid, float nDotL, float radius)
{
	return normalValid > 0.0f && nDotL <= -radius;
}

static inline bool
shCullsBackFaces(float normalValid, float facetDotSun)
{
	return normalValid > 0.0f && facetDotSun < 0.0f;
}

#define SH_CLASS_DARK (-0.6f)

static inline bool
shClassDark(float normalDotSun)
{
	return normalDotSun < SH_CLASS_DARK;
}

// Depth is affine in screen space across a plane, so the side whose two taps extrapolate to the centre lies on its plane.
static inline bool
shFacetPositive(float d, float n1, float n2, float p1, float p2, bool hasN, bool hasP)
{
	if(!hasN || !hasP)
		return !hasN;
	return SH_ABS(2.0f*p1 - p2 - d) <= SH_ABS(2.0f*n1 - n2 - d);
}

static inline bool
shFrontCounterClockwise(SHVec3 right, SHVec3 up, SHVec3 at)
{
	float det = (right.y*up.z - right.z*up.y)*at.x + (right.z*up.x - right.x*up.z)*at.y + (right.x*up.y - right.y*up.x)*at.z;
	return det < 0.0f;
}

static inline unsigned int
shPhaseX(unsigned int frame)
{
	return frame & 1u;
}

static inline unsigned int
shPhaseY(unsigned int frame)
{
	return (frame >> 1) & 1u;
}

static inline unsigned int
shRaySource(unsigned int h, unsigned int off, unsigned int size)
{
	unsigned int s = 2u*h + off;
	return s < size - 1u ? s : size - 1u;
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
shClampBoxNeeded(float prev, float cur)
{
	return prev != cur;
}

static inline bool
shDisoccluded(float prevViewZ, float viewZ)
{
	return SH_ABS(prevViewZ - viewZ) > 0.05f*viewZ + 0.1f;
}

struct SHTap { unsigned int index; float weight; };

static inline SHTap
shUpsampleTap(unsigned int g, unsigned int off, int k)
{
	SHTap t;
	if(g < off){
		t.index = 0u;
		t.weight = 0.5f;
		return t;
	}
	unsigned int p = g - off;
	t.index = p/2u + (unsigned int)k;
	t.weight = (p & 1u) != 0u ? 0.5f : k == 0 ? 1.0f : 0.0f;
	return t;
}

static inline float
shUpsampleWeight(float tapDistance, float distance)
{
	return 1.0f/(1e-4f + SH_ABS(tapDistance - distance)/SH_MAX(distance, 1e-3f));
}

#define SH_NORMAL_WEIGHT_FLOOR 1e-4f

static inline float
shUpsampleNormalWeight(float validA, float validB, float cosine)
{
	if(!(validA > 0.0f) || !(validB > 0.0f))
		return 1.0f;
	float c = SH_MAX(cosine, 0.0f);
	c *= c;
	c *= c;
	c *= c;
	return SH_MAX(c, SH_NORMAL_WEIGHT_FLOOR);
}

static inline bool
shClampTapAgrees(float validA, float validB, float cosine)
{
	return shUpsampleNormalWeight(validA, validB, cosine) >= 0.5f;
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

enum { SH_CAST_NONE = 0, SH_CAST_OPAQUE = 1, SH_CAST_ALPHA = 2 };

static inline int
shMeshCast(bool vertexAlpha, int materialAlpha, bool textureAlpha, bool texCoords, bool alphaCasters)
{
	if(vertexAlpha || materialAlpha != 255)
		return SH_CAST_NONE;
	if(!textureAlpha)
		return SH_CAST_OPAQUE;
	return texCoords && alphaCasters ? SH_CAST_ALPHA : SH_CAST_NONE;
}

static inline bool
shRecordsAlphaRef(bool depthWrite, int alphaFunc)
{
	return depthWrite && alphaFunc == 1;
}

static inline float
shAlphaThreshold(float recorded)
{
	return SH_MAX(recorded, 0.5f);
}

static inline unsigned int
shAddressMode(int addressing)
{
	return addressing == 2 ? 1u : addressing == 3 || addressing == 4 ? 2u : 0u;
}

static inline float
shAddress(float t, unsigned int mode)
{
	if(mode == 1u){
		float m = t - 2.0f*SH_FLOOR(t*0.5f);
		return m > 1.0f ? 2.0f - m : m;
	}
	if(mode == 2u)
		return SH_MIN(SH_MAX(t, 0.0f), 1.0f);
	return t - SH_FLOOR(t);
}

static inline bool
shAlphaIndexValid(unsigned int base, unsigned int geometry, unsigned int count)
{
	return base < count && geometry < count - base;
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
