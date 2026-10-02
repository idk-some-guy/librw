#ifndef RW_METAL_METALBLOOM_H
#define RW_METAL_METALBLOOM_H

#define BLOOM_LEVELS 6

#ifdef __METAL_VERSION__
#define BLOOM_CLAMP(x) clamp(x, 0.0f, 65504.0f)
#else
#include <math.h>
#define BLOOM_CLAMP(x) fminf(fmaxf(x, 0.0f), 65504.0f)
#endif

static inline float
bloomKarisWeight(float r, float g, float b)
{
	return 1.0f/(1.0f + 0.2126f*BLOOM_CLAMP(r) + 0.7152f*BLOOM_CLAMP(g) + 0.0722f*BLOOM_CLAMP(b));
}

static inline float
bloomRadius(float softness)
{
	float s = softness < 0.0f ? 0.0f : softness > 3.0f ? 3.0f : softness;
	return 0.5f + 0.5f*s;
}

static inline int
bloomLevelSpan(int span, int level)
{
	int n = span >> (level + 1);
	return n < 1 ? 1 : n;
}

#endif
