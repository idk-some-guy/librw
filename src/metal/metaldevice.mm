#ifdef RW_METAL
#include "metalobjc.h"
#include "metalpass.h"
#include "metalstate.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

#define PLUGIN_ID 0

namespace rw {
namespace metal {

MetalGlobals metalGlobals;
MetalCaps metalCaps;
RasterStats rasterStats;

static PassManager passManager;
static Raster *currentFrameBuffer;
static FrameStats frameStats;

static const char *frameShaderSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VSOut { float4 pos [[position]]; float2 uv; };\n"
"static VSOut fullScreen(uint vid, float z) {\n"
"	VSOut o;\n"
"	o.uv = float2((vid << 1) & 2, vid & 2);\n"
"	o.pos = float4(o.uv.x*2.0 - 1.0, 1.0 - o.uv.y*2.0, z, 1.0);\n"
"	return o;\n"
"}\n"
"vertex VSOut compositeVS(uint vid [[vertex_id]]) { return fullScreen(vid, 0.0); }\n"
"fragment float4 compositeFS(VSOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
"	constexpr sampler smp(filter::linear, address::clamp_to_edge);\n"
"	return float4(tex.sample(smp, in.uv).rgb, 1.0);\n"
"}\n"
"vertex VSOut clearVS(uint vid [[vertex_id]], constant float &depth [[buffer(0)]]) { return fullScreen(vid, depth); }\n"
"fragment float4 clearFS(constant float4 &color [[buffer(0)]]) { return color; }\n";

static id<MTLLibrary>
compileLibrary(id<MTLDevice> device, const char *src)
{
	NSError *err = nil;
	id<MTLLibrary> lib;

	lib = [device newLibraryWithSource:[NSString stringWithUTF8String:src] options:nil error:&err];
	if(lib == nil)
		RWERROR((ERR_GENERAL, err.localizedDescription.UTF8String));
	return lib;
}

static id<MTLRenderPipelineState>
makePipeline(id<MTLDevice> device, id<MTLLibrary> lib, NSString *vs, NSString *fs,
	MTLPixelFormat colorFormat, MTLPixelFormat depthFormat, bool writeColor)
{
	MTLRenderPipelineDescriptor *desc;
	NSError *err = nil;
	id<MTLRenderPipelineState> pipe;

	desc = [MTLRenderPipelineDescriptor new];
	desc.vertexFunction = [lib newFunctionWithName:vs];
	desc.fragmentFunction = [lib newFunctionWithName:fs];
	desc.colorAttachments[0].pixelFormat = colorFormat;
	desc.colorAttachments[0].writeMask = writeColor ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
	desc.depthAttachmentPixelFormat = depthFormat;
	desc.stencilAttachmentPixelFormat = depthFormat;
	pipe = [device newRenderPipelineStateWithDescriptor:desc error:&err];
	if(pipe == nil)
		RWERROR((ERR_GENERAL, err.localizedDescription.UTF8String));
	return pipe;
}

static id<MTLDepthStencilState>
makeClearDepthState(id<MTLDevice> device, bool writeDepth, bool writeStencil)
{
	MTLDepthStencilDescriptor *desc;
	MTLStencilDescriptor *stencil;

	desc = [MTLDepthStencilDescriptor new];
	desc.depthCompareFunction = MTLCompareFunctionAlways;
	desc.depthWriteEnabled = writeDepth;
	if(writeStencil){
		stencil = [MTLStencilDescriptor new];
		stencil.stencilCompareFunction = MTLCompareFunctionAlways;
		stencil.stencilFailureOperation = MTLStencilOperationReplace;
		stencil.depthFailureOperation = MTLStencilOperationReplace;
		stencil.depthStencilPassOperation = MTLStencilOperationReplace;
		stencil.readMask = 0xFF;
		stencil.writeMask = 0xFF;
		desc.frontFaceStencil = stencil;
		desc.backFaceStencil = stencil;
	}
	return [device newDepthStencilStateWithDescriptor:desc];
}

static int
createFramePipelines(MetalContext *ctx)
{
	id<MTLLibrary> lib;
	int c, d;

	lib = compileLibrary(ctx->device, frameShaderSrc);
	if(lib == nil)
		return 0;
	ctx->compositePipeline = makePipeline(ctx->device, lib, @"compositeVS", @"compositeFS",
		MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid, true);
	for(c = 0; c < 2; c++)
		for(d = 0; d < 2; d++){
			ctx->clearPipelines[c][d] = makePipeline(ctx->device, lib, @"clearVS", @"clearFS",
				MTLPixelFormatRGBA8Unorm,
				d ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid, c);
			ctx->clearDepthStates[c][d] = makeClearDepthState(ctx->device, c, d);
			if(ctx->clearPipelines[c][d] == nil)
				return 0;
		}
	return ctx->compositePipeline != nil;
}

static int
createContext(void)
{
	MetalContext *ctx;
	id<MTLDevice> device;

	device = MTLCreateSystemDefaultDevice();
	if(device == nil){
		RWERROR((ERR_GENERAL, "MTLCreateSystemDefaultDevice() failed"));
		return 0;
	}

	ctx = new MetalContext();
	ctx->device = device;
	ctx->queue = [device newCommandQueue];
	ctx->frameSemaphore = dispatch_semaphore_create(MAXFRAMESINFLIGHT);
	if(!createFramePipelines(ctx)){
		delete ctx;
		return 0;
	}

	metalCaps.bcSupported = device.supportsBCTextureCompression;
	metalCaps.maxAnisotropy = 16.0f;
	metalCaps.maxSamples = 1;
	for(uint32 n = 8; n > 1; n >>= 1)
		if([device supportsTextureSampleCount:n]){
			metalCaps.maxSamples = n;
			break;
		}

	metalGlobals.context = ctx;
	return 1;
}

void
startFrame(void)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || ctx->frameStarted)
		return;
	dispatch_semaphore_wait(ctx->frameSemaphore, DISPATCH_TIME_FOREVER);
	ctx->frameStarted = true;
	beginFrameState();
}

static id<MTLCommandBuffer>
getCommandBuffer(MetalContext *ctx)
{
	if(ctx->commandBuffer == nil){
		startFrame();
		ctx->commandBuffer = [ctx->queue commandBuffer];
	}
	return ctx->commandBuffer;
}

static void
finishFrame(MetalContext *ctx, id<CAMetalDrawable> drawable)
{
	id<MTLCommandBuffer> cb = ctx->commandBuffer;
	dispatch_semaphore_t sem = ctx->frameSemaphore;
	uint64 frameId = getFrameId();

	if(!ctx->frameStarted)
		return;
	if(cb){
		if(drawable){
			[cb presentDrawable:drawable];
			frameStats.framesPresented++;
		}
		[cb addCompletedHandler:^(id<MTLCommandBuffer> done){
			if(done.error)
				fprintf(stderr, "rw::metal: command buffer error: %s\n", done.error.localizedDescription.UTF8String);
			frameCompleted(frameId);
			dispatch_semaphore_signal(sem);
		}];
		[cb commit];
		ctx->lastCommitted = cb;
	}else{
		frameCompleted(frameId);
		dispatch_semaphore_signal(sem);
	}
	ctx->commandBuffer = nil;
	ctx->frameStarted = false;
}

static void
waitForFrames(MetalContext *ctx)
{
	int i;
	for(i = 0; i < MAXFRAMESINFLIGHT; i++)
		dispatch_semaphore_wait(ctx->frameSemaphore, DISPATCH_TIME_FOREVER);
	for(i = 0; i < MAXFRAMESINFLIGHT; i++)
		dispatch_semaphore_signal(ctx->frameSemaphore);
}

static void
setViewport(MetalContext *ctx, Raster *target)
{
	MTLViewport vp = { 0.0, 0.0, (double)ctx->encoderWidth, (double)ctx->encoderHeight, 0.0, 1.0 };
	Raster *fb = currentFrameBuffer;

	if(fb && fb != target && fb->parent == target){
		vp.originX = fb->offsetX;
		vp.originY = fb->offsetY;
		vp.width = fb->width;
		vp.height = fb->height;
	}
	setEncoderViewport(vp.originX, vp.originY, vp.width, vp.height);
}

static void
beginPass(MetalContext *ctx, const PassAction *a)
{
	Raster *fb = (Raster*)a->target.color;
	id<MTLTexture> color = getRasterTexture(fb);
	id<MTLTexture> depth = getRasterTexture((Raster*)a->target.depth);
	MTLRenderPassDescriptor *desc;
	const PassClear *c = &a->clear;

	if(color == nil && depth == nil)
		return;
	if(color && depth && (depth.width != color.width || depth.height != color.height))
		depth = nil;

	desc = [MTLRenderPassDescriptor renderPassDescriptor];
	if(color){
		desc.colorAttachments[0].texture = color;
		desc.colorAttachments[0].loadAction = c->flags & PASSCLEAR_COLOR ? MTLLoadActionClear : MTLLoadActionLoad;
		desc.colorAttachments[0].storeAction = MTLStoreActionStore;
		desc.colorAttachments[0].clearColor = MTLClearColorMake(c->color[0], c->color[1], c->color[2], c->color[3]);
	}
	if(depth){
		desc.depthAttachment.texture = depth;
		desc.depthAttachment.loadAction = c->flags & PASSCLEAR_DEPTH ? MTLLoadActionClear : MTLLoadActionLoad;
		desc.depthAttachment.storeAction = MTLStoreActionStore;
		desc.depthAttachment.clearDepth = c->depth;
		desc.stencilAttachment.texture = depth;
		desc.stencilAttachment.loadAction = c->flags & PASSCLEAR_STENCIL ? MTLLoadActionClear : MTLLoadActionLoad;
		desc.stencilAttachment.storeAction = MTLStoreActionStore;
		desc.stencilAttachment.clearStencil = c->stencil;
	}

	ctx->encoder = [getCommandBuffer(ctx) renderCommandEncoderWithDescriptor:desc];
	invalidateEncoderState();
	ctx->encoderHasDepth = depth != nil;
	ctx->encoderWidth = (uint32)(color ? color.width : depth.width);
	ctx->encoderHeight = (uint32)(color ? color.height : depth.height);
	if(fb)
		setViewport(ctx, fb);
}

static void
drawClearQuad(MetalContext *ctx, const PassAction *a)
{
	const PassClear *c = &a->clear;
	id<MTLRenderCommandEncoder> enc = ctx->encoder;
	int writeColor = (c->flags & PASSCLEAR_COLOR) != 0;
	int writeDepth = ctx->encoderHasDepth && (c->flags & PASSCLEAR_DEPTH);
	int writeStencil = ctx->encoderHasDepth && (c->flags & PASSCLEAR_STENCIL);
	int32 x0 = 0, y0 = 0, x1 = ctx->encoderWidth, y1 = ctx->encoderHeight;
	MTLViewport vp = { 0.0, 0.0, (double)ctx->encoderWidth, (double)ctx->encoderHeight, 0.0, 1.0 };
	MTLScissorRect r;
	float depth = c->depth;

	if(enc == nil || !(writeColor || writeDepth || writeStencil))
		return;
	if(c->subRect){
		x0 = MAX(c->x, 0);
		y0 = MAX(c->y, 0);
		x1 = MIN(c->x + c->w, x1);
		y1 = MIN(c->y + c->h, y1);
		if(x1 <= x0 || y1 <= y0)
			return;
	}
	r.x = x0;
	r.y = y0;
	r.width = x1 - x0;
	r.height = y1 - y0;

	[enc setViewport:vp];
	[enc setScissorRect:r];
	[enc setCullMode:MTLCullModeNone];
	[enc setRenderPipelineState:ctx->clearPipelines[writeColor][ctx->encoderHasDepth]];
	if(ctx->encoderHasDepth){
		[enc setDepthStencilState:ctx->clearDepthStates[writeDepth][writeStencil]];
		[enc setStencilReferenceValue:c->stencil];
	}
	[enc setVertexBytes:&depth length:sizeof(depth) atIndex:0];
	[enc setFragmentBytes:c->color length:sizeof(c->color) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	invalidateEncoderState();
	setViewport(ctx, (Raster*)a->target.color);
}

static void
runPassActions(void)
{
	MetalContext *ctx = getContext();
	int i;

	if(ctx == nil)
		return;
	for(i = 0; i < passManager.numActions; i++){
		const PassAction *a = &passManager.actions[i];
		switch(a->type){
		case PASSACTION_END:
			[ctx->encoder endEncoding];
			ctx->encoder = nil;
			break;
		case PASSACTION_BEGIN:
			beginPass(ctx, a);
			break;
		case PASSACTION_CLEARQUAD:
			drawClearQuad(ctx, a);
			break;
		}
	}
}

static PassTarget
getCameraTarget(Camera *cam)
{
	PassTarget t = { nil, nil };
	if(cam->frameBuffer)
		t.color = cam->frameBuffer->parent;
	if(cam->zBuffer)
		t.depth = cam->zBuffer->parent;
	return t;
}

void
forgetRasterTarget(Raster *raster)
{
	if(currentFrameBuffer && (currentFrameBuffer == raster || currentFrameBuffer->parent == raster))
		currentFrameBuffer = nil;
	passManager.forget(raster);
	runPassActions();
}

void
clearNewRasterTarget(Raster *raster)
{
	PassTarget t = { raster, nil };
	PassClear c = PassClear();

	c.flags = PASSCLEAR_COLOR;
	passManager.clearOffscreen(t, c);
	@autoreleasepool {
		runPassActions();
	}
}

FrameStats
getFrameStats(void)
{
	return frameStats;
}

RasterStats
getRasterStats(void)
{
	return rasterStats;
}

int32
getMaxFramesInFlight(void)
{
	return MAXFRAMESINFLIGHT;
}

void
holdFrameForTest(double seconds)
{
	MetalContext *ctx = getContext();
	if(ctx == nil)
		return;
	@autoreleasepool {
		passManager.flush();
		runPassActions();
		id<MTLSharedEvent> ev = [ctx->device newSharedEvent];
		[getCommandBuffer(ctx) encodeWaitForEvent:ev value:1];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds*NSEC_PER_SEC)),
			dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ ev.signaledValue = 1; });
	}
}

static bool32
copyTexturePixels(MetalContext *ctx, id<MTLTexture> tex, int32 x, int32 y, int32 w, int32 h, uint8 *dst)
{
	id<MTLBuffer> buf;
	id<MTLCommandBuffer> cb;
	id<MTLBlitCommandEncoder> blit;
	uint32 stride, size;

	stride = w*4;
	size = stride*h;
	buf = [ctx->device newBufferWithLength:size options:MTLResourceStorageModeShared];
	cb = getCommandBuffer(ctx);
	blit = [cb blitCommandEncoder];
	[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
		sourceOrigin:MTLOriginMake(x, y, 0)
		sourceSize:MTLSizeMake(w, h, 1)
		toBuffer:buf destinationOffset:0
		destinationBytesPerRow:stride destinationBytesPerImage:size];
	[blit endEncoding];
	[cb commit];
	ctx->lastCommitted = cb;
	[cb waitUntilCompleted];
	ctx->commandBuffer = nil;
	if(cb.error)
		fprintf(stderr, "rw::metal: command buffer error: %s\n", cb.error.localizedDescription.UTF8String);
	if(cb.status != MTLCommandBufferStatusCompleted)
		return 0;
	memcpy(dst, buf.contents, size);
	return 1;
}

bool32
readRasterPixels(Raster *raster, uint8 *dst)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex;

	if(ctx == nil)
		return 0;
	@autoreleasepool {
		tex = getRasterTexture(raster->parent);
		if(tex == nil || tex.pixelFormat != MTLPixelFormatRGBA8Unorm ||
		   raster->width <= 0 || raster->height <= 0 ||
		   raster->offsetX < 0 || raster->offsetY < 0 ||
		   raster->offsetX + raster->width > (int32)tex.width ||
		   raster->offsetY + raster->height > (int32)tex.height)
			return 0;

		passManager.flush();
		runPassActions();
		return copyTexturePixels(ctx, tex, raster->offsetX, raster->offsetY,
			raster->width, raster->height, dst);
	}
}

bool32
readDepthPixel(Raster *zbuffer, int32 x, int32 y, float32 *depth)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex;
	id<MTLBuffer> buf;
	id<MTLCommandBuffer> cb;
	id<MTLBlitCommandEncoder> blit;

	if(ctx == nil || zbuffer == nil)
		return 0;
	@autoreleasepool {
		tex = getRasterTexture(zbuffer->parent);
		if(tex == nil || tex.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 ||
		   x < 0 || y < 0 || x >= zbuffer->width || y >= zbuffer->height ||
		   zbuffer->offsetX + x >= (int32)tex.width || zbuffer->offsetY + y >= (int32)tex.height)
			return 0;

		passManager.flush();
		runPassActions();
		buf = [ctx->device newBufferWithLength:4 options:MTLResourceStorageModeShared];
		cb = getCommandBuffer(ctx);
		blit = [cb blitCommandEncoder];
		[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
			sourceOrigin:MTLOriginMake(zbuffer->offsetX + x, zbuffer->offsetY + y, 0)
			sourceSize:MTLSizeMake(1, 1, 1)
			toBuffer:buf destinationOffset:0
			destinationBytesPerRow:4 destinationBytesPerImage:4
			options:MTLBlitOptionDepthFromDepthStencil];
		[blit endEncoding];
		[cb commit];
		ctx->lastCommitted = cb;
		[cb waitUntilCompleted];
		ctx->commandBuffer = nil;
		if(cb.error)
			fprintf(stderr, "rw::metal: command buffer error: %s\n", cb.error.localizedDescription.UTF8String);
		if(cb.status != MTLCommandBufferStatusCompleted)
			return 0;
		memcpy(depth, buf.contents, 4);
	}
	return 1;
}

bool32
writeRasterPixels(Raster *raster, const uint8 *src)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex;
	id<MTLBuffer> buf;
	id<MTLBlitCommandEncoder> blit;
	uint32 stride, size;

	if(ctx == nil)
		return 0;
	@autoreleasepool {
		tex = getRasterTexture(raster->parent);
		if(tex == nil || tex.pixelFormat != MTLPixelFormatRGBA8Unorm ||
		   raster->width <= 0 || raster->height <= 0 ||
		   raster->offsetX < 0 || raster->offsetY < 0 ||
		   raster->offsetX + raster->width > (int32)tex.width ||
		   raster->offsetY + raster->height > (int32)tex.height)
			return 0;

		passManager.flush();
		runPassActions();
		stride = raster->width*4;
		size = stride*raster->height;
		buf = [ctx->device newBufferWithBytes:src length:size options:MTLResourceStorageModeShared];
		blit = [getCommandBuffer(ctx) blitCommandEncoder];
		[blit copyFromBuffer:buf sourceOffset:0
			sourceBytesPerRow:stride sourceBytesPerImage:size
			sourceSize:MTLSizeMake(raster->width, raster->height, 1)
			toTexture:tex destinationSlice:0 destinationLevel:0
			destinationOrigin:MTLOriginMake(raster->offsetX, raster->offsetY, 0)];
		[blit endEncoding];
	}
	return 1;
}

bool32
encodeTextureUpload(void *texture, int32 level, int32 width, int32 height,
	const uint8 *bytes, uint32 bytesPerRow, uint32 bytesPerImage)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
	id<MTLBuffer> buf;
	id<MTLBlitCommandEncoder> blit;

	if(ctx == nil || tex == nil)
		return 0;
	@autoreleasepool {
		buf = [ctx->device newBufferWithBytes:bytes length:bytesPerImage options:MTLResourceStorageModeShared];
		if(buf == nil)
			return 0;
		passManager.flush();
		runPassActions();
		blit = [getCommandBuffer(ctx) blitCommandEncoder];
		[blit copyFromBuffer:buf sourceOffset:0
			sourceBytesPerRow:bytesPerRow sourceBytesPerImage:bytesPerImage
			sourceSize:MTLSizeMake(width, height, 1)
			toTexture:tex destinationSlice:0 destinationLevel:level
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
	}
	return 1;
}

void
encodeMipmapGeneration(void *texture)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
	id<MTLBlitCommandEncoder> blit;

	if(ctx == nil || tex == nil || tex.mipmapLevelCount < 2)
		return;
	@autoreleasepool {
		passManager.flush();
		runPassActions();
		blit = [getCommandBuffer(ctx) blitCommandEncoder];
		[blit generateMipmapsForTexture:tex];
		[blit endEncoding];
	}
}

void
waitForGPUWrites(uint64 frameId)
{
	MetalContext *ctx = getContext();
	id<MTLCommandBuffer> cb;

	if(ctx == nil)
		return;
	@autoreleasepool {
		if(frameId == getFrameId() && ctx->commandBuffer){
			passManager.flush();
			runPassActions();
			cb = ctx->commandBuffer;
			[cb commit];
			ctx->lastCommitted = cb;
			ctx->commandBuffer = nil;
		}else
			cb = ctx->lastCommitted;
		[cb waitUntilCompleted];
		if(cb.error)
			fprintf(stderr, "rw::metal: command buffer error: %s\n", cb.error.localizedDescription.UTF8String);
	}
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	MetalContext *ctx = getContext();
	Raster *dst = Raster::getCurrentContext();
	MetalRaster *natras;
	id<MTLTexture> stex, dtex;
	id<MTLBlitCommandEncoder> blit;
	int32 dx, dy, w, h;

	if(ctx == nil || raster == nil || dst == nil || raster->type != Raster::CAMERA)
		return 0;
	if(dst->type != Raster::NORMAL && dst->type != Raster::TEXTURE && dst->type != Raster::CAMERATEXTURE)
		return 0;
	stex = getRasterTexture(raster->parent);
	dtex = getRasterTexture(dst->parent);
	if(stex == nil || dtex == nil || dtex.pixelFormat != MTLPixelFormatRGBA8Unorm)
		return 0;
	dx = dst->offsetX + x;
	dy = dst->offsetY + y;
	w = MIN(MIN(raster->width, dst->width - x), (int32)dtex.width - dx);
	h = MIN(MIN(raster->height, dst->height - y), (int32)dtex.height - dy);
	if(x < 0 || y < 0 || dx < 0 || dy < 0 || w <= 0 || h <= 0)
		return 0;
	@autoreleasepool {
		passManager.flush();
		runPassActions();
		blit = [getCommandBuffer(ctx) blitCommandEncoder];
		[blit copyFromTexture:stex sourceSlice:0 sourceLevel:0
			sourceOrigin:MTLOriginMake(raster->offsetX, raster->offsetY, 0)
			sourceSize:MTLSizeMake(w, h, 1)
			toTexture:dtex destinationSlice:0 destinationLevel:0
			destinationOrigin:MTLOriginMake(dx, dy, 0)];
		[blit endEncoding];
	}
	natras = GETMETALRASTEREXT(dst->parent);
	if(natras->autogenMipmap && dtex.mipmapLevelCount > 1){
		encodeMipmapGeneration(natras->texture);
		rasterStats.mipmapBlits++;
		natras->filledMask = (1u << dtex.mipmapLevelCount) - 1;
		natras->filledLevels = (int8)dtex.mipmapLevelCount;
	}else{
		natras->filledMask = 1;
		natras->filledLevels = 1;
	}
	natras->gpuWriteFrame = getFrameId();
	return 1;
}

void
resolveRasterTarget(Raster *raster)
{
	if(raster == nil)
		return;
	passManager.resolve(raster->parent);
	@autoreleasepool {
		runPassActions();
	}
}

bool32
rasterHasPendingWork(Raster *raster)
{
	const void *r;

	if(raster == nil)
		return 0;
	r = raster->parent;
	return (passManager.hasPending && (passManager.pendingTarget.color == r || passManager.pendingTarget.depth == r)) ||
		(passManager.isOpen() && (passManager.openTarget.color == r || passManager.openTarget.depth == r));
}

static bool
usesTarget(const PassTarget &t, Raster *raster)
{
	return raster && (t.color == raster->parent || t.depth == raster->parent);
}

bool32
beginDraw(void)
{
	static bool feedbackReported;
	MetalContext *ctx = getContext();
	Raster *raster;
	int32 i;

	if(ctx == nil)
		return 0;
	for(i = 0; i < MAXNUMSTAGES; i++){
		raster = getStageRaster(i);
		if(raster == nil || raster->platform != PLATFORM_METAL)
			continue;
		if(usesTarget(passManager.current, raster)){
			if(!feedbackReported){
				feedbackReported = true;
				RWERROR((ERR_GENERAL, "draw samples the raster it renders into"));
			}
			return 0;
		}
		if(rasterHasPendingWork(raster))
			resolveRasterTarget(raster);
	}
	@autoreleasepool {
		if(!passManager.draw())
			return 0;
		runPassActions();
	}
	return ctx->encoder != nil;
}

static void
addVideoMode(const GLFWvidmode *mode)
{
	int i;

	for(i = 1; i < metalGlobals.numModes; i++){
		if(metalGlobals.modes[i].mode.width == mode->width &&
		   metalGlobals.modes[i].mode.height == mode->height &&
		   metalGlobals.modes[i].mode.redBits == mode->redBits &&
		   metalGlobals.modes[i].mode.greenBits == mode->greenBits &&
		   metalGlobals.modes[i].mode.blueBits == mode->blueBits){
			if(mode->refreshRate > metalGlobals.modes[i].mode.refreshRate)
				metalGlobals.modes[i].mode.refreshRate = mode->refreshRate;
			return;
		}
	}

	metalGlobals.modes[metalGlobals.numModes].mode = *mode;
	metalGlobals.modes[metalGlobals.numModes].flags = VIDEOMODEEXCLUSIVE;
	metalGlobals.numModes++;
}

static void
makeVideoModeList(GLFWmonitor *monitor)
{
	int i, num;
	const GLFWvidmode *modes;

	modes = glfwGetVideoModes(monitor, &num);
	rwFree(metalGlobals.modes);
	metalGlobals.modes = rwNewT(DisplayMode, num+1, ID_DRIVER | MEMDUR_EVENT);

	metalGlobals.modes[0].mode = *glfwGetVideoMode(monitor);
	metalGlobals.modes[0].flags = 0;
	metalGlobals.numModes = 1;

	for(i = 0; i < num; i++)
		addVideoMode(&modes[i]);

	for(i = 0; i < metalGlobals.numModes; i++){
		num = metalGlobals.modes[i].mode.redBits +
			metalGlobals.modes[i].mode.greenBits +
			metalGlobals.modes[i].mode.blueBits;
		for(metalGlobals.modes[i].depth = 1; metalGlobals.modes[i].depth < num; metalGlobals.modes[i].depth <<= 1);
	}
}

static void
makeWindowedModeList(int width, int height)
{
	rwFree(metalGlobals.modes);
	metalGlobals.modes = rwNewT(DisplayMode, 1, ID_DRIVER | MEMDUR_EVENT);

	metalGlobals.modes[0].mode.width = width;
	metalGlobals.modes[0].mode.height = height;
	metalGlobals.modes[0].mode.redBits = 8;
	metalGlobals.modes[0].mode.greenBits = 8;
	metalGlobals.modes[0].mode.blueBits = 8;
	metalGlobals.modes[0].mode.refreshRate = GLFW_DONT_CARE;
	metalGlobals.modes[0].depth = 32;
	metalGlobals.modes[0].flags = 0;
	metalGlobals.numModes = 1;
	metalGlobals.currentMode = 0;
}

static int
openGLFW(EngineOpenParams *openparams)
{
	GLFWmonitor **monitors;

	metalGlobals.winWidth = openparams->width;
	metalGlobals.winHeight = openparams->height;
	metalGlobals.winTitle = openparams->windowtitle;
	metalGlobals.winHidden = openparams->hidden;
	metalGlobals.pWindow = openparams->window;

	@autoreleasepool {
		if(metalGlobals.context == nil && !createContext())
			return 0;

		glfwInitHint(GLFW_COCOA_MENUBAR, metalGlobals.winHidden ? GLFW_FALSE : GLFW_TRUE);
		if(!glfwInit()){
			RWERROR((ERR_GENERAL, "glfwInit() failed"));
			return 0;
		}

		monitors = glfwGetMonitors(&metalGlobals.numMonitors);
		if(metalGlobals.numMonitors == 0 && metalGlobals.winHidden){
			metalGlobals.monitor = nil;
			metalGlobals.currentMonitor = 0;
			makeWindowedModeList(metalGlobals.winWidth, metalGlobals.winHeight);
			return 1;
		}
		if(metalGlobals.numMonitors == 0){
			RWERROR((ERR_GENERAL, "no monitor found"));
			rwFree(metalGlobals.modes);
			metalGlobals.modes = nil;
			metalGlobals.numModes = 0;
			metalGlobals.currentMode = 0;
			glfwTerminate();
			return 0;
		}
		metalGlobals.monitor = monitors[0];

		makeVideoModeList(metalGlobals.monitor);
	}

	return 1;
}

static int
closeGLFW(void)
{
	@autoreleasepool {
		glfwTerminate();
	}
	return 1;
}

static void
glfwerr(int error, const char *desc)
{
	fprintf(stderr, "GLFW Error: %s\n", desc);
}

static int
startGLFW(void)
{
	MetalContext *ctx = getContext();
	GLFWwindow *win;
	DisplayMode *mode;
	NSWindow *nswin;
	NSView *view;
	CAMetalLayer *layer;
	int w, h;

	if(ctx == nil || metalGlobals.modes == nil)
		return 0;
	mode = &metalGlobals.modes[metalGlobals.currentMode];

	@autoreleasepool {
		glfwSetErrorCallback(glfwerr);
		glfwWindowHint(GLFW_RED_BITS, mode->mode.redBits);
		glfwWindowHint(GLFW_GREEN_BITS, mode->mode.greenBits);
		glfwWindowHint(GLFW_BLUE_BITS, mode->mode.blueBits);
		glfwWindowHint(GLFW_REFRESH_RATE, mode->mode.refreshRate);
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_VISIBLE, metalGlobals.winHidden ? GLFW_FALSE : GLFW_TRUE);
		glfwWindowHint(GLFW_FOCUSED, metalGlobals.winHidden ? GLFW_FALSE : GLFW_TRUE);

		if((mode->flags & VIDEOMODEEXCLUSIVE) && !metalGlobals.winHidden)
			win = glfwCreateWindow(mode->mode.width, mode->mode.height, metalGlobals.winTitle, metalGlobals.monitor, nil);
		else
			win = glfwCreateWindow(metalGlobals.winWidth, metalGlobals.winHeight, metalGlobals.winTitle, nil, nil);
		if(win == nil){
			RWERROR((ERR_GENERAL, "glfwCreateWindow() failed"));
			return 0;
		}

		nswin = glfwGetCocoaWindow(win);
		view = nswin.contentView;

		layer = [CAMetalLayer layer];
		layer.device = ctx->device;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.framebufferOnly = YES;
		layer.maximumDrawableCount = 3;
		layer.displaySyncEnabled = YES;
		layer.contentsScale = nswin.backingScaleFactor;
		glfwGetFramebufferSize(win, &w, &h);
		layer.drawableSize = CGSizeMake(w, h);

		view.layer = layer;
		view.wantsLayer = YES;

		ctx->window = win;
		ctx->layer = layer;
	}

	metalGlobals.window = win;
	*metalGlobals.pWindow = win;
	return 1;
}

static void
finishGPUWork(void)
{
	MetalContext *ctx = getContext();

	if(ctx == nil)
		return;
	@autoreleasepool {
		passManager.flush();
		runPassActions();
		finishFrame(ctx, nil);
		waitForFrames(ctx);
	}
}

static int
stopGLFW(void)
{
	MetalContext *ctx = getContext();

	finishGPUWork();
	@autoreleasepool {
		if(ctx){
			ctx->encoder = nil;
			ctx->commandBuffer = nil;
			ctx->lastCommitted = nil;
			ctx->layer = nil;
			ctx->window = nil;
		}
		currentFrameBuffer = nil;
		glfwDestroyWindow(metalGlobals.window);
	}
	metalGlobals.window = nil;
	return 1;
}

static int
initMetal(void)
{
	if(!initState())
		return 0;
	openIm3D();
	prewarmPipelines();
	return 1;
}

static int
termMetal(void)
{
	finishGPUWork();
	closeIm3D();
	termRaster();
	termState();
	return 1;
}

static int
finalizeMetal(void)
{
	return 1;
}

static int
deviceSystemGLFW(DeviceReq req, void *arg, int32 n)
{
	GLFWmonitor **monitors;
	VideoMode *rwmode;

	switch(req){
	case DEVICEOPEN:
		return openGLFW((EngineOpenParams*)arg);
	case DEVICECLOSE:
		return closeGLFW();

	case DEVICEINIT:
		return startGLFW() && initMetal();
	case DEVICETERM:
		return termMetal() && stopGLFW();

	case DEVICEFINALIZE:
		return finalizeMetal();


	case DEVICEGETNUMSUBSYSTEMS:
		return metalGlobals.numMonitors;

	case DEVICEGETCURRENTSUBSYSTEM:
		return metalGlobals.currentMonitor;

	case DEVICESETSUBSYSTEM:
		monitors = glfwGetMonitors(&metalGlobals.numMonitors);
		if(n >= metalGlobals.numMonitors)
			return 0;
		metalGlobals.currentMonitor = n;
		metalGlobals.monitor = monitors[metalGlobals.currentMonitor];
		return 1;

	case DEVICEGETSUBSSYSTEMINFO:
		monitors = glfwGetMonitors(&metalGlobals.numMonitors);
		if(n >= metalGlobals.numMonitors)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, glfwGetMonitorName(monitors[n]), sizeof(SubSystemInfo::name));
		return 1;


	case DEVICEGETNUMVIDEOMODES:
		return metalGlobals.numModes;

	case DEVICEGETCURRENTVIDEOMODE:
		return metalGlobals.currentMode;

	case DEVICESETVIDEOMODE:
		if(n >= metalGlobals.numModes)
			return 0;
		metalGlobals.currentMode = n;
		return 1;

	case DEVICEGETVIDEOMODEINFO:
		rwmode = (VideoMode*)arg;
		rwmode->width = metalGlobals.modes[n].mode.width;
		rwmode->height = metalGlobals.modes[n].mode.height;
		rwmode->depth = metalGlobals.modes[n].depth;
		rwmode->flags = metalGlobals.modes[n].flags;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		return metalCaps.maxSamples;
	case DEVICEGETMULTISAMPLINGLEVELS:
		if(metalGlobals.numSamples == 0)
			return 1;
		return metalGlobals.numSamples;
	case DEVICESETMULTISAMPLINGLEVELS:
		metalGlobals.numSamples = MIN((uint32)n, metalCaps.maxSamples);
		return 1;
	default:
		assert(0 && "not implemented");
		return 0;
	}
	return 1;
}

static void
beginUpdate(Camera *cam)
{
	float view[16], proj[16];
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	view[0]  = -inv.right.x;
	view[1]  =  inv.right.y;
	view[2]  =  inv.right.z;
	view[3]  =  0.0f;
	view[4]  = -inv.up.x;
	view[5]  =  inv.up.y;
	view[6]  =  inv.up.z;
	view[7]  =  0.0f;
	view[8]  =  -inv.at.x;
	view[9]  =   inv.at.y;
	view[10] =  inv.at.z;
	view[11] =  0.0f;
	view[12] = -inv.pos.x;
	view[13] =  inv.pos.y;
	view[14] =  inv.pos.z;
	view[15] =  1.0f;
	memcpy(&cam->devView, &view, sizeof(RawMatrix));
	setViewMatrix(view);

	float32 invwx = 1.0f/cam->viewWindow.x;
	float32 invwy = 1.0f/cam->viewWindow.y;
	float32 invz = 1.0f/(cam->farPlane-cam->nearPlane);

	proj[0] = invwx;
	proj[1] = 0.0f;
	proj[2] = 0.0f;
	proj[3] = 0.0f;

	proj[4] = 0.0f;
	proj[5] = invwy;
	proj[6] = 0.0f;
	proj[7] = 0.0f;

	proj[8] = cam->viewOffset.x*invwx;
	proj[9] = cam->viewOffset.y*invwy;
	proj[12] = -proj[8];
	proj[13] = -proj[9];
	if(cam->projection == Camera::PERSPECTIVE){
		proj[10] = (cam->farPlane+cam->nearPlane)*invz;
		proj[11] = 1.0f;

		proj[14] = -2.0f*cam->nearPlane*cam->farPlane*invz;
		proj[15] = 0.0f;
	}else{
		proj[10] = 2.0f*invz;
		proj[11] = 0.0f;

		proj[14] = -(cam->farPlane+cam->nearPlane)*invz;
		proj[15] = 1.0f;
	}
	memcpy(&cam->devProj, &proj, sizeof(RawMatrix));
	setProjectionMatrix(proj);
	setFogPlanes(cam->fogPlane, cam->farPlane);

	currentFrameBuffer = cam->frameBuffer;
	@autoreleasepool {
		passManager.beginUpdate(getCameraTarget(cam));
		runPassActions();
		MetalContext *ctx = getContext();
		if(ctx && ctx->encoder)
			setViewport(ctx, (Raster*)passManager.openTarget.color);
	}
}

static void
endUpdate(Camera *cam)
{
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	Raster *fb = cam->frameBuffer;
	Raster *prev;
	PassClear clear = PassClear();
	RGBAf colf;

	if(fb == nil)
		return;

	convColor(&colf, col);
	if(mode & Camera::CLEARIMAGE)
		clear.flags |= PASSCLEAR_COLOR;
	if(mode & Camera::CLEARZ)
		clear.flags |= PASSCLEAR_DEPTH;
	if(mode & Camera::CLEARSTENCIL)
		clear.flags |= PASSCLEAR_STENCIL;
	clear.color[0] = colf.red;
	clear.color[1] = colf.green;
	clear.color[2] = colf.blue;
	clear.color[3] = colf.alpha;
	clear.depth = 1.0f;
	clear.stencil = 0;
	if(fb != fb->parent){
		clear.subRect = true;
		clear.x = fb->offsetX;
		clear.y = fb->offsetY;
		clear.w = fb->width;
		clear.h = fb->height;
	}

	prev = currentFrameBuffer;
	currentFrameBuffer = fb;
	@autoreleasepool {
		passManager.clear(getCameraTarget(cam), clear);
		runPassActions();
		currentFrameBuffer = prev;
		MetalContext *ctx = getContext();
		if(ctx && ctx->encoder)
			setViewport(ctx, (Raster*)passManager.openTarget.color);
	}
}

static void
syncLayer(MetalContext *ctx)
{
	NSWindow *nswin = glfwGetCocoaWindow(ctx->window);
	CGSize size = ctx->layer.drawableSize;
	int w, h;

	if(nswin && ctx->layer.contentsScale != nswin.backingScaleFactor)
		ctx->layer.contentsScale = nswin.backingScaleFactor;
	glfwGetFramebufferSize(ctx->window, &w, &h);
	if(w > 0 && h > 0 && (size.width != w || size.height != h))
		ctx->layer.drawableSize = CGSizeMake(w, h);
}

static void
composite(MetalContext *ctx, Raster *raster, id<MTLTexture> target)
{
	id<MTLTexture> src = nil;
	MTLRenderPassDescriptor *desc;
	id<MTLRenderCommandEncoder> enc;

	if(raster && raster->parent->type == Raster::CAMERA && ctx->compositePipeline)
		src = getRasterTexture(raster->parent);

	desc = [MTLRenderPassDescriptor renderPassDescriptor];
	desc.colorAttachments[0].texture = target;
	desc.colorAttachments[0].loadAction = src ? MTLLoadActionDontCare : MTLLoadActionClear;
	desc.colorAttachments[0].storeAction = MTLStoreActionStore;
	desc.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
	enc = [getCommandBuffer(ctx) renderCommandEncoderWithDescriptor:desc];
	if(src){
		[enc setRenderPipelineState:ctx->compositePipeline];
		[enc setFragmentTexture:src atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[enc endEncoding];
}

bool32
compositeCameraPixels(Raster *raster, uint8 *dst)
{
	MetalContext *ctx = getContext();
	MTLTextureDescriptor *desc;
	id<MTLTexture> tex;

	if(ctx == nil || raster == nil || raster->parent->type != Raster::CAMERA)
		return 0;
	@autoreleasepool {
		desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			width:raster->parent->width height:raster->parent->height mipmapped:NO];
		desc.usage = MTLTextureUsageRenderTarget;
		desc.storageMode = MTLStorageModePrivate;
		tex = [ctx->device newTextureWithDescriptor:desc];
		if(tex == nil)
			return 0;
		passManager.flush();
		runPassActions();
		composite(ctx, raster, tex);
		return copyTexturePixels(ctx, tex, 0, 0, raster->parent->width, raster->parent->height, dst);
	}
}

static void
waitWithoutDrawable(MetalContext *ctx)
{
	NSWindow *nswin = ctx->window ? (NSWindow*)glfwGetCocoaWindow(ctx->window) : nil;
	NSScreen *screen = nswin.screen;
	NSInteger fps = screen && screen.maximumFramesPerSecond > 0 ? screen.maximumFramesPerSecond : 60;
	[NSThread sleepForTimeInterval:1.0/fps];
}

static void
showRaster(Raster *raster, uint32 flags)
{
	MetalContext *ctx = getContext();
	id<CAMetalDrawable> drawable = nil;

	if(ctx == nil)
		return;
	@autoreleasepool {
		passManager.show();
		runPassActions();
		if(ctx->layer && ctx->window){
			syncLayer(ctx);
			ctx->layer.displaySyncEnabled = (flags & Raster::FLIPWAITVSYNCH) != 0;
			if(ctx->layer.drawableSize.width > 0 && ctx->layer.drawableSize.height > 0)
				drawable = [ctx->layer nextDrawable];
			if(drawable){
				frameStats.drawablesAcquired++;
				composite(ctx, raster, drawable.texture);
			}
		}
		frameStats.framesShown++;
		finishFrame(ctx, drawable);
		if(drawable == nil)
			waitWithoutDrawable(ctx);
		drawable = nil;
	}
}

Device renderdevice = {
	-1.0f, 1.0f,
	metal::beginUpdate,
	metal::endUpdate,
	metal::clearCamera,
	metal::showRaster,
	metal::rasterRenderFast,
	metal::setRenderState,
	metal::getRenderState,
	metal::im2DRenderLine,
	metal::im2DRenderTriangle,
	metal::im2DRenderPrimitive,
	metal::im2DRenderIndexedPrimitive,
	metal::im3DTransform,
	metal::im3DRenderPrimitive,
	metal::im3DRenderIndexedPrimitive,
	metal::im3DEnd,
	metal::deviceSystemGLFW
};

}
}
#endif
