#ifndef RW_METAL_METALRELIT_H
#define RW_METAL_METALRELIT_H

#ifdef __METAL_VERSION__
#define RL_MIN fmin
#define RL_MAX fmax
#define RL_SQRT sqrt
#define RL_ABS fabs
#else
#include <math.h>
#define RL_MIN fminf
#define RL_MAX fmaxf
#define RL_SQRT sqrtf
#define RL_ABS fabsf
#endif

struct RLColor { float r, g, b; };
struct RLVec3 { float x, y, z; };
struct RLSplit { float full, sun; };

// Shader fast math folds this NaN test away (fmax(NaN, 0) is 0, fully live): clamp the weight on the CPU.
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

static inline float
relitShadowedPrelit(float full, float sunPart, float classic, float shadow, float strength)
{
	float open = 1.0f - RL_MIN(RL_MAX(shadow, 0.0f), 1.0f);
	return full - open*sunPart - RL_MIN(RL_MAX(strength, 0.0f), 1.0f)*open*classic;
}

static inline float
relitPlainShadowed(float c, float shadow, float strength)
{
	float open = 1.0f - RL_MIN(RL_MAX(shadow, 0.0f), 1.0f);
	return c*(1.0f - RL_MIN(RL_MAX(strength, 0.0f), 1.0f)*open);
}

static inline float
relitWeighted(float c, float w)
{
	return w < 1.0f ? c*w : c;
}

static inline RLSplit
relitSplitAdd(float classic, float hemi, float sun, float mat)
{
	float room = RL_MAX(mat - classic, 0.0f);
	float base = RL_MIN(hemi*mat, room);
	RLSplit s;
	s.sun = RL_MIN(sun*mat, room - base);
	s.full = classic + base + s.sun;
	return s;
}

static inline float
relitSunCos(float c, bool twoSided)
{
	return twoSided ? RL_ABS(c) : RL_MAX(c, 0.0f);
}

#ifndef __METAL_VERSION__
static inline float
relitSnapWeight(float w)
{
	float c = relitClampWeight(w);
	return c > 1.0f - 1e-6f ? 1.0f : c;
}

static inline void
relitCofactor(const float c0[3], const float c1[3], const float c2[3], float out[9])
{
	out[0] = c1[1]*c2[2] - c1[2]*c2[1];
	out[1] = c1[2]*c2[0] - c1[0]*c2[2];
	out[2] = c1[0]*c2[1] - c1[1]*c2[0];
	out[3] = c2[1]*c0[2] - c2[2]*c0[1];
	out[4] = c2[2]*c0[0] - c2[0]*c0[2];
	out[5] = c2[0]*c0[1] - c2[1]*c0[0];
	out[6] = c0[1]*c1[2] - c0[2]*c1[1];
	out[7] = c0[2]*c1[0] - c0[0]*c1[2];
	out[8] = c0[0]*c1[1] - c0[1]*c1[0];
}
#endif

static inline RLVec3
relitFillNormal(RLVec3 n, float valid, RLVec3 g, RLVec3 fallback)
{
	float gg = g.x*g.x + g.y*g.y + g.z*g.z;
	if(gg < 1e-30f)
		g = fallback;
	else{
		float gl = RL_SQRT(gg);
		g.x /= gl;
		g.y /= gl;
		g.z /= gl;
	}
	if(n.x*g.x + n.y*g.y + n.z*g.z < 0.0f){
		n.x = -n.x;
		n.y = -n.y;
		n.z = -n.z;
	}
	float v = RL_MIN(RL_MAX(valid, 0.0f), 1.0f);
	float nn = n.x*n.x + n.y*n.y + n.z*n.z;
	float d = nn > 1e-30f ? v/RL_SQRT(nn) : 0.0f;
	RLVec3 r = { n.x*d + g.x*(1.0f - v), n.y*d + g.y*(1.0f - v), n.z*d + g.z*(1.0f - v) };
	float rr = r.x*r.x + r.y*r.y + r.z*r.z;
	if(rr < 1e-12f)
		return g;
	float rl = RL_SQRT(rr);
	r.x /= rl;
	r.y /= rl;
	r.z /= rl;
	return r;
}

#endif
