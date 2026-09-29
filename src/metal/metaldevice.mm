#ifdef RW_METAL
#include "metalobjc.h"
#include "metalpass.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

#define PLUGIN_ID 0

namespace rw {
namespace metal {

MetalGlobals metalGlobals;
MetalCaps metalCaps;

static PassManager passManager;
static Raster *currentFrameBuffer;

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

static id<MTLCommandBuffer>
getCommandBuffer(MetalContext *ctx)
{
	if(ctx->commandBuffer == nil){
		if(!ctx->frameStarted){
			dispatch_semaphore_wait(ctx->frameSemaphore, DISPATCH_TIME_FOREVER);
			ctx->frameStarted = true;
		}
		ctx->commandBuffer = [ctx->queue commandBuffer];
	}
	return ctx->commandBuffer;
}

static void
finishFrame(MetalContext *ctx, id<CAMetalDrawable> drawable)
{
	id<MTLCommandBuffer> cb = ctx->commandBuffer;
	dispatch_semaphore_t sem = ctx->frameSemaphore;

	if(!ctx->frameStarted)
		return;
	if(cb){
		if(drawable)
			[cb presentDrawable:drawable];
		[cb addCompletedHandler:^(id<MTLCommandBuffer>){ dispatch_semaphore_signal(sem); }];
		[cb commit];
	}else
		dispatch_semaphore_signal(sem);
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
	[ctx->encoder setViewport:vp];
}

static void
beginPass(MetalContext *ctx, const PassAction *a)
{
	Raster *fb = (Raster*)a->target.color;
	id<MTLTexture> color = getRasterTexture(fb);
	id<MTLTexture> depth = getRasterTexture((Raster*)a->target.depth);
	MTLRenderPassDescriptor *desc;
	const PassClear *c = &a->clear;

	if(color == nil)
		return;
	if(depth && (depth.width != color.width || depth.height != color.height))
		depth = nil;

	desc = [MTLRenderPassDescriptor renderPassDescriptor];
	desc.colorAttachments[0].texture = color;
	desc.colorAttachments[0].loadAction = c->flags & PASSCLEAR_COLOR ? MTLLoadActionClear : MTLLoadActionLoad;
	desc.colorAttachments[0].storeAction = MTLStoreActionStore;
	desc.colorAttachments[0].clearColor = MTLClearColorMake(c->color[0], c->color[1], c->color[2], c->color[3]);
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
	ctx->encoderHasDepth = depth != nil;
	ctx->encoderWidth = (uint32)color.width;
	ctx->encoderHeight = (uint32)color.height;
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
	MTLScissorRect full = { 0, 0, ctx->encoderWidth, ctx->encoderHeight };
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
	[enc setScissorRect:full];
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

bool32
readRasterPixels(Raster *raster, uint8 *dst)
{
	MetalContext *ctx = getContext();
	id<MTLTexture> tex;
	id<MTLBuffer> buf;
	id<MTLCommandBuffer> cb;
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
		buf = [ctx->device newBufferWithLength:size options:MTLResourceStorageModeShared];
		cb = getCommandBuffer(ctx);
		blit = [cb blitCommandEncoder];
		[blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
			sourceOrigin:MTLOriginMake(raster->offsetX, raster->offsetY, 0)
			sourceSize:MTLSizeMake(raster->width, raster->height, 1)
			toBuffer:buf destinationOffset:0
			destinationBytesPerRow:stride destinationBytesPerImage:size];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		ctx->commandBuffer = nil;
		if(cb.status != MTLCommandBufferStatusCompleted)
			return 0;
		memcpy(dst, buf.contents, size);
	}
	return 1;
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

static int
stopGLFW(void)
{
	MetalContext *ctx = getContext();

	@autoreleasepool {
		if(ctx){
			passManager.flush();
			runPassActions();
			finishFrame(ctx, nil);
			waitForFrames(ctx);
			ctx->encoder = nil;
			ctx->commandBuffer = nil;
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
	openIm2D();
	openIm3D();
	return 1;
}

static int
termMetal(void)
{
	closeIm3D();
	closeIm2D();
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

	currentFrameBuffer = fb;
	@autoreleasepool {
		passManager.clear(getCameraTarget(cam), clear);
		runPassActions();
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
			if(drawable)
				composite(ctx, raster, drawable.texture);
		}
		finishFrame(ctx, drawable);
		drawable = nil;
	}
}

Device renderdevice = {
	-1.0f, 1.0f,
	metal::beginUpdate,
	metal::endUpdate,
	metal::clearCamera,
	metal::showRaster,
	null::rasterRenderFast,
	null::setRenderState,
	null::getRenderState,
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
