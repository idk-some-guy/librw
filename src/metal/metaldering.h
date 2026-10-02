#ifndef RW_METAL_METALDERING_H
#define RW_METAL_METALDERING_H

#ifdef __METAL_VERSION__
#define DERING_ISNAN(v) isnan(v)
#else
#define DERING_ISNAN(v) ((v) != (v))
#endif

static inline float
deringClamp(float v, float lo, float hi)
{
	if(DERING_ISNAN(v))
		return lo;
	return v < lo ? lo : v > hi ? hi : v;
}

static inline int
deringTexel(int x, int outSpan, int inSpan)
{
	int t = (int)(((float)x + 0.5f)*(float)inSpan/(float)outSpan);
	return t < 0 ? 0 : t >= inSpan ? inSpan - 1 : t;
}

#endif
