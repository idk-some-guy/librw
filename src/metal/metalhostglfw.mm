#ifdef RW_METAL
#ifdef LIBRW_GLFW
#include "metalobjc.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

#define PLUGIN_ID 0

namespace rw {
namespace metal {

struct GlfwGlobals
{
	GLFWwindow **pWindow;
	GLFWwindow *window;
	int numMonitors;

	GLFWvidmode *vidModes;
	MetalHostMode *modes;
	int numModes;
	int modeDisplay;

	int winWidth, winHeight;
	const char *winTitle;
	bool winHidden;
	bool createdHidden;

	int32 lastWidth, lastHeight;
	float32 lastScale;
};

static GlfwGlobals glfwGlobals;
static CAMetalLayer *surfaceLayer;

static GLFWmonitor*
monitorAt(int32 display)
{
	GLFWmonitor **monitors = glfwGetMonitors(&glfwGlobals.numMonitors);
	if(display < 0 || display >= glfwGlobals.numMonitors)
		return nil;
	return monitors[display];
}

static int32
modeDepth(const GLFWvidmode *mode)
{
	int32 bits = mode->redBits + mode->greenBits + mode->blueBits;
	int32 depth;
	for(depth = 1; depth < bits; depth <<= 1);
	return depth;
}

static void
describeMode(MetalHostMode *desc, const GLFWvidmode *mode)
{
	desc->width = mode->width;
	desc->height = mode->height;
	desc->depth = modeDepth(mode);
	desc->refresh = mode->refreshRate;
}

static void
freeModes(void)
{
	rwFree(glfwGlobals.vidModes);
	rwFree(glfwGlobals.modes);
	glfwGlobals.vidModes = nil;
	glfwGlobals.modes = nil;
	glfwGlobals.numModes = 0;
}

static void
addVideoMode(const GLFWvidmode *mode)
{
	int i;

	for(i = 1; i < glfwGlobals.numModes; i++){
		if(glfwGlobals.vidModes[i].width == mode->width &&
		   glfwGlobals.vidModes[i].height == mode->height &&
		   glfwGlobals.vidModes[i].redBits == mode->redBits &&
		   glfwGlobals.vidModes[i].greenBits == mode->greenBits &&
		   glfwGlobals.vidModes[i].blueBits == mode->blueBits){
			if(mode->refreshRate > glfwGlobals.vidModes[i].refreshRate)
				glfwGlobals.vidModes[i].refreshRate = mode->refreshRate;
			return;
		}
	}

	glfwGlobals.vidModes[glfwGlobals.numModes] = *mode;
	glfwGlobals.modes[glfwGlobals.numModes].flags = VIDEOMODEEXCLUSIVE;
	glfwGlobals.numModes++;
}

static void
makeVideoModeList(GLFWmonitor *monitor)
{
	int i, num;
	const GLFWvidmode *modes;

	modes = glfwGetVideoModes(monitor, &num);
	freeModes();
	glfwGlobals.vidModes = rwNewT(GLFWvidmode, num+1, ID_DRIVER | MEMDUR_EVENT);
	glfwGlobals.modes = rwNewT(MetalHostMode, num+1, ID_DRIVER | MEMDUR_EVENT);

	glfwGlobals.vidModes[0] = *glfwGetVideoMode(monitor);
	glfwGlobals.modes[0].flags = 0;
	glfwGlobals.numModes = 1;

	for(i = 0; i < num; i++)
		addVideoMode(&modes[i]);

	for(i = 0; i < glfwGlobals.numModes; i++)
		describeMode(&glfwGlobals.modes[i], &glfwGlobals.vidModes[i]);
}

static void
makeWindowedModeList(int width, int height)
{
	freeModes();
	glfwGlobals.vidModes = rwNewT(GLFWvidmode, 1, ID_DRIVER | MEMDUR_EVENT);
	glfwGlobals.modes = rwNewT(MetalHostMode, 1, ID_DRIVER | MEMDUR_EVENT);

	glfwGlobals.vidModes[0].width = width;
	glfwGlobals.vidModes[0].height = height;
	glfwGlobals.vidModes[0].redBits = 8;
	glfwGlobals.vidModes[0].greenBits = 8;
	glfwGlobals.vidModes[0].blueBits = 8;
	glfwGlobals.vidModes[0].refreshRate = GLFW_DONT_CARE;
	glfwGlobals.modes[0].flags = 0;
	glfwGlobals.numModes = 1;
	describeMode(&glfwGlobals.modes[0], &glfwGlobals.vidModes[0]);
}

static bool32
openHost(EngineOpenParams *openparams)
{
	glfwGlobals.winWidth = openparams->width;
	glfwGlobals.winHeight = openparams->height;
	glfwGlobals.winTitle = openparams->windowtitle;
	glfwGlobals.winHidden = openparams->hidden;
	glfwGlobals.pWindow = openparams->window;

	glfwInitHint(GLFW_COCOA_MENUBAR, glfwGlobals.winHidden ? GLFW_FALSE : GLFW_TRUE);
	if(!glfwInit()){
		RWERROR((ERR_GENERAL, "glfwInit() failed"));
		return 0;
	}

	glfwGetMonitors(&glfwGlobals.numMonitors);
	if(glfwGlobals.numMonitors == 0 && glfwGlobals.winHidden){
		makeWindowedModeList(glfwGlobals.winWidth, glfwGlobals.winHeight);
		glfwGlobals.modeDisplay = 0;
		return 1;
	}
	if(glfwGlobals.numMonitors == 0){
		RWERROR((ERR_GENERAL, "no monitor found"));
		freeModes();
		glfwTerminate();
		return 0;
	}
	makeVideoModeList(monitorAt(0));
	glfwGlobals.modeDisplay = 0;
	return 1;
}

static void
closeHost(void)
{
	glfwTerminate();
}

static int32
numDisplays(void)
{
	glfwGetMonitors(&glfwGlobals.numMonitors);
	return glfwGlobals.numMonitors;
}

static const char*
displayName(int32 display)
{
	GLFWmonitor *monitor = monitorAt(display);
	return monitor ? glfwGetMonitorName(monitor) : "";
}

static bool32
displayMode(int32 display, MetalHostMode *mode)
{
	GLFWmonitor *monitor = monitorAt(display);
	if(monitor == nil)
		return 0;
	describeMode(mode, glfwGetVideoMode(monitor));
	mode->flags = 0;
	return 1;
}

static const MetalHostMode*
getModes(int32 display, int32 *numModes)
{
	GLFWmonitor *monitor;

	if(display != glfwGlobals.modeDisplay && (monitor = monitorAt(display)) != nil){
		makeVideoModeList(monitor);
		glfwGlobals.modeDisplay = display;
	}
	*numModes = glfwGlobals.numModes;
	return glfwGlobals.modes;
}

static void
glfwerr(int error, const char *desc)
{
	fprintf(stderr, "GLFW Error: %s\n", desc);
}

static void*
createSurface(int32 display, int32 mode, bool32 windowed, bool32 hidden)
{
	GLFWvidmode *vm;
	GLFWwindow *win;
	NSWindow *nswin;
	NSView *view;
	CAMetalLayer *layer;
	int w, h;

	if(glfwGlobals.vidModes == nil || mode < 0 || mode >= glfwGlobals.numModes)
		return nil;
	vm = &glfwGlobals.vidModes[mode];

	glfwSetErrorCallback(glfwerr);
	glfwWindowHint(GLFW_RED_BITS, vm->redBits);
	glfwWindowHint(GLFW_GREEN_BITS, vm->greenBits);
	glfwWindowHint(GLFW_BLUE_BITS, vm->blueBits);
	glfwWindowHint(GLFW_REFRESH_RATE, vm->refreshRate);
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, hidden ? GLFW_FALSE : GLFW_TRUE);
	glfwWindowHint(GLFW_FOCUSED, hidden ? GLFW_FALSE : GLFW_TRUE);

	if(!windowed && !hidden)
		win = glfwCreateWindow(vm->width, vm->height, glfwGlobals.winTitle, monitorAt(display), nil);
	else
		win = glfwCreateWindow(glfwGlobals.winWidth, glfwGlobals.winHeight, glfwGlobals.winTitle, nil, nil);
	if(win == nil){
		RWERROR((ERR_GENERAL, "glfwCreateWindow() failed"));
		return nil;
	}

	nswin = glfwGetCocoaWindow(win);
	view = nswin.contentView;

	layer = [CAMetalLayer layer];
	layer.contentsScale = nswin.backingScaleFactor;
	glfwGetFramebufferSize(win, &w, &h);
	layer.drawableSize = CGSizeMake(w, h);

	view.layer = layer;
	view.wantsLayer = YES;

	surfaceLayer = layer;
	glfwGlobals.window = win;
	glfwGlobals.createdHidden = hidden;
	glfwGlobals.lastWidth = w;
	glfwGlobals.lastHeight = h;
	glfwGlobals.lastScale = nswin.backingScaleFactor;
	if(glfwGlobals.pWindow)
		*glfwGlobals.pWindow = win;
	return (__bridge void*)layer;
}

static void
destroySurface(void)
{
	surfaceLayer = nil;
	glfwDestroyWindow(glfwGlobals.window);
	glfwGlobals.window = nil;
}

static void
drawableSize(int32 *width, int32 *height)
{
	int w = 0, h = 0;
	if(glfwGlobals.window)
		glfwGetFramebufferSize(glfwGlobals.window, &w, &h);
	*width = w;
	*height = h;
}

static float32
backingScale(void)
{
	NSWindow *nswin = glfwGlobals.window ? (NSWindow*)glfwGetCocoaWindow(glfwGlobals.window) : nil;
	return nswin ? nswin.backingScaleFactor : 0.0f;
}

static int32
refreshRate(void)
{
	NSWindow *nswin = glfwGlobals.window ? (NSWindow*)glfwGetCocoaWindow(glfwGlobals.window) : nil;
	NSScreen *screen = nswin.screen;
	return screen ? (int32)screen.maximumFramesPerSecond : 0;
}

// a window created hidden on request still renders and presents
static bool32
visible(void)
{
	if(glfwGlobals.window == nil)
		return 0;
	if(glfwGetWindowAttrib(glfwGlobals.window, GLFW_ICONIFIED))
		return 0;
	return glfwGlobals.createdHidden || glfwGetWindowAttrib(glfwGlobals.window, GLFW_VISIBLE);
}

static bool32
pollSizeChange(void)
{
	int32 w, h;
	float32 scale;

	if(glfwGlobals.window == nil)
		return 0;
	drawableSize(&w, &h);
	scale = backingScale();
	if(w == glfwGlobals.lastWidth && h == glfwGlobals.lastHeight && scale == glfwGlobals.lastScale)
		return 0;
	glfwGlobals.lastWidth = w;
	glfwGlobals.lastHeight = h;
	glfwGlobals.lastScale = scale;
	return 1;
}

MetalHost glfwHost = {
	openHost,
	closeHost,
	numDisplays,
	displayName,
	displayMode,
	getModes,
	createSurface,
	destroySurface,
	drawableSize,
	backingScale,
	refreshRate,
	visible,
	pollSizeChange
};

}
}
#endif
#endif
