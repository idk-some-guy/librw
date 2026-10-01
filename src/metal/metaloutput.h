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

#define EDR_REFERENCE_WHITE_NITS 100.0f
#define EDR_SLOPE 2.2748815f

static inline float
srgbToLinear(float c)
{
	return c <= 0.04045f ? c/12.92f : OUTPUT_POW((c + 0.055f)/1.055f, 2.4f);
}

static inline float
edrPeak(float nits, float headroom)
{
	if(!(headroom > 1.0f))
		return 1.0f;
	return OUTPUT_MAX(1.0f, OUTPUT_MIN(nits/EDR_REFERENCE_WHITE_NITS, headroom));
}

static inline float
edrChannel(float x, float peak)
{
	float base = srgbToLinear(OUTPUT_MIN(OUTPUT_MAX(x, 0.0f), 1.0f));
	float e = OUTPUT_MAX(x - 1.0f, 0.0f);
	float room = peak - 1.0f;
	if(!(room > 0.0f))
		return base;
	return base + room*e*EDR_SLOPE/(room + e*EDR_SLOPE);
}

static inline bool
edrWanted(int tier, int hdrOutput, float potential)
{
	return tier != 0 && hdrOutput == 0 && potential > 1.0f;
}

#endif
