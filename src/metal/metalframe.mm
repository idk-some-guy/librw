#ifdef RW_METAL
#include "metalobjc.h"
#include "metalframe.h"
#include "metalstate.h"
#include "metalkeys.h"
#include "rwmetalshader.h"
#include "metalbloom.h"
#include "metaloutput.h"
#include "metaldering.h"
#include "metalaa.h"
#include "metalprepass.h"
#include "metaltemporal.h"
#include "metalraytrace.h"
#include <algorithm>
#include <string>
#include <vector>
#include <simd/simd.h>
#import <AppKit/AppKit.h>
#import <MetalFX/MetalFX.h>

namespace rw {
namespace metal {

#include "shaders/frame_metal.inc"
#include "shaders/grade_metal.inc"
#include "shaders/output_metal.inc"
#include "shaders/bloom_metal.inc"
#include "shaders/bloomh_metal.inc"
#include "shaders/dering_metal.inc"
#include "shaders/deringh_metal.inc"
#include "shaders/fxaa_metal.inc"
#include "shaders/aah_metal.inc"
#include "shaders/motion_metal.inc"
#include "shaders/im3d_metal.inc"
#include "shaders/reactive_metal.inc"
#include "shaders/temporalh_metal.inc"
#include "shaders/taa_metal.inc"
extern const char *prepassh_metal_src;
extern const char *header_metal_src;
extern const char *im2d_metal_src;

#define JITTER_SIGN 1.0f
enum { REACTIVE_DRAWS = 4096 };
enum FrameMath { MATH_FAST, MATH_RELAXED, MATH_SAFE };

static const char *frameHead =
"#include <metal_stdlib>\n"
"using namespace metal;\n";

static const char *defaultHostGrade =
"static float4\n"
"hostGrade(float4 c, constant float4 *params)\n"
"{\n"
"	return c;\n"
"}\n";

struct GradeFrame
{
	float32 bloomWeight;
	uint32 grade;
	float32 bloomNorm;
	float32 pad;
};

struct TaaFrame
{
	uint32 width, height, valid, pad;
	float32 jitterX, jitterY, pad2[2];
};

struct FrameState
{
	RenderSettings settings = { TIER_CLASSIC, UPSCALE_NATIVE, 0, HDROUTPUT_AUTO, EMITTERPEAK_DEFAULT, SOFTNESS_DEFAULT, ANTIALIAS_OFF };
	int32 activeTier = TIER_CLASSIC;
	int32 tierReason = TIERREASON_NONE;
	bool capsOverride = false;
	TierCaps testCaps = { false, false, false };
	bool available = false;
	RenderSettings pending;
	bool havePending = false;

	int32 phase = PHASE_CLASSIC;
	Raster *mainColor = nil, *mainDepth = nil;
	Raster *sceneBase = nil, *sceneDepth = nil;
	id<MTLTexture> output = nil, black = nil;
	id<MTLTexture> graded = nil, upscaled = nil;
	id<MTLFXSpatialScaler> scaler = nil;
	int32 factor = -1;
	bool scalerReported = false;
	bool scalerFail = false;
	uint32 scalerAttempts = 0, scalerLogs = 0;
	id<MTLComputePipelineState> deringInPlace = nil, deringCopy = nil;
	bool deringCopyForced = false;
	int32 deringNanX = -1, deringNanY = -1;
	bool upscaledFail = false, upscaledGaveUp = false, upscaledReported = false;
	uint32 upscaledAttempts = 0, upscaledLogs = 0;
	id<MTLTexture> aaSource = nil;
	bool aaFail = false, aaGaveUp = false, aaReported = false;
	uint32 aaAllocs = 0, aaLogs = 0;
	id<MTLComputePipelineState> fxaa = nil;
	uint32 samples = 0;
	bool dropping = false;
	bool unendedReported = false;
	bool targetsReported = false;

	int32 internalWidth = 0, internalHeight = 0, outputWidth = 0, outputHeight = 0;
	int32 upscaler = UPSCALER_NONE;
	bool32 edr = 0;
	float32 headroom = 1.0f, potentialHeadroom = 1.0f, peak = 1.0f;
	bool outputDirty = true;
	bool edrOverride = false;
	float32 testCurrent = 1.0f, testPotential = 1.0f;
	bool testScreenLost = false;
	uint32 edrLogs = 0;
	CGColorSpaceRef screenSpace = nil;
	bool screenKnown = false;
	float32 bloomWeight = 0.0f;
	float32 emissiveScale = 1.0f;

	id<MTLLibrary> library = nil;
	id<MTLComputePipelineState> grade = nil;
	std::string gradeSource;
	bool32 gradeResult = 0;
	uint32 gradeBuilds = 0;
	id<MTLRenderPipelineState> compositeBGRA = nil, copyScene = nil, copyPost = nil;
	id<MTLRenderPipelineState> compositeEdr = nil, classicEdr = nil;
	id<MTLTexture> bloomDown[BLOOM_LEVELS], bloomUp[BLOOM_LEVELS];
	id<MTLComputePipelineState> bloomDownFirst = nil, bloomDownPipe = nil, bloomUpPipe = nil;
	bool karis = true;
	Raster *prepassRaster = nil;
	id<MTLTexture> prepassNormal = nil, prepassMotion = nil, prepassNormalMS = nil, prepassMotionMS = nil;
	bool prepassOpen = false, prepassDone = false, prepassFail = false, prepassReported = false;
	bool rayWorkDone = false;
	uint32 prepassPasses = 0;
	PassTarget prepassReturn = { nil, nil };
	uint32 prepassDrawsLast = 0;
	id<MTLComputePipelineState> farMotion = nil, prepassResolve = nil;
	uint32 shows = 0, prepassShow = 0;
	bool prepassShowKnown = false;
	bool prepassValid = false, prevPrepassValid = false;
	uint32 prepassValidShow = 0;
	id<MTLFXTemporalScaler> temporal = nil;
	bool temporalFail = false, temporalReported = false, temporalNoTargets = false;
	uint32 temporalLogs = 0;
	bool temporalReset = true, temporalResetLast = false;
	int32 targetsTier = -1;
	bool targetsTemporal = false;
	uint32 jitterIndex = 0;
	TMJitter jitter = { 0.0f, 0.0f };
	bool jitterOn = false, jitterApplied = false;
	bool temporalHistory = false;
	uint32 temporalHistoryShow = 0;
	id<MTLTexture> reactive = nil;
	id<MTLRenderPipelineState> reactive2D = nil, reactive2DUV2 = nil, reactive3D = nil;
	MTLRenderPassDescriptor *reactivePass = nil;
	bool reactiveBuilt = false, reactiveReported = false;
	std::vector<ReactiveDraw> reactiveDraws;
	int32 reactiveDepth = 0;
	id<MTLComputePipelineState> taa = nil;
	id<MTLTexture> taaHistory[2] = { nil, nil };
	uint32 taaIndex = 0;
	bool taaFail = false, taaGaveUp = false, taaReported = false;
	uint32 taaAllocs = 0, taaLogs = 0, taaRequests = 0;
	id<MTLTexture> white = nil, testShadow = nil;
	char tail[512] = "";
	char lastTail[512] = "";
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
	if(!frame.available)
		return TIER_CLASSIC;
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
	if(n.antiAliasing != ANTIALIAS_FXAA && n.antiAliasing != ANTIALIAS_TAA)
		n.antiAliasing = ANTIALIAS_OFF;
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
	if(n.antiAliasing == ANTIALIAS_TAA && frame.settings.antiAliasing != ANTIALIAS_TAA &&
	   (active != TIER_MODERN || !prepassShadersReady() || frame.farMotion == nil || frame.prepassResolve == nil ||
	    (n.upscale == UPSCALE_NATIVE && frame.taa == nil))){
		frame.taaRequests++;
		fprintf(stderr, "rw::metal: anti-aliasing taa requested, using fxaa; taa needs depth and motion vectors\n");
	}
	bool changed = active != frame.activeTier;
	if(changed || memcmp(&n, &frame.settings, sizeof(n)) != 0)
		frame.temporalReset = true;
	frame.settings = n;
	frame.activeTier = active;
	frame.tierReason = reason;
	frame.outputDirty = true;
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

static bool
taaRuns(void)
{
	return frame.activeTier == TIER_MODERN && frame.settings.antiAliasing == ANTIALIAS_TAA &&
		frame.settings.upscale == UPSCALE_NATIVE && frame.upscaler == UPSCALER_NONE && frame.taa && !frame.taaGaveUp &&
		frame.sceneDepth && prepassShadersReady() && frame.farMotion && frame.prepassResolve;
}

static int32
activeAntiAlias(void)
{
	if(frame.activeTier == TIER_CLASSIC)
		return ANTIALIAS_OFF;
	if(frame.temporal)
		return ANTIALIAS_TEMPORAL;
	if(frame.aaGaveUp || frame.settings.antiAliasing == ANTIALIAS_OFF)
		return ANTIALIAS_OFF;
	if(taaRuns())
		return ANTIALIAS_TAA;
	return frame.fxaa ? ANTIALIAS_FXAA : ANTIALIAS_OFF;
}

static bool
temporalConsumer(void)
{
	return frame.temporal != nil || activeAntiAlias() == ANTIALIAS_TAA;
}

static TMJitter
appliedJitter(void)
{
	return frame.jitterApplied ? frame.jitter : TMJitter{ 0.0f, 0.0f };
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
	f.antiAliasing = activeAntiAlias();
	TMJitter j = appliedJitter();
	f.jitterX = j.x;
	f.jitterY = j.y;
	f.prepassValid = frame.prepassValid;
	f.temporalReset = frame.temporalResetLast;
	return f;
}

const char*
frameStatsTail(void)
{
	FrameInfo f = getFrameInfo();
	snprintf(frame.tail, sizeof(frame.tail), " tier %s internal %dx%d output %dx%d upscaler %s edr %s "
		"headroom %.2f/%.2f peak %.2f bloom %.3f aa %s prepass %u equal %u unkeyed %u",
		tierName(f.tier), f.internalWidth, f.internalHeight, f.outputWidth, f.outputHeight,
		upscalerName(f.upscaler), f.edr ? "on" : "off",
		f.headroom, f.potentialHeadroom, f.peak, f.bloom, antiAliasName(f.antiAliasing), frame.prepassDrawsLast,
		prepassEqualLastFrame(), getStateStats().prepassUnkeyed);
	return frame.tail;
}

const char*
frameLastStatsTail(void)
{
	return frame.lastTail[0] ? frame.lastTail : " tier none";
}

uint32
activeFormatMask(void)
{
	if(frame.activeTier == TIER_CLASSIC)
		return 1u << COLORFMT_RGBA8;
	uint32 mask = 1u << COLORFMT_RGBA8 | 1u << COLORFMT_SCENE | 1u << COLORFMT_OVERLAY;
	if(frame.activeTier == TIER_MODERN)
		mask |= 1u << COLORFMT_PREPASS;
	return mask;
}

static id<MTLTexture>
newTexture(MTLPixelFormat format, int32 w, int32 h, MTLTextureUsage usage, uint32 samples)
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
		width:w height:h mipmapped:NO];
	d.usage = usage;
	d.storageMode = MTLStorageModePrivate;
	if(samples > 1){
		d.textureType = MTLTextureType2DMultisample;
		d.sampleCount = samples;
	}
	return [getContext()->device newTextureWithDescriptor:d];
}

static id<MTLLibrary>
frameLibrary(const char **parts, NSString **error, FrameMath math = MATH_FAST)
{
	NSMutableString *all = [NSMutableString string];
	NSError *err = nil;
	MTLCompileOptions *opts = nil;
	for(int i = 0; parts[i]; i++)
		[all appendString:[NSString stringWithUTF8String:parts[i]]];
	if(math != MATH_FAST){
		opts = [MTLCompileOptions new];
		if(@available(macOS 15.0, *))
			opts.mathMode = math == MATH_SAFE ? MTLMathModeSafe : MTLMathModeRelaxed;
		else
			opts.fastMathEnabled = NO;
	}
	id<MTLLibrary> lib = [getContext()->device newLibraryWithSource:all options:opts error:&err];
	if(lib == nil && error)
		*error = err.localizedDescription;
	return lib;
}

static id<MTLComputePipelineState>
buildGrade(const char *host, NSString **error)
{
	const char *parts[] = { frameHead, host, grade_metal_src, nil };
	NSError *err = nil;
	frame.gradeBuilds++;
	id<MTLLibrary> lib = frameLibrary(parts, error, MATH_RELAXED);
	if(lib == nil)
		return nil;
	id<MTLFunction> fn = [lib newFunctionWithName:@"gradeKernel"];
	if(fn == nil){
		if(error)
			*error = @"no gradeKernel";
		return nil;
	}
	id<MTLComputePipelineState> p = [getContext()->device newComputePipelineStateWithFunction:fn error:&err];
	if(p == nil && error)
		*error = err.localizedDescription;
	return p;
}

uint32
gradeBuildsForTest(void)
{
	return frame.gradeBuilds;
}

bool32
setGradeShader(const char *src)
{
	NSString *err = nil;

	if(getContext() == nil)
		return 0;
	if(src == nil)
		src = defaultHostGrade;
	if(frame.gradeSource == src)
		return frame.gradeResult;
	frame.gradeSource = src;
	@autoreleasepool {
		id<MTLComputePipelineState> p = buildGrade(src, &err);
		if(p == nil){
			fprintf(stderr, "rw::metal: grade shader failed to compile, keeping the previous one: %s\n",
				err ? err.UTF8String : "no gradeKernel");
			frame.gradeResult = 0;
			return 0;
		}
		frame.grade = p;
	}
	frame.gradeResult = 1;
	return 1;
}

static id<MTLRenderPipelineState>
framePipeline(NSString *fs, MTLPixelFormat format)
{
	MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
	NSError *err = nil;
	d.vertexFunction = [frame.library newFunctionWithName:@"frameVS"];
	d.fragmentFunction = [frame.library newFunctionWithName:fs];
	d.colorAttachments[0].pixelFormat = format;
	id<MTLRenderPipelineState> p = [getContext()->device newRenderPipelineStateWithDescriptor:d error:&err];
	if(p == nil)
		fprintf(stderr, "rw::metal: frame pipeline %s failed: %s\n", fs.UTF8String,
			err ? err.localizedDescription.UTF8String : "no function");
	return p;
}

bool32
frameInit(void)
{
	NSString *err = nil;
	const char *parts[] = { frameHead, output_metal_src, frame_metal_src, nil };
	uint16 zero[4] = { 0, 0, 0, 0 };

	@autoreleasepool {
		frame.library = frameLibrary(parts, &err);
		if(frame.library){
			frame.compositeBGRA = framePipeline(@"frameCompositeFS", MTLPixelFormatBGRA8Unorm);
			frame.copyScene = framePipeline(@"frameCopySceneFS", MTLPixelFormatRGBA8Unorm);
			frame.copyPost = framePipeline(@"frameCopyPostFS", MTLPixelFormatRGBA8Unorm);
			frame.compositeEdr = framePipeline(@"frameCompositeEdrFS", MTLPixelFormatRGBA16Float);
			frame.classicEdr = framePipeline(@"frameClassicEdrFS", MTLPixelFormatRGBA16Float);
		}else
			fprintf(stderr, "rw::metal: frame shaders failed to compile: %s\n", err ? err.UTF8String : "");
		frame.grade = buildGrade(defaultHostGrade, &err);
		frame.gradeSource = defaultHostGrade;
		frame.gradeResult = frame.grade != nil;
		if(frame.grade == nil)
			fprintf(stderr, "rw::metal: grade kernel failed to build: %s\n", err ? err.UTF8String : "");
		const char *bloomParts[] = { frameHead, bloomh_metal_src, bloom_metal_src, nil };
		id<MTLLibrary> bloomLib = frameLibrary(bloomParts, &err, MATH_RELAXED);
		if(bloomLib){
			NSError *perr = nil;
			id<MTLDevice> dev = getContext()->device;
			frame.bloomDownFirst = [dev newComputePipelineStateWithFunction:[bloomLib newFunctionWithName:@"bloomDownFirst"] error:&perr];
			frame.bloomDownPipe = [dev newComputePipelineStateWithFunction:[bloomLib newFunctionWithName:@"bloomDown"] error:&perr];
			frame.bloomUpPipe = [dev newComputePipelineStateWithFunction:[bloomLib newFunctionWithName:@"bloomUp"] error:&perr];
		}else{
			frame.bloomDownFirst = frame.bloomDownPipe = frame.bloomUpPipe = nil;
			fprintf(stderr, "rw::metal: bloom shaders failed to compile: %s\n", err ? err.UTF8String : "");
		}
		const char *deringParts[] = { frameHead, deringh_metal_src, dering_metal_src, nil };
		id<MTLLibrary> deringLib = frameLibrary(deringParts, &err, MATH_SAFE);
		if(deringLib){
			NSError *perr = nil;
			id<MTLDevice> dev = getContext()->device;
			frame.deringInPlace = [dev newComputePipelineStateWithFunction:[deringLib newFunctionWithName:@"deringInPlace"] error:&perr];
			frame.deringCopy = [dev newComputePipelineStateWithFunction:[deringLib newFunctionWithName:@"deringCopy"] error:&perr];
		}else{
			frame.deringInPlace = frame.deringCopy = nil;
			fprintf(stderr, "rw::metal: dering shaders failed to compile: %s\n", err ? err.UTF8String : "");
		}
		const char *aaParts[] = { frameHead, aah_metal_src, fxaa_metal_src, nil };
		NSError *aerr = nil;
		id<MTLLibrary> aaLib = frameLibrary(aaParts, &err);
		id<MTLFunction> aaFn = aaLib ? [aaLib newFunctionWithName:@"fxaaKernel"] : nil;
		frame.fxaa = aaFn ? [getContext()->device newComputePipelineStateWithFunction:aaFn error:&aerr] : nil;
		if(frame.fxaa == nil)
			fprintf(stderr, "rw::metal: FXAA kernel failed to build: %s\n",
				aerr ? aerr.localizedDescription.UTF8String : aaLib ? "no fxaaKernel" : err ? err.UTF8String : "");
			const char *motionParts[] = { frameHead, prepassh_metal_src, motion_metal_src, nil };
			id<MTLLibrary> motionLib = frameLibrary(motionParts, &err);
			if(motionLib){
				NSError *perr = nil;
				id<MTLDevice> dev = getContext()->device;
				frame.farMotion = [dev newComputePipelineStateWithFunction:[motionLib newFunctionWithName:@"farMotion"] error:&perr];
				frame.prepassResolve = [dev newComputePipelineStateWithFunction:[motionLib newFunctionWithName:@"prepassResolve"]
					error:&perr];
			}else
				frame.farMotion = frame.prepassResolve = nil;
			if(frame.farMotion == nil || frame.prepassResolve == nil)
				fprintf(stderr, "rw::metal: motion kernels failed to build; modern frames draw without the prepass\n");
		const char *taaParts[] = { frameHead, temporalh_metal_src, taa_metal_src, nil };
		NSError *terr = nil;
		id<MTLLibrary> taaLib = frameLibrary(taaParts, &err);
		id<MTLFunction> taaFn = taaLib ? [taaLib newFunctionWithName:@"taaResolve"] : nil;
		frame.taa = taaFn ? [getContext()->device newComputePipelineStateWithFunction:taaFn error:&terr] : nil;
		if(frame.taa == nil)
			fprintf(stderr, "rw::metal: TAA kernel failed to build: %s\n",
				terr ? terr.localizedDescription.UTF8String : taaLib ? "no taaResolve" : err ? err.UTF8String : "");
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
			width:1 height:1 mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModeShared;
		frame.black = [getContext()->device newTextureWithDescriptor:d];
		[frame.black replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:zero bytesPerRow:sizeof(zero)];
		uint16 one[4] = { 0x3C00, 0x3C00, 0x3C00, 0x3C00 };
		frame.white = [getContext()->device newTextureWithDescriptor:d];
		[frame.white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:one bytesPerRow:sizeof(one)];
		frame.testShadow = nil;
	}
	frame.lastTail[0] = '\0';
	frame.available = frame.compositeBGRA && frame.copyScene && frame.copyPost && frame.grade && frame.black && frame.white;
	return frame.available;
}

static void dropPrepassTargets(void);

static void
dropSceneTargets(void)
{
	frame.dropping = true;
	dropPrepassTargets();
	finishPrepassFrame(false);
	frame.prepassOpen = false;
	frame.prepassDone = false;
	if(frame.sceneBase){
		frame.sceneBase->destroy();
		frame.sceneBase = nil;
	}
	if(frame.sceneDepth){
		frame.sceneDepth->destroy();
		frame.sceneDepth = nil;
	}
	frame.dropping = false;
	frame.output = nil;
	frame.graded = nil;
	frame.upscaled = nil;
	frame.upscaledGaveUp = false;
	frame.aaSource = nil;
	frame.aaGaveUp = false;
	frame.taaHistory[0] = frame.taaHistory[1] = nil;
	frame.taaGaveUp = false;
	frame.scaler = nil;
	frame.temporal = nil;
	frame.reactive = nil;
	frame.targetsTier = -1;
	frame.targetsTemporal = false;
	frame.temporalReset = true;
	frame.jitterOn = false;
	frame.jitterApplied = false;
	frame.jitter = TMJitter{ 0.0f, 0.0f };
	frame.temporalHistory = false;
	frame.factor = -1;
	frame.upscaler = UPSCALER_NONE;
	for(int i = 0; i < BLOOM_LEVELS; i++){
		frame.bloomDown[i] = nil;
		frame.bloomUp[i] = nil;
	}
	frame.mainColor = nil;
	frame.mainDepth = nil;
	frame.internalWidth = frame.internalHeight = 0;
	frame.outputWidth = frame.outputHeight = 0;
	frame.samples = 0;
	frame.phase = PHASE_CLASSIC;
}

static id<MTLFXSpatialScaler>
makeSpatialScaler(int32 iw, int32 ih, int32 ow, int32 oh)
{
	id<MTLDevice> dev = getContext()->device;
	frame.scalerAttempts++;
	if(frame.scalerFail || ![MTLFXSpatialScalerDescriptor supportsDevice:dev])
		return nil;
	MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
	d.inputWidth = iw;
	d.inputHeight = ih;
	d.outputWidth = ow;
	d.outputHeight = oh;
	d.colorTextureFormat = MTLPixelFormatRGBA16Float;
	d.outputTextureFormat = MTLPixelFormatRGBA16Float;
	d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
	return [d newSpatialScalerWithDevice:dev];
}

static id<MTLFXTemporalScaler>
makeTemporalScaler(int32 iw, int32 ih, int32 ow, int32 oh)
{
	id<MTLDevice> dev = getContext()->device;
	if(frame.temporalFail || frame.temporalNoTargets || ![MTLFXTemporalScalerDescriptor supportsDevice:dev])
		return nil;
	MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
	d.colorTextureFormat = MTLPixelFormatRGBA16Float;
	d.depthTextureFormat = MTLPixelFormatDepth32Float_Stencil8;
	d.motionTextureFormat = MTLPixelFormatRG16Float;
	d.outputTextureFormat = MTLPixelFormatRGBA16Float;
	d.inputWidth = iw;
	d.inputHeight = ih;
	d.outputWidth = ow;
	d.outputHeight = oh;
	d.autoExposureEnabled = NO;
	if(@available(macOS 14.4, *)){
		d.reactiveMaskTextureEnabled = YES;
		d.reactiveMaskTextureFormat = MTLPixelFormatR8Unorm;
	}
	return [d newTemporalScalerWithDevice:dev];
}

static id<MTLRenderPipelineState>
reactivePipeline(id<MTLLibrary> lib, NSString *vs, uint32 layout)
{
	MTLVertexDescriptor *vd = layoutVertexDescriptor(layout);
	if(lib == nil || vd == nil)
		return nil;
	MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
	NSError *err = nil;
	MTLFunctionConstantValues *off = [MTLFunctionConstantValues new];
	bool no = false;
	[off setConstantValue:&no type:MTLDataTypeBool atIndex:6];
	[off setConstantValue:&no type:MTLDataTypeBool atIndex:7];
	[off setConstantValue:&no type:MTLDataTypeBool atIndex:8];
	d.vertexFunction = [lib newFunctionWithName:vs constantValues:off error:&err];
	d.fragmentFunction = [lib newFunctionWithName:@"reactiveFS" constantValues:off error:&err];
	d.vertexDescriptor = vd;
	d.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
	d.colorAttachments[0].blendingEnabled = YES;
	d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationMax;
	d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationMax;
	d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
	d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
	d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
	id<MTLRenderPipelineState> p = [getContext()->device newRenderPipelineStateWithDescriptor:d error:&err];
	if(p == nil)
		fprintf(stderr, "rw::metal: reactive pipeline %s failed: %s\n", vs.UTF8String,
			err ? err.localizedDescription.UTF8String : "no function");
	return p;
}

void
frameBuildReactive(void)
{
	if(frame.reactiveBuilt)
		return;
	frame.reactiveBuilt = true;
	frame.reactiveDraws.reserve(REACTIVE_DRAWS);
	frame.reactivePass = [MTLRenderPassDescriptor renderPassDescriptor];
	frame.reactivePass.colorAttachments[0].loadAction = MTLLoadActionClear;
	frame.reactivePass.colorAttachments[0].storeAction = MTLStoreActionStore;
	frame.reactivePass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	const char *parts[] = { header_metal_src, im2d_metal_src, im3d_metal_src, reactive_metal_src, nil };
	NSString *err = nil;
	id<MTLLibrary> lib = frameLibrary(parts, &err);
	if(lib == nil)
		fprintf(stderr, "rw::metal: reactive shaders failed to compile: %s\n", err ? err.UTF8String : "");
	frame.reactive2D = reactivePipeline(lib, @"im2dVS", im2dVertexLayout);
	frame.reactive2DUV2 = im2dUV2VertexLayout ? reactivePipeline(lib, @"im2dVS", im2dUV2VertexLayout) : nil;
	frame.reactive3D = reactivePipeline(lib, @"im3dVS", im3dVertexLayout);
}

static bool ensurePrepassTargets(void);

static bool
ensureSceneTargets(Raster *main, Raster *depth)
{
	int32 ow = main->width, oh = main->height;
	int32 factor = frame.settings.upscale;
	int32 iw = internalSpan(ow, factor), ih = internalSpan(oh, factor);
	uint32 samples = metalGlobals.numSamples > 1 ? metalGlobals.numSamples : 1;
	MTLTextureUsage outputUsage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	MTLTextureUsage gradedUsage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

	bool wantTemporal = frame.activeTier == TIER_MODERN && frame.settings.antiAliasing == ANTIALIAS_TAA && (iw != ow || ih != oh);
	bool temporal = wantTemporal && depth && prepassShadersReady() && frame.farMotion && frame.prepassResolve;

	if(frame.sceneBase && frame.mainColor == main && frame.mainDepth == depth &&
	   frame.outputWidth == ow && frame.outputHeight == oh && frame.factor == factor && frame.samples == samples &&
	   frame.targetsTier == frame.activeTier && frame.targetsTemporal == temporal)
		return true;
	dropSceneTargets();
	if(wantTemporal && !temporal && !frame.temporalReported){
		frame.temporalReported = true;
		frame.temporalLogs++;
		fprintf(stderr, "rw::metal: temporal scaler unavailable for %dx%d to %dx%d; taa needs %s; using the spatial scaler\n",
			iw, ih, ow, oh, depth ? "the prepass shaders" : "a depth raster");
	}
	if(iw != ow || ih != oh){
		if(temporal){
			frame.temporal = makeTemporalScaler(iw, ih, ow, oh);
			if(frame.temporal == nil && !frame.temporalReported){
				frame.temporalReported = true;
				frame.temporalLogs++;
				fprintf(stderr, "rw::metal: temporal scaler unavailable for %dx%d to %dx%d; using the spatial scaler\n",
					iw, ih, ow, oh);
			}
		}
		if(frame.temporal){
			outputUsage |= frame.temporal.outputTextureUsage;
			gradedUsage |= frame.temporal.colorTextureUsage;
		}else{
			frame.scaler = makeSpatialScaler(iw, ih, ow, oh);
			if(frame.scaler == nil){
				if(!frame.scalerReported){
					frame.scalerReported = true;
					frame.scalerLogs++;
					fprintf(stderr, "rw::metal: spatial scaler unavailable for %dx%d to %dx%d; rendering at native resolution\n",
						iw, ih, ow, oh);
				}
				iw = ow;
				ih = oh;
			}else{
				outputUsage |= frame.scaler.outputTextureUsage;
				gradedUsage |= frame.scaler.colorTextureUsage;
			}
		}
	}
	frame.sceneBase = createSceneRaster(iw, ih);
	frame.sceneDepth = depth ? Raster::create(iw, ih, 0, Raster::ZBUFFER) : nil;
	frame.output = newTexture(MTLPixelFormatRGBA16Float, ow, oh, outputUsage, 1);
	frame.graded = frame.scaler || frame.temporal ? newTexture(MTLPixelFormatRGBA16Float, iw, ih, gradedUsage, 1) : nil;
	if(frame.sceneBase == nil || (depth && frame.sceneDepth == nil) || frame.output == nil ||
	   ((frame.scaler || frame.temporal) && frame.graded == nil)){
		dropSceneTargets();
		if(!frame.targetsReported){
			frame.targetsReported = true;
			fprintf(stderr, "rw::metal: scene targets for %dx%d could not be created; rendering classic frames\n", ow, oh);
		}
		return false;
	}
	frame.mainColor = main;
	frame.mainDepth = depth;
	frame.outputWidth = ow;
	frame.outputHeight = oh;
	frame.internalWidth = iw;
	frame.internalHeight = ih;
	frame.samples = samples;
	frame.factor = factor;
	frame.targetsTier = frame.activeTier;
	frame.targetsTemporal = temporal;
	if(frame.temporal && !ensurePrepassTargets()){
		frame.temporalNoTargets = true;
		frame.temporalReported = true;
		frame.temporalLogs++;
		fprintf(stderr, "rw::metal: the temporal scaler has no motion target; using the spatial scaler\n");
		dropSceneTargets();
		return ensureSceneTargets(main, depth);
	}
	frame.upscaler = frame.temporal ? UPSCALER_TEMPORAL : frame.scaler ? UPSCALER_SPATIAL : UPSCALER_NONE;
	return true;
}

void
setBloomKarisForTest(bool32 on)
{
	frame.karis = on != 0;
}

void
setDeringCopyForTest(bool32 copy)
{
	frame.deringCopyForced = copy != 0;
}

void
setDeringNanForTest(int32 x, int32 y)
{
	frame.deringNanX = x;
	frame.deringNanY = y;
}

void
setAntiAliasFailForTest(bool32 fail)
{
	frame.aaFail = fail != 0;
}

void
antiAliasCountsForTest(uint32 *allocs, uint32 *logs)
{
	*allocs = frame.aaAllocs;
	*logs = frame.aaLogs;
}

bool32
antiAliasTextureForTest(void)
{
	return frame.aaSource != nil;
}

void
setUpscaledFailForTest(bool32 fail)
{
	frame.upscaledFail = fail != 0;
}

void
upscaledCountsForTest(uint32 *attempts, uint32 *logs)
{
	*attempts = frame.upscaledAttempts;
	*logs = frame.upscaledLogs;
}

void
setScalerFailForTest(bool32 fail)
{
	frame.scalerFail = fail != 0;
}

void
scalerCountsForTest(uint32 *attempts, uint32 *logs)
{
	*attempts = frame.scalerAttempts;
	*logs = frame.scalerLogs;
}

static void
dispatchOver(id<MTLComputeCommandEncoder> enc, id<MTLTexture> t)
{
	[enc dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
}

static void
encodeBloom(void)
{
	uint32 karis = frame.karis ? 1 : 0;
	float32 radius = bloomRadius(frame.settings.softness);
	id<MTLComputeCommandEncoder> enc = [frameCommandBuffer() computeCommandEncoder];
	[enc setComputePipelineState:frame.bloomDownFirst];
	[enc setTexture:getRasterTexture(frame.sceneBase) atIndex:0];
	[enc setTexture:frame.bloomDown[0] atIndex:2];
	[enc setBytes:&karis length:sizeof(karis) atIndex:0];
	dispatchOver(enc, frame.bloomDown[0]);
	[enc setComputePipelineState:frame.bloomDownPipe];
	for(int i = 1; i < BLOOM_LEVELS; i++){
		[enc setTexture:frame.bloomDown[i-1] atIndex:0];
		[enc setTexture:frame.bloomDown[i] atIndex:2];
		dispatchOver(enc, frame.bloomDown[i]);
	}
	[enc setComputePipelineState:frame.bloomUpPipe];
	[enc setBytes:&radius length:sizeof(radius) atIndex:0];
	id<MTLTexture> lower = frame.bloomDown[BLOOM_LEVELS-1];
	for(int i = BLOOM_LEVELS-2; i >= 0; i--){
		[enc setTexture:lower atIndex:0];
		[enc setTexture:frame.bloomDown[i] atIndex:1];
		[enc setTexture:frame.bloomUp[i] atIndex:2];
		dispatchOver(enc, frame.bloomUp[i]);
		lower = frame.bloomUp[i];
	}
	[enc endEncoding];
}

static bool
ensureBloomTargets(void)
{
	if(frame.bloomDown[0] && frame.bloomUp[0])
		return true;
	for(int i = 0; i < BLOOM_LEVELS; i++){
		int32 bw = bloomLevelSpan(frame.internalWidth, i), bh = bloomLevelSpan(frame.internalHeight, i);
		frame.bloomDown[i] = newTexture(MTLPixelFormatRGBA16Float, bw, bh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, 1);
		frame.bloomUp[i] = i < BLOOM_LEVELS-1 ?
			newTexture(MTLPixelFormatRGBA16Float, bw, bh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, 1) : nil;
		if(frame.bloomDown[i] == nil || (i < BLOOM_LEVELS-1 && frame.bloomUp[i] == nil)){
			for(int j = 0; j < BLOOM_LEVELS; j++){
				frame.bloomDown[j] = nil;
				frame.bloomUp[j] = nil;
			}
			return false;
		}
	}
	return true;
}

static bool
ensureUpscaled(void)
{
	if(frame.upscaled || frame.upscaledGaveUp)
		return frame.upscaled != nil;
	frame.upscaledAttempts++;
	frame.upscaled = frame.upscaledFail ? nil : newTexture(MTLPixelFormatRGBA16Float, frame.outputWidth,
		frame.outputHeight, MTLTextureUsageShaderRead | frame.scaler.outputTextureUsage, 1);
	if(frame.upscaled)
		return true;
	frame.upscaledGaveUp = true;
	if(!frame.upscaledReported){
		frame.upscaledReported = true;
		frame.upscaledLogs++;
		fprintf(stderr, "rw::metal: dering copy texture for %dx%d could not be created; the upscaled frame goes out unclamped\n",
			frame.outputWidth, frame.outputHeight);
	}
	return false;
}

static void
injectNan(id<MTLTexture> t)
{
	uint16 px[4] = { 0x7e00, 0x7e00, 0x7e00, 0x3c00 };
	id<MTLBuffer> buf = [getContext()->device newBufferWithBytes:px length:sizeof(px) options:MTLResourceStorageModeShared];
	id<MTLBlitCommandEncoder> blit = [frameCommandBuffer() blitCommandEncoder];
	[blit copyFromBuffer:buf sourceOffset:0 sourceBytesPerRow:sizeof(px) sourceBytesPerImage:sizeof(px)
		sourceSize:MTLSizeMake(1, 1, 1) toTexture:t destinationSlice:0 destinationLevel:0
		destinationOrigin:MTLOriginMake(frame.deringNanX, frame.deringNanY, 0)];
	[blit endEncoding];
}

static void
encodeUpscale(void)
{
	bool inPlace = frame.deringInPlace && !frame.deringCopyForced &&
		getContext()->device.readWriteTextureSupport == MTLReadWriteTextureTier2;
	bool copy = !inPlace && frame.deringCopy && ensureUpscaled();
	frame.scaler.colorTexture = frame.graded;
	frame.scaler.outputTexture = copy ? frame.upscaled : frame.output;
	frame.scaler.inputContentWidth = frame.internalWidth;
	frame.scaler.inputContentHeight = frame.internalHeight;
	[frame.scaler encodeToCommandBuffer:frameCommandBuffer()];
	if(frame.deringNanX >= 0 && frame.deringNanX < frame.outputWidth && frame.deringNanY >= 0 &&
	   frame.deringNanY < frame.outputHeight)
		injectNan(frame.scaler.outputTexture);
	if(!inPlace && !copy)
		return;
	id<MTLComputeCommandEncoder> enc = [frameCommandBuffer() computeCommandEncoder];
	[enc setComputePipelineState:inPlace ? frame.deringInPlace : frame.deringCopy];
	[enc setTexture:frame.graded atIndex:0];
	if(inPlace)
		[enc setTexture:frame.output atIndex:1];
	else{
		[enc setTexture:frame.upscaled atIndex:1];
		[enc setTexture:frame.output atIndex:2];
	}
	dispatchOver(enc, frame.output);
	[enc endEncoding];
}

static bool
ensureAntiAlias(void)
{
	if(frame.aaSource)
		return true;
	frame.aaAllocs++;
	frame.aaSource = frame.aaFail ? nil : newTexture(MTLPixelFormatRGBA16Float, frame.internalWidth, frame.internalHeight,
		MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, 1);
	if(frame.aaSource)
		return true;
	frame.aaGaveUp = true;
	if(!frame.aaReported){
		frame.aaReported = true;
		frame.aaLogs++;
		fprintf(stderr, "rw::metal: FXAA texture for %dx%d could not be created; rendering without anti-aliasing\n",
			frame.internalWidth, frame.internalHeight);
	}
	return false;
}

static void
encodeAntiAlias(id<MTLTexture> dst)
{
	id<MTLComputeCommandEncoder> enc = [frameCommandBuffer() computeCommandEncoder];
	[enc setComputePipelineState:frame.fxaa];
	[enc setTexture:frame.aaSource atIndex:0];
	[enc setTexture:dst atIndex:1];
	[enc dispatchThreads:MTLSizeMake(frame.internalWidth, frame.internalHeight, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	[enc endEncoding];
}

static bool
ensureReactive(void)
{
	if(frame.reactive && frame.reactive.width == (NSUInteger)frame.internalWidth &&
	   frame.reactive.height == (NSUInteger)frame.internalHeight)
		return true;
	MTLTextureUsage u = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	if(@available(macOS 14.4, *))
		u |= frame.temporal.reactiveTextureUsage;
	frame.reactive = newTexture(MTLPixelFormatR8Unorm, frame.internalWidth, frame.internalHeight, u, 1);
	return frame.reactive != nil;
}

static id<MTLRenderPipelineState>
reactivePipelineFor(uint32 layout)
{
	if(layout == im2dVertexLayout)
		return frame.reactive2D;
	if(im2dUV2VertexLayout && layout == im2dUV2VertexLayout)
		return frame.reactive2DUV2;
	if(layout == im3dVertexLayout)
		return frame.reactive3D;
	return nil;
}

static void
encodeReactive(void)
{
	if(!ensureReactive())
		return;
	frame.reactivePass.colorAttachments[0].texture = frame.reactive;
	id<MTLRenderCommandEncoder> e = [frameCommandBuffer() renderCommandEncoderWithDescriptor:frame.reactivePass];
	frame.reactivePass.colorAttachments[0].texture = nil;
	for(const ReactiveDraw &r : frame.reactiveDraws){
		id<MTLRenderPipelineState> pipe = reactivePipelineFor(r.layout);
		if(pipe == nil || r.vertices == nil)
			continue;
		[e setRenderPipelineState:pipe];
		[e setViewport:r.viewport];
		[e setVertexBuffer:r.vertices offset:r.vertexOffset atIndex:BUFFER_VERTEX];
		[e setVertexBuffer:r.blocks[0] offset:r.blockOffsets[0] atIndex:BUFFER_SCENE];
		[e setVertexBuffer:r.blocks[1] offset:r.blockOffsets[1] atIndex:BUFFER_OBJECT];
		[e setVertexBuffer:r.blocks[2] offset:r.blockOffsets[2] atIndex:BUFFER_STATE];
		[e setFragmentTexture:r.texture atIndex:0];
		[e setFragmentSamplerState:r.sampler atIndex:0];
		if(r.indexed)
			[e drawIndexedPrimitives:(MTLPrimitiveType)r.primitive indexCount:r.count indexType:(MTLIndexType)r.indexType
				indexBuffer:r.indices indexBufferOffset:r.indexOffset];
		else
			[e drawPrimitives:(MTLPrimitiveType)r.primitive vertexStart:0 vertexCount:r.count];
	}
	[e endEncoding];
	frame.reactiveDraws.clear();
}

static bool
temporalResetNow(void)
{
	return frame.temporalReset || !frame.prepassValid || !frame.prevPrepassValid;
}

static void
temporalEncoded(bool reset)
{
	frame.temporalReset = false;
	frame.temporalResetLast = reset;
	frame.temporalHistory = !reset;
	frame.temporalHistoryShow = frame.shows;
}

static void
encodeTemporal(void)
{
	id<MTLFXTemporalScaler> t = frame.temporal;
	bool reset = temporalResetNow();
	t.colorTexture = frame.graded;
	t.depthTexture = getRasterTexture(frame.sceneDepth);
	t.motionTexture = frame.prepassMotion;
	if(@available(macOS 14.4, *))
		t.reactiveMaskTexture = frame.reactive;
	t.outputTexture = frame.output;
	t.inputContentWidth = frame.internalWidth;
	t.inputContentHeight = frame.internalHeight;
	TMJitter j = appliedJitter();
	t.jitterOffsetX = JITTER_SIGN*j.x;
	t.jitterOffsetY = JITTER_SIGN*j.y;
	t.motionVectorScaleX = frame.internalWidth;
	t.motionVectorScaleY = frame.internalHeight;
	t.depthReversed = NO;
	t.reset = reset;
	[t encodeToCommandBuffer:frameCommandBuffer()];
	temporalEncoded(reset);
}

static void
dropTaa(void)
{
	if(frame.taaHistory[0] == nil && frame.taaHistory[1] == nil)
		return;
	frame.taaHistory[0] = frame.taaHistory[1] = nil;
	frame.temporalHistory = false;
}

static bool
ensureTaa(void)
{
	if(frame.taaHistory[0] && frame.taaHistory[1])
		return true;
	frame.taaAllocs++;
	bool inputs = !frame.taaFail && ensurePrepassTargets() && ensureReactive();
	for(int i = 0; i < 2; i++)
		frame.taaHistory[i] = inputs ? newTexture(MTLPixelFormatRGBA16Float, frame.outputWidth, frame.outputHeight,
			MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, 1) : nil;
	if(frame.taaHistory[0] && frame.taaHistory[1]){
		frame.taaIndex = 0;
		frame.temporalReset = true;
		return true;
	}
	frame.taaHistory[0] = frame.taaHistory[1] = nil;
	frame.taaGaveUp = true;
	if(!frame.taaReported){
		frame.taaReported = true;
		frame.taaLogs++;
		fprintf(stderr, "rw::metal: TAA history for %dx%d could not be created; using FXAA\n", frame.outputWidth,
			frame.outputHeight);
	}
	return false;
}

static void
encodeTaa(void)
{
	bool reset = temporalResetNow();
	TMJitter j = appliedJitter();
	TaaFrame p = { (uint32)frame.outputWidth, (uint32)frame.outputHeight, reset ? 0u : 1u, 0u, j.x, j.y, { 0.0f, 0.0f } };
	id<MTLComputeCommandEncoder> enc = [frameCommandBuffer() computeCommandEncoder];
	[enc setComputePipelineState:frame.taa];
	[enc setTexture:frame.aaSource atIndex:0];
	[enc setTexture:frame.taaHistory[frame.taaIndex] atIndex:1];
	[enc setTexture:frame.prepassMotion atIndex:2];
	[enc setTexture:frame.reactive atIndex:3];
	[enc setTexture:frame.output atIndex:4];
	[enc setTexture:frame.taaHistory[frame.taaIndex ^ 1] atIndex:5];
	[enc setTexture:getRasterTexture(frame.sceneDepth) atIndex:6];
	[enc setBytes:&p length:sizeof(p) atIndex:0];
	dispatchOver(enc, frame.output);
	[enc endEncoding];
	frame.taaIndex ^= 1;
	temporalEncoded(reset);
}

static void
encodeGrade(const GradeParams *p)
{
	id<MTLTexture> dest = frame.graded ? frame.graded : frame.output;
	int32 method = activeAntiAlias();
	if(method == ANTIALIAS_TAA && !(ensureAntiAlias() && ensureTaa()))
		method = activeAntiAlias();
	if(method != ANTIALIAS_TAA)
		dropTaa();
	if(method != ANTIALIAS_FXAA && method != ANTIALIAS_TAA)
		frame.aaSource = nil;
	bool aa = method == ANTIALIAS_FXAA && ensureAntiAlias();
	bool taa = method == ANTIALIAS_TAA;
	float32 weight = p ? clampRange(p->bloomWeight, 0.0f, 1.0f, 0.0f) : 0.0f;
	float32 constants[MAXGRADECONSTANTS*4] = {};

	if(frame.bloomDownFirst == nil || frame.bloomDownPipe == nil || frame.bloomUpPipe == nil ||
	   (weight > 0.0f && !ensureBloomTargets()))
		weight = 0.0f;
	if(p && p->constants && p->numConstants > 0)
		memcpy(constants, p->constants, (p->numConstants > MAXGRADECONSTANTS ? MAXGRADECONSTANTS : p->numConstants)*4*sizeof(float32));
	if(weight > 0.0f)
		encodeBloom();
	frame.bloomWeight = weight;
	GradeFrame g = { weight, p && p->grade ? 1u : 0u, 1.0f/BLOOM_LEVELS, 0.0f };
	id<MTLComputeCommandEncoder> enc = [frameCommandBuffer() computeCommandEncoder];
	[enc setComputePipelineState:frame.grade];
	[enc setTexture:getRasterTexture(frame.sceneBase) atIndex:0];
	[enc setTexture:weight > 0.0f ? frame.bloomUp[0] : frame.black atIndex:2];
	[enc setTexture:aa || taa ? frame.aaSource : dest atIndex:3];
	[enc setBytes:&g length:sizeof(g) atIndex:0];
	[enc setBytes:constants length:sizeof(constants) atIndex:1];
	[enc dispatchThreads:MTLSizeMake(frame.internalWidth, frame.internalHeight, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	[enc endEncoding];
	if(aa)
		encodeAntiAlias(dest);
	if(taa){
		encodeReactive();
		encodeTaa();
	}else if(frame.temporal){
		if(@available(macOS 14.4, *))
			encodeReactive();
		encodeTemporal();
	}else if(frame.scaler)
		encodeUpscale();
}

void
beginScene(Camera *cam)
{
	frame.emissiveScale = 1.0f;
	relitSceneBegin();
	rtSceneBegin();
	if(getContext() == nil || cam == nil || cam->frameBuffer == nil)
		return;
	@autoreleasepool {
		if(frame.phase == PHASE_SCENE || frame.phase == PHASE_PREPASS)
			endScene(nil);
		frame.prepassOpen = false;
		frame.prepassDone = false;
		frame.rayWorkDone = false;
		frame.prepassPasses = 0;
		frame.prepassDrawsLast = 0;
		frame.prevPrepassValid = frame.prepassValid && frame.prepassValidShow + 1 == frame.shows;
		frame.prepassValid = false;
		frame.reactiveDraws.clear();
		frame.reactiveDepth = 0;
		Raster *main = cam->frameBuffer->parent;
		Raster *depth = cam->zBuffer ? cam->zBuffer->parent : nil;
		bool enhanced = frame.activeTier != TIER_CLASSIC && frame.available && main->type == Raster::CAMERA &&
			ensureSceneTargets(main, depth);
		bool history = frame.prevPrepassValid && frame.temporalHistory && frame.temporalHistoryShow + 1 == frame.shows;
		frame.jitterOn = enhanced && frame.activeTier == TIER_MODERN && temporalConsumer() && !frame.temporalReset && history;
		frame.jitter = frame.jitterOn ? tmJitter(frame.jitterIndex++) : TMJitter{ 0.0f, 0.0f };
		frame.jitterApplied = false;
		frame.phase = phaseOnBeginScene(frame.phase, enhanced);
	}
}

void
endScene(const GradeParams *params)
{
	if(frame.prepassOpen)
		endPrepass();
	if(getContext() == nil || frame.phase != PHASE_SCENE)
		return;
	@autoreleasepool {
		flushPasses();
		finishPrepassFrame(true);
		encodeGrade(params);
		if(frame.internalWidth > 0)
			snprintf(frame.lastTail, sizeof(frame.lastTail), "%s", frameStatsTail());
		frame.phase = phaseOnEndScene(frame.phase);
		PassTarget t = { frame.mainColor, frame.mainDepth };
		PassClear c = PassClear();
		c.flags = PASSCLEAR_COLOR;
		if(frame.mainDepth)
			c.flags |= PASSCLEAR_DEPTH | PASSCLEAR_STENCIL;
		c.depth = 1.0f;
		clearPassTarget(t, c);
	}
}

static bool
jitterSpan(Camera *cam, int32 *w, int32 *h)
{
	Raster *fb = cam->frameBuffer;
	if(!frame.jitterOn || (frame.phase != PHASE_SCENE && frame.phase != PHASE_PREPASS) || fb == nil ||
	   fb->parent != frame.mainColor)
		return false;
	*w = frame.internalWidth;
	*h = frame.internalHeight;
	if(fb != fb->parent){
		HostRect r = { fb->offsetX, fb->offsetY, fb->width, fb->height };
		r = scaleRect(r, frame.outputWidth, frame.outputHeight, frame.internalWidth, frame.internalHeight);
		*w = r.width;
		*h = r.height;
	}
	return *w > 0 && *h > 0;
}

void
frameJitterProjection(Camera *cam, float32 *proj)
{
	int32 w, h;
	if(!jitterSpan(cam, &w, &h))
		return;
	tmJitterProjection(proj, frame.jitter.x, frame.jitter.y, w, h);
	frame.jitterApplied = true;
}

bool
frameJitterIm2D(Camera *cam, float32 *nx, float32 *ny)
{
	int32 w, h;
	if(!frame.jitterApplied || !jitterSpan(cam, &w, &h))
		return false;
	tmJitterNdc(frame.jitter.x, frame.jitter.y, w, h, nx, ny);
	return true;
}

void
frameCameraCut(void)
{
	frame.temporalReset = true;
}

bool
frameModernTier(void)
{
	return frame.activeTier == TIER_MODERN;
}

bool
frameRelitActive(void)
{
	return frame.activeTier == TIER_MODERN && frame.phase == PHASE_SCENE;
}

id<MTLTexture>
frameShadowTexture(void)
{
	return frame.testShadow ? frame.testShadow : frame.white;
}

bool
frameShadowSource(void)
{
	return frame.testShadow != nil;
}

void
setShadowFactorForTest(float32 v)
{
	if(v < 0.0f || getContext() == nil){
		frame.testShadow = nil;
		return;
	}
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
		width:1 height:1 mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModeShared;
	frame.testShadow = [getContext()->device newTextureWithDescriptor:d];
	__fp16 h = (__fp16)v;
	uint16 bits, texel[4];
	memcpy(&bits, &h, sizeof(bits));
	for(int i = 0; i < 4; i++)
		texel[i] = bits;
	[frame.testShadow replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:texel bytesPerRow:sizeof(texel)];
	prewarmShadowTwins();
}

bool
frameTemporalActive(void)
{
	return (frame.phase == PHASE_SCENE || frame.phase == PHASE_PREPASS) && temporalConsumer();
}

bool
frameReactiveOpen(void)
{
	return frame.reactiveDepth > 0 && frame.phase == PHASE_SCENE && frame.activeTier == TIER_MODERN;
}

void
frameAddReactive(const ReactiveDraw &r)
{
	if(frame.reactiveDraws.size() < frame.reactiveDraws.capacity()){
		frame.reactiveDraws.push_back(r);
		return;
	}
	if(!frame.reactiveReported){
		frame.reactiveReported = true;
		fprintf(stderr, "rw::metal: more than %d reactive draws in a frame; the rest are left out of the reactive mask\n",
			REACTIVE_DRAWS);
	}
}

void
beginReactive(void)
{
	frame.reactiveDepth++;
}

void
endReactive(void)
{
	if(frame.reactiveDepth > 0)
		frame.reactiveDepth--;
}

bool32
readReactiveForTest(int32 x, int32 y, float32 *v)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex = frame.reactive;
	if(ctx == nil || tex == nil || x < 0 || y < 0 || x >= (int32)tex.width || y >= (int32)tex.height)
		return 0;
	@autoreleasepool {
		flushPasses();
		id<MTLBuffer> buf = [ctx->device newBufferWithLength:4 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = frameCommandBuffer();
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
			sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:0
			destinationBytesPerRow:1 destinationBytesPerImage:1];
		[blit endEncoding];
		if(!commitAndWait(cb))
			return 0;
		*v = ((const uint8*)buf.contents)[0]/255.0f;
	}
	return 1;
}

void
setTemporalFailForTest(bool32 fail)
{
	frame.temporalFail = fail != 0;
	frame.temporalReported = false;
	dropSceneTargets();
}

void
setPrepassTargetsFailForTest(bool32 fail)
{
	frame.prepassFail = fail != 0;
	frame.prepassReported = false;
	frame.temporalReported = false;
	frame.temporalNoTargets = false;
	dropSceneTargets();
}

uint32
temporalLogsForTest(void)
{
	return frame.temporalLogs;
}

void
setJitterIndexForTest(uint32 index)
{
	frame.jitterIndex = index;
}

void
setTaaFailForTest(bool32 fail)
{
	frame.taaFail = fail != 0;
}

void
taaCountsForTest(uint32 *allocs, uint32 *logs, uint32 *requests)
{
	*allocs = frame.taaAllocs;
	*logs = frame.taaLogs;
	*requests = frame.taaRequests;
}

bool32
taaHistoryForTest(void)
{
	return frame.taaHistory[0] != nil && frame.taaHistory[1] != nil;
}

const void*
reactivePassForTest(void)
{
	return (__bridge const void*)frame.reactivePass;
}

bool32
enhancedFrame(void)
{
	return frame.phase == PHASE_SCENE || frame.phase == PHASE_PREPASS || frame.phase == PHASE_POST;
}

void
setEmissiveScale(float32 s)
{
	frame.emissiveScale = clampRange(s, 1.0f, 64.0f, 1.0f);
}

float32
getEmissiveScale(void)
{
	return frame.emissiveScale;
}

float32
frameEmissiveScale(void)
{
	return frame.emissiveScale;
}

static FrameRoute
currentRoute(void)
{
	FrameRoute r = { frame.phase, frame.mainColor, frame.mainDepth, frame.sceneBase, frame.sceneDepth };
	return r;
}

PassTarget
frameRoute(const PassTarget &t)
{
	return routeTarget(currentRoute(), t);
}

void
frameNoteUpdate(const PassTarget &t)
{
	if(t.color && t.color == frame.mainColor)
		frame.phase = phaseOnMainUpdate(frame.phase);
}

uint32
frameColorFormat(Raster *color)
{
	return routeColorFormat(currentRoute(), color);
}

Raster*
frameSceneBase(void)
{
	return frame.sceneBase;
}

bool
frameSubViewport(Raster *fb, Raster *target, HostRect *r)
{
	if(target == nil || (target != frame.sceneBase && target != frame.prepassRaster) || fb->parent != frame.mainColor ||
	   fb == fb->parent)
		return false;
	HostRect s = { fb->offsetX, fb->offsetY, fb->width, fb->height };
	*r = scaleRect(s, frame.outputWidth, frame.outputHeight, frame.internalWidth, frame.internalHeight);
	return true;
}

void
frameScaleClear(PassClear *c)
{
	HostRect s = { c->x, c->y, c->w, c->h };
	s = scaleRect(s, frame.outputWidth, frame.outputHeight, frame.internalWidth, frame.internalHeight);
	c->x = s.x;
	c->y = s.y;
	c->w = s.width;
	c->h = s.height;
}

void
frameShow(void)
{
	frame.shows++;
	if(frame.prepassOpen)
		endPrepass();
	if(frame.phase == PHASE_SCENE){
		if(!frame.unendedReported){
			frame.unendedReported = true;
			fprintf(stderr, "rw::metal: a scene was shown without endScene; ending it with a passthrough grade\n");
		}
		endScene(nil);
	}
	if(frame.phase == PHASE_CLASSIC)
		frame.bloomWeight = 0.0f;
	frame.phase = phaseOnShow(frame.phase);
}

static void
configureLayer(CAMetalLayer *layer, bool edr, CGColorSpaceRef screenSpace)
{
	if(edr){
		CGColorSpaceRef lin = screenSpace ? CGColorSpaceCreateExtendedLinearized(screenSpace) : nil;
		if(lin == nil)
			lin = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
		layer.pixelFormat = MTLPixelFormatRGBA16Float;
		layer.wantsExtendedDynamicRangeContent = YES;
		layer.colorspace = lin;
		CGColorSpaceRelease(lin);
	}else{
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.wantsExtendedDynamicRangeContent = NO;
		layer.colorspace = nil;
	}
}

static bool
sameSpace(CGColorSpaceRef a, CGColorSpaceRef b)
{
	return a == b || (a && b && CFEqual(a, b));
}

void
frameUpdateOutput(CAMetalLayer *layer, uint32 frameCount)
{
	float32 current = 1.0f, potential = 1.0f;
	CGColorSpaceRef space = nil;
	bool onScreen = false;

	if(!frame.outputDirty && (frameCount & 63) != 0)
		return;
	frame.outputDirty = false;
	if(frame.edrOverride){
		if(!frame.testScreenLost){
			current = frame.testCurrent;
			potential = frame.testPotential;
			onScreen = true;
		}
	}else if(metalGlobals.host && metalGlobals.host->surfaceScreen){
		NSScreen *screen = (__bridge NSScreen*)metalGlobals.host->surfaceScreen();
		if(screen){
			current = screen.maximumExtendedDynamicRangeColorComponentValue;
			potential = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
			space = CGColorSpaceRetain(screen.colorSpace.CGColorSpace);
			onScreen = true;
		}
	}
	// A fullscreen toggle reports no screen for a moment; hold the last one.
	if(onScreen)
		frame.screenKnown = true;
	else if(frame.screenKnown){
		current = frame.headroom;
		potential = frame.potentialHeadroom;
		space = frame.screenSpace ? CGColorSpaceRetain(frame.screenSpace) : nil;
	}
	bool want = edrWanted(frame.activeTier, frame.settings.hdrOutput, potential) &&
		frame.compositeEdr != nil && frame.classicEdr != nil;
	frame.headroom = current;
	frame.potentialHeadroom = potential;
	frame.peak = want ? edrPeak(frame.settings.emitterPeakNits, current) : 1.0f;
	if(layer && (want != (frame.edr != 0) || (want && !sameSpace(space, frame.screenSpace)))){
		configureLayer(layer, want, space);
		frame.edrLogs++;
		fprintf(stderr, "rw::metal: EDR output %s, headroom %.2f of %.2f, emitter peak %.2f\n",
			want ? "on" : "off", current, potential, frame.peak);
	}
	if(frame.screenSpace)
		CGColorSpaceRelease(frame.screenSpace);
	frame.screenSpace = space;
	frame.edr = want;
}

void
displayChanged(void)
{
	frame.outputDirty = true;
}

void
setEdrForTest(float32 current, float32 potential, bool32 notify)
{
	frame.edrOverride = current > 0.0f;
	frame.testCurrent = current;
	frame.testPotential = potential;
	if(notify)
		frame.outputDirty = true;
}

void
setEdrScreenLostForTest(bool32 lost)
{
	frame.testScreenLost = lost != 0;
}

uint32
edrLogsForTest(void)
{
	return frame.edrLogs;
}

bool32
layerIsEdrForTest(void)
{
	MetalContext *ctx = getContext();
	CAMetalLayer *layer = ctx ? ctx->layer : nil;
	return layer && layer.pixelFormat == MTLPixelFormatRGBA16Float && layer.wantsExtendedDynamicRangeContent &&
		layer.colorspace != nil;
}

bool
frameEncodeComposite(Raster *raster, id<MTLTexture> target)
{
	float32 uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	float32 out[4] = { frame.peak, 0.0f, 0.0f, 0.0f };
	bool enhanced = raster && raster->parent == frame.mainColor && frame.output &&
		(frame.phase == PHASE_POST || frame.phase == PHASE_SHOWN);
	bool half = target.pixelFormat == MTLPixelFormatRGBA16Float;
	id<MTLRenderPipelineState> pipe = nil;
	id<MTLTexture> classic = nil;

	if(enhanced)
		pipe = half ? frame.compositeEdr : target.pixelFormat == MTLPixelFormatBGRA8Unorm ? frame.compositeBGRA : nil;
	else if(half){
		pipe = frame.classicEdr;
		if(raster && raster->parent->type == Raster::CAMERA)
			classic = getRasterTexture(raster->parent);
	}else
		return false;
	if(pipe == nil)
		return false;
	MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
	desc.colorAttachments[0].texture = target;
	desc.colorAttachments[0].loadAction = enhanced || classic ? MTLLoadActionDontCare : MTLLoadActionClear;
	desc.colorAttachments[0].storeAction = MTLStoreActionStore;
	desc.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
	id<MTLRenderCommandEncoder> enc = [frameCommandBuffer() renderCommandEncoderWithDescriptor:desc];
	if(enhanced || classic){
		[enc setRenderPipelineState:pipe];
		[enc setVertexBytes:uv length:sizeof(uv) atIndex:0];
		if(enhanced){
			[enc setFragmentTexture:frame.output atIndex:0];
			[enc setFragmentTexture:getRasterTexture(frame.mainColor) atIndex:1];
			[enc setFragmentBytes:out length:sizeof(out) atIndex:0];
		}else
			[enc setFragmentTexture:classic atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[enc endEncoding];
	return true;
}

int32
frameCopyRoute(Raster *src)
{
	if(src == nil || src->parent != frame.mainColor || frame.phase == PHASE_CLASSIC)
		return COPY_DIRECT;
	return COPY_RENDER;
}

bool
frameReadsComposite(Raster *src)
{
	return src && src->parent == frame.mainColor && frame.output && frame.phase == PHASE_SHOWN;
}

bool32
frameRenderCopy(Raster *src, id<MTLTexture> dst, int32 dx, int32 dy, int32 w, int32 h)
{
	bool scene = frame.phase == PHASE_SCENE;
	id<MTLRenderPipelineState> pipe = scene ? frame.copyScene : frame.copyPost;
	float32 uv[4];

	if(pipe == nil || frame.outputWidth <= 0 || frame.outputHeight <= 0)
		return 0;
	uv[0] = (float32)src->offsetX/frame.outputWidth;
	uv[1] = (float32)src->offsetY/frame.outputHeight;
	uv[2] = (float32)w/frame.outputWidth;
	uv[3] = (float32)h/frame.outputHeight;
	MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];
	desc.colorAttachments[0].texture = dst;
	desc.colorAttachments[0].loadAction = MTLLoadActionLoad;
	desc.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLRenderCommandEncoder> enc = [frameCommandBuffer() renderCommandEncoderWithDescriptor:desc];
	MTLViewport vp = { (double)dx, (double)dy, (double)w, (double)h, 0.0, 1.0 };
	[enc setViewport:vp];
	[enc setRenderPipelineState:pipe];
	[enc setVertexBytes:uv length:sizeof(uv) atIndex:0];
	if(scene)
		[enc setFragmentTexture:getRasterTexture(frame.sceneBase) atIndex:0];
	else{
		[enc setFragmentTexture:frame.output atIndex:0];
		[enc setFragmentTexture:getRasterTexture(frame.mainColor) atIndex:1];
	}
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
	return 1;
}

static bool32
readHalfPixel(id<MTLTexture> tex, int32 x, int32 y, float32 rgba[4])
{
	MetalContext *ctx = getContext();

	if(ctx == nil || tex == nil || (tex.pixelFormat != MTLPixelFormatRGBA16Float && tex.pixelFormat != MTLPixelFormatRG16Float) ||
	   x < 0 || y < 0 || x >= (int32)tex.width || y >= (int32)tex.height)
		return 0;
	int32 channels = tex.pixelFormat == MTLPixelFormatRG16Float ? 2 : 4;
	@autoreleasepool {
		flushPasses();
		id<MTLBuffer> buf = [ctx->device newBufferWithLength:8 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = frameCommandBuffer();
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
			sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:0
			destinationBytesPerRow:channels*2 destinationBytesPerImage:channels*2];
		[blit endEncoding];
		if(!commitAndWait(cb))
			return 0;
		const __fp16 *h = (const __fp16*)buf.contents;
		for(int i = 0; i < 4; i++)
			rgba[i] = i < channels ? (float32)h[i] : 0.0f;
	}
	return 1;
}

static bool32
readSnormPixel(id<MTLTexture> tex, int32 x, int32 y, float32 rgba[4])
{
	MetalContext *ctx = getContext();

	if(ctx == nil || tex == nil || tex.pixelFormat != MTLPixelFormatRG16Snorm || x < 0 || y < 0 ||
	   x >= (int32)tex.width || y >= (int32)tex.height)
		return 0;
	@autoreleasepool {
		flushPasses();
		id<MTLBuffer> buf = [ctx->device newBufferWithLength:4 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = frameCommandBuffer();
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
			sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:0
			destinationBytesPerRow:4 destinationBytesPerImage:4];
		[blit endEncoding];
		if(!commitAndWait(cb))
			return 0;
		const int16 *v = (const int16*)buf.contents;
		for(int i = 0; i < 4; i++)
			rgba[i] = i < 2 ? std::max(v[i]/32767.0f, -1.0f) : 0.0f;
	}
	return 1;
}

bool32
readFrameOutput(int32 x, int32 y, float32 rgba[4])
{
	if(x >= frame.outputWidth || y >= frame.outputHeight)
		return 0;
	return readHalfPixel(frame.output, x, y, rgba);
}

bool32
readScenePixel(int32 x, int32 y, float32 rgba[4])
{
	if(frame.sceneBase == nil)
		return 0;
	return readHalfPixel(getRasterTexture(frame.sceneBase), x, y, rgba);
}

static void
dropPrepassTargets(void)
{
	if(frame.prepassRaster){
		frame.prepassRaster->destroy();
		frame.prepassRaster = nil;
	}
	frame.prepassNormal = frame.prepassMotion = nil;
	frame.prepassNormalMS = frame.prepassMotionMS = nil;
}

static bool
ensurePrepassTargets(void)
{
	int32 w = frame.internalWidth, h = frame.internalHeight;
	if(frame.prepassRaster && frame.prepassRaster->width == w && frame.prepassRaster->height == h &&
	   GETMETALRASTEREXT(frame.prepassRaster)->numSamples == frame.samples)
		return true;
	dropPrepassTargets();
	MTLTextureUsage u = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	if(!frame.prepassFail){
		frame.prepassNormal = newTexture(MTLPixelFormatRG16Snorm, w, h, u, 1);
		frame.prepassMotion = newTexture(MTLPixelFormatRG16Float, w, h, u, 1);
		if(frame.samples > 1){
			MTLTextureUsage twin = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			frame.prepassNormalMS = newTexture(MTLPixelFormatRG16Snorm, w, h, twin, frame.samples);
			frame.prepassMotionMS = newTexture(MTLPixelFormatRG16Float, w, h, twin, frame.samples);
		}
		frame.prepassRaster = Raster::create(w, h, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	}
	if(frame.prepassNormal == nil || frame.prepassMotion == nil || frame.prepassRaster == nil ||
	   (frame.samples > 1 && (frame.prepassNormalMS == nil || frame.prepassMotionMS == nil))){
		dropPrepassTargets();
		if(!frame.prepassReported){
			frame.prepassReported = true;
			fprintf(stderr, "rw::metal: prepass targets for %dx%d could not be created; modern frames draw without the prepass\n", w, h);
		}
		return false;
	}
	GETMETALRASTEREXT(frame.prepassRaster)->numSamples = frame.samples;
	return true;
}

bool
frameIsPrepassTarget(Raster *r)
{
	return r && r == frame.prepassRaster;
}

void
framePrepassAttachments(MTLRenderPassDescriptor *desc, uint32 samples)
{
	bool ms = samples > 1;
	id<MTLTexture> tex[2] = { frame.prepassNormal, frame.prepassMotion };
	id<MTLTexture> twin[2] = { frame.prepassNormalMS, frame.prepassMotionMS };
	bool reopen = frame.prepassPasses > 0;
	for(int i = 0; i < 2; i++){
		desc.colorAttachments[i].texture = ms ? twin[i] : tex[i];
		desc.colorAttachments[i].loadAction = reopen ? MTLLoadActionLoad : MTLLoadActionClear;
		desc.colorAttachments[i].storeAction = MTLStoreActionStore;
		desc.colorAttachments[i].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	}
	frame.prepassPasses++;
}

bool32
beginPrepass(void)
{
	if(getContext() == nil || frame.phase != PHASE_SCENE || frame.activeTier != TIER_MODERN || frame.prepassOpen ||
	   frame.prepassDone || frame.sceneDepth == nil || !prepassShadersReady() || frame.farMotion == nil ||
	   frame.prepassResolve == nil || !ensurePrepassTargets())
		return 0;
	@autoreleasepool {
		frame.prepassReturn = currentPassTarget();
		frame.phase = PHASE_PREPASS;
		frame.prepassOpen = true;
		frame.prepassPasses = 0;
		bool continuous = frame.prepassShowKnown && frame.shows == frame.prepassShow + 1;
		frame.prepassShow = frame.shows;
		frame.prepassShowKnown = true;
		beginPrepassState(continuous);
		setPassDepthSplit(frame.prepassRaster);
		PassTarget t = { frame.prepassRaster, frame.sceneDepth };
		resumePassTarget(t);
	}
	return 1;
}

static void
frameRayWork(void)
{
	MetalContext *ctx = getContext();
	if(frame.rayWorkDone || frame.dropping || ctx == nil || ctx->encoder != nil || !rtFrameWorkEnabled())
		return;
	frame.rayWorkDone = true;
	rtEncodeFrame(frameCommandBuffer(), true);
}

void
endPrepass(void)
{
	if(!frame.prepassOpen)
		return;
	@autoreleasepool {
		frame.prepassOpen = false;
		frame.prepassDone = true;
		frame.phase = PHASE_SCENE;
		frame.prepassDrawsLast = endPrepassState();
		rtEndCasters();
		resumePassTarget(frame.prepassReturn);
		frameRayWork();
	}
}

void
framePrepassPassEnded(void)
{
	if(frame.dropping || frame.prepassMotion == nil || frame.sceneDepth == nil)
		return;
	bool ms = frame.prepassMotionMS != nil;
	struct { simd_float4x4 inv, vp, prev; simd_uint2 size; } p;
	const PrepassMatrices &m = prepassMatrices();
	simd_float4x4 uploaded;
	memcpy(&uploaded, &m.uploadedViewProj, 64);
	p.inv = simd_inverse(uploaded);
	memcpy(&p.vp, &m.viewProj, 64);
	memcpy(&p.prev, &m.prevViewProj, 64);
	p.size = simd_make_uint2((uint32)frame.prepassMotion.width, (uint32)frame.prepassMotion.height);
	id<MTLComputeCommandEncoder> enc = [frameCommandBuffer() computeCommandEncoder];
	[enc setComputePipelineState:ms ? frame.prepassResolve : frame.farMotion];
	[enc setTexture:getRasterTexture(frame.sceneDepth) atIndex:0];
	[enc setTexture:frame.prepassMotion atIndex:1];
	if(ms){
		[enc setTexture:frame.prepassNormalMS atIndex:2];
		[enc setTexture:frame.prepassMotionMS atIndex:3];
		[enc setTexture:frame.prepassNormal atIndex:4];
	}
	[enc setBytes:&p length:sizeof(p) atIndex:0];
	dispatchOver(enc, frame.prepassMotion);
	[enc endEncoding];
	if(prepassDrawsSoFar() > 0){
		frame.prepassValid = true;
		frame.prepassValidShow = frame.shows;
	}
	if(!frame.prepassOpen && frame.prepassDone)
		frameRayWork();
}

static bool32
readResolvedDepth(Raster *zb, int32 x, int32 y, float32 *depth)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex = zb ? getRasterTexture(zb) : nil;

	if(ctx == nil || tex == nil || tex.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 || x < 0 || y < 0 ||
	   x >= (int32)tex.width || y >= (int32)tex.height)
		return 0;
	@autoreleasepool {
		flushPasses();
		id<MTLBuffer> buf = [ctx->device newBufferWithLength:4 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = frameCommandBuffer();
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
			sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:0
			destinationBytesPerRow:4 destinationBytesPerImage:4 options:MTLBlitOptionDepthFromDepthStencil];
		[blit endEncoding];
		if(!commitAndWait(cb))
			return 0;
		memcpy(depth, buf.contents, 4);
	}
	return 1;
}

bool32
readPrepassForTest(int32 x, int32 y, float32 normal[3], float32 motion[2], float32 *depth)
{
	float32 n[4], m[4];
	if(frame.prepassNormal == nil || !readHalfPixel(frame.prepassMotion, x, y, m))
		return 0;
	if(!readSnormPixel(frame.prepassNormal, x, y, n))
		return 0;
	PPVec3 d = n[0] == 0.0f && n[1] == 0.0f ? PPVec3{ 0.0f, 0.0f, 0.0f } : ppOctDecode(n[0], n[1]);
	normal[0] = d.x;
	normal[1] = d.y;
	normal[2] = d.z;
	motion[0] = m[0];
	motion[1] = m[1];
	return readResolvedDepth(frame.sceneDepth, x, y, depth);
}

void
frameForget(Raster *raster)
{
	if(frame.dropping || raster == nil)
		return;
	if(raster == frame.mainColor || raster == frame.mainDepth)
		dropSceneTargets();
}

void
frameTerm(void)
{
	dropSceneTargets();
	frame.unendedReported = false;
	frame.targetsReported = false;
	frame.scalerReported = false;
	frame.upscaledReported = false;
	frame.aaReported = false;
	frame.taaReported = false;
	frame.temporalReported = false;
	frame.temporalNoTargets = false;
	frame.reactiveBuilt = false;
	frame.reactive2D = frame.reactive2DUV2 = frame.reactive3D = nil;
	frame.reactivePass = nil;
	frame.reactiveDraws.clear();
	frame.prepassValid = false;
	frame.edr = 0;
	frame.outputDirty = true;
	frame.screenKnown = false;
	if(frame.screenSpace)
		CGColorSpaceRelease(frame.screenSpace);
	frame.screenSpace = nil;
}

void
frameTierChanged(void)
{
	if(getContext() == nil)
		return;
	prewarmFormats(activeFormatMask());
	if(frame.activeTier == TIER_CLASSIC){
		if(frame.phase == PHASE_SCENE || frame.phase == PHASE_PREPASS)
			endScene(nil);
		dropSceneTargets();
	}
}

}
}
#endif
