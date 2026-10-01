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

static uint64
Im2DHostKey(uint32 fmt)
{
	metal::PipelineDesc d;
	memset(&d, 0, sizeof(d));
	d.shader = metal::im2dShader->shaderId;
	d.variant = metal::VARIANT_ALPHATEST & metal::im2dShader->variantMask;
	d.vertexLayout = metal::im2dVertexLayout;
	d.blendEnable = true;
	d.srcBlend = BLENDINVDESTALPHA;
	d.destBlend = BLENDSRCALPHASAT;
	d.writeMask = 15;
	d.colorFormat = fmt;
	d.depthFormat = metal::DEPTHFMT_D32S8;
	d.sampleCount = 1;
	return metal::pipelineKey(d);
}

static uint64
Im2DKey(uint32 fmt)
{
	metal::PipelineDesc d;
	memset(&d, 0, sizeof(d));
	d.shader = metal::im2dShader->shaderId;
	d.variant = metal::VARIANT_ALPHATEST & metal::im2dShader->variantMask;
	d.vertexLayout = metal::im2dVertexLayout;
	d.blendEnable = true;
	d.srcBlend = BLENDSRCALPHA;
	d.destBlend = BLENDINVSRCALPHA;
	d.writeMask = 15;
	d.colorFormat = fmt;
	d.depthFormat = metal::DEPTHFMT_NONE;
	d.sampleCount = 1;
	return metal::pipelineKey(d);
}

static bool
CheckClassicPrewarm(void)
{
	const char *name = "the classic tier prewarms only the camera format";
	bool rgba8 = metal::pipelineCached(Im2DKey(metal::COLORFMT_RGBA8)) != 0;
	bool scene = metal::pipelineCached(Im2DKey(metal::COLORFMT_SCENE)) != 0;
	bool overlay = metal::pipelineCached(Im2DKey(metal::COLORFMT_OVERLAY)) != 0;
	if(!rgba8 || scene || overlay)
		Detail("  cached: rgba8 %d scene %d overlay %d\n", rgba8, scene, overlay);
	return Report(rgba8 && !scene && !overlay, name);
}

static bool
CheckHostPrewarmAcrossTiers(void)
{
	const char *name = "a host pipeline prewarmed in the classic tier gains the scene and overlay formats at the first switch";
	metal::TierCaps m1 = { true, false, false };
	bool32 built = metal::prewarmIm2DShader(metal::im2dShader, 0, 1, BLENDINVDESTALPHA, BLENDSRCALPHASAT, 1);
	bool rgba8 = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_RGBA8)) != 0;
	bool sceneBefore = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_SCENE)) != 0;
	metal::setTierCapsForTest(&m1);
	SetTier(metal::TIER_ENHANCED);
	bool scene = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_SCENE)) != 0;
	bool overlay = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_OVERLAY)) != 0;
	SetTier(metal::TIER_CLASSIC);
	metal::setTierCapsForTest(nil);
	bool ok = built && rgba8 && !sceneBefore && scene && overlay;
	if(!ok)
		Detail("  built %d; classic: rgba8 %d scene %d; enhanced: scene %d overlay %d\n", built, rgba8, sceneBefore, scene, overlay);
	return Report(ok, name);
}

static bool
CheckEnhancedPrewarm(void)
{
	const char *name = "an enhanced tier has the scene and overlay formats warm; switching tiers builds nothing again";
	metal::TierCaps m1 = { true, false, false };
	metal::setTierCapsForTest(&m1);
	SetTier(metal::TIER_ENHANCED);
	bool scene = metal::pipelineCached(Im2DKey(metal::COLORFMT_SCENE)) != 0;
	bool overlay = metal::pipelineCached(Im2DKey(metal::COLORFMT_OVERLAY)) != 0;
	metal::StateStats before = metal::getStateStats();
	SetTier(metal::TIER_CLASSIC);
	SetTier(metal::TIER_ENHANCED);
	SetTier(metal::TIER_CLASSIC);
	metal::setTierCapsForTest(nil);
	metal::StateStats after = metal::getStateStats();
	bool ok = scene && overlay && after.pipelinesAtInit == before.pipelinesAtInit && after.pipelinesLate == before.pipelinesLate &&
		after.formatPrewarms == before.formatPrewarms;
	if(!ok)
		Detail("  scene %d overlay %d; pipelines at init %u -> %u, late %u -> %u, format prewarms %u -> %u\n", scene, overlay,
			before.pipelinesAtInit, after.pipelinesAtInit, before.pipelinesLate, after.pipelinesLate,
			before.formatPrewarms, after.formatPrewarms);
	return Report(ok, name);
}

static bool
CheckTierLatchedMidFrame(void)
{
	const char *name = "a tier written mid-frame applies when the frame ends";
	RGBA col = { 0, 0, 0, 255 };
	metal::TierCaps m1 = { true, false, false };
	metal::setTierCapsForTest(&m1);
	cam->clear(&col, Camera::CLEARIMAGE);
	cam->beginUpdate();
	metal::startFrame();
	SetTier(metal::TIER_ENHANCED);
	int32 during = metal::getFrameInfo().tier;
	int32 written = metal::getRenderSettings().tier;
	cam->endUpdate();
	cam->beginUpdate();
	int32 update = metal::getFrameInfo().tier;
	cam->endUpdate();
	cam->showRaster(0);
	int32 after = metal::getFrameInfo().tier;
	SetTier(metal::TIER_CLASSIC);
	metal::setTierCapsForTest(nil);
	bool ok = during == metal::TIER_CLASSIC && written == metal::TIER_ENHANCED && update == metal::TIER_CLASSIC &&
		after == metal::TIER_ENHANCED && metal::getFrameInfo().tier == metal::TIER_CLASSIC;
	if(!ok)
		Detail("  during the frame %d, written %d, at the next update %d, after the frame %d, reset %d\n", during, written,
			update, after, metal::getFrameInfo().tier);
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
	CheckClassicPrewarm();
	CheckHostPrewarmAcrossTiers();
	CheckTierFallback();
	CheckEnhancedPrewarm();
	CheckTierLatchedMidFrame();
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
