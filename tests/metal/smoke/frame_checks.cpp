#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include "frame_checks.h"

using namespace rw;

static char details[8192];
static int detailsLen = 0;
static int failures = 0;
static Camera *cam;

static void
Detail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(details + detailsLen, sizeof(details) - detailsLen, fmt, ap);
	va_end(ap);
	if(n > 0)
		detailsLen += n;
	if(detailsLen >= (int)sizeof(details))
		detailsLen = sizeof(details) - 1;
}

static bool
Report(bool ok, const char *name)
{
	printf("%s %s\n%s", ok ? "PASS" : "FAIL", name, details);
	fflush(stdout);
	details[0] = '\0';
	detailsLen = 0;
	if(!ok)
		failures++;
	return ok;
}

static void
SetTier(int32 tier)
{
	metal::RenderSettings s = metal::getRenderSettings();
	s.tier = tier;
	metal::setRenderSettings(&s);
}

static bool
CheckSettingsDefaults(void)
{
	const char *name = "render settings default to the classic tier and clamp their ranges";
	metal::RenderSettings s = metal::getRenderSettings();
	bool ok = s.tier == metal::TIER_CLASSIC && s.upscale == metal::UPSCALE_NATIVE && !s.dynamicResolution &&
		s.hdrOutput == metal::HDROUTPUT_AUTO && s.emitterPeakNits == 1000.0f && s.softness == 1.0f;
	if(!ok)
		Detail("  defaults tier %d upscale %d dynamic %d hdr %d peak %.1f softness %.2f\n", s.tier, s.upscale,
			s.dynamicResolution, s.hdrOutput, s.emitterPeakNits, s.softness);
	metal::RenderSettings bad = s;
	bad.tier = 7;
	bad.upscale = -2;
	bad.dynamicResolution = 1;
	bad.hdrOutput = 5;
	bad.emitterPeakNits = 99999.0f;
	bad.softness = -1.0f;
	metal::setRenderSettings(&bad);
	metal::RenderSettings got = metal::getRenderSettings();
	bool clamped = got.tier == metal::TIER_CLASSIC && got.upscale == metal::UPSCALE_NATIVE && !got.dynamicResolution &&
		got.hdrOutput == metal::HDROUTPUT_AUTO && got.emitterPeakNits == 4000.0f && got.softness == 0.0f &&
		metal::getFrameInfo().tier == metal::TIER_CLASSIC;
	if(!clamped)
		Detail("  clamped tier %d upscale %d dynamic %d hdr %d peak %.1f softness %.2f\n", got.tier, got.upscale,
			got.dynamicResolution, got.hdrOutput, got.emitterPeakNits, got.softness);
	metal::setRenderSettings(&s);
	return Report(ok && clamped, name);
}

static bool
CheckTierFallback(void)
{
	const char *name = "a tier above the device's capability falls back with a reason";
	metal::TierCaps noRT = { true, false, false };
	metal::TierCaps noGPU = { false, false, false };
	metal::setTierCapsForTest(&noRT);
	SetTier(metal::TIER_MODERN);
	metal::FrameInfo a = metal::getFrameInfo();
	int32 kept = metal::getRenderSettings().tier;
	metal::setTierCapsForTest(&noGPU);
	SetTier(metal::TIER_ENHANCED);
	metal::FrameInfo b = metal::getFrameInfo();
	metal::setTierCapsForTest(nil);
	SetTier(metal::TIER_MODERN);
	metal::FrameInfo c = metal::getFrameInfo();
	SetTier(metal::TIER_CLASSIC);
	bool ok = a.tier == metal::TIER_ENHANCED && a.tierReason == metal::TIERREASON_NORAYTRACING && kept == metal::TIER_MODERN;
	ok = ok && b.tier == metal::TIER_CLASSIC && b.tierReason == metal::TIERREASON_NOAPPLEGPU;
	ok = ok && c.tier == metal::getSupportedTier() && (c.tierReason == metal::TIERREASON_NONE) == (c.tier == metal::TIER_MODERN);
	ok = ok && metal::getFrameInfo().tier == metal::TIER_CLASSIC;
	if(!ok)
		Detail("  no ray tracing: tier %d reason %d kept %d; no Apple GPU: tier %d reason %d; device: tier %d reason %d supported %d\n",
			a.tier, a.tierReason, kept, b.tier, b.tierReason, c.tier, c.tierReason, metal::getSupportedTier());
	return Report(ok, name);
}

static bool
CheckStatsTail(void)
{
	const char *name = "the stats line ends with the tier, frame sizes, upscaler, EDR state and bloom weight";
	metal::logStats();
	const char *line = metal::getStatsLine();
	const char *tail = strstr(line, " samples ");
	char tier[16], ups[16], edr[8];
	int iw, ih, ow, oh;
	unsigned samples;
	float hr, ph, peak, bloom;
	int n = tail ? sscanf(tail, " samples %u tier %15s internal %dx%d output %dx%d upscaler %15s edr %7s headroom %f/%f peak %f bloom %f",
		&samples, tier, &iw, &ih, &ow, &oh, ups, edr, &hr, &ph, &peak, &bloom) : 0;
	bool ok = n == 12 && strcmp(tier, "classic") == 0 && strcmp(ups, "none") == 0 && strcmp(edr, "off") == 0 &&
		hr == 1.0f && ph == 1.0f && peak == 1.0f && bloom == 0.0f && line[strlen(line)-1] == '\n';
	if(!ok)
		Detail("  parsed %d fields from \"%s\"\n", n, line);
	return Report(ok, name);
}

int
RunFrameChecks(Camera *camera)
{
	failures = 0;
	cam = camera;
	CheckSettingsDefaults();
	CheckTierFallback();
	CheckStatsTail();
	SetTier(metal::TIER_CLASSIC);
	return failures;
}

int
RunRestartFrameChecks(bool (*restart)(uint32 samples), Camera *(*camera)(void))
{
	failures = 0;
	cam = camera();
	return failures;
}
