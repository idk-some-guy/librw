#ifdef LIBRW_COCOA
#include <src/metal/metalobjc.h>
#import <AppKit/AppKit.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <unistd.h>
#include "cocoa_checks.h"

using namespace rw::metal;

static NSWindow*
Win(void *window)
{
	return (__bridge NSWindow*)window;
}

void
CocoaPumpEvents(void)
{
	@autoreleasepool {
		NSEvent *event;
		while((event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:NSDate.distantPast
			inMode:NSDefaultRunLoopMode dequeue:YES]) != nil)
			[NSApp sendEvent:event];
	}
}

void
CocoaWindowSize(void *window, int *width, int *height)
{
	NSSize size = Win(window).contentView.bounds.size;
	*width = (int)size.width;
	*height = (int)size.height;
}

void
CocoaFramebufferSize(void *window, int *width, int *height)
{
	NSView *view = Win(window).contentView;
	NSRect r = [view convertRectToBacking:view.bounds];
	*width = (int)r.size.width;
	*height = (int)r.size.height;
}

void
CocoaSetWindowSize(void *window, int width, int height)
{
	@autoreleasepool {
		[Win(window) setContentSize:NSMakeSize(width, height)];
	}
	CocoaPumpEvents();
}

float
CocoaContentScale(void *window)
{
	return (float)Win(window).backingScaleFactor;
}

int
CocoaWindowShown(void *window)
{
	return Win(window).visible;
}

bool
CocoaWindowInfoOf(void *window, CocoaWindowInfo *info)
{
	NSWindow *win = Win(window);
	if(win == nil)
		return false;
	info->shown = win.visible;
	info->key = win.keyWindow;
	info->miniaturized = win.miniaturized;
	info->occlusionVisible = (win.occlusionState & NSWindowOcclusionStateVisible) != 0;
	info->accessory = NSApp.activationPolicy == NSApplicationActivationPolicyAccessory;
	info->number = (long)win.windowNumber;
	return true;
}

bool
CocoaLayerInfoOf(void *window, CocoaLayerInfo *info)
{
	NSWindow *win = Win(window);
	MetalContext *ctx = getContext();
	if(win == nil || ctx == nil || ctx->layer == nil)
		return false;
	CALayer *layer = win.contentView.layer;
	info->hostView = [win.contentView isKindOfClass:NSClassFromString(@"RWMetalHostView")];
	info->metalLayer = [layer isKindOfClass:[CAMetalLayer class]];
	info->deviceLayer = layer == ctx->layer;
	info->gravityAspect = [layer.contentsGravity isEqualToString:kCAGravityResizeAspect];
	info->backgroundBlack = layer.backgroundColor != nil &&
		CGColorEqualToColor(layer.backgroundColor, CGColorGetConstantColor(kCGColorBlack));
	info->drawableWidth = (int)ctx->layer.drawableSize.width;
	info->drawableHeight = (int)ctx->layer.drawableSize.height;
	info->contentsScale = ctx->layer.contentsScale;
	return true;
}

bool
CocoaServerWindow(long number, int *onScreen)
{
	bool found = false;
	*onScreen = 0;
	NSArray *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID));
	for(NSDictionary *w in windows){
		if([w[(__bridge NSString*)kCGWindowNumber] longValue] != number ||
		   [w[(__bridge NSString*)kCGWindowOwnerPID] intValue] != getpid())
			continue;
		found = true;
		*onScreen = [w[(__bridge NSString*)kCGWindowIsOnscreen] boolValue];
	}
	return found;
}

static CGDirectDisplayID
ZeroDisplay(void)
{
	NSScreen *screen = NSScreen.screens.firstObject;
	return screen ? [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] : kCGNullDirectDisplay;
}

bool
CocoaCurrentDisplayPixels(int *width, int *height)
{
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ZeroDisplay());
	if(mode == nil)
		return false;
	*width = (int)CGDisplayModeGetPixelWidth(mode);
	*height = (int)CGDisplayModeGetPixelHeight(mode);
	CGDisplayModeRelease(mode);
	return true;
}

bool
CocoaNativeDisplayPixels(int *width, int *height)
{
	NSDictionary *options = @{ (__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes: @YES };
	NSArray *modes = CFBridgingRelease(CGDisplayCopyAllDisplayModes(ZeroDisplay(), (__bridge CFDictionaryRef)options));
	long bestArea = 0;
	*width = 0;
	*height = 0;
	for(id m in modes){
		CGDisplayModeRef mode = (__bridge CGDisplayModeRef)m;
		int w = (int)CGDisplayModeGetPixelWidth(mode), h = (int)CGDisplayModeGetPixelHeight(mode);
		if(CGDisplayModeGetIOFlags(mode) & kDisplayModeNativeFlag){
			*width = w;
			*height = h;
			return true;
		}
		if(CGDisplayModeIsUsableForDesktopGUI(mode) && ((long)w*h > bestArea || ((long)w*h == bestArea && w > *width))){
			bestArea = (long)w*h;
			*width = w;
			*height = h;
		}
	}
	return *width > 0;
}

bool
CocoaScreenPixels(int *width, int *height)
{
	NSScreen *screen = NSScreen.screens.firstObject;
	if(screen == nil)
		return false;
	*width = (int)(screen.frame.size.width*screen.backingScaleFactor + 0.5);
	*height = (int)(screen.frame.size.height*screen.backingScaleFactor + 0.5);
	return true;
}
#endif
