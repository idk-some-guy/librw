#ifndef RW_METAL_METALAA_H
#define RW_METAL_METALAA_H

#ifdef __METAL_VERSION__
#define AA_MIN fmin
#define AA_MAX fmax
#define AA_ABS fabs
#else
#include <math.h>
#define AA_MIN fminf
#define AA_MAX fmaxf
#define AA_ABS fabsf
#endif

#define FXAA_EDGE_THRESHOLD 0.125f
#define FXAA_EDGE_THRESHOLD_MIN 0.0312f
#define FXAA_SUBPIX 0.75f
#define FXAA_STEPS 12

static inline float
aaLuma(float r, float g, float b)
{
	return 0.299f*AA_MIN(AA_MAX(r, 0.0f), 1.0f) + 0.587f*AA_MIN(AA_MAX(g, 0.0f), 1.0f) +
		0.114f*AA_MIN(AA_MAX(b, 0.0f), 1.0f);
}

static inline bool
aaIsEdge(float lumaMax, float lumaMin)
{
	return lumaMax - lumaMin >= AA_MAX(FXAA_EDGE_THRESHOLD_MIN, lumaMax*FXAA_EDGE_THRESHOLD);
}

static inline float
aaStep(int i)
{
	return i < 5 ? 1.0f : i == 5 ? 1.5f : i < 10 ? 2.0f : i == 10 ? 4.0f : 8.0f;
}

static inline float
aaSubpixBlend(float average, float lumaM, float range)
{
	float c = AA_MIN(AA_MAX(AA_ABS(average - lumaM)/range, 0.0f), 1.0f);
	float d = (-2.0f*c + 3.0f)*c*c;
	return d*d*FXAA_SUBPIX;
}

#endif
