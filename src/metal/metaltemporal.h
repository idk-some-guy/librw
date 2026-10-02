#ifndef RW_METAL_METALTEMPORAL_H
#define RW_METAL_METALTEMPORAL_H

#ifdef __METAL_VERSION__
#define TM_MIN fmin
#define TM_MAX fmax
#define TM_ABS fabs
#else
#include <math.h>
#define TM_MIN fminf
#define TM_MAX fmaxf
#define TM_ABS fabsf
#endif

#define TM_JITTER_PHASES 8
#define TM_HISTORY_ALPHA 0.1f
#define TM_CUT_DISTANCE 10.0f

struct TMColor { float r, g, b; };
struct TMJitter { float x, y; };

static inline float
tmHalton(unsigned int index, unsigned int base)
{
	float f = 1.0f, r = 0.0f;
	while(index > 0){
		f /= (float)base;
		r += f*(float)(index % base);
		index /= base;
	}
	return r;
}

static inline TMJitter
tmJitter(unsigned int frame)
{
	TMJitter j;
	unsigned int i = frame % TM_JITTER_PHASES + 1;
	j.x = tmHalton(i, 2) - 0.5f;
	j.y = tmHalton(i, 3) - 0.5f;
	return j;
}

static inline float
tmLuma(TMColor c)
{
	return 0.299f*TM_MAX(c.r, 0.0f) + 0.587f*TM_MAX(c.g, 0.0f) + 0.114f*TM_MAX(c.b, 0.0f);
}

static inline TMColor
tmScale(TMColor c, float s)
{
	TMColor r = { c.r*s, c.g*s, c.b*s };
	return r;
}

static inline TMColor
tmCompress(TMColor c)
{
	return tmScale(c, 1.0f/(1.0f + tmLuma(c)));
}

static inline TMColor
tmExpand(TMColor c)
{
	return tmScale(c, 1.0f/TM_MAX(1.0f - tmLuma(c), 1e-4f));
}

static inline TMColor
tmToYCoCg(TMColor c)
{
	TMColor r = { 0.25f*c.r + 0.5f*c.g + 0.25f*c.b, 0.5f*c.r - 0.5f*c.b, -0.25f*c.r + 0.5f*c.g - 0.25f*c.b };
	return r;
}

static inline TMColor
tmFromYCoCg(TMColor c)
{
	TMColor r = { c.r + c.g - c.b, c.r + c.b, c.r - c.g - c.b };
	return r;
}

static inline TMColor
tmMinC(TMColor a, TMColor b)
{
	TMColor r = { TM_MIN(a.r, b.r), TM_MIN(a.g, b.g), TM_MIN(a.b, b.b) };
	return r;
}

static inline TMColor
tmMaxC(TMColor a, TMColor b)
{
	TMColor r = { TM_MAX(a.r, b.r), TM_MAX(a.g, b.g), TM_MAX(a.b, b.b) };
	return r;
}

static inline TMColor
tmMix(TMColor a, TMColor b, float t)
{
	TMColor r = { a.r + (b.r - a.r)*t, a.g + (b.g - a.g)*t, a.b + (b.b - a.b)*t };
	return r;
}

static inline TMColor
tmClipAabb(TMColor h, TMColor mn, TMColor mx)
{
	TMColor c = { 0.5f*(mn.r + mx.r), 0.5f*(mn.g + mx.g), 0.5f*(mn.b + mx.b) };
	TMColor e = { 0.5f*(mx.r - mn.r) + 1e-6f, 0.5f*(mx.g - mn.g) + 1e-6f, 0.5f*(mx.b - mn.b) + 1e-6f };
	TMColor v = { h.r - c.r, h.g - c.g, h.b - c.b };
	float unit = TM_MAX(TM_MAX(TM_ABS(v.r)/e.r, TM_ABS(v.g)/e.g), TM_ABS(v.b)/e.b);
	if(unit <= 1.0f)
		return h;
	TMColor r = { c.r + v.r/unit, c.g + v.g/unit, c.b + v.b/unit };
	return r;
}

static inline bool
tmHistoryValid(float u, float v)
{
	return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

static inline float
tmHistoryWeight(float reactive, bool valid)
{
	if(!valid)
		return 0.0f;
	float r = TM_MIN(TM_MAX(reactive, 0.0f), 1.0f);
	return (1.0f - TM_HISTORY_ALPHA)*(1.0f - r);
}

#ifndef __METAL_VERSION__
inline void
tmJitterProjection(float proj[16], float jx, float jy, int width, int height)
{
	float nx = 2.0f*jx/(float)width;
	float ny = -2.0f*jy/(float)height;
	for(int c = 0; c < 4; c++){
		proj[c*4+0] += nx*proj[c*4+3];
		proj[c*4+1] += ny*proj[c*4+3];
	}
}

inline bool
tmCameraCut(float dx, float dy, float dz)
{
	return dx*dx + dy*dy + dz*dz > TM_CUT_DISTANCE*TM_CUT_DISTANCE;
}
#endif

#endif
