#ifndef RW_METAL_METALPREPASS_H
#define RW_METAL_METALPREPASS_H

#ifdef __METAL_VERSION__
#define PP_ABS fabs
#define PP_SQRT sqrt
#else
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#define PP_ABS fabsf
#define PP_SQRT sqrtf
#endif

#define PP_MOTION_OFFSCREEN 2.0f

struct PPVec2 { float x, y; };
struct PPVec3 { float x, y, z; };

static inline float
ppSign(float v)
{
	return v >= 0.0f ? 1.0f : -1.0f;
}

static inline PPVec2
ppOctEncode(float x, float y, float z)
{
	PPVec2 r;
	float s = PP_ABS(x) + PP_ABS(y) + PP_ABS(z);
	if(!(s > 0.0f)){
		r.x = 0.0f;
		r.y = 0.0f;
		return r;
	}
	x /= s;
	y /= s;
	if(z < 0.0f){
		r.x = (1.0f - PP_ABS(y))*ppSign(x);
		r.y = (1.0f - PP_ABS(x))*ppSign(y);
	}else{
		r.x = x;
		r.y = y;
	}
	return r;
}

static inline PPVec3
ppOctDecode(float u, float v)
{
	PPVec3 n;
	n.z = 1.0f - PP_ABS(u) - PP_ABS(v);
	if(n.z < 0.0f){
		n.x = (1.0f - PP_ABS(v))*ppSign(u);
		n.y = (1.0f - PP_ABS(u))*ppSign(v);
	}else{
		n.x = u;
		n.y = v;
	}
	float l = PP_SQRT(n.x*n.x + n.y*n.y + n.z*n.z);
	n.x /= l;
	n.y /= l;
	n.z /= l;
	return n;
}

static inline PPVec2
ppMotion(float cx, float cy, float cw, float px, float py, float pw)
{
	PPVec2 m;
	if(!(cw > 0.0f) || !(pw > 0.0f)){
		m.x = PP_MOTION_OFFSCREEN;
		m.y = PP_MOTION_OFFSCREEN;
		return m;
	}
	m.x = 0.5f*(px/pw - cx/cw);
	m.y = -0.5f*(py/pw - cy/cw);
	return m;
}

#ifndef __METAL_VERSION__
struct PrepassDraw
{
	bool blend;
	bool ztest;
	bool zwrite;
	bool sceneDepth;
	bool instanced;
};

inline bool
prepassEligible(const PrepassDraw &d)
{
	return !d.blend && d.ztest && d.zwrite && d.sceneDepth && d.instanced;
}

inline uint64_t
prepassMix(uint64_t h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char*)p;
	for(size_t i = 0; i < n; i++){
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

inline uint64_t
prepassKey(const void *inst, const float world[16], const float *bone0)
{
	uint64_t h = 14695981039346656037ull;
	h = prepassMix(h, &inst, sizeof(inst));
	h = prepassMix(h, world, 16*sizeof(float));
	if(bone0)
		h = prepassMix(h, bone0, 16*sizeof(float));
	return h;
}
#endif

#endif
