#ifndef RW_METAL_METALBLOOM_H
#define RW_METAL_METALBLOOM_H

#define BLOOM_LEVELS 6

static inline float
bloomKarisWeight(float r, float g, float b)
{
	return 1.0f/(1.0f + 0.2126f*r + 0.7152f*g + 0.0722f*b);
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
