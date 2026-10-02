#ifdef RW_METAL
#include "metalobjc.h"
#include "metalframe.h"
#include "metalstate.h"
#include "metalkeys.h"
#include "rwmetalshader.h"
#include "metalbloom.h"
#include "metaloutput.h"
#include "metaldering.h"
#include <string>
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

struct FrameState
{
	RenderSettings settings = { TIER_CLASSIC, UPSCALE_NATIVE, 0, HDROUTPUT_AUTO, EMITTERPEAK_DEFAULT, SOFTNESS_DEFAULT };
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
	CGColorSpaceRef screenSpace = nil;
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

uint32
activeFormatMask(void)
{
	if(frame.activeTier == TIER_CLASSIC)
		return 1u << COLORFMT_RGBA8;
	return 1u << COLORFMT_RGBA8 | 1u << COLORFMT_SCENE | 1u << COLORFMT_OVERLAY;
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
frameLibrary(const char **parts, NSString **error)
{
	NSMutableString *all = [NSMutableString string];
	NSError *err = nil;
	for(int i = 0; parts[i]; i++)
		[all appendString:[NSString stringWithUTF8String:parts[i]]];
	id<MTLLibrary> lib = [getContext()->device newLibraryWithSource:all options:nil error:&err];
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
	id<MTLLibrary> lib = frameLibrary(parts, error);
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
		id<MTLLibrary> bloomLib = frameLibrary(bloomParts, &err);
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
		id<MTLLibrary> deringLib = frameLibrary(deringParts, &err);
		if(deringLib){
			NSError *perr = nil;
			id<MTLDevice> dev = getContext()->device;
			frame.deringInPlace = [dev newComputePipelineStateWithFunction:[deringLib newFunctionWithName:@"deringInPlace"] error:&perr];
			frame.deringCopy = [dev newComputePipelineStateWithFunction:[deringLib newFunctionWithName:@"deringCopy"] error:&perr];
		}else{
			frame.deringInPlace = frame.deringCopy = nil;
			fprintf(stderr, "rw::metal: dering shaders failed to compile: %s\n", err ? err.UTF8String : "");
		}
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
			width:1 height:1 mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModeShared;
		frame.black = [getContext()->device newTextureWithDescriptor:d];
		[frame.black replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:zero bytesPerRow:sizeof(zero)];
	}
	frame.available = frame.compositeBGRA && frame.copyScene && frame.copyPost && frame.grade && frame.black;
	return frame.available;
}

static void
dropSceneTargets(void)
{
	frame.dropping = true;
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
	frame.scaler = nil;
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

static bool
ensureSceneTargets(Raster *main, Raster *depth)
{
	int32 ow = main->width, oh = main->height;
	int32 factor = frame.settings.upscale;
	int32 iw = internalSpan(ow, factor), ih = internalSpan(oh, factor);
	uint32 samples = metalGlobals.numSamples > 1 ? metalGlobals.numSamples : 1;
	MTLTextureUsage outputUsage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	MTLTextureUsage gradedUsage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

	if(frame.sceneBase && frame.mainColor == main && frame.mainDepth == depth &&
	   frame.outputWidth == ow && frame.outputHeight == oh && frame.factor == factor && frame.samples == samples)
		return true;
	dropSceneTargets();
	if(iw != ow || ih != oh){
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
	frame.sceneBase = createSceneRaster(iw, ih);
	frame.sceneDepth = depth ? Raster::create(iw, ih, 0, Raster::ZBUFFER) : nil;
	frame.output = newTexture(MTLPixelFormatRGBA16Float, ow, oh, outputUsage, 1);
	frame.graded = frame.scaler ? newTexture(MTLPixelFormatRGBA16Float, iw, ih, gradedUsage, 1) : nil;
	if(frame.sceneBase == nil || (depth && frame.sceneDepth == nil) || frame.output == nil ||
	   (frame.scaler && frame.graded == nil)){
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
	frame.upscaler = frame.scaler ? UPSCALER_SPATIAL : UPSCALER_NONE;
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
	if(frame.upscaled == nil)
		frame.upscaled = newTexture(MTLPixelFormatRGBA16Float, frame.outputWidth, frame.outputHeight,
			MTLTextureUsageShaderRead | frame.scaler.outputTextureUsage, 1);
	return frame.upscaled != nil;
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

static void
encodeGrade(const GradeParams *p)
{
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
	[enc setTexture:frame.graded ? frame.graded : frame.output atIndex:3];
	[enc setBytes:&g length:sizeof(g) atIndex:0];
	[enc setBytes:constants length:sizeof(constants) atIndex:1];
	[enc dispatchThreads:MTLSizeMake(frame.internalWidth, frame.internalHeight, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	[enc endEncoding];
	if(frame.scaler)
		encodeUpscale();
}

void
beginScene(Camera *cam)
{
	frame.emissiveScale = 1.0f;
	if(getContext() == nil || cam == nil || cam->frameBuffer == nil)
		return;
	@autoreleasepool {
		if(frame.phase == PHASE_SCENE)
			endScene(nil);
		Raster *main = cam->frameBuffer->parent;
		Raster *depth = cam->zBuffer ? cam->zBuffer->parent : nil;
		bool enhanced = frame.activeTier != TIER_CLASSIC && frame.available && main->type == Raster::CAMERA &&
			ensureSceneTargets(main, depth);
		frame.phase = phaseOnBeginScene(frame.phase, enhanced);
	}
}

void
endScene(const GradeParams *params)
{
	if(getContext() == nil || frame.phase != PHASE_SCENE)
		return;
	@autoreleasepool {
		flushPasses();
		encodeGrade(params);
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

bool32
enhancedFrame(void)
{
	return frame.phase == PHASE_SCENE || frame.phase == PHASE_POST;
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
	if(target == nil || target != frame.sceneBase || fb->parent != frame.mainColor || fb == fb->parent)
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

	if(!frame.outputDirty && (frameCount & 63) != 0)
		return;
	frame.outputDirty = false;
	if(frame.edrOverride){
		current = frame.testCurrent;
		potential = frame.testPotential;
	}else if(metalGlobals.host && metalGlobals.host->surfaceScreen){
		NSScreen *screen = (__bridge NSScreen*)metalGlobals.host->surfaceScreen();
		if(screen){
			current = screen.maximumExtendedDynamicRangeColorComponentValue;
			potential = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
			space = CGColorSpaceRetain(screen.colorSpace.CGColorSpace);
		}
	}
	bool want = edrWanted(frame.activeTier, frame.settings.hdrOutput, potential) &&
		frame.compositeEdr != nil && frame.classicEdr != nil;
	frame.headroom = current;
	frame.potentialHeadroom = potential;
	frame.peak = want ? edrPeak(frame.settings.emitterPeakNits, current) : 1.0f;
	if(layer && (want != (frame.edr != 0) || (want && !sameSpace(space, frame.screenSpace)))){
		configureLayer(layer, want, space);
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

	if(ctx == nil || tex == nil || tex.pixelFormat != MTLPixelFormatRGBA16Float || x < 0 || y < 0 ||
	   x >= (int32)tex.width || y >= (int32)tex.height)
		return 0;
	@autoreleasepool {
		flushPasses();
		id<MTLBuffer> buf = [ctx->device newBufferWithLength:8 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = frameCommandBuffer();
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
			sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:0
			destinationBytesPerRow:8 destinationBytesPerImage:8];
		[blit endEncoding];
		if(!commitAndWait(cb))
			return 0;
		const __fp16 *h = (const __fp16*)buf.contents;
		for(int i = 0; i < 4; i++)
			rgba[i] = h[i];
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
	frame.edr = 0;
	frame.outputDirty = true;
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
		if(frame.phase == PHASE_SCENE)
			endScene(nil);
		dropSceneTargets();
	}
}

}
}
#endif
