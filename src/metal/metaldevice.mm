#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#include "rwmetal.h"
#include "rwmetalplg.h"
#include "rwmetalimpl.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

#include "metalobjc.h"

#define PLUGIN_ID 0

namespace rw {
namespace metal {

MetalGlobals metalGlobals;
MetalCaps metalCaps;

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

	ctx = new MetalContext;
	ctx->device = device;
	ctx->queue = [device newCommandQueue];

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
			ctx->layer = nil;
			ctx->window = nil;
		}
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
		metalGlobals.numSamples = (uint32)n;
		return 1;
	default:
		assert(0 && "not implemented");
		return 0;
	}
	return 1;
}

Device renderdevice = {
	-1.0f, 1.0f,
	null::beginUpdate,
	null::endUpdate,
	null::clearCamera,
	null::showRaster,
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
