#ifdef RW_METAL
#include "metalobjc.h"
#include "metalframe.h"
#include "metalstate.h"
#include "metalkeys.h"

namespace rw {
namespace metal {

struct FrameState
{
	RenderSettings settings = { TIER_CLASSIC, UPSCALE_NATIVE, 0, HDROUTPUT_AUTO, EMITTERPEAK_DEFAULT, SOFTNESS_DEFAULT };
	int32 activeTier = TIER_CLASSIC;
	int32 tierReason = TIERREASON_NONE;
	bool capsOverride = false;
	TierCaps testCaps = { false, false, false };
	int32 internalWidth = 0, internalHeight = 0, outputWidth = 0, outputHeight = 0;
	int32 upscaler = UPSCALER_NONE;
	bool32 edr = 0;
	float32 headroom = 1.0f, potentialHeadroom = 1.0f, peak = 1.0f;
	float32 bloomWeight = 0.0f;
	RenderSettings pending;
	bool havePending = false;
	char tail[256] = "";
};

static FrameState frame;

static TierCaps
deviceCaps(void)
{
	if(frame.capsOverride)
		return frame.testCaps;
	TierCaps c = { metalCaps.appleGPU, metalCaps.apple9, metalCaps.raytracing };
	return c;
}

int32
getSupportedTier(void)
{
	return supportedTier(deviceCaps());
}

void
setTierCapsForTest(const TierCaps *caps)
{
	frame.capsOverride = caps != nil;
	if(caps)
		frame.testCaps = *caps;
}

static RenderSettings
clampSettings(const RenderSettings *s)
{
	RenderSettings n = *s;

	if(n.tier < TIER_CLASSIC || n.tier >= NUMTIERS)
		n.tier = TIER_CLASSIC;
	if(n.upscale < UPSCALE_NATIVE || n.upscale >= NUMUPSCALES)
		n.upscale = UPSCALE_NATIVE;
	n.dynamicResolution = 0;
	n.hdrOutput = n.hdrOutput == HDROUTPUT_OFF ? HDROUTPUT_OFF : HDROUTPUT_AUTO;
	n.emitterPeakNits = clampRange(n.emitterPeakNits, EMITTERPEAK_MIN, EMITTERPEAK_MAX, EMITTERPEAK_DEFAULT);
	n.softness = clampRange(n.softness, SOFTNESS_MIN, SOFTNESS_MAX, SOFTNESS_DEFAULT);
	return n;
}

static void
applySettings(const RenderSettings *s)
{
	RenderSettings n = clampSettings(s);
	int32 reason, active;

	active = resolveTier(s->tier, getSupportedTier(), &reason);
	if(reason != TIERREASON_NONE && (n.tier != frame.settings.tier || reason != frame.tierReason))
		fprintf(stderr, "rw::metal: tier %s requested, using %s; %s %s\n", tierName(n.tier), tierName(active),
			tierName(n.tier), tierReasonText(reason));
	bool changed = active != frame.activeTier;
	frame.settings = n;
	frame.activeTier = active;
	frame.tierReason = reason;
	if(changed)
		frameTierChanged();
}

void
setRenderSettings(const RenderSettings *s)
{
	MetalContext *ctx = getContext();

	if(ctx && ctx->frameStarted){
		frame.pending = *s;
		frame.havePending = true;
		return;
	}
	frame.havePending = false;
	applySettings(s);
}

void
applyPendingSettings(void)
{
	if(!frame.havePending)
		return;
	frame.havePending = false;
	applySettings(&frame.pending);
}

RenderSettings
getRenderSettings(void)
{
	return frame.havePending ? clampSettings(&frame.pending) : frame.settings;
}

FrameInfo
getFrameInfo(void)
{
	FrameInfo f;
	f.tier = frame.activeTier;
	f.tierReason = frame.tierReason;
	f.internalWidth = frame.internalWidth;
	f.internalHeight = frame.internalHeight;
	f.outputWidth = frame.outputWidth;
	f.outputHeight = frame.outputHeight;
	f.upscaler = frame.upscaler;
	f.edr = frame.edr;
	f.headroom = frame.headroom;
	f.potentialHeadroom = frame.potentialHeadroom;
	f.peak = frame.peak;
	f.bloom = frame.bloomWeight;
	return f;
}

uint32
activeFormatMask(void)
{
	if(frame.activeTier == TIER_CLASSIC)
		return 1u << COLORFMT_RGBA8;
	return 1u << COLORFMT_RGBA8 | 1u << COLORFMT_SCENE | 1u << COLORFMT_OVERLAY;
}

void
frameTierChanged(void)
{
	if(getContext() != nil)
		prewarmFormats(activeFormatMask());
}

const char*
frameStatsTail(void)
{
	FrameInfo f = getFrameInfo();
	snprintf(frame.tail, sizeof(frame.tail), " tier %s internal %dx%d output %dx%d upscaler %s edr %s "
		"headroom %.2f/%.2f peak %.2f bloom %.3f",
		tierName(f.tier), f.internalWidth, f.internalHeight, f.outputWidth, f.outputHeight,
		f.upscaler == UPSCALER_SPATIAL ? "spatial" : "none", f.edr ? "on" : "off",
		f.headroom, f.potentialHeadroom, f.peak, f.bloom);
	return frame.tail;
}

}
}
#endif
