#ifndef RW_METAL_METALOUTPUT_H
#define RW_METAL_METALOUTPUT_H

#ifdef __METAL_VERSION__
#define OUTPUT_MIN fmin
#define OUTPUT_MAX fmax
#define OUTPUT_POW pow
#else
#include <math.h>
#define OUTPUT_MIN fminf
#define OUTPUT_MAX fmaxf
#define OUTPUT_POW powf
#endif

#define SDR_SHOULDER_KNEE 1.0f

static inline float
sdrShoulder(float x, float m)
{
	if(!(m > 1.0f))
		return x;
	float w = (m - 1.0f)/(m - 1.0f + SDR_SHOULDER_KNEE);
	return OUTPUT_MIN(x + w*(1.0f - x), 1.0f);
}

#endif
