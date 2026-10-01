#ifdef RW_METAL
#ifdef LIBRW_COCOA
#include "metalobjc.h"
#include "metalmodes.h"
#include "metaldrawable.h"

#import <AppKit/AppKit.h>
#include <IOKit/graphics/IOGraphicsTypes.h>

#define PLUGIN_ID 0

static bool hostSizeDirty;

@interface RWMetalHostView : NSView
@end

@implementation RWMetalHostView

- (CALayer *)makeBackingLayer
{
	return [CAMetalLayer layer];
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}

- (void)updateLayer
{
}

- (void)setFrameSize:(NSSize)size
{
	[super setFrameSize:size];
	hostSizeDirty = true;
}

- (void)viewDidChangeBackingProperties
{
	[super viewDidChangeBackingProperties];
	hostSizeDirty = true;
}

@end

namespace rw {
namespace metal {

static_assert(HOSTMODE_EXCLUSIVE == VIDEOMODEEXCLUSIVE, "host mode flags");

struct CocoaGlobals
{
	void **pWindow;
	int32 winWidth, winHeight;
	const char *winTitle;
	bool winHidden;
	bool createdApp;
	bool launched;
	bool createdHidden;
	bool fullscreen;

	MetalHostMode *modes;
	int32 numModes;
	int32 modeDisplay;
	int32 nativeWidth, nativeHeight;
	int32 renderWidth, renderHeight;

	int32 lastWidth, lastHeight;
	float32 lastScale;
	char name[128];
};

static CocoaGlobals cocoaGlobals;
static NSWindow *hostWindow;
static RWMetalHostView *hostView;
static NSMutableArray *hostObservers;

static NSScreen*
screenAt(int32 display)
{
	if(NSApp == nil)
		return nil;
	NSArray<NSScreen*> *screens = NSScreen.screens;
	if(display < 0 || display >= (int32)screens.count)
		return nil;
	return screens[display];
}

static CGDirectDisplayID
displayOf(NSScreen *screen)
{
	return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

static bool
currentMode(CGDirectDisplayID display, HostMode *mode)
{
	CGDisplayModeRef cur = CGDisplayCopyDisplayMode(display);
	if(cur == nil)
		return false;
	mode->width = (int32_t)CGDisplayModeGetPixelWidth(cur);
	mode->height = (int32_t)CGDisplayModeGetPixelHeight(cur);
	mode->depth = HOSTMODE_DEPTH;
	mode->refresh = hostRefreshHz(CGDisplayModeGetRefreshRate(cur));
	mode->flags = 0;
	CGDisplayModeRelease(cur);
	return true;
}

static void
freeModes(void)
{
	rwFree(cocoaGlobals.modes);
	cocoaGlobals.modes = nil;
	cocoaGlobals.numModes = 0;
	cocoaGlobals.nativeWidth = 0;
	cocoaGlobals.nativeHeight = 0;
}

static void
makeWindowedModeList(int32 width, int32 height)
{
	freeModes();
	cocoaGlobals.modes = rwNewT(MetalHostMode, 1, ID_DRIVER | MEMDUR_EVENT);
	cocoaGlobals.modes[0].width = width;
	cocoaGlobals.modes[0].height = height;
	cocoaGlobals.modes[0].depth = HOSTMODE_DEPTH;
	cocoaGlobals.modes[0].refresh = 0;
	cocoaGlobals.modes[0].flags = 0;
	cocoaGlobals.numModes = 1;
}

static bool
makeModeList(int32 display)
{
	NSScreen *screen = screenAt(display);
	HostMode current;
	if(screen == nil || !currentMode(displayOf(screen), &current))
		return false;

	NSDictionary *options = @{ (__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes: @YES };
	CFArrayRef all = CGDisplayCopyAllDisplayModes(displayOf(screen), (__bridge CFDictionaryRef)options);
	int32_t n = all ? (int32_t)CFArrayGetCount(all) : 0;
	HostModeCandidate *cands = rwNewT(HostModeCandidate, n+1, ID_DRIVER | MEMDUR_FUNCTION);
	for(int32_t i = 0; i < n; i++){
		CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
		cands[i].width = (int32_t)CGDisplayModeGetPixelWidth(m);
		cands[i].height = (int32_t)CGDisplayModeGetPixelHeight(m);
		cands[i].refresh = hostRefreshHz(CGDisplayModeGetRefreshRate(m));
		cands[i].flags = (CGDisplayModeIsUsableForDesktopGUI(m) ? HOSTCAND_USABLE : 0) |
			((CGDisplayModeGetIOFlags(m) & kDisplayModeNativeFlag) ? HOSTCAND_NATIVE : 0);
	}
	if(all)
		CFRelease(all);

	int32_t nw, nh;
	hostNativeSize(cands, n, &nw, &nh);
	HostMode *list = rwNewT(HostMode, n+2, ID_DRIVER | MEMDUR_FUNCTION);
	int32_t num = hostBuildModeList(cands, n, nw, nh, current, list, n+2);

	freeModes();
	cocoaGlobals.modes = rwNewT(MetalHostMode, num, ID_DRIVER | MEMDUR_EVENT);
	for(int32_t i = 0; i < num; i++){
		cocoaGlobals.modes[i].width = list[i].width;
		cocoaGlobals.modes[i].height = list[i].height;
		cocoaGlobals.modes[i].depth = list[i].depth;
		cocoaGlobals.modes[i].refresh = list[i].refresh;
		cocoaGlobals.modes[i].flags = list[i].flags;
	}
	cocoaGlobals.numModes = num;
	cocoaGlobals.nativeWidth = nw;
	cocoaGlobals.nativeHeight = nh;
	rwFree(cands);
	rwFree(list);
	return true;
}

static bool32
openHost(EngineOpenParams *openparams)
{
	if(![NSThread isMainThread]){
		RWERROR((ERR_GENERAL, "the Cocoa host must run on the main thread"));
		return 0;
	}
	cocoaGlobals.winWidth = openparams->width;
	cocoaGlobals.winHeight = openparams->height;
	cocoaGlobals.winTitle = openparams->windowtitle;
	cocoaGlobals.winHidden = openparams->hidden;
	cocoaGlobals.pWindow = openparams->cocoaWindow;

	if(NSApp == nil){
		[NSApplication sharedApplication];
		cocoaGlobals.createdApp = true;
	}
	if(cocoaGlobals.createdApp)
		[NSApp setActivationPolicy:cocoaGlobals.winHidden ?
			NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];

	if(NSScreen.screens.count == 0 && cocoaGlobals.winHidden){
		makeWindowedModeList(cocoaGlobals.winWidth, cocoaGlobals.winHeight);
		cocoaGlobals.modeDisplay = 0;
		return 1;
	}
	if(!makeModeList(0)){
		RWERROR((ERR_GENERAL, "no display found"));
		freeModes();
		return 0;
	}
	cocoaGlobals.modeDisplay = 0;
	return 1;
}

static void
closeHost(void)
{
	freeModes();
}

static int32
numDisplays(void)
{
	return NSApp ? (int32)NSScreen.screens.count : 0;
}

static const char*
displayName(int32 display)
{
	NSScreen *screen = screenAt(display);
	const char *s = screen ? screen.localizedName.UTF8String : nil;
	strlcpy(cocoaGlobals.name, s ? s : "", sizeof(cocoaGlobals.name));
	return cocoaGlobals.name;
}

static bool32
displayMode(int32 display, MetalHostMode *mode)
{
	NSScreen *screen = screenAt(display);
	HostMode cur;
	if(screen == nil || !currentMode(displayOf(screen), &cur))
		return 0;
	mode->width = cur.width;
	mode->height = cur.height;
	mode->depth = cur.depth;
	mode->refresh = cur.refresh;
	mode->flags = 0;
	return 1;
}

static const MetalHostMode*
getModes(int32 display, int32 *numModes)
{
	if(display != cocoaGlobals.modeDisplay && makeModeList(display))
		cocoaGlobals.modeDisplay = display;
	*numModes = cocoaGlobals.numModes;
	return cocoaGlobals.modes;
}

static bool32
fullscreenSize(int32 mode, int32 *width, int32 *height)
{
	if(cocoaGlobals.modes == nil || mode < 0 || mode >= cocoaGlobals.numModes)
		return 0;
	hostRenderSize(cocoaGlobals.modes[mode].width, cocoaGlobals.modes[mode].height,
		cocoaGlobals.nativeWidth, cocoaGlobals.nativeHeight, width, height);
	return 1;
}

bool32
cocoaFullscreenSizeForTest(int32 mode, int32 *width, int32 *height)
{
	return fullscreenSize(mode, width, height);
}

static void
drawableSize(int32 *width, int32 *height)
{
	if(hostWindow == nil){
		*width = 0;
		*height = 0;
		return;
	}
	if(cocoaGlobals.fullscreen){
		*width = cocoaGlobals.renderWidth;
		*height = cocoaGlobals.renderHeight;
		return;
	}
	NSSize points = hostView.bounds.size;
	hostWindowDrawable((float)points.width, (float)points.height, (float)hostWindow.backingScaleFactor, width, height);
}

static float32
backingScale(void)
{
	return hostWindow ? (float32)hostWindow.backingScaleFactor : 0.0f;
}

static void
observe(NSWindow *win)
{
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	NSArray<NSNotificationName> *names = @[ NSWindowDidResizeNotification,
		NSWindowDidChangeBackingPropertiesNotification, NSWindowDidChangeScreenNotification,
		NSWindowDidEnterFullScreenNotification, NSWindowDidExitFullScreenNotification ];
	hostObservers = [NSMutableArray array];
	for(NSNotificationName name in names)
		[hostObservers addObject:[center addObserverForName:name object:win queue:nil
			usingBlock:^(NSNotification *note){ hostSizeDirty = true; }]];
}

static void*
createSurface(int32 display, int32 mode, bool32 windowed, bool32 hidden)
{
	if(cocoaGlobals.modes == nil || mode < 0 || mode >= cocoaGlobals.numModes){
		RWERROR((ERR_GENERAL, "invalid video mode"));
		return nil;
	}
	bool fullscreen = !windowed && !hidden;
	NSScreen *screen = screenAt(display);
	NSRect content = NSMakeRect(0, 0, cocoaGlobals.winWidth, cocoaGlobals.winHeight);
	NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
		NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
	NSWindow *win = [[NSWindow alloc] initWithContentRect:content styleMask:style
		backing:NSBackingStoreBuffered defer:NO screen:screen];
	if(win == nil){
		RWERROR((ERR_GENERAL, "cannot create the window"));
		return nil;
	}
	win.releasedWhenClosed = NO;
	win.restorable = NO;
	win.tabbingMode = NSWindowTabbingModeDisallowed;
	win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
	win.title = [NSString stringWithUTF8String:cocoaGlobals.winTitle ? cocoaGlobals.winTitle : ""];
	win.backgroundColor = NSColor.blackColor;

	RWMetalHostView *view = [[RWMetalHostView alloc] initWithFrame:content];
	view.wantsLayer = YES;
	view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
	win.contentView = view;
	CAMetalLayer *layer = (CAMetalLayer*)view.layer;
	if(![layer isKindOfClass:[CAMetalLayer class]]){
		RWERROR((ERR_GENERAL, "the view has no Metal layer"));
		[win close];
		return nil;
	}
	layer.contentsGravity = kCAGravityResizeAspect;
	layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
	layer.contentsScale = win.backingScaleFactor;

	hostWindow = win;
	hostView = view;
	cocoaGlobals.createdHidden = hidden;
	cocoaGlobals.fullscreen = fullscreen;
	if(fullscreen)
		fullscreenSize(mode, &cocoaGlobals.renderWidth, &cocoaGlobals.renderHeight);
	int32 w, h;
	drawableSize(&w, &h);
	layer.drawableSize = CGSizeMake(w, h);
	observe(win);

	if(!hidden){
		[win center];
		if(cocoaGlobals.createdApp && !cocoaGlobals.launched){
			[NSApp finishLaunching];
			cocoaGlobals.launched = true;
		}
		[win makeKeyAndOrderFront:nil];
		if(cocoaGlobals.createdApp)
			[NSApp activate];
		if(fullscreen)
			[win toggleFullScreen:nil];
	}

	hostSizeDirty = false;
	cocoaGlobals.lastWidth = w;
	cocoaGlobals.lastHeight = h;
	cocoaGlobals.lastScale = backingScale();
	if(cocoaGlobals.pWindow)
		*cocoaGlobals.pWindow = (__bridge void*)win;
	return (__bridge void*)layer;
}

static void
destroySurface(void)
{
	if(hostWindow == nil)
		return;
	for(id observer in hostObservers)
		[NSNotificationCenter.defaultCenter removeObserver:observer];
	hostObservers = nil;
	[hostWindow orderOut:nil];
	[hostWindow close];
	hostWindow = nil;
	hostView = nil;
	cocoaGlobals.fullscreen = false;
	if(cocoaGlobals.pWindow)
		*cocoaGlobals.pWindow = nil;
}

static int32
refreshRate(void)
{
	if(hostWindow == nil)
		return 0;
	NSScreen *screen = hostWindow.screen ? hostWindow.screen : NSScreen.screens.firstObject;
	return screen ? (int32)screen.maximumFramesPerSecond : 0;
}

// a window created hidden on request is never ordered in, yet renders and presents
static bool32
visible(void)
{
	if(hostWindow == nil || hostWindow.miniaturized)
		return 0;
	return cocoaGlobals.createdHidden || (hostWindow.occlusionState & NSWindowOcclusionStateVisible) != 0;
}

static bool32
pollSizeChange(void)
{
	int32 w, h;
	float32 scale;

	if(hostWindow == nil)
		return 0;
	drawableSize(&w, &h);
	scale = backingScale();
	bool changed = hostSizeDirty || w != cocoaGlobals.lastWidth || h != cocoaGlobals.lastHeight ||
		scale != cocoaGlobals.lastScale;
	hostSizeDirty = false;
	cocoaGlobals.lastWidth = w;
	cocoaGlobals.lastHeight = h;
	cocoaGlobals.lastScale = scale;
	return changed;
}

MetalHost cocoaHost = {
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
