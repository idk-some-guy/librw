#ifndef RW_METAL_METALRELIT_H
#define RW_METAL_METALRELIT_H

#ifdef __METAL_VERSION__
#define RL_MIN fmin
#define RL_MAX fmax
#else
#include <math.h>
#define RL_MIN fminf
#define RL_MAX fmaxf
#endif

struct RLColor { float r, g, b; };
struct RLSplit { float full, sun; };

static inline float
relitClampWeight(float w)
{
	if(!(w == w))
		return 1.0f;
	return RL_MIN(RL_MAX(w, 0.0f), 1.0f);
}

static inline float
relitPrelitScale(float w, bool prelit)
{
	return prelit ? relitClampWeight(w) : 1.0f;
}

static inline float
relitLiveShare(float w, bool prelit)
{
	return prelit ? 1.0f - relitClampWeight(w) : 1.0f;
}

static inline RLColor
relitHemisphere(float nz, RLColor top, RLColor bottom)
{
	float t = RL_MIN(RL_MAX(nz*0.5f + 0.5f, 0.0f), 1.0f);
	RLColor c = { bottom.r + (top.r - bottom.r)*t, bottom.g + (top.g - bottom.g)*t, bottom.b + (top.b - bottom.b)*t };
	return c;
}

static inline RLColor
relitDesaturate(RLColor c, float s)
{
	float l = 0.299f*c.r + 0.587f*c.g + 0.114f*c.b;
	RLColor r = { l + (c.r - l)*s, l + (c.g - l)*s, l + (c.b - l)*s };
	return r;
}

static inline RLSplit
relitSplit(float base, float sun, float mat)
{
	RLSplit s;
	s.full = RL_MIN(RL_MAX(base + sun, 0.0f), 1.0f)*mat;
	s.sun = s.full - RL_MIN(RL_MAX(base, 0.0f), 1.0f)*mat;
	return s;
}

static inline float
relitShadowed(float full, float sunPart, float shadow)
{
	return full - (1.0f - RL_MIN(RL_MAX(shadow, 0.0f), 1.0f))*sunPart;
}

#endif
