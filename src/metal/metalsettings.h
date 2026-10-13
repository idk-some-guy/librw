#ifndef RW_METAL_METALSETTINGS_H
#define RW_METAL_METALSETTINGS_H

#include <stdint.h>

namespace rw {
namespace metal {

enum
{
	TIER_CLASSIC = 0,
	TIER_ENHANCED,
	TIER_MODERN,
	NUMTIERS
};

enum
{
	UPSCALE_NATIVE = 0,
	UPSCALE_QUALITY,
	UPSCALE_BALANCED,
	UPSCALE_PERFORMANCE,
	UPSCALE_ULTRA,
	NUMUPSCALES
};

enum
{
	HDROUTPUT_AUTO = 0,
	HDROUTPUT_OFF
};

enum
{
	ANTIALIAS_OFF = 0,
	ANTIALIAS_FXAA,
	ANTIALIAS_TAA,
	ANTIALIAS_TEMPORAL
};

enum
{
	SHADOWQUALITY_OFF = 0,
	SHADOWQUALITY_LOW,
	SHADOWQUALITY_MEDIUM,
	SHADOWQUALITY_HIGH,
	NUMSHADOWQUALITIES
};

enum
{
	DEBUGVIEW_NONE = 0,
	DEBUGVIEW_SHADOW,
	DEBUGVIEW_NORMALS,
	DEBUGVIEW_MOTION,
	DEBUGVIEW_REACTIVE,
	DEBUGVIEW_LIGHTS,
	NUMDEBUGVIEWS
};

enum
{
	TIERREASON_NONE = 0,
	TIERREASON_NOAPPLEGPU,
	TIERREASON_NORAYTRACING
};

enum
{
	UPSCALER_NONE = 0,
	UPSCALER_SPATIAL,
	UPSCALER_TEMPORAL
};

#define EMITTERPEAK_MIN 400.0f
#define EMITTERPEAK_MAX 4000.0f
#define EMITTERPEAK_DEFAULT 1000.0f
#define SOFTNESS_MIN 0.0f
#define SOFTNESS_MAX 3.0f
#define SOFTNESS_DEFAULT 1.0f

struct TierCaps
{
	bool appleGPU;
	bool apple9;
	bool raytracing;
};

inline int32_t
supportedTier(const TierCaps &c)
{
	if(!c.appleGPU)
		return TIER_CLASSIC;
	if(c.apple9 && c.raytracing)
		return TIER_MODERN;
	return TIER_ENHANCED;
}

inline int32_t
resolveTier(int32_t requested, int32_t supported, int32_t *reason)
{
	*reason = TIERREASON_NONE;
	if(requested < TIER_CLASSIC || requested >= NUMTIERS)
		return TIER_CLASSIC;
	if(requested <= supported)
		return requested;
	*reason = supported == TIER_CLASSIC ? TIERREASON_NOAPPLEGPU : TIERREASON_NORAYTRACING;
	return supported;
}

inline const char*
tierName(int32_t tier)
{
	switch(tier){
	case TIER_ENHANCED: return "enhanced";
	case TIER_MODERN: return "modern";
	}
	return "classic";
}

inline const char*
tierReasonText(int32_t reason)
{
	switch(reason){
	case TIERREASON_NOAPPLEGPU: return "needs an Apple GPU (family Apple 7)";
	case TIERREASON_NORAYTRACING: return "needs hardware ray tracing (GPU family Apple 9)";
	}
	return "";
}

inline const char*
antiAliasName(int32_t aa)
{
	switch(aa){
	case ANTIALIAS_FXAA: return "fxaa";
	case ANTIALIAS_TAA: return "taa";
	case ANTIALIAS_TEMPORAL: return "temporal";
	}
	return "off";
}

inline const char*
shadowQualityName(int32_t q)
{
	switch(q){
	case SHADOWQUALITY_LOW: return "low";
	case SHADOWQUALITY_MEDIUM: return "medium";
	case SHADOWQUALITY_HIGH: return "high";
	}
	return "off";
}

inline const char*
upscalerName(int32_t u)
{
	switch(u){
	case UPSCALER_SPATIAL: return "spatial";
	case UPSCALER_TEMPORAL: return "temporal";
	}
	return "none";
}

inline float
upscaleRatio(int32_t factor)
{
	switch(factor){
	case UPSCALE_QUALITY: return 1.5f;
	case UPSCALE_BALANCED: return 1.7f;
	case UPSCALE_PERFORMANCE: return 2.0f;
	case UPSCALE_ULTRA: return 3.0f;
	}
	return 1.0f;
}

inline int32_t
internalSpan(int32_t output, int32_t factor)
{
	if(output <= 0)
		return 0;
	int32_t n = (int32_t)((float)output/upscaleRatio(factor) + 0.5f);
	return n < 1 ? 1 : n;
}

inline float
clampRange(float v, float lo, float hi, float fallback)
{
	if(v != v)
		return fallback;
	return v < lo ? lo : v > hi ? hi : v;
}

}
}

#endif
