#include <cstdio>
#include <cstring>
#include <cmath>
#include "metalsettings.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static void
TestSupportedTier(void)
{
	TierCaps none = { false, false, false };
	TierCaps m1 = { true, false, false };
	TierCaps m2rt = { true, false, true };
	TierCaps a9 = { true, true, false };
	TierCaps m3 = { true, true, true };
	CHECK(supportedTier(none) == TIER_CLASSIC);
	CHECK(supportedTier(m1) == TIER_ENHANCED);
	CHECK(supportedTier(m2rt) == TIER_ENHANCED);
	CHECK(supportedTier(a9) == TIER_ENHANCED);
	CHECK(supportedTier(m3) == TIER_MODERN);
}

static void
TestResolveTier(void)
{
	int32_t reason = -1;
	CHECK(resolveTier(TIER_MODERN, TIER_MODERN, &reason) == TIER_MODERN && reason == TIERREASON_NONE);
	CHECK(resolveTier(TIER_ENHANCED, TIER_MODERN, &reason) == TIER_ENHANCED && reason == TIERREASON_NONE);
	CHECK(resolveTier(TIER_CLASSIC, TIER_ENHANCED, &reason) == TIER_CLASSIC && reason == TIERREASON_NONE);
	CHECK(resolveTier(TIER_MODERN, TIER_ENHANCED, &reason) == TIER_ENHANCED && reason == TIERREASON_NORAYTRACING);
	CHECK(resolveTier(TIER_MODERN, TIER_CLASSIC, &reason) == TIER_CLASSIC && reason == TIERREASON_NOAPPLEGPU);
	CHECK(resolveTier(TIER_ENHANCED, TIER_CLASSIC, &reason) == TIER_CLASSIC && reason == TIERREASON_NOAPPLEGPU);
	CHECK(resolveTier(7, TIER_MODERN, &reason) == TIER_CLASSIC && reason == TIERREASON_NONE);
	CHECK(resolveTier(-1, TIER_MODERN, &reason) == TIER_CLASSIC && reason == TIERREASON_NONE);
}

static void
TestNames(void)
{
	CHECK(strcmp(tierName(TIER_CLASSIC), "classic") == 0);
	CHECK(strcmp(tierName(TIER_ENHANCED), "enhanced") == 0);
	CHECK(strcmp(tierName(TIER_MODERN), "modern") == 0);
	CHECK(strcmp(tierName(9), "classic") == 0);
	CHECK(strcmp(tierReasonText(TIERREASON_NONE), "") == 0);
	CHECK(strcmp(tierReasonText(TIERREASON_NORAYTRACING), "needs hardware ray tracing (GPU family Apple 9)") == 0);
	CHECK(strcmp(tierReasonText(TIERREASON_NOAPPLEGPU), "needs an Apple GPU (family Apple 7)") == 0);
}

static void
TestUpscale(void)
{
	CHECK(upscaleRatio(UPSCALE_NATIVE) == 1.0f);
	CHECK(upscaleRatio(UPSCALE_QUALITY) == 1.5f);
	CHECK(upscaleRatio(UPSCALE_BALANCED) == 1.7f);
	CHECK(upscaleRatio(UPSCALE_PERFORMANCE) == 2.0f);
	CHECK(upscaleRatio(UPSCALE_ULTRA) == 3.0f);
	CHECK(upscaleRatio(-1) == 1.0f && upscaleRatio(NUMUPSCALES) == 1.0f);
	CHECK(internalSpan(3840, UPSCALE_NATIVE) == 3840);
	CHECK(internalSpan(3840, UPSCALE_QUALITY) == 2560);
	CHECK(internalSpan(3840, UPSCALE_BALANCED) == 2259);
	CHECK(internalSpan(2160, UPSCALE_BALANCED) == 1271);
	CHECK(internalSpan(3840, UPSCALE_PERFORMANCE) == 1920);
	CHECK(internalSpan(2160, UPSCALE_ULTRA) == 720);
	CHECK(internalSpan(1, UPSCALE_ULTRA) == 1);
	CHECK(internalSpan(0, UPSCALE_QUALITY) == 0);
	CHECK(internalSpan(-5, UPSCALE_QUALITY) == 0);
}

static void
TestClamp(void)
{
	CHECK(clampRange(1000.0f, EMITTERPEAK_MIN, EMITTERPEAK_MAX, EMITTERPEAK_DEFAULT) == 1000.0f);
	CHECK(clampRange(10.0f, EMITTERPEAK_MIN, EMITTERPEAK_MAX, EMITTERPEAK_DEFAULT) == 400.0f);
	CHECK(clampRange(9000.0f, EMITTERPEAK_MIN, EMITTERPEAK_MAX, EMITTERPEAK_DEFAULT) == 4000.0f);
	CHECK(clampRange(NAN, EMITTERPEAK_MIN, EMITTERPEAK_MAX, EMITTERPEAK_DEFAULT) == 1000.0f);
	CHECK(clampRange(-1.0f, SOFTNESS_MIN, SOFTNESS_MAX, SOFTNESS_DEFAULT) == 0.0f);
	CHECK(clampRange(2.5f, SOFTNESS_MIN, SOFTNESS_MAX, SOFTNESS_DEFAULT) == 2.5f);
	CHECK(EMITTERPEAK_DEFAULT == 1000.0f && SOFTNESS_DEFAULT == 1.0f);
}

static void
TestAntiAliasNames(void)
{
	CHECK(ANTIALIAS_OFF == 0 && ANTIALIAS_FXAA == 1 && ANTIALIAS_TAA == 2);
	CHECK(strcmp(antiAliasName(ANTIALIAS_OFF), "off") == 0);
	CHECK(strcmp(antiAliasName(ANTIALIAS_FXAA), "fxaa") == 0);
	CHECK(strcmp(antiAliasName(ANTIALIAS_TAA), "taa") == 0);
	CHECK(strcmp(antiAliasName(7), "off") == 0);
	CHECK(strcmp(upscalerName(UPSCALER_TEMPORAL), "temporal") == 0 && strcmp(antiAliasName(ANTIALIAS_TEMPORAL), "temporal") == 0);
	CHECK(strcmp(upscalerName(UPSCALER_SPATIAL), "spatial") == 0 && strcmp(upscalerName(UPSCALER_NONE), "none") == 0);
	CHECK(UPSCALER_TEMPORAL == UPSCALER_SPATIAL + 1 && ANTIALIAS_TEMPORAL == ANTIALIAS_TAA + 1);
}

int
main(void)
{
	TestSupportedTier();
	TestResolveTier();
	TestNames();
	TestUpscale();
	TestClamp();
	TestAntiAliasNames();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all settings tests passed\n");
	return 0;
}
