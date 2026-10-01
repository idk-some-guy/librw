#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include <src/d3d/rwd3dimpl.h>
#include "state_checks.h"
#include "world_checks.h"
#include "target_checks.h"
#include "host_checks.h"
#include "objc_checks.h"
#include "msaa_checks.h"
#ifdef LIBRW_COCOA
#include <src/metal/metaldrawable.h>
#include "cocoa_checks.h"
#endif

using namespace rw;

struct Context
{
#ifdef LIBRW_COCOA
	void *window;
#else
	GLFWwindow *window;
#endif
	Camera *camera;
	Frame *frame;
};

static Context ctx;
static uint32 framesAtStart;
static uint32 engineSamples = 1;
static int failures = 0;
static char details[4096];
static int detailsLen = 0;

static const RGBA RED = { 255, 0, 0, 255 };
static const RGBA GREEN = { 0, 255, 0, 255 };
static const RGBA BLUE = { 0, 0, 255, 255 };
static const RGBA GREY = { 128, 128, 128, 255 };

#ifdef LIBRW_COCOA
static double Seconds(void) { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void FramebufferSize(int *w, int *h) { CocoaFramebufferSize(ctx.window, w, h); }
static void WindowSize(int *w, int *h) { CocoaWindowSize(ctx.window, w, h); }
static void SetWindowSize(int w, int h) { CocoaSetWindowSize(ctx.window, w, h); }
static float WindowContentScale(void) { return CocoaContentScale(ctx.window); }
static int WindowShown(void) { return CocoaWindowShown(ctx.window); }
static void PumpEvents(void) { CocoaPumpEvents(); }
#else
static double Seconds(void) { return glfwGetTime(); }
static void FramebufferSize(int *w, int *h) { glfwGetFramebufferSize(ctx.window, w, h); }
static void WindowSize(int *w, int *h) { glfwGetWindowSize(ctx.window, w, h); }
static void SetWindowSize(int w, int h) { glfwSetWindowSize(ctx.window, w, h); }
static float WindowContentScale(void) { float xs, ys; glfwGetWindowContentScale(ctx.window, &xs, &ys); return xs; }
static int WindowShown(void) { return glfwGetWindowAttrib(ctx.window, GLFW_VISIBLE); }
static void PumpEvents(void) {}
#endif

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

static bool
PixelIs(Image *img, int x, int y, RGBA want)
{
	uint8 *p = &img->pixels[y*img->stride + x*img->bpp];
	if(p[0] == want.red && p[1] == want.green && p[2] == want.blue && p[3] == want.alpha)
		return true;
	Detail("  pixel (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,%d\n", x, y,
	       p[0], p[1], p[2], p[3], want.red, want.green, want.blue, want.alpha);
	return false;
}

static Image*
ReadBack(void)
{
	Image *img = ctx.camera->frameBuffer->toImage();
	if(img == nil)
		Detail("  toImage returned nil\n");
	else if(img->depth != 32 || img->bpp != 4 || img->stride != img->width*4){
		Detail("  image layout depth %d bpp %d stride %d\n", img->depth, img->bpp, img->stride);
		img->destroy();
		return nil;
	}
	return img;
}

static bool
CornersAndCentreAre(RGBA want)
{
	Image *img = ReadBack();
	if(img == nil)
		return false;
	int w = img->width, h = img->height;
	bool ok = PixelIs(img, 0, 0, want);
	ok &= PixelIs(img, w-1, 0, want);
	ok &= PixelIs(img, 0, h-1, want);
	ok &= PixelIs(img, w-1, h-1, want);
	ok &= PixelIs(img, w/2, h/2, want);
	img->destroy();
	return ok;
}

static void
Clear(RGBA col, uint32 mode)
{
	ctx.camera->clear(&col, mode);
}

static void
DrawFrameFlags(RGBA col, uint32 flags)
{
	Clear(col, Camera::CLEARIMAGE | Camera::CLEARZ | Camera::CLEARSTENCIL);
	ctx.camera->beginUpdate();
	ctx.camera->endUpdate();
	ctx.camera->showRaster(flags);
	PumpEvents();
}

static void
DrawFrame(RGBA col)
{
	DrawFrameFlags(col, 0);
}

static bool
CreateRasters(int w, int h)
{
	ctx.camera->frameBuffer = Raster::create(w, h, 0, Raster::CAMERA);
	ctx.camera->zBuffer = Raster::create(w, h, 0, Raster::ZBUFFER);
	return ctx.camera->frameBuffer && ctx.camera->zBuffer;
}

static void
DestroyRasters(void)
{
	if(ctx.camera->frameBuffer)
		ctx.camera->frameBuffer->destroy();
	if(ctx.camera->zBuffer)
		ctx.camera->zBuffer->destroy();
	ctx.camera->frameBuffer = nil;
	ctx.camera->zBuffer = nil;
}

static bool
StartEngine(int w, int h)
{
	static EngineOpenParams params;
#ifdef LIBRW_COCOA
	params.cocoaWindow = &ctx.window;
	params.host = &metal::cocoaHost;
#else
	params.window = &ctx.window;
#endif
	params.width = w;
	params.height = h;
	params.windowtitle = "metal_smoke";
	params.hidden = true;

	if(!Engine::init()){
		Detail("  Engine::init failed\n");
		return false;
	}
	registerNativeDataPlugin();
	registerSkinPlugin();
	registerMatFXPlugin();
	if(!Engine::open(&params)){
		Detail("  Engine::open failed\n");
		return false;
	}
	Engine::setMultiSamplingLevels(engineSamples);
	if(!Engine::start()){
		Detail("  Engine::start failed\n");
		return false;
	}
	framesAtStart = metal::getFrameStats().framesShown;
	ctx.camera = Camera::create();
	ctx.frame = rw::Frame::create();
	ctx.camera->setFrame(ctx.frame);
	return CreateRasters(w, h);
}

static void
StopEngine(void)
{
	DestroyRasters();
	ctx.camera->destroy();
	ctx.frame->destroy();
	Engine::stop();
	Engine::close();
	Engine::term();
}

static bool
RestartEngine(void)
{
	StopEngine();
	return StartEngine(640, 480);
}

static bool
RestartEngineSamples(uint32 samples)
{
	engineSamples = samples;
	return RestartEngine();
}

static Camera*
CurrentCamera(void)
{
	return ctx.camera;
}

static bool
CheckPrewarmLine(void)
{
	const char *line = metal::getPrewarmLine();
	unsigned n = 0;
	double ms = -1.0;
	char want[128];
	bool ok = sscanf(line, "rw::metal: prewarm %u pipelines in %lf ms", &n, &ms) == 2;
	if(ok){
		snprintf(want, sizeof(want), "rw::metal: prewarm %u pipelines in %.1f ms\n", n, ms);
		ok = strcmp(line, want) == 0 && n == 51 && n == metal::getStateStats().pipelinesAtInit && ms >= 0.0;
	}
	if(!ok)
		Detail("  got \"%s\"\n", line);
	return Report(ok, "the prewarm line reports 51 pipelines and the time in ms");
}

struct StatsTail
{
	double customPerFrame, materialPerFrame, waitMaxMs, hostPrewarmMs;
	unsigned withoutDrawable, samples;
};

static bool
ParseStatsTail(const char *line, StatsTail *t)
{
	const char *tail = strstr(line, " custom uploads/frame ");
	return tail && sscanf(tail, " custom uploads/frame %lf material uploads/frame %lf frames without drawable %u "
	                      "drawable wait max %lf ms host prewarm %lf ms samples %u",
	                      &t->customPerFrame, &t->materialPerFrame, &t->withoutDrawable, &t->waitMaxMs,
	                      &t->hostPrewarmMs, &t->samples) == 6;
}

static bool
CheckStatsLineAtStop(void)
{
	const char *line = metal::getStatsLine();
	unsigned frames, ringPeak, grows, late, host, skinned, strips, staged, direct, mips, waits, mismatches, dropped;
	double drawsPerFrame, passesPerFrame = -1.0, copiesPerFrame = -1.0;
	StatsTail t;
	int n = sscanf(line, "rw::metal: stats frames %u draws/frame %lf ring peak %u ring grows %u late pipelines %u "
	               "host pipelines %u skinned unrouted %u strip restarts %u staged uploads %u direct uploads %u "
	               "mipmap blits %u gpu waits %u block mismatches %u dropped draws %u passes/frame %lf copies/frame %lf",
	               &frames, &drawsPerFrame, &ringPeak, &grows, &late, &host, &skinned, &strips, &staged, &direct, &mips,
	               &waits, &mismatches, &dropped, &passesPerFrame, &copiesPerFrame);
	metal::StateStats s = metal::getStateStats();
	bool ok = n == 16 && line[strlen(line)-1] == '\n';
	ok = ok && frames == metal::getFrameStats().framesShown - framesAtStart && frames > 0;
	ok = ok && drawsPerFrame > 0.0 && ringPeak > 0;
	ok = ok && passesPerFrame > 0.0 && copiesPerFrame >= 0.0;
	ok = ok && late == s.pipelinesLate && host == s.pipelinesHost && skinned == s.skinnedUnrouted && mismatches == s.blockSizeMismatches &&
	     dropped == s.droppedDraws && grows == s.ringGrows;
	ok = ok && grows >= 1 && s.ringPeakBytes >= ForcedRingGrowthBytes();
	ok = ok && ParseStatsTail(line, &t) && t.samples == 1 && t.hostPrewarmMs > 0.0;
	if(!ok)
		Detail("  got \"%s\", ring peak %u bytes, forced growth %u bytes\n", line, s.ringPeakBytes,
		       ForcedRingGrowthBytes());
	return Report(ok, "engine stop prints a stats line with the counters since engine start");
}

static bool
CheckOpen(void)
{
	bool ok = StartEngine(640, 480);
	return Report(ok && ctx.window != nil, "open hidden 640x480 with CAMERA and ZBUFFER rasters");
}

static bool
CheckClearColour(RGBA col, const char *name)
{
	DrawFrame(col);
	return Report(CornersAndCentreAre(col), name);
}

static bool
CheckSubRectClear(void)
{
	Raster *parent = ctx.camera->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 10, 20, 16, 10 };
	sub->subRaster(parent, &r);

	DrawFrame(GREEN);
	ctx.camera->frameBuffer = sub;
	Clear(RED, Camera::CLEARIMAGE);
	ctx.camera->frameBuffer = parent;
	ctx.camera->showRaster(0);
	sub->destroy();

	Image *img = ReadBack();
	if(img == nil)
		return Report(false, "sub-rectangle clear");
	int count = 0, outside = 0;
	for(int y = 0; y < img->height; y++)
		for(int x = 0; x < img->width; x++){
			uint8 *p = &img->pixels[y*img->stride + x*4];
			if(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255){
				count++;
				if(y < r.y || y >= r.y+r.h || x < r.x || x >= r.x+r.w)
					outside++;
			}
		}
	bool ok = count == r.w*r.h && outside == 0;
	if(!ok)
		Detail("  %d red pixels, %d outside the rectangle, expected %d and 0\n", count, outside, r.w*r.h);
	ok &= PixelIs(img, r.x, r.y, RED);
	ok &= PixelIs(img, r.x+r.w-1, r.y+r.h-1, RED);
	ok &= PixelIs(img, r.x-1, r.y, GREEN);
	ok &= PixelIs(img, r.x, r.y+r.h, GREEN);
	img->destroy();
	return Report(ok, "sub-rectangle clear 16x10 at 10,20");
}

static bool
CheckClearWithPassOpen(void)
{
	Raster *parent = ctx.camera->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 0, 0, 8, 8 };
	sub->subRaster(parent, &r);

	DrawFrame(GREEN);
	ctx.camera->beginUpdate();
	ctx.camera->frameBuffer = sub;
	Clear(BLUE, Camera::CLEARIMAGE);
	ctx.camera->frameBuffer = parent;
	Clear(RED, Camera::CLEARIMAGE | Camera::CLEARZ);
	ctx.camera->endUpdate();
	ctx.camera->showRaster(0);
	sub->destroy();
	return Report(CornersAndCentreAre(RED), "full clear while a pass is open");
}

static bool
CheckDepthOnlyClear(void)
{
	DrawFrame(RED);
	Clear(GREEN, Camera::CLEARZ);
	ctx.camera->beginUpdate();
	ctx.camera->endUpdate();
	ctx.camera->showRaster(0);
	return Report(CornersAndCentreAre(RED), "depth-only clear leaves colour");
}

static bool
CheckShowWithoutUpdate(void)
{
	Clear(BLUE, Camera::CLEARIMAGE | Camera::CLEARZ);
	ctx.camera->showRaster(0);
	return Report(CornersAndCentreAre(BLUE), "show after clear with nothing drawn");
}

static bool
CheckTwentyFrames(void)
{
	RGBA col = { 0, 0, 0, 255 };
	metal::FrameStats before = metal::getFrameStats();
	for(int i = 0; i < 20; i++){
		col.red = i*10;
		col.green = 255 - i*10;
		col.blue = i;
		DrawFrame(col);
	}
	metal::FrameStats after = metal::getFrameStats();
	uint32 shown = after.framesShown - before.framesShown;
	uint32 acquired = after.drawablesAcquired - before.drawablesAcquired;
	uint32 presented = after.framesPresented - before.framesPresented;
	bool ok = CornersAndCentreAre(col);
	if(shown != 20 || acquired != 20 || presented != 20){
		Detail("  frames shown %u, drawables acquired %u, frames presented %u, expected 20 each\n",
		       shown, acquired, presented);
		ok = false;
	}
	return Report(ok, "twenty frames in a row, each composited and presented");
}

static bool
CompositeIs(uint8 *px, int w, int x, int y, RGBA want)
{
	uint8 *p = &px[(y*w + x)*4];
	if(p[2] == want.red && p[1] == want.green && p[0] == want.blue && p[3] == 255)
		return true;
	Detail("  composite pixel (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,255\n", x, y,
	       p[2], p[1], p[0], p[3], want.red, want.green, want.blue);
	return false;
}

static uint8*
CompositeBack(void)
{
	Raster *fb = ctx.camera->frameBuffer;
	uint8 *px = new uint8[fb->width*fb->height*4];
	if(!metal::compositeCameraPixels(fb, px)){
		Detail("  compositeCameraPixels failed\n");
		delete[] px;
		return nil;
	}
	return px;
}

static bool
CheckCompositeColour(void)
{
	RGBA col = { 200, 100, 50, 255 };
	DrawFrame(col);
	uint8 *px = CompositeBack();
	if(px == nil)
		return Report(false, "composite into BGRA8 keeps 200,100,50");
	int w = ctx.camera->frameBuffer->width, h = ctx.camera->frameBuffer->height;
	bool ok = CompositeIs(px, w, 0, 0, col);
	ok &= CompositeIs(px, w, w-1, 0, col);
	ok &= CompositeIs(px, w, 0, h-1, col);
	ok &= CompositeIs(px, w, w-1, h-1, col);
	ok &= CompositeIs(px, w, w/2, h/2, col);
	delete[] px;
	return Report(ok, "composite into BGRA8 keeps 200,100,50");
}

static bool
CheckCompositeOrientation(void)
{
	Raster *parent = ctx.camera->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 10, 2, 16, 6 };
	sub->subRaster(parent, &r);

	DrawFrame(GREEN);
	ctx.camera->frameBuffer = sub;
	Clear(RED, Camera::CLEARIMAGE);
	ctx.camera->frameBuffer = parent;
	sub->destroy();

	uint8 *px = CompositeBack();
	if(px == nil)
		return Report(false, "composite keeps a clear near the top left at the top left");
	int w = parent->width, h = parent->height;
	bool ok = CompositeIs(px, w, r.x, r.y, RED);
	ok &= CompositeIs(px, w, r.x+r.w-1, r.y+r.h-1, RED);
	ok &= CompositeIs(px, w, r.x-1, r.y, GREEN);
	ok &= CompositeIs(px, w, r.x, r.y+r.h, GREEN);
	ok &= CompositeIs(px, w, r.x, h-1-r.y, GREEN);
	ok &= CompositeIs(px, w, w-1-r.x, r.y, GREEN);
	delete[] px;
	return Report(ok, "composite keeps a clear near the top left at the top left");
}

static metal::MetalRaster*
MetalExt(Raster *raster)
{
	using namespace metal;
	return GETMETALRASTEREXT(raster);
}

static bool
BytesEqual(const uint8 *got, const uint8 *want, int n, const char *what)
{
	for(int i = 0; i < n; i++)
		if(got[i] != want[i]){
			Detail("  %s: byte %d is %d, expected %d\n", what, i, got[i], want[i]);
			return false;
		}
	return true;
}

static Image*
MakeImage(int depth, int w, int h, const uint8 *pixels, const uint8 *palette, int paletteSize)
{
	Image *img = Image::create(w, h, depth);
	img->allocate();
	memcpy(img->pixels, pixels, img->stride*h);
	if(palette)
		memcpy(img->palette, palette, paletteSize*4);
	return img;
}

static bool
CheckImageRoundTrip(Image *img, int32 wantFormat, int wantDepth, const uint8 *want, const char *name)
{
	int w = img->width, h = img->height;
	Raster *ras = Raster::createFromImage(img);
	img->destroy();
	if(ras == nil){
		Detail("  createFromImage returned nil\n");
		return Report(false, name);
	}
	bool ok = true;
	if((ras->format & 0xF00) != wantFormat || ras->getNumLevels() != 1){
		Detail("  raster format 0x%x with %d levels, expected 0x%x with 1\n",
		       ras->format & 0xF00, ras->getNumLevels(), wantFormat);
		ok = false;
	}
	Image *back = ras->toImage();
	if(back == nil || back->depth != wantDepth || back->width != w || back->height != h){
		Detail("  toImage gave %s depth %d %dx%d, expected depth %d %dx%d\n", back ? "image" : "nil",
		       back ? back->depth : 0, back ? back->width : 0, back ? back->height : 0, wantDepth, w, h);
		ok = false;
	}else{
		int rowBytes = w*back->bpp;
		ok &= BytesEqual(back->pixels, want, rowBytes, "top row");
		ok &= BytesEqual(back->pixels + (h-1)*back->stride, want + (h-1)*rowBytes, rowBytes, "bottom row");
	}
	if(back)
		back->destroy();
	ras->destroy();
	return Report(ok, name);
}

static void
CheckTexturesFromImages(void)
{
	static const uint8 rgba[] = {
		255, 0, 0, 255,   0, 255, 0, 128,   0, 0, 255, 0,
		10, 20, 30, 40,   50, 60, 70, 80,   90, 100, 110, 120,
	};
	CheckImageRoundTrip(MakeImage(32, 3, 2, rgba, nil, 0), Raster::C8888, 32, rgba,
	                    "32-bit image with alpha becomes C8888 and reads back exactly");

	static const uint8 opaque[] = {
		255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,
		10, 20, 30, 255,  50, 60, 70, 255,  90, 100, 110, 255,
	};
	static const uint8 opaqueRGB[] = {
		255, 0, 0,   0, 255, 0,   0, 0, 255,
		10, 20, 30,  50, 60, 70,  90, 100, 110,
	};
	CheckImageRoundTrip(MakeImage(32, 3, 2, opaque, nil, 0), Raster::C888, 24, opaqueRGB,
	                    "opaque 32-bit image becomes C888 and reads back exactly");

	static const uint8 rgb[] = {
		255, 0, 0,   0, 255, 0,   0, 0, 255,
		1, 2, 3,     4, 5, 6,     7, 8, 9,
	};
	CheckImageRoundTrip(MakeImage(24, 3, 2, rgb, nil, 0), Raster::C888, 24, rgb,
	                    "24-bit image becomes C888 and reads back exactly");

	static const uint8 argb1555[] = {
		0x00, 0xFC,   0xE0, 0x03,   0x1F, 0x80,
		0x21, 0x84,   0x34, 0x12,   0xFF, 0xFF,
	};
	CheckImageRoundTrip(MakeImage(16, 3, 2, argb1555, nil, 0), Raster::C1555, 16, argb1555,
	                    "16-bit image becomes C1555 and reads back exactly");

	static const uint8 palette[] = {
		255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 128,   9, 8, 7, 255,
	};
	static const uint8 indices[] = { 0, 1, 2, 3, 3, 0 };
	static const uint8 palettised[] = {
		255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 128,
		9, 8, 7, 255,     9, 8, 7, 255,     255, 0, 0, 255,
	};
	CheckImageRoundTrip(MakeImage(8, 3, 2, indices, palette, 4), Raster::C8888, 32, palettised,
	                    "8-bit palettised image with alpha becomes C8888 and reads back exactly");

	static const uint8 opaquePalette[] = {
		255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   9, 8, 7, 255,
	};
	static const uint8 palettisedRGB[] = {
		255, 0, 0,   0, 255, 0,   0, 0, 255,
		9, 8, 7,     9, 8, 7,     255, 0, 0,
	};
	CheckImageRoundTrip(MakeImage(4, 3, 2, indices, opaquePalette, 4), Raster::C888, 24, palettisedRGB,
	                    "4-bit palettised opaque image becomes C888 and reads back exactly");
}

static bool
CheckLockRoundTrip(int32 format, int bpp, const char *name)
{
	uint8 data[3*2*4];
	for(int i = 0; i < (int)sizeof(data); i++)
		data[i] = (uint8)(i*37 + 11);
	int n = 3*2*bpp;

	Raster *ras = Raster::create(3, 2, 0, format | Raster::TEXTURE);
	if(ras == nil){
		Detail("  Raster::create returned nil\n");
		return Report(false, name);
	}
	bool ok = true;
	uint8 *px = ras->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(px == nil || ras->stride != 3*bpp){
		Detail("  write lock gave %p with stride %d, expected stride %d\n", px, ras->stride, 3*bpp);
		ok = false;
	}else{
		memcpy(px, data, n);
		ras->unlock(0);
		px = ras->lock(0, Raster::LOCKREAD);
		if(px == nil){
			Detail("  read lock returned nil\n");
			ok = false;
		}else{
			ok &= BytesEqual(px, data, n, "read back");
			ras->unlock(0);
		}
	}
	ras->destroy();
	return Report(ok, name);
}

static void
CheckRasterFormats(void)
{
	CheckLockRoundTrip(Raster::C8888, 4, "C8888 texture write lock then read lock is exact");
	CheckLockRoundTrip(Raster::C888, 3, "C888 texture write lock then read lock is exact");
	CheckLockRoundTrip(Raster::C1555, 2, "C1555 texture write lock then read lock is exact");
	CheckLockRoundTrip(Raster::C565, 2, "C565 texture write lock then read lock is exact");
	CheckLockRoundTrip(Raster::C4444, 2, "C4444 texture write lock then read lock is exact");
	CheckLockRoundTrip(Raster::LUM8, 1, "LUM8 texture write lock then read lock is exact");
}

static bool
CheckDXTLevels(int dxt, bool alpha, int w, int h, int numLevels, const int *sizes, const char *name)
{
	int32 format = Raster::TEXTURE | Raster::DONTALLOCATE | (numLevels > 1 ? Raster::MIPMAP : 0);
	Raster *ras = Raster::create(w, h, 16, format);
	metal::allocateDXT(ras, dxt, numLevels, alpha);
	metal::MetalRaster *natras = MetalExt(ras);
	bool ok = natras->texture != nil && natras->isCompressed && natras->hasAlpha == alpha &&
	          ras->getNumLevels() == numLevels && !(ras->flags & Raster::DONTALLOCATE);
	if(!ok)
		Detail("  texture %p compressed %d alpha %d levels %d, expected levels %d\n", natras->texture,
		       natras->isCompressed, natras->hasAlpha, ras->getNumLevels(), numLevels);
	for(int i = 0; ok && i < numLevels; i++){
		uint8 *px = ras->lock(i, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
		int lw = w >> i, lh = h >> i;
		lw = lw < 1 ? 1 : lw;
		lh = lh < 1 ? 1 : lh;
		if(px == nil || ras->width != lw || ras->height != lh){
			Detail("  level %d lock %p size %dx%d, expected %dx%d\n", i, px, ras->width, ras->height, lw, lh);
			ok = false;
			break;
		}
		for(int j = 0; j < sizes[i]; j++)
			px[j] = (uint8)(i*50 + j*7);
		ras->unlock(i);
	}
	for(int i = 0; ok && i < numLevels; i++){
		uint8 want[64];
		for(int j = 0; j < sizes[i]; j++)
			want[j] = (uint8)(i*50 + j*7);
		uint8 *px = ras->lock(i, Raster::LOCKREAD);
		if(px == nil){
			Detail("  level %d read lock returned nil\n", i);
			ok = false;
			break;
		}
		char what[32];
		snprintf(what, sizeof(what), "level %d", i);
		ok &= BytesEqual(px, want, sizes[i], what);
		ras->unlock(i);
	}
	ras->destroy();
	return Report(ok, name);
}

static void
CheckDXT(void)
{
	static const int dxt1Sizes[] = { 32, 8, 8, 8 };
	CheckDXTLevels(1, false, 8, 8, 4, dxt1Sizes, "DXT1 8x8 with 4 levels of 32, 8, 8 and 8 bytes");
	static const int dxt5Sizes[] = { 48 };
	CheckDXTLevels(5, true, 12, 4, 1, dxt5Sizes, "DXT5 12x4 with one level of 48 bytes");
	static const int dxt3Sizes[] = { 32, 16 };
	CheckDXTLevels(3, true, 2, 6, 2, dxt3Sizes, "DXT3 2x6 below the block width, levels of 32 and 16 bytes");
}

static const uint8 nativeRGBA[] = {
	255, 0, 0, 255,   0, 255, 0, 128,   0, 0, 255, 64,     10, 20, 30, 40,
	50, 60, 70, 80,   90, 100, 110, 120,   1, 2, 3, 4,   200, 201, 202, 203,
};
static const int nativeDXTSizes[] = { 32, 8, 8, 8 };

static uint8
NativeDXTByte(int level, int i)
{
	return (uint8)(level*50 + i*7 + 3);
}

static void
WriteNativeHeader(StreamMemory *s, uint32 platform)
{
	char name[32] = "metal_native", mask[32] = "";
	s->writeU32(platform);
	s->writeU32(0x1102);
	s->write8(name, 32);
	s->write8(mask, 32);
}

static uint32
BuildD3D8C8888(uint8 *body, uint32 cap)
{
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(Raster::C8888);
	s.writeI32(1);
	s.writeU16(4);
	s.writeU16(2);
	s.writeU8(32);
	s.writeU8(1);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	s.writeU32(sizeof(nativeRGBA));
	for(int i = 0; i < (int)sizeof(nativeRGBA); i += 4){
		uint8 bgra[4] = { nativeRGBA[i+2], nativeRGBA[i+1], nativeRGBA[i], nativeRGBA[i+3] };
		s.write8(bgra, 4);
	}
	return s.getLength();
}

static uint32
BuildD3D8DXT1(uint8 *body, uint32 cap)
{
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(Raster::C565 | Raster::MIPMAP);
	s.writeI32(0);
	s.writeU16(8);
	s.writeU16(8);
	s.writeU8(16);
	s.writeU8(4);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(1);
	for(int l = 0; l < 4; l++){
		s.writeU32(nativeDXTSizes[l]);
		for(int i = 0; i < nativeDXTSizes[l]; i++)
			s.writeU8(NativeDXTByte(l, i));
	}
	return s.getLength();
}

static uint32
BuildXboxDXT1(uint8 *body, uint32 cap)
{
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_XBOX);
	s.writeI32(Raster::C565 | Raster::MIPMAP);
	s.writeI16(0);
	s.writeI16(0);
	s.writeU16(8);
	s.writeU16(8);
	s.writeU8(16);
	s.writeU8(4);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0x0C);
	s.writeI32(32 + 8 + 8 + 8);
	for(int l = 0; l < 4; l++)
		for(int i = 0; i < nativeDXTSizes[l]; i++)
			s.writeU8(NativeDXTByte(l, i));
	return s.getLength();
}

static Texture*
ReadNativeOnlyChunk(const uint8 *body, uint32 size)
{
	static uint8 chunk[2048];
	StreamMemory s;
	s.open(chunk, 0, sizeof(chunk));
	writeChunkHeader(&s, ID_STRUCT, size);
	s.write8(body, size);
	s.seek(0, 0);
	Texture *tex = Texture::streamReadNative(&s);
	s.close();
	if(tex == nil || tex->raster == nil){
		Detail("  native read gave texture %p\n", tex);
		return nil;
	}
	return tex;
}

static Texture*
ReadNativeOnly(uint32 (*build)(uint8 *body, uint32 cap))
{
	static uint8 body[2048];
	return ReadNativeOnlyChunk(body, build(body, sizeof(body)));
}

static Texture*
ReadAndConvertChunk(const uint8 *body, uint32 size)
{
	Texture *tex = ReadNativeOnlyChunk(body, size);
	if(tex == nil)
		return nil;
	tex->raster = Raster::convertTexToCurrentPlatform(tex->raster);
	if(tex->raster == nil){
		Detail("  conversion returned nil\n");
		tex->destroy();
		return nil;
	}
	return tex;
}

static Texture*
ReadAndConvertNative(uint32 (*build)(uint8 *body, uint32 cap))
{
	static uint8 body[2048];
	return ReadAndConvertChunk(body, build(body, sizeof(body)));
}

static bool
CheckD3DNativeUncompressed(void)
{
	const char *name = "D3D8 native C8888 texture converts to a top-down RGBA Metal raster";
	Texture *tex = ReadAndConvertNative(BuildD3D8C8888);
	if(tex == nil)
		return Report(false, name);
	Raster *ras = tex->raster;
	bool ok = ras->platform == PLATFORM_METAL && ras->width == 4 && ras->height == 2 &&
	          (ras->format & 0xF00) == Raster::C8888 && ras->getNumLevels() == 1;
	if(!ok)
		Detail("  platform %d size %dx%d format 0x%x levels %d\n", ras->platform,
		       ras->width, ras->height, ras->format & 0xF00, ras->getNumLevels());
	uint8 *px = ok ? ras->lock(0, Raster::LOCKREAD) : nil;
	if(ok && px == nil){
		Detail("  read lock returned nil\n");
		ok = false;
	}
	if(px){
		ok &= BytesEqual(px, nativeRGBA, 16, "row 0");
		ok &= BytesEqual(px + ras->stride, nativeRGBA + 16, 16, "row 1");
		ras->unlock(0);
	}
	tex->destroy();
	return Report(ok, name);
}

struct NativeDXTCase
{
	int dxt;
	bool alpha;
	uint32 format;
	int w, h, levels;
	int sizes[4];
};

static const NativeDXTCase dxt1Case = { 1, false, Raster::C565, 8, 8, 4, { 32, 8, 8, 8 } };
static const NativeDXTCase dxt5Case = { 5, true, Raster::C4444, 12, 4, 4, { 48, 32, 16, 16 } };
static const NativeDXTCase dxt1AlphaCase = { 1, true, Raster::C1555, 4, 8, 2, { 16, 8 } };

static int
LevelDim(int size, int level)
{
	size >>= level;
	return size < 1 ? 1 : size;
}

static const NativeDXTCase *buildDXTCase;

static uint32
BuildD3D8DXTCase(uint8 *body, uint32 cap)
{
	const NativeDXTCase *c = buildDXTCase;
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(c->format | (c->levels > 1 ? Raster::MIPMAP : 0));
	s.writeI32(c->alpha);
	s.writeU16(c->w);
	s.writeU16(c->h);
	s.writeU8(16);
	s.writeU8(c->levels);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(c->dxt);
	for(int l = 0; l < c->levels; l++){
		s.writeU32(c->sizes[l]);
		for(int i = 0; i < c->sizes[l]; i++)
			s.writeU8(NativeDXTByte(l, i));
	}
	return s.getLength();
}

static bool
CheckNativeDXT(uint32 (*build)(uint8 *body, uint32 cap), const NativeDXTCase *c, const char *name)
{
	buildDXTCase = c;
	Texture *tex = ReadAndConvertNative(build);
	if(tex == nil)
		return Report(false, name);
	Raster *ras = tex->raster;
	metal::MetalRaster *natras = ras->platform == PLATFORM_METAL ? MetalExt(ras) : nil;
	bool ok = natras && natras->texture && natras->isCompressed && natras->hasAlpha == c->alpha &&
	          ras->width == c->w && ras->height == c->h && ras->getNumLevels() == c->levels &&
	          (ras->format & 0xF00) == c->format;
	if(!ok)
		Detail("  platform %d size %dx%d format 0x%x levels %d compressed %d alpha %d\n", ras->platform,
		       ras->width, ras->height, ras->format & 0xF00, ras->getNumLevels(),
		       natras ? natras->isCompressed : -1, natras ? natras->hasAlpha : -1);
	for(int l = 0; ok && l < c->levels; l++){
		uint8 want[64];
		for(int i = 0; i < c->sizes[l]; i++)
			want[i] = NativeDXTByte(l, i);
		uint8 *px = ras->lock(l, Raster::LOCKREAD);
		int lw = LevelDim(c->w, l), lh = LevelDim(c->h, l);
		if(px == nil || ras->width != lw || ras->height != lh){
			Detail("  level %d lock %p size %dx%d, expected %dx%d\n", l, px, ras->width, ras->height, lw, lh);
			ok = false;
			if(px)
				ras->unlock(l);
			break;
		}
		char what[32];
		snprintf(what, sizeof(what), "level %d", l);
		ok &= BytesEqual(px, want, c->sizes[l], what);
		ras->unlock(l);
	}
	tex->destroy();
	return Report(ok, name);
}

struct NativePlainCase
{
	const char *name;
	uint32 platform;
	uint32 format;
	int depth;
	int w, h, levels;
	uint8 corner[4][4];
	RGBA stored[4];
};

static const NativePlainCase plainCases[] = {
	{ "D3D8 native C565 8x4 with 2 levels converts directly, top-down", PLATFORM_D3D8, Raster::C565, 16, 8, 4, 2,
	  { { 0x00, 0xF8 }, { 0xE0, 0x07 }, { 0x1F, 0x00 }, { 0xFF, 0xFF } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } } },
	{ "D3D8 native C4444 8x4 with 2 levels converts directly, top-down", PLATFORM_D3D8, Raster::C4444, 16, 8, 4, 2,
	  { { 0x00, 0xFF }, { 0xF0, 0xF0 }, { 0x0F, 0x80 }, { 0x21, 0x43 } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 136 }, { 51, 34, 17, 68 } } },
	{ "D3D8 native C1555 8x4 with 2 levels converts directly, top-down", PLATFORM_D3D8, Raster::C1555, 16, 8, 4, 2,
	  { { 0x00, 0xFC }, { 0xE0, 0x83 }, { 0x1F, 0x00 }, { 0xFF, 0x7F } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 0 }, { 255, 255, 255, 0 } } },
	{ "D3D8 native LUM8 8x4 with 2 levels converts directly, top-down", PLATFORM_D3D8, Raster::LUM8, 8, 8, 4, 2,
	  { { 0x00 }, { 0x40 }, { 0xC8 }, { 0xFF } },
	  { { 0, 0, 0, 255 }, { 64, 64, 64, 255 }, { 200, 200, 200, 255 }, { 255, 255, 255, 255 } } },
	{ "D3D8 native C888 8x4 with 2 levels converts directly, top-down", PLATFORM_D3D8, Raster::C888, 32, 8, 4, 2,
	  { { 0, 0, 255, 0 }, { 0, 255, 0, 7 }, { 255, 0, 0, 9 }, { 3, 2, 1, 0 } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 1, 2, 3, 255 } } },
	{ "Xbox native C8888 8x4 with 4 swizzled levels converts directly, top-down", PLATFORM_XBOX, Raster::C8888, 32, 8, 4, 4,
	  { { 0, 0, 255, 255 }, { 0, 255, 0, 128 }, { 255, 0, 0, 0 }, { 3, 2, 1, 4 } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 128 }, { 0, 0, 255, 0 }, { 1, 2, 3, 4 } } },
	{ "Xbox native C565 8x4 with 4 swizzled levels converts directly, top-down", PLATFORM_XBOX, Raster::C565, 16, 8, 4, 4,
	  { { 0x00, 0xF8 }, { 0xE0, 0x07 }, { 0x1F, 0x00 }, { 0xFF, 0xFF } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } } },
	{ "Xbox native C8888 8x4 with 2 of 4 swizzled levels reports 2 levels", PLATFORM_XBOX, Raster::C8888, 32, 8, 4, 2,
	  { { 0, 0, 255, 255 }, { 0, 255, 0, 128 }, { 255, 0, 0, 0 }, { 3, 2, 1, 4 } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 128 }, { 0, 0, 255, 0 }, { 1, 2, 3, 4 } } },
	{ "D3D9 native C8888 8x4 with 2 of 4 levels reports 2 levels", PLATFORM_D3D9, Raster::C8888, 32, 8, 4, 2,
	  { { 0, 0, 255, 255 }, { 0, 255, 0, 128 }, { 255, 0, 0, 0 }, { 3, 2, 1, 4 } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 128 }, { 0, 0, 255, 0 }, { 1, 2, 3, 4 } } },
	{ "D3D9 native C1555 8x4 with 2 of 4 levels reports 2 levels", PLATFORM_D3D9, Raster::C1555, 16, 8, 4, 2,
	  { { 0x00, 0xFC }, { 0xE0, 0x83 }, { 0x1F, 0x00 }, { 0xFF, 0x7F } },
	  { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 0 }, { 255, 255, 255, 0 } } },
};

static int
CornerIndex(int x, int y, int w, int h)
{
	if(x == 0 && y == 0) return 0;
	if(x == w-1 && y == 0) return 1;
	if(x == 0 && y == h-1) return 2;
	if(x == w-1 && y == h-1) return 3;
	return -1;
}

static void
CornerPos(int k, int w, int h, int *x, int *y)
{
	*x = k & 1 ? w-1 : 0;
	*y = k & 2 ? h-1 : 0;
}

static void
NativePlainPixel(const NativePlainCase *c, int l, int x, int y, uint8 *out)
{
	int bpp = c->depth/8;
	int k = l < 2 ? CornerIndex(x, y, LevelDim(c->w, l), LevelDim(c->h, l)) : -1;
	if(k >= 0){
		memcpy(out, c->corner[l == 0 ? k : 3-k], bpp);
		return;
	}
	for(int i = 0; i < bpp; i++)
		out[i] = (uint8)(l*97 + y*31 + x*7 + i*53 + 5);
}

static int
NativeLockPixel(const NativePlainCase *c, const uint8 *src, uint8 *dst)
{
	switch(c->format){
	case Raster::C8888:
		dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
		return 4;
	case Raster::C888:
		dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
		return 3;
	}
	memcpy(dst, src, c->depth/8);
	return c->depth/8;
}

static int
SwizzleIndex(int x, int y, int w, int h)
{
	int idx = 0, bit = 0;
	for(int i = 1; i < w || i < h; i <<= 1){
		if(i < w){
			if(x & i) idx |= 1 << bit;
			bit++;
		}
		if(i < h){
			if(y & i) idx |= 1 << bit;
			bit++;
		}
	}
	return idx;
}

static const NativePlainCase *buildPlainCase;

static uint32
BuildNativePlain(uint8 *body, uint32 cap)
{
	const NativePlainCase *c = buildPlainCase;
	int bpp = c->depth/8;
	bool alpha = c->format == Raster::C8888 || c->format == Raster::C1555 || c->format == Raster::C4444;
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, c->platform);
	if(c->platform == PLATFORM_XBOX){
		s.writeI32(c->format | Raster::MIPMAP);
		s.writeI16(alpha);
		s.writeI16(0);
	}else if(c->platform == PLATFORM_D3D9){
		int32 d3dformat;
		switch(c->format){
		case Raster::C8888: d3dformat = d3d::D3DFMT_A8R8G8B8; break;
		case Raster::C888: d3dformat = d3d::D3DFMT_X8R8G8B8; break;
		case Raster::C1555: d3dformat = d3d::D3DFMT_A1R5G5B5; break;
		case Raster::C555: d3dformat = d3d::D3DFMT_X1R5G5B5; break;
		case Raster::C565: d3dformat = d3d::D3DFMT_R5G6B5; break;
		case Raster::C4444: d3dformat = d3d::D3DFMT_A4R4G4B4; break;
		default:
			Detail("  no D3D9 format for raster format 0x%x\n", c->format);
			return 0;
		}
		s.writeI32(c->format | Raster::MIPMAP);
		s.writeI32(d3dformat);
	}else{
		s.writeU32(c->format | Raster::MIPMAP);
		s.writeI32(alpha);
	}
	s.writeU16(c->w);
	s.writeU16(c->h);
	s.writeU8(c->depth);
	s.writeU8(c->levels);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	if(c->platform == PLATFORM_XBOX){
		int total = 0;
		for(int l = 0; l < c->levels; l++)
			total += LevelDim(c->w, l)*LevelDim(c->h, l)*bpp;
		s.writeI32(total);
	}
	for(int l = 0; l < c->levels; l++){
		int lw = LevelDim(c->w, l), lh = LevelDim(c->h, l);
		uint8 level[256];
		for(int y = 0; y < lh; y++)
			for(int x = 0; x < lw; x++){
				int i = c->platform == PLATFORM_XBOX ? SwizzleIndex(x, y, lw, lh) : y*lw + x;
				NativePlainPixel(c, l, x, y, &level[i*bpp]);
			}
		if(c->platform != PLATFORM_XBOX)
			s.writeU32(lw*lh*bpp);
		s.write8(level, lw*lh*bpp);
	}
	return s.getLength();
}

static bool
CheckNativePlain(const NativePlainCase *c)
{
	buildPlainCase = c;
	Texture *tex = ReadAndConvertNative(BuildNativePlain);
	if(tex == nil)
		return Report(false, c->name);
	Raster *ras = tex->raster;
	metal::MetalRaster *natras = ras->platform == PLATFORM_METAL ? MetalExt(ras) : nil;
	bool ok = natras && natras->texture && !natras->isCompressed && ras->width == c->w && ras->height == c->h &&
	          (int32)(ras->format & 0xF00) == (int32)c->format && ras->getNumLevels() == c->levels &&
	          TextureLevels(natras->texture) == c->levels && natras->filledLevels == c->levels;
	if(!ok)
		Detail("  platform %d size %dx%d format 0x%x levels %d texture levels %d filled %d\n", ras->platform,
		       ras->width, ras->height, ras->format & 0xF00, ras->getNumLevels(),
		       natras ? TextureLevels(natras->texture) : 0, natras ? natras->filledLevels : 0);
	for(int l = 0; ok && l < c->levels; l++){
		int lw = LevelDim(c->w, l), lh = LevelDim(c->h, l);
		uint8 *px = ras->lock(l, Raster::LOCKREAD);
		if(px == nil || ras->width != lw || ras->height != lh){
			Detail("  level %d lock %p size %dx%d, expected %dx%d\n", l, px, ras->width, ras->height, lw, lh);
			ok = false;
			if(px)
				ras->unlock(l);
			break;
		}
		for(int y = 0; ok && y < lh; y++)
			for(int x = 0; ok && x < lw; x++){
				uint8 src[4], want[4];
				NativePlainPixel(c, l, x, y, src);
				int n = NativeLockPixel(c, src, want);
				char what[48];
				snprintf(what, sizeof(what), "level %d pixel (%d,%d)", l, x, y);
				ok &= BytesEqual(px + y*ras->stride + x*n, want, n, what);
			}
		ras->unlock(l);
		for(int k = 0; ok && l < 2 && k < 4; k++){
			int x, y;
			uint8 texel[4];
			CornerPos(k, lw, lh, &x, &y);
			RGBA want = c->stored[l == 0 ? k : 3-k];
			uint8 wantBytes[4] = { want.red, want.green, want.blue, want.alpha };
			char what[48];
			snprintf(what, sizeof(what), "stored level %d texel (%d,%d)", l, x, y);
			ok = TextureTexelAt(natras->texture, l, x, y, texel) && BytesEqual(texel, wantBytes, 4, what);
		}
	}
	tex->destroy();
	return Report(ok, c->name);
}

static uint32
BuildD3D9A8L8(uint8 *body, uint32 cap)
{
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D9);
	s.writeI32(Raster::C8888);
	s.writeI32(d3d::D3DFMT_A8L8);
	s.writeU16(4);
	s.writeU16(4);
	s.writeU8(16);
	s.writeU8(1);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(8);
	s.writeU32(32);
	for(int i = 0; i < 32; i++)
		s.writeU8(i);
	return s.getLength();
}

static bool
CheckNativeWithoutImage(void)
{
	Texture *tex = ReadAndConvertNative(BuildD3D9A8L8);
	bool ok = tex && tex->raster->platform == PLATFORM_D3D9 && tex->raster->width == 4;
	if(!ok)
		Detail("  texture %p platform %d, expected the D3D9 raster back\n", tex, tex ? tex->raster->platform : -1);
	if(tex)
		tex->destroy();
	return Report(ok, "D3D9 native A8L8 texture that has no image keeps its source raster");
}

static bool
CheckNativeWithoutDevice(void)
{
	void *saved = metal::metalGlobals.context;
	metal::metalGlobals.context = nil;
	Texture *plain = ReadAndConvertNative(BuildD3D8C8888);
	Texture *dxt = ReadAndConvertNative(BuildD3D8DXT1);
	metal::metalGlobals.context = saved;
	bool ok = plain && plain->raster->platform == PLATFORM_D3D8 &&
	          dxt && dxt->raster->platform == PLATFORM_D3D8;
	if(!ok)
		Detail("  plain %p dxt %p, expected both to keep their D3D8 raster\n", plain, dxt);
	if(plain)
		plain->destroy();
	if(dxt)
		dxt->destroy();
	return Report(ok, "native texture conversion without a device keeps the source raster");
}

static const uint8 nativeOpaqueRGBA[] = {
	255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,   10, 20, 30, 255,
	50, 60, 70, 255,  90, 100, 110, 255,  1, 2, 3, 255,   200, 201, 202, 255,
};

static uint32
BuildD3D8OpaqueC8888(uint8 *body, uint32 cap)
{
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(Raster::C8888);
	s.writeI32(0);
	s.writeU16(4);
	s.writeU16(2);
	s.writeU8(32);
	s.writeU8(1);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	s.writeU32(sizeof(nativeOpaqueRGBA));
	for(int i = 0; i < (int)sizeof(nativeOpaqueRGBA); i += 4){
		uint8 bgra[4] = { nativeOpaqueRGBA[i+2], nativeOpaqueRGBA[i+1], nativeOpaqueRGBA[i], nativeOpaqueRGBA[i+3] };
		s.write8(bgra, 4);
	}
	return s.getLength();
}

static bool
CheckD3DNativeOpaqueC8888(void)
{
	const char *name = "D3D8 native opaque C8888 converts directly and stays C8888, not the image path's C888";
	Texture *tex = ReadAndConvertNative(BuildD3D8OpaqueC8888);
	if(tex == nil)
		return Report(false, name);
	Raster *ras = tex->raster;
	bool ok = ras->platform == PLATFORM_METAL && ras->width == 4 && ras->height == 2 &&
	          (ras->format & 0xF00) == Raster::C8888;
	if(!ok)
		Detail("  platform %d size %dx%d format 0x%x\n", ras->platform, ras->width, ras->height, ras->format & 0xF00);
	uint8 *px = ok ? ras->lock(0, Raster::LOCKREAD) : nil;
	if(px){
		ok &= BytesEqual(px, nativeOpaqueRGBA, 16, "row 0");
		ok &= BytesEqual(px + ras->stride, nativeOpaqueRGBA + 16, 16, "row 1");
		ras->unlock(0);
	}else
		ok = false;
	tex->destroy();
	return Report(ok, name);
}

struct NativeAlphaCase
{
	const char *name;
	uint32 format;
	int depth;
	int bpp;
	uint8 texels[8][4];
	bool alpha;
};

static const NativeAlphaCase alphaCases[] = {
	{ "D3D8 native C8888 with every alpha at 255 converts without alpha", Raster::C8888, 32, 4,
	  { { 1, 2, 3, 255 }, { 4, 5, 6, 255 }, { 7, 8, 9, 255 }, { 0, 0, 0, 255 },
	    { 9, 9, 9, 255 }, { 8, 8, 8, 255 }, { 7, 7, 7, 255 }, { 6, 6, 6, 255 } }, false },
	{ "D3D8 native C8888 with one texel at alpha 254 converts with alpha", Raster::C8888, 32, 4,
	  { { 1, 2, 3, 255 }, { 4, 5, 6, 255 }, { 7, 8, 9, 255 }, { 0, 0, 0, 255 },
	    { 9, 9, 9, 255 }, { 8, 8, 8, 255 }, { 7, 7, 7, 255 }, { 6, 6, 6, 254 } }, true },
	{ "D3D8 native C1555 with every alpha bit set converts without alpha", Raster::C1555, 16, 2,
	  { { 0x00, 0xFC }, { 0xE0, 0x83 }, { 0x1F, 0x80 }, { 0xFF, 0xFF },
	    { 0x12, 0x80 }, { 0x34, 0x81 }, { 0x56, 0x82 }, { 0x78, 0x83 } }, false },
	{ "D3D8 native C1555 with one alpha bit clear converts with alpha", Raster::C1555, 16, 2,
	  { { 0x00, 0xFC }, { 0xE0, 0x83 }, { 0x1F, 0x80 }, { 0xFF, 0xFF },
	    { 0x12, 0x80 }, { 0x34, 0x81 }, { 0x56, 0x82 }, { 0x78, 0x03 } }, true },
	{ "D3D8 native C4444 with every alpha at 15 converts without alpha", Raster::C4444, 16, 2,
	  { { 0x00, 0xFF }, { 0xF0, 0xF0 }, { 0x0F, 0xF0 }, { 0x21, 0xF3 },
	    { 0x12, 0xF0 }, { 0x34, 0xF1 }, { 0x56, 0xF2 }, { 0x78, 0xF3 } }, false },
	{ "D3D8 native C4444 with one alpha at 14 converts with alpha", Raster::C4444, 16, 2,
	  { { 0x00, 0xFF }, { 0xF0, 0xF0 }, { 0x0F, 0xF0 }, { 0x21, 0xF3 },
	    { 0x12, 0xF0 }, { 0x34, 0xF1 }, { 0x56, 0xF2 }, { 0x78, 0xE3 } }, true },
	{ "D3D8 native PAL8 whose used palette entries are opaque converts without alpha", Raster::PAL8 | Raster::C8888, 8, 1,
	  { { 0 }, { 1 }, { 2 }, { 3 }, { 0 }, { 1 }, { 2 }, { 3 } }, false },
	{ "D3D8 native PAL8 that uses a transparent palette entry converts with alpha", Raster::PAL8 | Raster::C8888, 8, 1,
	  { { 0 }, { 1 }, { 2 }, { 3 }, { 0 }, { 1 }, { 2 }, { 4 } }, true },
	{ "D3D8 native PAL4 whose used palette entries are opaque converts without alpha", Raster::PAL4 | Raster::C8888, 4, 1,
	  { { 0 }, { 1 }, { 2 }, { 3 }, { 0 }, { 1 }, { 2 }, { 3 } }, false },
	{ "D3D8 native PAL4 that uses a transparent palette entry converts with alpha", Raster::PAL4 | Raster::C8888, 4, 1,
	  { { 0 }, { 1 }, { 2 }, { 3 }, { 0 }, { 1 }, { 2 }, { 4 } }, true },
};

static const NativeAlphaCase *buildAlphaCase;

// entries 0-3 opaque, 4 and up at alpha 0
static uint32
BuildNativeAlpha(uint8 *body, uint32 cap)
{
	const NativeAlphaCase *c = buildAlphaCase;
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(c->format);
	s.writeI32(0);
	s.writeU16(4);
	s.writeU16(2);
	s.writeU8(c->depth);
	s.writeU8(1);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	int pal = c->format & Raster::PAL4 ? 32 : c->format & Raster::PAL8 ? 256 : 0;
	for(int i = 0; i < pal; i++){
		uint8 e[4] = { (uint8)(i*40), (uint8)(i*20), (uint8)(i*10), (uint8)(i < 4 ? 255 : 0) };
		s.write8(e, 4);
	}
	s.writeU32(8*c->bpp);
	for(int i = 0; i < 8; i++)
		s.write8(c->texels[i], c->bpp);
	return s.getLength();
}

static bool
CheckNativeAlpha(const NativeAlphaCase *c)
{
	buildAlphaCase = c;
	Texture *tex = ReadAndConvertNative(BuildNativeAlpha);
	if(tex == nil)
		return Report(false, c->name);
	Raster *ras = tex->raster;
	metal::MetalRaster *natras = ras->platform == PLATFORM_METAL ? MetalExt(ras) : nil;
	bool ok = natras && natras->texture && !!natras->hasAlpha == c->alpha;
	if(!ok)
		Detail("  platform %d format 0x%x alpha %d, expected alpha %d\n", ras->platform, ras->format,
		       natras ? natras->hasAlpha : -1, c->alpha);
	tex->destroy();
	return Report(ok, c->name);
}

static bool
CheckFallbackUnlocksSource(void)
{
	Raster *ras = Raster::create(4, 4, 8, Raster::PAL8 | Raster::C565 | Raster::TEXTURE, PLATFORM_D3D8);
	if(ras == nil)
		return Report(false, "a D3D8 raster that has no image comes back unlocked");
	Raster *out = Raster::convertTexToCurrentPlatform(ras);
	bool ok = out == ras && ras->pixels == nil &&
	          !(ras->privateFlags & (Raster::PRIVATELOCK_READ | Raster::PRIVATELOCK_WRITE));
	if(!ok)
		Detail("  returned %p for %p, pixels %p, lock flags 0x%x\n", out, ras, ras->pixels,
		       ras->privateFlags & (Raster::PRIVATELOCK_READ | Raster::PRIVATELOCK_WRITE));
	out->destroy();
	return Report(ok, "a D3D8 raster that has no image comes back unlocked");
}

static void
CheckNativeTextures(void)
{
	CheckD3DNativeUncompressed();
	CheckD3DNativeOpaqueC8888();
	CheckFallbackUnlocksSource();
	CheckNativeDXT(BuildD3D8DXT1, &dxt1Case, "D3D8 native DXT1 texture keeps its 4 levels as BC1, unflipped");
	CheckNativeDXT(BuildXboxDXT1, &dxt1Case, "Xbox native DXT1 texture keeps its 4 levels as BC1, unflipped");
	CheckNativeDXT(BuildD3D8DXTCase, &dxt5Case, "D3D8 native DXT5 12x4 keeps its 4 levels of 48, 32, 16 and 16 bytes");
	CheckNativeDXT(BuildD3D8DXTCase, &dxt1AlphaCase, "D3D8 native DXT1 4x8 with alpha keeps its alpha and 2 levels");
	for(int i = 0; i < (int)(sizeof(plainCases)/sizeof(plainCases[0])); i++)
		CheckNativePlain(&plainCases[i]);
	for(int i = 0; i < (int)(sizeof(alphaCases)/sizeof(alphaCases[0])); i++)
		CheckNativeAlpha(&alphaCases[i]);
	CheckNativeWithoutImage();
	CheckNativeWithoutDevice();
}

static bool
CheckLockWriteChangesPixels(void)
{
	static const uint8 rgba[] = {
		255, 0, 0, 255,   0, 255, 0, 128,   0, 0, 255, 0,
		10, 20, 30, 40,   50, 60, 70, 80,   90, 100, 110, 120,
	};
	uint8 want[sizeof(rgba)];
	memcpy(want, rgba, sizeof(rgba));
	want[16] = 1; want[17] = 2; want[18] = 3; want[19] = 4;

	Image *img = MakeImage(32, 3, 2, rgba, nil, 0);
	Raster *ras = Raster::createFromImage(img);
	img->destroy();
	bool ok = ras != nil;
	if(ok){
		uint8 *px = ras->lock(0, Raster::LOCKWRITE);
		ok = px != nil && BytesEqual(px, rgba, sizeof(rgba), "fetched on write lock");
		if(px){
			memcpy(px + ras->stride + 4, want + 16, 4);
			ras->unlock(0);
		}
		Image *back = ras->toImage();
		ok &= back != nil && BytesEqual(back->pixels, want, sizeof(want), "after unlock");
		if(back)
			back->destroy();
		ras->destroy();
	}
	return Report(ok, "write lock changes one pixel, the rest stay");
}

static bool
PixelNear(const uint8 *p, int r, int g, int b, int a, int tol, const char *what)
{
	int want[4] = { r, g, b, a };
	for(int i = 0; i < 4; i++){
		int d = p[i] - want[i];
		if(d < -tol || d > tol){
			Detail("  %s is %d,%d,%d,%d, expected %d,%d,%d,%d\n", what, p[0], p[1], p[2], p[3], r, g, b, a);
			return false;
		}
	}
	return true;
}

static bool
CheckMipmapGeneration(void)
{
	static const uint8 R[4] = { 200, 0, 0, 255 }, G[4] = { 0, 200, 0, 255 };
	static const uint8 B[4] = { 0, 0, 200, 255 }, W[4] = { 100, 100, 100, 255 };
	uint8 pixels[4*4*4];
	for(int y = 0; y < 4; y++)
		for(int x = 0; x < 4; x++){
			const uint8 *c = y < 2 ? (x < 2 ? R : G) : (x < 2 ? B : W);
			memcpy(&pixels[(y*4 + x)*4], c, 4);
		}
	Image *img = MakeImage(32, 4, 4, pixels, nil, 0);
	Raster *ras = Raster::create(4, 4, 32, Raster::C8888 | Raster::TEXTURE | Raster::MIPMAP | Raster::AUTOMIPMAP);
	bool ok = ras != nil && ras->setFromImage(img) != nil;
	img->destroy();
	if(!ok){
		Detail("  create or setFromImage failed\n");
		if(ras)
			ras->destroy();
		return Report(false, "automatic mipmaps generate 3 levels from a 4x4 texture");
	}
	if(ras->getNumLevels() != 1){
		Detail("  %d lockable levels, expected 1\n", ras->getNumLevels());
		ok = false;
	}
	uint8 *px = ras->lock(1, Raster::LOCKREAD);
	if(px == nil || ras->width != 2 || ras->height != 2){
		Detail("  level 1 lock %p size %dx%d\n", px, ras->width, ras->height);
		ok = false;
	}else{
		ok &= PixelNear(px, 200, 0, 0, 255, 0, "level 1 top left");
		ok &= PixelNear(px + 4, 0, 200, 0, 255, 0, "level 1 top right");
		ok &= PixelNear(px + ras->stride, 0, 0, 200, 255, 0, "level 1 bottom left");
		ok &= PixelNear(px + ras->stride + 4, 100, 100, 100, 255, 0, "level 1 bottom right");
	}
	if(px)
		ras->unlock(1);
	px = ras->lock(2, Raster::LOCKREAD);
	if(px == nil || ras->width != 1 || ras->height != 1){
		Detail("  level 2 lock %p\n", px);
		ok = false;
	}else
		ok &= PixelNear(px, 75, 75, 75, 255, 1, "level 2");
	if(px)
		ras->unlock(2);
	px = ras->lock(3, Raster::LOCKREAD);
	if(px){
		Detail("  level 3 locked, expected only 3 levels\n");
		ras->unlock(3);
		ok = false;
	}
	ras->destroy();

	ras = Raster::create(5, 3, 32, Raster::C8888 | Raster::TEXTURE | Raster::MIPMAP);
	if(ras == nil || ras->getNumLevels() != 3){
		Detail("  5x3 MIPMAP raster has %d levels, expected 3\n", ras ? ras->getNumLevels() : 0);
		ok = false;
	}
	if(ras)
		ras->destroy();
	return Report(ok, "automatic mipmaps generate 3 levels from a 4x4 texture");
}

static bool
CheckCameraTexture(void)
{
	bool ok = true;
	Raster *ct = Raster::create(8, 4, 0, Raster::CAMERATEXTURE);
	if(ct == nil){
		Detail("  Raster::create returned nil\n");
		return Report(false, "camera texture: default format, write, read, clear");
	}
	metal::MetalRaster *natras = MetalExt(ct);
	if((ct->format & 0xF00) != Raster::C888 || ct->stride != 8*3 || natras->hasAlpha){
		Detail("  format 0x%x stride %d alpha %d\n", ct->format, ct->stride, natras->hasAlpha);
		ok = false;
	}
	void *sample = metal::getRasterSampleTexture(ct);
	if(sample == nil || sample == natras->texture){
		Detail("  sample texture %p, texture %p; expected a separate view\n", sample, natras->texture);
		ok = false;
	}

	uint8 data[8*4*3];
	for(int i = 0; i < (int)sizeof(data); i++)
		data[i] = (uint8)(i*13 + 5);
	uint8 *px = ct->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(px){
		memcpy(px, data, sizeof(data));
		ct->unlock(0);
		px = ct->lock(0, Raster::LOCKREAD);
		ok &= px != nil && BytesEqual(px, data, sizeof(data), "written pixels");
		if(px)
			ct->unlock(0);
	}else{
		Detail("  write lock returned nil\n");
		ok = false;
	}

	Raster *fb = ctx.camera->frameBuffer, *zb = ctx.camera->zBuffer;
	ctx.camera->frameBuffer = ct;
	ctx.camera->zBuffer = nil;
	Clear(RED, Camera::CLEARIMAGE);
	ctx.camera->frameBuffer = fb;
	ctx.camera->zBuffer = zb;
	if(!metal::rasterHasPendingWork(ct)){
		Detail("  clear left nothing pending for the camera texture\n");
		ok = false;
	}
	metal::resolveRasterTarget(ct);
	if(metal::rasterHasPendingWork(ct)){
		Detail("  resolveRasterTarget left work pending or a pass open\n");
		ok = false;
	}
	Image *img = ct->toImage();
	if(img == nil || img->depth != 24){
		Detail("  toImage after clear gave %p depth %d\n", img, img ? img->depth : 0);
		ok = false;
	}else{
		uint8 red[8*4*3];
		for(int i = 0; i < 8*4; i++){
			red[i*3] = 255;
			red[i*3+1] = 0;
			red[i*3+2] = 0;
		}
		ok &= BytesEqual(img->pixels, red, sizeof(red), "after clear");
	}
	if(img)
		img->destroy();
	ct->destroy();

	ct = Raster::create(8, 4, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	if(ct == nil || metal::getRasterSampleTexture(ct) != MetalExt(ct)->texture){
		Detail("  C8888 camera texture should sample its own texture\n");
		ok = false;
	}
	if(ct)
		ct->destroy();
	return Report(ok, "camera texture: default format, write, read, clear");
}

static bool
WhiteTexel(void *white)
{
	int w = 0, h = 0;
	uint8 texel[4] = { 0, 0, 0, 0 };
	if(!TextureTexel(white, &w, &h, texel) || w != 1 || h != 1 ||
	   texel[0] != 255 || texel[1] != 255 || texel[2] != 255 || texel[3] != 255){
		Detail("  white texture %p is %dx%d with texel %d,%d,%d,%d\n", white, w, h,
		       texel[0], texel[1], texel[2], texel[3]);
		return false;
	}
	return true;
}

static bool
CheckWhiteTexture(void)
{
	void *white = metal::getWhiteTexture();
	bool ok = white != nil && metal::getWhiteTexture() == white;
	if(!ok)
		Detail("  white texture %p\n", white);
	ok &= WhiteTexel(white);
	return Report(ok, "one 1x1 white texture for empty stages");
}

static bool
FieldsAreDefaults(Raster *ras, const char *name)
{
	metal::MetalRaster *n = MetalExt(ras);
	bool ok = n->texture == nil && n->sampleTexture == nil && n->format == 0 && n->bpp == 0 &&
		!n->isCompressed && !n->hasAlpha && !n->autogenMipmap && n->numLevels == 1 &&
		n->filterMode == 0 && n->addressU == 0 && n->addressV == 0 &&
		n->maxAnisotropy == 1 && n->filledMask == 0 && n->filledLevels == 0;
	if(!ok)
		Detail("  %s: texture %p view %p format %d bpp %d compressed %d alpha %d autogen %d levels %d "
		       "filter %d address %d,%d aniso %d filled mask 0x%x filled %d\n", name,
		       n->texture, n->sampleTexture, n->format, n->bpp, n->isCompressed, n->hasAlpha,
		       n->autogenMipmap, n->numLevels, n->filterMode, n->addressU, n->addressV,
		       n->maxAnisotropy, n->filledMask, n->filledLevels);
	return ok;
}

static bool
CheckUnallocatedRasterDefaults(void)
{
	bool ok = true;
	Raster *ras = Raster::create(8, 4, 32, Raster::C8888 | Raster::TEXTURE | Raster::AUTOMIPMAP | Raster::MIPMAP);
	if(ras)
		ras->destroy();
	ras = Raster::create(8, 4, 0, Raster::CAMERATEXTURE | Raster::DONTALLOCATE);
	if(ras == nil){
		Detail("  DONTALLOCATE camera texture was not created\n");
		ok = false;
	}else{
		ok &= FieldsAreDefaults(ras, "DONTALLOCATE camera texture");
		ras->destroy();
	}
	ras = Raster::create(0, 0, 32, Raster::C8888 | Raster::TEXTURE);
	if(ras == nil){
		Detail("  zero-size texture was not created\n");
		ok = false;
	}else{
		ok &= FieldsAreDefaults(ras, "zero-size texture");
		ras->destroy();
	}
	return Report(ok, "rasters without storage start with default sampler fields");
}

static bool
CheckImageIntoRasterWithoutTexture(void)
{
	bool ok = true;
	Raster *ras = Raster::create(4, 4, 32, Raster::C8888 | Raster::TEXTURE | Raster::DONTALLOCATE);
	Image *img = Image::create(4, 4, 32);
	img->allocate();
	memset(img->pixels, 0x80, img->stride*img->height);
	if(ras == nil){
		Detail("  DONTALLOCATE texture was not created\n");
		ok = false;
	}else{
		if(metal::rasterFromImage(ras, img)){
			Detail("  rasterFromImage reported success without a texture\n");
			ok = false;
		}
		if(ras->pixels != nil){
			Detail("  raster left locked\n");
			ok = false;
		}
		ras->destroy();
	}
	img->destroy();
	return Report(ok, "image into a raster without a texture fails cleanly");
}

static bool
CheckDestroyEvictsRaster(void)
{
	Raster *ras = Raster::create(4, 4, 32, Raster::C8888 | Raster::TEXTURE);
	if(ras == nil)
		return Report(false, "destroying a bound raster clears its stage");
	SetRenderStatePtr(TEXTURERASTER, ras);
	bool ok = GetRenderStatePtr(TEXTURERASTER) == ras;
	ras->destroy();
	void *bound = GetRenderStatePtr(TEXTURERASTER);
	if(bound != nil){
		Detail("  stage 0 still holds %p\n", bound);
		ok = false;
	}
	return Report(ok, "destroying a bound raster clears its stage");
}

using metal::Im2DVertex;

static const RGBA WHITE = { 255, 255, 255, 255 };

static Im2DVertex
Vert(float x, float y, RGBA c, float u = 0.0f, float v = 0.0f)
{
	Im2DVertex vt;
	float recipz = 1.0f/ctx.camera->nearPlane;
	vt.setScreenX(x);
	vt.setScreenY(y);
	vt.setScreenZ(im2d::GetNearZ());
	vt.setCameraZ(ctx.camera->nearPlane);
	vt.setRecipCameraZ(recipz);
	vt.setColor(c.red, c.green, c.blue, c.alpha);
	vt.setU(u, recipz);
	vt.setV(v, recipz);
	return vt;
}

static void
Quad(Im2DVertex *v, float x0, float y0, float x1, float y1, RGBA c,
     float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f)
{
	v[0] = Vert(x0, y0, c, u0, v0);
	v[1] = Vert(x1, y0, c, u1, v0);
	v[2] = Vert(x1, y1, c, u1, v1);
	v[3] = Vert(x0, y1, c, u0, v1);
}

static void
DrawQuad(float x0, float y0, float x1, float y1, RGBA c)
{
	Im2DVertex v[4];
	Quad(v, x0, y0, x1, y1, c);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
}

static void
Set2DState(void)
{
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(ZWRITEENABLE, 0);
	SetRenderState(CULLMODE, CULLNONE);
	SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
	SetRenderState(ALPHATESTREF, 10);
	SetRenderState(TEXTUREFILTER, Texture::NEAREST);
	SetRenderState(TEXTUREADDRESS, Texture::WRAP);
	SetRenderState(FOGENABLE, 0);
}

static void
BeginFrame(RGBA col)
{
	Clear(col, Camera::CLEARIMAGE | Camera::CLEARZ | Camera::CLEARSTENCIL);
	ctx.camera->beginUpdate();
	Set2DState();
}

static void
EndFrame(void)
{
	Set2DState();
	ctx.camera->endUpdate();
	ctx.camera->showRaster(0);
}

static bool
PixelNearRGBA(Image *img, int x, int y, RGBA want, int tol)
{
	uint8 *p = &img->pixels[y*img->stride + x*4];
	int w[4] = { want.red, want.green, want.blue, want.alpha };
	for(int i = 0; i < 4; i++){
		int d = p[i] - w[i];
		if(d < -tol || d > tol){
			Detail("  pixel (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,%d within %d\n", x, y,
			       p[0], p[1], p[2], p[3], want.red, want.green, want.blue, want.alpha, tol);
			return false;
		}
	}
	return true;
}

static int
CountColour(Image *img, RGBA c)
{
	int n = 0;
	for(int y = 0; y < img->height; y++)
		for(int x = 0; x < img->width; x++){
			uint8 *p = &img->pixels[y*img->stride + x*4];
			if(p[0] == c.red && p[1] == c.green && p[2] == c.blue && p[3] == c.alpha)
				n++;
		}
	return n;
}

static bool
RectIs(Image *img, int x0, int y0, int x1, int y1, RGBA in, RGBA out)
{
	bool ok = true;
	for(int y = y0; ok && y < y1; y++)
		for(int x = x0; ok && x < x1; x++)
			ok &= PixelIs(img, x, y, in);
	for(int x = x0; x < x1; x++){
		ok &= PixelIs(img, x, y0-1, out);
		ok &= PixelIs(img, x, y1, out);
	}
	for(int y = y0; y < y1; y++){
		ok &= PixelIs(img, x0-1, y, out);
		ok &= PixelIs(img, x1, y, out);
	}
	return ok;
}

static bool
CheckScissorAfterSubRectClear(void)
{
	Raster *parent = ctx.camera->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 10, 20, 16, 10 };
	sub->subRaster(parent, &r);

	BeginFrame(GREY);
	ctx.camera->frameBuffer = sub;
	Clear(RED, Camera::CLEARIMAGE);
	ctx.camera->frameBuffer = parent;
	DrawQuad(0.0f, 0.0f, (float)parent->width, (float)parent->height, GREEN);
	EndFrame();
	sub->destroy();
	return Report(CornersAndCentreAre(GREEN), "a full-target im2d quad after a sub-rectangle clear is not clipped");
}

static bool
CheckDrawAfterSubRasterClearKeepsTarget(void)
{
	Raster *parent = ctx.camera->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 10, 20, 16, 10 };
	sub->subRaster(parent, &r);

	BeginFrame(GREY);
	ctx.camera->frameBuffer = sub;
	Clear(RED, Camera::CLEARIMAGE);
	ctx.camera->frameBuffer = parent;
	DrawQuad(100.0f, 100.0f, 120.0f, 110.0f, GREEN);
	EndFrame();
	sub->destroy();

	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= RectIs(img, 100, 100, 120, 110, GREEN, GREY);
		ok &= PixelIs(img, 10, 20, RED);
		ok &= PixelIs(img, 25, 29, RED);
		int n = CountColour(img, GREEN);
		if(n != 20*10){
			Detail("  %d pixels have the quad colour, expected %d\n", n, 20*10);
			ok = false;
		}
		img->destroy();
	}
	return Report(ok, "a draw after clearing a sub-raster keeps the open camera's viewport and scissor");
}

static bool
CheckIm2DSolidQuad(void)
{
	static const RGBA col = { 200, 100, 50, 255 };
	BeginFrame(GREY);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	DrawQuad(100.0f, 50.0f, 164.0f, 90.0f, col);
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= RectIs(img, 100, 50, 164, 90, col, GREY);
		int n = CountColour(img, col);
		if(n != 64*40){
			Detail("  %d pixels have the quad colour, expected %d\n", n, 64*40);
			ok = false;
		}
		img->destroy();
	}
	return Report(ok, "im2d fan quad covers exactly 64x40 at 100,50 with the near screen z");
}

static RGBA
TexelColour(int i, int j)
{
	RGBA c = { (uint8)(10 + i*60), (uint8)(20 + j*60), (uint8)(250 - (i + j*4)*15), 255 };
	return c;
}

static Raster*
MakeTexelRaster(void)
{
	Raster *ras = Raster::create(4, 4, 32, Raster::C8888 | Raster::TEXTURE);
	if(ras == nil)
		return nil;
	uint8 *px = ras->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(px == nil){
		ras->destroy();
		return nil;
	}
	for(int j = 0; j < 4; j++)
		for(int i = 0; i < 4; i++){
			RGBA c = TexelColour(i, j);
			uint8 *p = px + j*ras->stride + i*4;
			p[0] = c.red;
			p[1] = c.green;
			p[2] = c.blue;
			p[3] = c.alpha;
		}
	ras->unlock(0);
	return ras;
}

static bool
CheckIm2DTexturedQuad(void)
{
	Raster *ras = MakeTexelRaster();
	if(ras == nil)
		return Report(false, "im2d textured quad shows each texel as a 4x4 block");
	Im2DVertex v[4];
	BeginFrame(GREY);
	SetRenderStatePtr(TEXTURERASTER, ras);
	SetRenderState(TEXTUREFILTER, Texture::NEAREST);
	Quad(v, 200.0f, 100.0f, 216.0f, 116.0f, WHITE);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		for(int y = 0; ok && y < 16; y++)
			for(int x = 0; ok && x < 16; x++)
				ok &= PixelIs(img, 200+x, 100+y, TexelColour(x/4, y/4));
		ok &= PixelIs(img, 199, 100, GREY);
		ok &= PixelIs(img, 216, 115, GREY);
		ok &= PixelIs(img, 200, 99, GREY);
		ok &= PixelIs(img, 215, 116, GREY);
		img->destroy();
	}
	ras->destroy();
	return Report(ok, "im2d textured quad shows each texel as a 4x4 block");
}

static float
EdgeFn(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax)*(py - ay) - (by - ay)*(px - ax);
}

static bool
Barycentric(const Im2DVertex &a, const Im2DVertex &b, const Im2DVertex &c, float px, float py, RGBA *out)
{
	float area = EdgeFn(a.x, a.y, b.x, b.y, c.x, c.y);
	float wa = EdgeFn(b.x, b.y, c.x, c.y, px, py)/area;
	float wb = EdgeFn(c.x, c.y, a.x, a.y, px, py)/area;
	float wc = EdgeFn(a.x, a.y, b.x, b.y, px, py)/area;
	if(wa < 0.0f || wb < 0.0f || wc < 0.0f)
		return false;
	out->red = (uint8)(wa*a.r + wb*b.r + wc*c.r + 0.5f);
	out->green = (uint8)(wa*a.g + wb*b.g + wc*c.g + 0.5f);
	out->blue = (uint8)(wa*a.b + wb*b.b + wc*c.b + 0.5f);
	out->alpha = (uint8)(wa*a.a + wb*b.a + wc*c.a + 0.5f);
	return true;
}

static bool
CheckIm2DVertexColours(void)
{
	static const RGBA corner[4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
	Im2DVertex v[4];
	Quad(v, 300.0f, 200.0f, 364.0f, 264.0f, WHITE);
	for(int i = 0; i < 4; i++)
		v[i].setColor(corner[i].red, corner[i].green, corner[i].blue, corner[i].alpha);
	BeginFrame(GREY);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		static const int pts[][2] = { { 300, 200 }, { 363, 200 }, { 363, 263 }, { 300, 263 }, { 332, 232 }, { 340, 210 }, { 310, 250 } };
		for(int i = 0; i < 7; i++){
			float px = pts[i][0] + 0.5f, py = pts[i][1] + 0.5f;
			RGBA want;
			if(!Barycentric(v[0], v[1], v[2], px, py, &want))
				Barycentric(v[0], v[2], v[3], px, py, &want);
			ok &= PixelNearRGBA(img, pts[i][0], pts[i][1], want, 1);
		}
		img->destroy();
	}
	return Report(ok, "im2d vertex colours interpolate across the fan's two triangles");
}

static bool
BlendedQuadIs(void (*setup)(void), RGBA want, int tol, const char *name)
{
	static const RGBA halfRed = { 255, 0, 0, 128 };
	BeginFrame(BLUE);
	setup();
	DrawQuad(20.0f, 20.0f, 60.0f, 60.0f, halfRed);
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= PixelNearRGBA(img, 20, 20, want, tol);
		ok &= PixelNearRGBA(img, 59, 59, want, tol);
		ok &= PixelNearRGBA(img, 40, 40, want, tol);
		ok &= PixelIs(img, 19, 20, BLUE);
		ok &= PixelIs(img, 60, 59, BLUE);
		img->destroy();
	}
	return Report(ok, name);
}

static void BlendDefault(void) { SetRenderState(VERTEXALPHA, 1); }
static void BlendExplicit(void)
{
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDONE);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
}
static void BlendAdditive(void)
{
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDONE);
}
static void BlendOff(void) { SetRenderState(VERTEXALPHA, 0); }

static void
CheckIm2DBlending(void)
{
	static const RGBA blended = { 128, 0, 127, 191 };
	static const RGBA additive = { 255, 0, 255, 255 };
	static const RGBA unblended = { 255, 0, 0, 128 };
	BlendedQuadIs(BlendDefault, blended, 1, "im2d half-transparent quad blends with the engine's default blend");
	BlendedQuadIs(BlendExplicit, blended, 1, "im2d half-transparent quad blends with source alpha, inverse source alpha");
	BlendedQuadIs(BlendAdditive, additive, 0, "im2d half-transparent quad adds with ONE, ONE");
	BlendedQuadIs(BlendOff, unblended, 0, "im2d quad without vertex alpha writes its colour unblended");
}

static bool
CheckIm2DAlphaTest(void)
{
	static const uint8 alphas[4] = { 0, 9, 10, 255 };
	Raster *ras = Raster::create(4, 1, 32, Raster::C8888 | Raster::TEXTURE);
	uint8 *px = ras ? ras->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH) : nil;
	if(px == nil){
		if(ras)
			ras->destroy();
		return Report(false, "im2d alpha test discards texels below the reference");
	}
	for(int i = 0; i < 4; i++){
		px[i*4] = 0;
		px[i*4+1] = 255;
		px[i*4+2] = 0;
		px[i*4+3] = alphas[i];
	}
	ras->unlock(0);

	static const RGBA green = { 0, 255, 0, 255 };
	static const RGBA ten = { 0, 10, 245, 245 };
	bool ok = true;
	for(int pass = 0; pass < 2; pass++){
		Im2DVertex v[4];
		BeginFrame(BLUE);
		SetRenderStatePtr(TEXTURERASTER, ras);
		if(pass == 1)
			SetRenderState(ALPHATESTREF, 128);
		Quad(v, 100.0f, 300.0f, 116.0f, 304.0f, WHITE);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
		EndFrame();
		Image *img = ReadBack();
		if(img == nil){
			ok = false;
			break;
		}
		ok &= PixelIs(img, 101, 301, BLUE);
		ok &= PixelIs(img, 105, 301, BLUE);
		if(pass == 0)
			ok &= PixelNearRGBA(img, 109, 301, ten, 1);
		else
			ok &= PixelIs(img, 109, 301, BLUE);
		ok &= PixelIs(img, 113, 301, green);
		img->destroy();
	}
	ras->destroy();
	return Report(ok, "im2d alpha test discards texels below the reference");
}

static bool
CheckIm2DSubRaster(void)
{
	static const RGBA col = { 10, 200, 30, 255 };
	Raster *parent = ctx.camera->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 40, 300, 32, 16 };
	sub->subRaster(parent, &r);

	Clear(GREY, Camera::CLEARIMAGE | Camera::CLEARZ);
	ctx.camera->frameBuffer = sub;
	ctx.camera->beginUpdate();
	Set2DState();
	DrawQuad(-10.0f, -10.0f, 20.0f, 8.0f, col);
	DrawQuad(24.0f, 10.0f, 50.0f, 30.0f, col);
	ctx.camera->endUpdate();
	ctx.camera->frameBuffer = parent;
	ctx.camera->showRaster(0);
	sub->destroy();

	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= PixelIs(img, 40, 300, col);
		ok &= PixelIs(img, 59, 307, col);
		ok &= PixelIs(img, 60, 300, GREY);
		ok &= PixelIs(img, 40, 308, GREY);
		ok &= PixelIs(img, 39, 300, GREY);
		ok &= PixelIs(img, 40, 299, GREY);
		ok &= PixelIs(img, 64, 310, col);
		ok &= PixelIs(img, 71, 315, col);
		ok &= PixelIs(img, 72, 315, GREY);
		ok &= PixelIs(img, 71, 316, GREY);
		ok &= PixelIs(img, 63, 310, GREY);
		int n = CountColour(img, col);
		if(n != 20*8 + 8*6){
			Detail("  %d pixels have the quad colour, expected %d\n", n, 20*8 + 8*6);
			ok = false;
		}
		img->destroy();
	}
	return Report(ok, "im2d into a 32x16 sub-raster camera is offset and clipped to it");
}

static bool
CheckIm2DLineAndTriangle(void)
{
	static const RGBA col = { 250, 250, 0, 255 };
	Im2DVertex line[4] = {
		Vert(50.0f, 400.5f, col), Vert(600.0f, 10.0f, RED), Vert(90.0f, 400.5f, col), Vert(610.0f, 20.0f, RED),
	};
	Im2DVertex tri[4] = {
		Vert(400.0f, 400.0f, col), Vert(440.0f, 400.0f, col), Vert(400.0f, 440.0f, col), Vert(600.0f, 470.0f, RED),
	};
	BeginFrame(GREY);
	im2d::RenderLine(line, 4, 0, 2);
	im2d::RenderTriangle(tri, 4, 0, 1, 2);
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		for(int x = 52; x < 88; x++)
			ok &= PixelIs(img, x, 400, col);
		ok &= PixelIs(img, 70, 399, GREY);
		ok &= PixelIs(img, 70, 401, GREY);
		ok &= PixelIs(img, 95, 400, GREY);
		ok &= PixelIs(img, 45, 400, GREY);
		ok &= PixelIs(img, 400, 400, col);
		ok &= PixelIs(img, 430, 405, col);
		ok &= PixelIs(img, 405, 430, col);
		ok &= PixelIs(img, 436, 410, GREY);
		ok &= PixelIs(img, 399, 420, GREY);
		ok &= PixelIs(img, 420, 399, GREY);
		if(CountColour(img, RED) != 0){
			Detail("  vertices the calls did not name were drawn\n");
			ok = false;
		}
		img->destroy();
	}
	return Report(ok, "im2d RenderLine and RenderTriangle draw only the named vertices");
}

static RGBA
AddressTexel(int x, int y, bool clamp)
{
	int i = x/4, j = y/4;
	if(clamp){
		i = i > 3 ? 3 : i;
		j = j > 3 ? 3 : j;
	}
	return TexelColour(i % 4, j % 4);
}

static bool
CheckIm2DAddressing(void)
{
	Raster *ras = MakeTexelRaster();
	if(ras == nil)
		return Report(false, "im2d texture coordinates past 1 wrap and clamp");
	bool ok = true;
	for(int pass = 0; pass < 2; pass++){
		bool clamp = pass == 1 && !metal::gl3Addressing;
		Im2DVertex v[4];
		BeginFrame(GREY);
		SetRenderStatePtr(TEXTURERASTER, ras);
		SetRenderState(TEXTUREADDRESS, pass == 1 ? Texture::CLAMP : Texture::WRAP);
		Quad(v, 500.0f, 100.0f, 532.0f, 132.0f, WHITE, 0.0f, 0.0f, 2.0f, 2.0f);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
		EndFrame();
		Image *img = ReadBack();
		if(img == nil){
			ok = false;
			break;
		}
		for(int y = 0; ok && y < 32; y++)
			for(int x = 0; ok && x < 32; x++)
				ok &= PixelIs(img, 500+x, 100+y, AddressTexel(x, y, clamp));
		img->destroy();
	}
	ras->destroy();
	return Report(ok, metal::gl3Addressing ? "im2d texture coordinates past 1 wrap, CLAMP set after binding follows gl3"
	                                       : "im2d texture coordinates past 1 wrap and clamp");
}

struct PrimCase
{
	PrimitiveType type;
	const char *name;
	int numVerts;
	float xy[6][2];
	int lit[3][2];
	int unlit[4][2];
};

static const PrimCase primCases[] = {
	{ PRIMTYPELINELIST, "line list", 4,
	  { { 20.0f, 150.5f }, { 60.0f, 150.5f }, { 20.0f, 160.5f }, { 60.0f, 160.5f } },
	  { { 30, 150 }, { 50, 160 }, { 40, 150 } },
	  { { 30, 151 }, { 30, 149 }, { 30, 155 }, { 65, 150 } } },
	{ PRIMTYPEPOLYLINE, "polyline", 3,
	  { { 80.0f, 150.5f }, { 120.5f, 150.5f }, { 120.5f, 190.0f } },
	  { { 100, 150 }, { 120, 170 }, { 90, 150 } },
	  { { 100, 151 }, { 119, 170 }, { 121, 170 }, { 100, 170 } } },
	{ PRIMTYPETRILIST, "triangle list", 6,
	  { { 140.0f, 150.0f }, { 180.0f, 150.0f }, { 180.0f, 190.0f }, { 140.0f, 150.0f }, { 180.0f, 190.0f }, { 140.0f, 190.0f } },
	  { { 140, 150 }, { 179, 189 }, { 140, 189 } },
	  { { 139, 150 }, { 180, 150 }, { 140, 149 }, { 140, 190 } } },
	{ PRIMTYPETRISTRIP, "triangle strip", 4,
	  { { 200.0f, 150.0f }, { 200.0f, 190.0f }, { 240.0f, 150.0f }, { 240.0f, 190.0f } },
	  { { 200, 150 }, { 239, 189 }, { 239, 150 } },
	  { { 199, 150 }, { 240, 150 }, { 200, 149 }, { 200, 190 } } },
	{ PRIMTYPETRIFAN, "triangle fan", 6,
	  { { 260.0f, 150.0f }, { 300.0f, 150.0f }, { 300.0f, 170.0f }, { 300.0f, 190.0f }, { 260.0f, 190.0f }, { 260.0f, 170.0f } },
	  { { 260, 150 }, { 299, 189 }, { 260, 189 } },
	  { { 259, 150 }, { 300, 150 }, { 260, 149 }, { 260, 190 } } },
	{ PRIMTYPEPOINTLIST, "point list", 3,
	  { { 320.5f, 150.5f }, { 330.5f, 160.5f }, { 340.5f, 170.5f } },
	  { { 320, 150 }, { 330, 160 }, { 340, 170 } },
	  { { 321, 150 }, { 320, 151 }, { 331, 160 }, { 339, 170 } } },
};

static bool
CheckIm2DPrimitives(bool indexed)
{
	static const RGBA col = { 20, 40, 220, 255 };
	const int n = sizeof(primCases)/sizeof(primCases[0]);
	BeginFrame(GREY);
	for(int c = 0; c < n; c++){
		const PrimCase *pc = &primCases[c];
		Im2DVertex v[8];
		uint16 idx[8];
		for(int i = 0; i < pc->numVerts; i++){
			int slot = indexed ? pc->numVerts-1-i : i;
			v[slot] = Vert(pc->xy[i][0], pc->xy[i][1], col);
			idx[i] = slot;
		}
		v[pc->numVerts] = Vert(600.0f, 460.0f, RED);
		if(indexed)
			im2d::RenderIndexedPrimitive(pc->type, v, pc->numVerts+1, idx, pc->numVerts);
		else
			im2d::RenderPrimitive(pc->type, v, pc->numVerts);
	}
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	for(int c = 0; ok && c < n; c++){
		const PrimCase *pc = &primCases[c];
		bool caseOk = true;
		for(int i = 0; i < 3; i++)
			caseOk &= PixelIs(img, pc->lit[i][0], pc->lit[i][1], col);
		for(int i = 0; i < 4; i++)
			caseOk &= PixelIs(img, pc->unlit[i][0], pc->unlit[i][1], GREY);
		if(!caseOk)
			Detail("  in the %s\n", pc->name);
		ok &= caseOk;
	}
	if(img){
		ok &= PixelIs(img, 600, 460, GREY);
		img->destroy();
	}
	return Report(ok, indexed ? "im2d indexed line list, polyline, triangle list, strip, fan and points"
	                          : "im2d line list, polyline, triangle list, strip, fan and points");
}

static bool
CheckIm2DCulling(void)
{
	static const RGBA col = { 90, 90, 250, 255 };
	bool ok = true;
	for(int pass = 0; pass < 2; pass++){
		Im2DVertex cw[4], ccw[4];
		Quad(cw, 400.0f, 150.0f, 440.0f, 190.0f, col);
		Quad(ccw, 460.0f, 150.0f, 500.0f, 190.0f, col);
		Im2DVertex t = ccw[1];
		ccw[1] = ccw[3];
		ccw[3] = t;
		BeginFrame(GREY);
		SetRenderState(CULLMODE, pass == 0 ? CULLBACK : CULLFRONT);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, cw, 4);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, ccw, 4);
		EndFrame();
		Image *img = ReadBack();
		if(img == nil){
			ok = false;
			break;
		}
		ok &= PixelIs(img, 420, 170, pass == 0 ? GREY : col);
		ok &= PixelIs(img, 480, 170, pass == 0 ? col : GREY);
		img->destroy();
	}
	return Report(ok, "im2d culling treats a clockwise screen quad as back-facing, as gl3 does");
}

static bool
CheckIm2DRingGrowth(void)
{
	static const RGBA col = { 250, 120, 10, 255 };
	BeginFrame(GREY);
	DrawQuad(20.0f, 300.0f, 40.0f, 320.0f, RED);
	uint32 before = metal::getStateStats().ringSize;
	int n = (int)(before/sizeof(Im2DVertex)) + 3000;
	n -= n % 3;
	Im2DVertex *big = new Im2DVertex[n];
	Im2DVertex q[4];
	Quad(q, 60.0f, 300.0f, 80.0f, 320.0f, col);
	big[0] = q[0]; big[1] = q[1]; big[2] = q[2];
	big[3] = q[0]; big[4] = q[2]; big[5] = q[3];
	Im2DVertex degenerate = Vert(5.0f, 5.0f, RED);
	for(int i = 6; i < n; i++)
		big[i] = degenerate;
	im2d::RenderPrimitive(PRIMTYPETRILIST, big, n);
	uint32 after = metal::getStateStats().ringSize;
	uint16 idx[4] = { 0, 1, 2, 3 };
	Quad(q, 100.0f, 300.0f, 120.0f, 320.0f, GREEN);
	im2d::RenderIndexedPrimitive(PRIMTYPETRIFAN, q, 4, idx, 4);
	EndFrame();
	delete[] big;
	Image *img = ReadBack();
	bool ok = img != nil;
	if(after <= before){
		Detail("  ring stayed at %u bytes after %d vertices\n", after, n);
		ok = false;
	}
	if(img){
		ok &= RectIs(img, 20, 300, 40, 320, RED, GREY);
		ok &= RectIs(img, 60, 300, 80, 320, col, GREY);
		ok &= RectIs(img, 100, 300, 120, 320, GREEN, GREY);
		ok &= PixelIs(img, 5, 5, GREY);
		img->destroy();
	}
	return Report(ok, "im2d draws before, during and after the ring grows in one frame");
}

static bool
CheckIm2DSamplesClearedCameraTexture(void)
{
	Raster *ct = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	uint8 *px = ct ? ct->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH) : nil;
	if(px == nil){
		if(ct)
			ct->destroy();
		return Report(false, "im2d samples a camera texture's pending clear");
	}
	for(int i = 0; i < 16*16; i++){
		px[i*4] = 0;
		px[i*4+1] = 255;
		px[i*4+2] = 0;
		px[i*4+3] = 255;
	}
	ct->unlock(0);

	Raster *fb = ctx.camera->frameBuffer, *zb = ctx.camera->zBuffer;
	Clear(GREY, Camera::CLEARIMAGE | Camera::CLEARZ);
	ctx.camera->frameBuffer = ct;
	ctx.camera->zBuffer = nil;
	Clear(RED, Camera::CLEARIMAGE);
	ctx.camera->frameBuffer = fb;
	ctx.camera->zBuffer = zb;
	bool pending = metal::rasterHasPendingWork(ct);
	ctx.camera->beginUpdate();
	Set2DState();
	SetRenderStatePtr(TEXTURERASTER, ct);
	Im2DVertex v[4];
	Quad(v, 10.0f, 400.0f, 26.0f, 416.0f, WHITE);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil && pending;
	if(!pending)
		Detail("  the camera texture's clear was not pending before the draw\n");
	if(img){
		ok &= RectIs(img, 10, 400, 26, 416, RED, GREY);
		img->destroy();
	}
	ct->destroy();
	return Report(ok, "im2d samples a camera texture's pending clear");
}

static bool
CheckIm2DFeedbackDropped(void)
{
	Raster *ct = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	if(ct == nil)
		return Report(false, "im2d sampling the target it draws into is reported and dropped");
	Raster *fb = ctx.camera->frameBuffer, *zb = ctx.camera->zBuffer;
	Error err;
	getError(&err);
	ctx.camera->frameBuffer = ct;
	ctx.camera->zBuffer = nil;
	Clear(BLUE, Camera::CLEARIMAGE);
	ctx.camera->beginUpdate();
	Set2DState();
	SetRenderStatePtr(TEXTURERASTER, ct);
	uint32 feedback = metal::getStateStats().feedbackDraws;
	DrawQuad(0.0f, 0.0f, 16.0f, 16.0f, WHITE);
	getError(&err);
	bool ok = err.code == ERR_GENERAL;
	if(!ok)
		Detail("  error code %u after the first draw, expected ERR_GENERAL\n", err.code);
	DrawQuad(0.0f, 0.0f, 16.0f, 16.0f, WHITE);
	getError(&err);
	if(err.code != 0){
		Detail("  the second draw reported again\n");
		ok = false;
	}
	feedback = metal::getStateStats().feedbackDraws - feedback;
	if(feedback != 2){
		Detail("  %u feedback draws counted, expected 2\n", feedback);
		ok = false;
	}
	SetRenderStatePtr(TEXTURERASTER, nil);
	ctx.camera->endUpdate();
	ctx.camera->frameBuffer = fb;
	ctx.camera->zBuffer = zb;
	metal::resolveRasterTarget(ct);
	Image *img = ct->toImage();
	if(img == nil || img->depth != 32){
		Detail("  toImage gave %p\n", img);
		ok = false;
	}else{
		for(int i = 0; ok && i < 16*16; i++)
			if(img->pixels[i*4] != 0 || img->pixels[i*4+1] != 0 || img->pixels[i*4+2] != 255){
				Detail("  pixel %d is %d,%d,%d, expected the blue clear\n", i,
				       img->pixels[i*4], img->pixels[i*4+1], img->pixels[i*4+2]);
				ok = false;
			}
	}
	if(img)
		img->destroy();
	ct->destroy();
	return Report(ok, "im2d sampling the target it draws into is reported and dropped");
}

static bool
CheckStaleStageOneTargetNotFeedback(void)
{
	const char *name = "a target left on stage 1 does not drop an im2d draw into it";
	Raster *ct = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	Texture *t = ct ? Texture::create(ct) : nil;
	if(t == nil){
		if(ct)
			ct->destroy();
		return Report(false, name);
	}
	metal::setTexture(1, t);
	Raster *fb = ctx.camera->frameBuffer, *zb = ctx.camera->zBuffer;
	ctx.camera->frameBuffer = ct;
	ctx.camera->zBuffer = nil;
	metal::StateStats before = metal::getStateStats();
	Clear(BLUE, Camera::CLEARIMAGE);
	ctx.camera->beginUpdate();
	Set2DState();
	if(metal::matfxEnvShader)
		metal::matfxEnvShader->use();
	DrawQuad(0.0f, 0.0f, 16.0f, 16.0f, RED);
	ctx.camera->endUpdate();
	metal::StateStats after = metal::getStateStats();
	ctx.camera->frameBuffer = fb;
	ctx.camera->zBuffer = zb;
	metal::setTexture(1, nil);
	bool ok = metal::matfxEnvShader != nil;
	if(after.feedbackDraws != before.feedbackDraws || after.droppedDraws != before.droppedDraws){
		Detail("  %u feedback and %u dropped draws counted, expected none\n",
		       after.feedbackDraws - before.feedbackDraws, after.droppedDraws - before.droppedDraws);
		ok = false;
	}
	metal::resolveRasterTarget(ct);
	Image *img = ct->toImage();
	if(img == nil || img->depth != 32){
		Detail("  toImage gave %p\n", img);
		ok = false;
	}else{
		bool red = true;
		for(int i = 0; red && i < 16*16; i++)
			red = PixelNearRGBA(img, i%16, i/16, RED, 0);
		ok &= red;
	}
	if(img)
		img->destroy();
	t->raster = nil;
	t->destroy();
	ct->destroy();
	return Report(ok, name);
}

static bool
CheckDepthAttachmentFeedbackDropped(void)
{
	const char *name = "im2d sampling the depth raster it draws with is dropped";
	uint32 feedback = metal::getStateStats().feedbackDraws;
	BeginFrame(GREY);
	SetRenderStatePtr(TEXTURERASTER, ctx.camera->zBuffer);
	DrawQuad(10.0f, 10.0f, 50.0f, 50.0f, RED);
	SetRenderStatePtr(TEXTURERASTER, nil);
	EndFrame();
	feedback = metal::getStateStats().feedbackDraws - feedback;
	bool ok = ctx.camera->zBuffer != nil;
	if(feedback != 1){
		Detail("  %u feedback draws counted, expected 1\n", feedback);
		ok = false;
	}
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelNearRGBA(img, 30, 30, GREY, 0);
		img->destroy();
	}
	return Report(ok, name);
}

static bool
CheckStaleStageNeverBreaksPass(void)
{
	const char *name = "a target with a pending clear left on stage 1 does not break the pass";
	BeginFrame(GREY);
	DrawQuad(0.0f, 0.0f, 8.0f, 8.0f, RED);
	Raster *p = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	Texture *t = p ? Texture::create(p) : nil;
	bool ok = t != nil && metal::rasterHasPendingWork(p);
	if(t && !ok)
		Detail("  the new camera texture has no pending clear\n");
	uint32 passes = 0, passCount = 0;
	bool pending = false;
	if(t){
		metal::setTexture(1, t);
		passes = metal::getFrameStats().renderPasses;
		DrawQuad(8.0f, 0.0f, 16.0f, 8.0f, GREEN);
		passCount = metal::getFrameStats().renderPasses - passes;
		pending = metal::rasterHasPendingWork(p);
		metal::setTexture(1, nil);
	}
	EndFrame();
	if(t){
		if(passCount != 0){
			Detail("  draw B opened %u passes, expected 0\n", passCount);
			ok = false;
		}
		if(!pending){
			Detail("  the stage 1 target's clear was resolved by draw B\n");
			ok = false;
		}
		t->raster = nil;
		t->destroy();
	}
	if(p)
		p->destroy();
	return Report(ok, name);
}

static bool
CheckForeignRasterDrawsWhite(void)
{
	static const RGBA col = { 200, 100, 50, 255 };
	Raster *ras = Raster::create(4, 4, 32, Raster::C8888 | Raster::TEXTURE, PLATFORM_D3D8);
	if(ras == nil)
		return Report(false, "a bound D3D8 raster is reported once and draws white");
	Error err;
	getError(&err);
	BeginFrame(GREY);
	SetRenderStatePtr(TEXTURERASTER, ras);
	getError(&err);
	bool ok = err.code == ERR_PLATFORM;
	if(!ok)
		Detail("  error code %u after binding, expected ERR_PLATFORM\n", err.code);
	ok &= GetRenderStatePtr(TEXTURERASTER) == ras;
	SetRenderState(TEXTUREFILTER, Texture::LINEAR);
	SetRenderState(TEXTUREADDRESS, Texture::CLAMP);
	DrawQuad(30.0f, 30.0f, 50.0f, 50.0f, col);
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderStatePtr(TEXTURERASTER, ras);
	getError(&err);
	if(err.code != 0){
		Detail("  binding again reported again\n");
		ok = false;
	}
	EndFrame();
	Image *img = ReadBack();
	if(img == nil)
		ok = false;
	else{
		ok &= RectIs(img, 30, 30, 50, 50, col, GREY);
		img->destroy();
	}
	ras->destroy();
	return Report(ok, "a bound D3D8 raster is reported once and draws white");
}

static bool
OverrideShaderQuadIs(metal::Shader *sh, RGBA want)
{
	if(sh == nil){
		Detail("  test shader did not compile\n");
		return false;
	}
	BeginFrame(GREY);
	metal::im2dOverrideShader = sh;
	DrawQuad(30.0f, 30.0f, 50.0f, 50.0f, WHITE);
	metal::im2dOverrideShader = nil;
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= RectIs(img, 30, 30, 50, 50, want, GREY);
		img->destroy();
	}
	sh->destroy();
	return ok;
}

static bool
CheckIm2DDefaultAttributes(void)
{
	static const RGBA want = { 51, 102, 153, 255 };
	return Report(OverrideShaderQuadIs(CreateDefaultAttribShader(), want),
		"missing float and integer attributes read (0,0,0,1) in an im2d draw");
}

static bool
CheckIm2DCustomConstants(void)
{
	static const float colour[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
	static const RGBA want = { 51, 102, 153, 255 };
	metal::setCustomConstants(colour, sizeof(colour));
	uint32 before = metal::getStateStats().customBlockBinds;
	bool ok = OverrideShaderQuadIs(CreateCustomConstantShader(), want);
	if(metal::getStateStats().customBlockBinds == before){
		Detail("  the custom block was never bound\n");
		ok = false;
	}
	return Report(ok, "custom constants reach an im2d override shader");
}

static bool
CheckStatsCountUploads(void)
{
	static const float k[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
	const char *name = "the stats line counts custom and material block uploads per frame";
	metal::Shader *cs = CreateCustomConstantShader();
	if(cs == nil){
		Detail("  test shader did not compile\n");
		return Report(false, name);
	}
	uint32 used = metal::checkShaderBlockSizes(cs, 0);
	bool ok = (used & 1<<metal::BUFFER_CUSTOM) && !(used & 1<<metal::BUFFER_MATERIAL);
	if(!ok)
		Detail("  the shader reads blocks 0x%x, expected custom and not material\n", used);
	metal::logStats();
	for(int f = 0; f < 2; f++){
		BeginFrame(GREY);
		metal::im2dOverrideShader = cs;
		for(int i = 0; i < 3; i++){
			metal::setCustomConstants(k, sizeof(k));
			DrawQuad(30.0f, 30.0f, 50.0f, 50.0f, WHITE);
		}
		metal::im2dOverrideShader = nil;
		EndFrame();
	}
	metal::logStats();
	metal::setCustomConstants(nil, 0);
	cs->destroy();
	const char *line = metal::getStatsLine();
	StatsTail t;
	ok = ok && ParseStatsTail(line, &t) && t.customPerFrame == 3.0 && t.materialPerFrame == 0.0 &&
	     t.withoutDrawable == 0 && t.samples == 1;
	if(!ok)
		Detail("  got \"%s\"\n", line);
	return Report(ok, name);
}

static bool
CheckStatsFramesWithoutDrawable(void)
{
	metal::logStats();
	metal::FrameStats f0 = metal::getFrameStats();
	for(int i = 0; i < 5; i++)
		DrawFrame(GREY);
	metal::logStats();
	metal::FrameStats f1 = metal::getFrameStats();
	const char *line = metal::getStatsLine();
	uint32 shown = f1.framesShown - f0.framesShown;
	uint32 want = shown - (f1.drawablesAcquired - f0.drawablesAcquired);
	StatsTail t;
	bool ok = shown == 5 && want == 0 && ParseStatsTail(line, &t) && t.withoutDrawable == want &&
	          t.waitMaxMs >= 0.0 && t.waitMaxMs < 1000.0;
	if(!ok)
		Detail("  got \"%s\", %u frames shown, %u without drawable, expected 5 and 0\n", line, shown, want);
	return Report(ok, "the stats line counts frames shown without a drawable and the longest drawable wait");
}

// the periodic check runs every 64 frames, so 64 frames after the wait reach it once
static bool
PeriodicStatsLinePrinted(uint32 seconds)
{
	char before[1024];
	metal::setStatsInterval(seconds);
	metal::logStats();
	snprintf(before, sizeof(before), "%s", metal::getStatsLine());
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	for(int i = 0; i < 64; i++)
		DrawFrame(GREY);
	bool printed = strcmp(before, metal::getStatsLine()) != 0;
	metal::setStatsInterval(0);
	return printed;
}

static bool
CheckNoPeriodicStatsByDefault(void)
{
	bool printed = PeriodicStatsLinePrinted(0);
	if(printed)
		Detail("  a periodic line was printed: \"%s\"\n", metal::getStatsLine());
	return Report(!printed, "with the stats interval at 0 no periodic stats line is printed");
}

static bool
CheckPeriodicStatsAfterInterval(void)
{
	bool printed = PeriodicStatsLinePrinted(1);
	const char *line = metal::getStatsLine();
	bool ok = printed && strncmp(line, "rw::metal: stats frames ", 24) == 0;
	if(!ok)
		Detail("  printed %d, line \"%s\"\n", printed, line);
	return Report(ok, "with the stats interval at 1 s a periodic stats line is printed after a second");
}

static const float staleConstants[4] = { 0.2f, 0.4f, 0.6f, 1.0f };

static bool
CheckCustomConstantsGoneAfterRestart(void)
{
	static const RGBA zero = { 0, 0, 0, 0 };
	uint32 before = metal::getStateStats().customBlockBinds;
	bool ok = OverrideShaderQuadIs(CreateCustomConstantShader(), zero);
	if(metal::getStateStats().customBlockBinds == before){
		Detail("  the custom block was never bound\n");
		ok = false;
	}
	return Report(ok, "after a restart a shader reading custom constants sees zeros, not the old constants");
}

static bool
CheckShortCustomConstantsReadZero(void)
{
	static const float colour[3] = { 0.2f, 0.4f, 0.6f };
	static const RGBA full = { 51, 102, 153, 255 };
	static const RGBA want = { 51, 102, 153, 0 };
	int ringSlots = metal::getMaxFramesInFlight();
	bool ok = true;
	for(int i = 0; i < ringSlots; i++){
		metal::setCustomConstants(staleConstants, sizeof(staleConstants));
		ok &= OverrideShaderQuadIs(CreateCustomConstantShader(), full);
	}
	metal::setCustomConstants(colour, sizeof(colour));
	ok &= OverrideShaderQuadIs(CreateCustomConstantShader(), want);
	return Report(ok, "custom constants shorter than the shader's block read zero past their end");
}

static bool
CheckOversizedCustomBlockDropped(void)
{
	metal::Shader *big = CreateOversizedCustomShader();
	if(big == nil)
		return Report(false, "a custom block larger than 1 KB is reported once and its draws are dropped");
	Error err;
	getError(&err);
	BeginFrame(GREY);
	metal::im2dOverrideShader = big;
	DrawQuad(30.0f, 30.0f, 50.0f, 50.0f, RED);
	getError(&err);
	bool ok = err.code != 0;
	if(!ok)
		Detail("  no error after the first draw\n");
	DrawQuad(30.0f, 30.0f, 50.0f, 50.0f, RED);
	getError(&err);
	if(err.code != 0){
		Detail("  the second draw reported again\n");
		ok = false;
	}
	metal::im2dOverrideShader = nil;
	DrawQuad(60.0f, 30.0f, 80.0f, 50.0f, GREEN);
	EndFrame();
	big->destroy();
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 40, 40, GREY);
		ok &= RectIs(img, 60, 30, 80, 50, GREEN, GREY);
		img->destroy();
	}
	return Report(ok, "a custom block larger than 1 KB is reported once and its draws are dropped");
}

static bool
CheckIm2DFailedPipelineDropped(void)
{
	metal::Shader *broken = CreateBrokenShader();
	if(broken == nil)
		return Report(false, "an im2d draw with a failed pipeline is dropped, the next one draws");
	BeginFrame(GREY);
	metal::im2dOverrideShader = broken;
	DrawQuad(30.0f, 30.0f, 50.0f, 50.0f, RED);
	metal::im2dOverrideShader = nil;
	DrawQuad(60.0f, 30.0f, 80.0f, 50.0f, GREEN);
	EndFrame();
	broken->destroy();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= PixelIs(img, 40, 40, GREY);
		ok &= RectIs(img, 60, 30, 80, 50, GREEN, GREY);
		img->destroy();
	}
	return Report(ok, "an im2d draw with a failed pipeline is dropped, the next one draws");
}

static void
UV2Quad(metal::Im2DVertexUV2 vt[4], float x0, float y0, float x1, float y1, RGBA c,
        float u, float v, const float uv2[8])
{
	Im2DVertex q[4];
	Quad(q, x0, y0, x1, y1, c, u, v, u, v);
	for(int i = 0; i < 4; i++){
		(Im2DVertex&)vt[i] = q[i];
		vt[i].u2 = uv2[i*2];
		vt[i].v2 = uv2[i*2+1];
	}
}

static const float uv2Corners[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
static uint16 uv2QuadIndices[6] = { 0, 1, 2, 0, 2, 3 };

static bool
CheckIm2DTwoUVs(void)
{
	const char *name = "a two-uv im2d draw hands both coordinate sets to the override shader";
	static const RGBA lowerLeft = { 66, 193, 128, 255 };
	static const RGBA upperRight = { 225, 18, 128, 255 };
	metal::Shader *sh = CreateUV2CoordShader();
	if(sh == nil){
		Detail("  test shader did not compile\n");
		return Report(false, name);
	}
	metal::Im2DVertexUV2 v[4];
	BeginFrame(GREY);
	metal::im2dOverrideShader = sh;
	UV2Quad(v, 100.0f, 100.0f, 164.0f, 164.0f, WHITE, 0.5f, 0.0f, uv2Corners);
	metal::im2DRenderIndexedPrimitiveUV2(PRIMTYPETRILIST, v, 4, uv2QuadIndices, 6);
	metal::im2dOverrideShader = nil;
	EndFrame();
	sh->destroy();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= PixelNearRGBA(img, 116, 148, lowerLeft, 1);
		ok &= PixelNearRGBA(img, 156, 104, upperRight, 1);
		ok &= PixelIs(img, 90, 90, GREY);
		img->destroy();
	}
	return Report(ok, name);
}

static bool
CheckIm2DTwoUVsNeedOverride(void)
{
	const char *name = "a two-uv im2d draw without an override shader is dropped and counted";
	metal::Im2DVertexUV2 v[4];
	BeginFrame(GREY);
	metal::StateStats before = metal::getStateStats();
	UV2Quad(v, 120.0f, 120.0f, 184.0f, 184.0f, RED, 0.0f, 0.0f, uv2Corners);
	metal::im2DRenderIndexedPrimitiveUV2(PRIMTYPETRILIST, v, 4, uv2QuadIndices, 6);
	metal::StateStats after = metal::getStateStats();
	EndFrame();
	bool ok = true;
	if(after.droppedDraws - before.droppedDraws != 1 || after.draws != before.draws){
		Detail("  %u dropped and %u drawn, expected 1 and 0\n",
		       after.droppedDraws - before.droppedDraws, after.draws - before.draws);
		ok = false;
	}
	const char *line = metal::getDropLine();
	if(strstr(line, "no override shader") == nil || strstr(line, "shader none") == nil){
		Detail("  the drop line is \"%s\"\n", line);
		ok = false;
	}
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 150, 150, GREY);
		img->destroy();
	}
	return Report(ok, name);
}

static bool
CheckIm2DTwoUVLayout(void)
{
	metal::AttribDesc a[16] = {};
	int32 n = metal::getVertexLayout(9, a, 16);
	bool ok = metal::im2dUV2VertexLayout == 9 && n == 4 &&
	          a[3].index == metal::ATTRIB_TEXCOORDS1 && a[3].offset == 28 && a[3].stride == 36 && a[0].stride == 36;
	if(!ok)
		Detail("  layout id %u; layout 9 has %d attributes, the last index %u offset %u stride %u, the first stride %u\n",
		       metal::im2dUV2VertexLayout, n, a[3].index, a[3].offset, a[3].stride, a[0].stride);
	return Report(ok, "the two-uv im2d layout is id 9 with the second uv at offset 28 of 36 bytes");
}

static bool
CheckHostPrewarm(void)
{
	const char *name = "a host shader prewarmed for im2d draws without a late pipeline";
	metal::Shader *sh = CreateUV2TextureShader();
	if(sh == nil){
		Detail("  test shader did not compile\n");
		return Report(false, name);
	}
	bool ok = true;
	if(sh->textureStages != -1){
		Detail("  a new shader has texture stages %d, expected -1\n", sh->textureStages);
		ok = false;
	}
	metal::StateStats s0 = metal::getStateStats();
	if(!metal::prewarmIm2DShader(sh, 1, 1, BLENDSRCALPHA, BLENDINVSRCALPHA, 1)){
		Detail("  the prewarm returned 0\n");
		ok = false;
	}
	metal::StateStats s1 = metal::getStateStats();
	if(s1.pipelinesHost != s0.pipelinesHost + 1 || s1.pipelinesAtInit != s0.pipelinesAtInit ||
	   s1.pipelinesLate != s0.pipelinesLate){
		Detail("  host %u, init %u, late %u pipelines built by the prewarm, expected 1, 0 and 0\n",
		       s1.pipelinesHost - s0.pipelinesHost, s1.pipelinesAtInit - s0.pipelinesAtInit,
		       s1.pipelinesLate - s0.pipelinesLate);
		ok = false;
	}
	if(sh->textureStages != 3){
		Detail("  texture stages %d after the prewarm, expected 3\n", sh->textureStages);
		ok = false;
	}
	if(metal::prewarmIm2DShader(nil, 1, 1, BLENDSRCALPHA, BLENDINVSRCALPHA, 1)){
		Detail("  the prewarm of no shader returned 1\n");
		ok = false;
	}

	static const RGBA redBlue[2] = { RED, BLUE };
	Texture *t0 = MakeTexture(1, 1, &WHITE, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	Texture *t1 = MakeTexture(2, 1, redBlue, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	if(t0 == nil || t1 == nil){
		if(t0)
			t0->destroy();
		if(t1)
			t1->destroy();
		sh->destroy();
		return Report(false, name);
	}
	metal::Im2DVertexUV2 v[4];
	BeginFrame(GREY);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderStatePtr(TEXTURERASTER, t0->raster);
	metal::setTexture(1, t1);
	metal::im2dOverrideShader = sh;
	UV2Quad(v, 200.0f, 100.0f, 264.0f, 164.0f, WHITE, 0.5f, 0.5f, uv2Corners);
	metal::im2DRenderIndexedPrimitiveUV2(PRIMTYPETRILIST, v, 4, uv2QuadIndices, 6);
	metal::im2dOverrideShader = nil;
	metal::setTexture(1, nil);
	EndFrame();
	uint32 late = metal::getStateStats().pipelinesLate - s0.pipelinesLate;
	if(late != 0){
		Detail("  %u pipelines built late by the draw, expected 0\n", late);
		ok = false;
	}
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 216, 132, RED);
		ok &= PixelIs(img, 248, 132, BLUE);
		img->destroy();
	}
	t0->destroy();
	t1->destroy();
	sh->destroy();
	return Report(ok, name);
}

struct NativePalCase
{
	const char *name;
	uint32 format;
	int w, h, levels;
	bool alpha;
};

static const NativePalCase palCases[] = {
	{ "D3D8 native PAL8 8x4 with an 8888 palette and 4 levels reads as the file stores it, every level exact",
	  Raster::PAL8 | Raster::C8888, 8, 4, 4, true },
	{ "D3D8 native PAL8 8x4 with an 888 palette and 4 levels reads as the file stores it, every level exact",
	  Raster::PAL8 | Raster::C888, 8, 4, 4, false },
	{ "D3D8 native PAL4 8x4 with an 8888 palette and 4 levels reads as the file stores it, every level exact",
	  Raster::PAL4 | Raster::C8888, 8, 4, 4, true },
	{ "D3D8 native PAL4 8x4 with an 888 palette and 4 levels reads as the file stores it, every level exact",
	  Raster::PAL4 | Raster::C888, 8, 4, 4, false },
};

static int
PalIndex(const NativePalCase *c, int l, int x, int y)
{
	return (l*37 + y*11 + x*5 + 1) % (c->format & Raster::PAL4 ? 16 : 256);
}

static void
PalEntry(const NativePalCase *c, int i, uint8 *rgba)
{
	rgba[0] = (uint8)(i*13 + 7);
	rgba[1] = (uint8)(i*29 + 3);
	rgba[2] = (uint8)(255 - i*7);
	rgba[3] = c->alpha ? (uint8)(i*17 + 40) : 255;
}

static const NativePalCase *buildPalCase;

static uint32
BuildNativePal(uint8 *body, uint32 cap)
{
	const NativePalCase *c = buildPalCase;
	int palSize = c->format & Raster::PAL4 ? 32 : 256;
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(c->format | Raster::MIPMAP);
	s.writeI32(c->alpha);
	s.writeU16(c->w);
	s.writeU16(c->h);
	s.writeU8(c->format & Raster::PAL4 ? 4 : 8);
	s.writeU8(c->levels);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	for(int i = 0; i < palSize; i++){
		uint8 e[4];
		PalEntry(c, i, e);
		s.write8(e, 4);
	}
	for(int l = 0; l < c->levels; l++){
		int lw = LevelDim(c->w, l), lh = LevelDim(c->h, l);
		s.writeU32(lw*lh);
		for(int y = 0; y < lh; y++)
			for(int x = 0; x < lw; x++)
				s.writeU8(PalIndex(c, l, x, y));
	}
	return s.getLength();
}

static bool
CheckNativePal(const NativePalCase *c)
{
	buildPalCase = c;
	int32 imagesBefore = Image::numAllocated;
	d3d::isP8supported = 0;
	Texture *tex = ReadAndConvertNative(BuildNativePal);
	d3d::isP8supported = 1;
	if(tex == nil)
		return Report(false, c->name);
	Raster *ras = tex->raster;
	metal::MetalRaster *natras = ras->platform == PLATFORM_METAL ? MetalExt(ras) : nil;
	int32 wantFormat = c->alpha ? Raster::C8888 : Raster::C888;
	int bpp = c->alpha ? 4 : 3;
	bool ok = natras && natras->texture && ras->width == c->w && ras->height == c->h &&
	          (int32)(ras->format & 0xF00) == wantFormat && ras->getNumLevels() == c->levels &&
	          TextureLevels(natras->texture) == c->levels;
	if(!ok)
		Detail("  platform %d size %dx%d format 0x%x levels %d texture levels %d\n", ras->platform,
		       ras->width, ras->height, ras->format & 0xF00, ras->getNumLevels(),
		       natras ? TextureLevels(natras->texture) : 0);
	for(int l = 0; ok && l < c->levels; l++){
		int lw = LevelDim(c->w, l), lh = LevelDim(c->h, l);
		uint8 *px = ras->lock(l, Raster::LOCKREAD);
		if(px == nil || ras->width != lw || ras->height != lh){
			Detail("  level %d lock %p size %dx%d, expected %dx%d\n", l, px, ras->width, ras->height, lw, lh);
			ok = false;
			if(px)
				ras->unlock(l);
			break;
		}
		for(int y = 0; ok && y < lh; y++)
			for(int x = 0; ok && x < lw; x++){
				uint8 want[4];
				PalEntry(c, PalIndex(c, l, x, y), want);
				char what[48];
				snprintf(what, sizeof(what), "level %d pixel (%d,%d)", l, x, y);
				ok &= BytesEqual(px + y*ras->stride + x*bpp, want, bpp, what);
			}
		ras->unlock(l);
	}
	tex->destroy();
	int32 leaked = Image::numAllocated - imagesBefore;
	if(leaked != 0){
		Detail("  %d images were left alive by the conversion\n", leaked);
		ok = false;
	}
	return Report(ok, c->name);
}

static const NativePalCase d3d9PalCases[] = {
	{ "D3D9 native PAL8 8x4 converts through the image fallback, every pixel from the palette",
	  Raster::PAL8 | Raster::C8888, 8, 4, 1, true },
	{ "D3D9 native PAL4 8x4 converts through the image fallback, every pixel from the palette",
	  Raster::PAL4 | Raster::C8888, 8, 4, 1, true },
};

static uint32
BuildD3D9NativePal(uint8 *body, uint32 cap)
{
	const NativePalCase *c = buildPalCase;
	int palSize = c->format & Raster::PAL4 ? 32 : 256;
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D9);
	s.writeI32(c->format);
	s.writeI32(d3d::D3DFMT_P8);
	s.writeU16(c->w);
	s.writeU16(c->h);
	s.writeU8(8);
	s.writeU8(1);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	for(int i = 0; i < palSize; i++){
		uint8 e[4];
		PalEntry(c, i, e);
		s.write8(e, 4);
	}
	s.writeU32(c->w*c->h);
	for(int y = 0; y < c->h; y++)
		for(int x = 0; x < c->w; x++)
			s.writeU8(PalIndex(c, 0, x, y));
	return s.getLength();
}

static bool
CheckD3D9NativePal(const NativePalCase *c)
{
	buildPalCase = c;
	int32 imagesBefore = Image::numAllocated;
	Texture *tex = ReadNativeOnly(BuildD3D9NativePal);
	if(tex == nil)
		return Report(false, c->name);
	bool imageOk = true;
	Image *img = d3d::rasterToImage(tex->raster);
	if(img == nil || (img->depth != 4 && img->depth != 8)){
		Detail("  d3d::rasterToImage gave image %p depth %d\n", img, img ? img->depth : 0);
		imageOk = false;
	}
	for(int y = 0; imageOk && y < c->h; y++)
		for(int x = 0; imageOk && x < c->w; x++){
			uint8 want[4];
			PalEntry(c, PalIndex(c, 0, x, y), want);
			char what[64];
			snprintf(what, sizeof(what), "image palette entry of pixel (%d,%d)", x, y);
			imageOk &= BytesEqual(img->palette + 4*img->pixels[y*img->stride + x], want, 4, what);
		}
	if(img)
		img->destroy();
	tex->destroy();
	tex = ReadAndConvertNative(BuildD3D9NativePal);
	if(tex == nil)
		return Report(false, c->name);
	Raster *ras = tex->raster;
	bool ok = ras->platform == PLATFORM_METAL && ras->width == c->w && ras->height == c->h &&
	          (ras->format & 0xF00) == Raster::C8888;
	if(!ok)
		Detail("  platform %d size %dx%d format 0x%x\n", ras->platform, ras->width, ras->height, ras->format & 0xF00);
	uint8 *px = ok ? ras->lock(0, Raster::LOCKREAD) : nil;
	if(ok && px == nil){
		Detail("  read lock returned nil\n");
		ok = false;
	}
	for(int y = 0; px && ok && y < c->h; y++)
		for(int x = 0; ok && x < c->w; x++){
			uint8 want[4];
			PalEntry(c, PalIndex(c, 0, x, y), want);
			char what[48];
			snprintf(what, sizeof(what), "pixel (%d,%d)", x, y);
			ok &= BytesEqual(px + y*ras->stride + x*4, want, 4, what);
		}
	if(px)
		ras->unlock(0);
	tex->destroy();
	int32 leaked = Image::numAllocated - imagesBefore;
	if(leaked != 0){
		Detail("  %d images were left alive by the conversion\n", leaked);
		ok = false;
	}
	return Report(ok && imageOk, c->name);
}

static bool
CheckFilledLevelsOutOfOrder(void)
{
	const char *name = "levels written out of order count once the prefix below them is filled";
	Raster *ras = Raster::create(8, 8, 32, Raster::C8888 | Raster::TEXTURE | Raster::MIPMAP);
	if(ras == nil)
		return Report(false, name);
	metal::MetalRaster *natras = MetalExt(ras);
	static const int order[4] = { 0, 2, 1, 3 };
	static const int wantFilled[4] = { 1, 1, 3, 4 };
	bool ok = true;
	for(int i = 0; ok && i < 4; i++){
		uint8 *px = ras->lock(order[i], Raster::LOCKWRITE | Raster::LOCKNOFETCH);
		if(px == nil){
			Detail("  lock of level %d returned nil\n", order[i]);
			ok = false;
			break;
		}
		ras->unlock(order[i]);
		if(natras->filledLevels != wantFilled[i]){
			Detail("  after level %d filledLevels %d, expected %d\n", order[i], natras->filledLevels, wantFilled[i]);
			ok = false;
		}
		if(order[i] == 2 && natras->filledMask != 0x5){
			Detail("  after level 2 filledMask 0x%x, expected 0x5\n", natras->filledMask);
			ok = false;
		}
	}
	ras->destroy();
	return Report(ok, name);
}

static uint16
C555Texel(int l, int x, int y)
{
	return (uint16)((l*997 + y*131 + x*37 + 5) & 0x7FFF);
}

static uint32
BuildD3D8C555(uint8 *body, uint32 cap)
{
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(Raster::C555 | Raster::MIPMAP);
	s.writeI32(0);
	s.writeU16(8);
	s.writeU16(4);
	s.writeU8(16);
	s.writeU8(4);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	for(int l = 0; l < 4; l++){
		int lw = LevelDim(8, l), lh = LevelDim(4, l);
		s.writeU32(lw*lh*2);
		for(int y = 0; y < lh; y++)
			for(int x = 0; x < lw; x++)
				s.writeU16(C555Texel(l, x, y));
	}
	return s.getLength();
}

static bool
CheckD3DNativeFallbackLevels(void)
{
	const char *name = "D3D8 native C555 8x4 with 4 levels converts through the image fallback, every level exact, no image left";
	int32 imagesBefore = Image::numAllocated;
	Texture *tex = ReadAndConvertNative(BuildD3D8C555);
	if(tex == nil)
		return Report(false, name);
	Raster *ras = tex->raster;
	metal::MetalRaster *natras = ras->platform == PLATFORM_METAL ? MetalExt(ras) : nil;
	bool ok = natras && natras->texture && ras->width == 8 && ras->height == 4 &&
	          (ras->format & 0xF00) == Raster::C1555 && ras->getNumLevels() == 4 &&
	          TextureLevels(natras->texture) == 4;
	if(!ok)
		Detail("  platform %d size %dx%d format 0x%x levels %d\n", ras->platform,
		       ras->width, ras->height, ras->format & 0xF00, ras->getNumLevels());
	for(int l = 0; ok && l < 4; l++){
		int lw = LevelDim(8, l), lh = LevelDim(4, l);
		uint8 *px = ras->lock(l, Raster::LOCKREAD);
		if(px == nil || ras->width != lw || ras->height != lh){
			Detail("  level %d lock %p size %dx%d, expected %dx%d\n", l, px, ras->width, ras->height, lw, lh);
			ok = false;
			if(px)
				ras->unlock(l);
			break;
		}
		for(int y = 0; ok && y < lh; y++)
			for(int x = 0; ok && x < lw; x++){
				uint16 t = C555Texel(l, x, y) | 0x8000;
				uint8 want[2] = { (uint8)(t & 0xFF), (uint8)(t >> 8) };
				char what[48];
				snprintf(what, sizeof(what), "level %d pixel (%d,%d)", l, x, y);
				ok &= BytesEqual(px + y*ras->stride + x*2, want, 2, what);
			}
		ras->unlock(l);
	}
	tex->destroy();
	int32 leaked = Image::numAllocated - imagesBefore;
	if(leaked != 0){
		Detail("  %d images were left alive by the conversion\n", leaked);
		ok = false;
	}
	return Report(ok, name);
}

static const RGBA mipColours[5] = {
	{ 200, 0, 0, 255 }, { 0, 200, 0, 255 }, { 0, 0, 200, 255 }, { 200, 200, 0, 255 }, { 0, 200, 200, 255 },
};

static bool
WriteFlatLevel(Raster *ras, int level, const uint8 *texel, int bpp)
{
	uint8 *px = ras->lock(level, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(px == nil){
		Detail("  level %d write lock failed\n", level);
		return false;
	}
	for(int y = 0; y < ras->height; y++)
		for(int x = 0; x < ras->width; x++)
			memcpy(px + y*ras->stride + x*bpp, texel, bpp);
	ras->unlock(level);
	return true;
}

static Raster*
MakeMetalMipRaster(int written)
{
	Raster *ras = Raster::create(16, 16, 32, Raster::C8888 | Raster::MIPMAP | Raster::TEXTURE);
	if(ras == nil)
		return nil;
	for(int l = 0; l < written; l++){
		RGBA c = mipColours[l];
		uint8 rgba[4] = { c.red, c.green, c.blue, c.alpha };
		if(!WriteFlatLevel(ras, l, rgba, 4)){
			ras->destroy();
			return nil;
		}
	}
	return ras;
}

static Raster*
MakeD3D8SourceWithTwoLevels(bool c555)
{
	static const uint8 c555Texels[2][2] = { { 0x00, 0x7C }, { 0xE0, 0x03 } };
	int32 format = (c555 ? Raster::C555 : Raster::C8888) | Raster::MIPMAP | Raster::TEXTURE;
	Raster *ras = Raster::create(16, 16, c555 ? 16 : 32, format, PLATFORM_D3D8);
	if(ras == nil)
		return nil;
	for(int l = 0; l < 2; l++){
		RGBA c = mipColours[l];
		uint8 bgra[4] = { c.blue, c.green, c.red, c.alpha };
		if(!WriteFlatLevel(ras, l, c555 ? c555Texels[l] : bgra, c555 ? 2 : 4)){
			ras->destroy();
			return nil;
		}
	}
	((RasterLevels*)GETD3DRASTEREXT(ras)->texture)->numlevels = 2;
	return ras;
}

static uint32
BuildD3D8Pal8TwoLevels(uint8 *body, uint32 cap)
{
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(Raster::PAL8 | Raster::C8888 | Raster::MIPMAP);
	s.writeI32(0);
	s.writeU16(16);
	s.writeU16(16);
	s.writeU8(8);
	s.writeU8(2);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(0);
	for(int i = 0; i < 256; i++){
		RGBA c = i < 5 ? mipColours[i] : WHITE;
		uint8 e[4] = { c.red, c.green, c.blue, c.alpha };
		s.write8(e, 4);
	}
	for(int l = 0; l < 2; l++){
		int n = LevelDim(16, l)*LevelDim(16, l);
		s.writeU32(n);
		for(int i = 0; i < n; i++)
			s.writeU8(l);
	}
	return s.getLength();
}

static uint32
BuildD3D8DXT1TwoLevels(uint8 *body, uint32 cap)
{
	static const uint8 red[8] = { 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0 };
	static const uint8 green[8] = { 0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0 };
	StreamMemory s;
	s.open(body, 0, cap);
	WriteNativeHeader(&s, PLATFORM_D3D8);
	s.writeU32(Raster::C565 | Raster::MIPMAP);
	s.writeI32(0);
	s.writeU16(16);
	s.writeU16(16);
	s.writeU8(16);
	s.writeU8(2);
	s.writeU8(Raster::TEXTURE);
	s.writeU8(1);
	s.writeU32(16*8);
	for(int i = 0; i < 16; i++)
		s.write8(red, 8);
	s.writeU32(4*8);
	for(int i = 0; i < 4; i++)
		s.write8(green, 8);
	return s.getLength();
}

static Raster*
ConvertRaster(Raster *ras)
{
	Raster *out = ras ? Raster::convertTexToCurrentPlatform(ras) : nil;
	if(out && out->platform != PLATFORM_METAL){
		Detail("  conversion kept platform %d\n", out->platform);
		out->destroy();
		return nil;
	}
	return out;
}

struct MipProbe
{
	const char *name;
	Raster *ras;
	Texture *tex;
	RGBA want;
	int textureLevels;
	int filledLevels;
};

static void
CheckMinifiedMipLevels(void)
{
	static const RGBA fullGreen = { 0, 255, 0, 255 };
	MipProbe probes[6] = {
		{ "a 16x16 raster with 2 of 5 levels written samples level 1 when minified to level 3", nil, nil, mipColours[1], 5, 2 },
		{ "a 16x16 raster with all 5 levels written samples level 3 in the same frame", nil, nil, mipColours[3], 5, 5 },
		{ "a D3D8 C8888 source with 2 levels converts to a 2-level texture that samples level 1", nil, nil, mipColours[1], 2, 2 },
		{ "a D3D8 C555 source with 2 levels converts through the image fallback to a 2-level texture", nil, nil, fullGreen, 2, 2 },
		{ "a D3D8 native PAL8 texture read as an image with 2 levels samples level 1", nil, nil, mipColours[1], 5, 2 },
		{ "a D3D8 native DXT1 texture with 2 levels samples level 1", nil, nil, fullGreen, 2, 2 },
	};
	const int n = sizeof(probes)/sizeof(probes[0]);
	bool made[6];

	probes[0].ras = MakeMetalMipRaster(2);
	probes[1].ras = MakeMetalMipRaster(5);
	probes[2].ras = ConvertRaster(MakeD3D8SourceWithTwoLevels(false));
	probes[3].ras = ConvertRaster(MakeD3D8SourceWithTwoLevels(true));
	d3d::isP8supported = 0;
	probes[4].tex = ReadAndConvertNative(BuildD3D8Pal8TwoLevels);
	d3d::isP8supported = 1;
	probes[5].tex = ReadAndConvertNative(BuildD3D8DXT1TwoLevels);
	for(int i = 4; i < 6; i++)
		probes[i].ras = probes[i].tex ? probes[i].tex->raster : nil;

	BeginFrame(GREY);
	for(int i = 0; i < n; i++){
		made[i] = probes[i].ras != nil;
		if(!made[i])
			continue;
		Im2DVertex v[4];
		SetRenderStatePtr(TEXTURERASTER, probes[i].ras);
		SetRenderState(TEXTUREFILTER, Texture::MIPNEAREST);
		Quad(v, 20.0f + i*10.0f, 440.0f, 22.0f + i*10.0f, 442.0f, WHITE);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
	}
	EndFrame();
	Image *img = ReadBack();
	for(int i = 0; i < n; i++){
		MipProbe *p = &probes[i];
		bool ok = made[i] && img != nil;
		if(!made[i])
			Detail("  the source raster could not be made or converted\n");
		if(ok){
			metal::MetalRaster *natras = MetalExt(p->ras);
			int levels = TextureLevels(natras->texture);
			if(levels != p->textureLevels || natras->filledLevels != p->filledLevels){
				Detail("  texture has %d levels with %d filled, expected %d with %d filled\n",
				       levels, natras->filledLevels, p->textureLevels, p->filledLevels);
				ok = false;
			}
			for(int y = 0; y < 2; y++)
				for(int x = 0; x < 2; x++)
					ok &= PixelIs(img, 20 + i*10 + x, 440 + y, p->want);
		}
		Report(ok, p->name);
		if(p->tex)
			p->tex->destroy();
		else if(p->ras)
			p->ras->destroy();
	}
	if(img)
		img->destroy();
}

static bool
CheckIm2DListIsNotStrip(bool indexed)
{
	static const RGBA col = { 20, 200, 60, 255 };
	static const float lineXY[4][2] = { { 20.0f, 200.5f }, { 60.5f, 200.5f }, { 60.5f, 240.5f }, { 20.0f, 240.5f } };
	static const float triXY[6][2] = {
		{ 300.0f, 200.0f }, { 340.0f, 200.0f }, { 300.0f, 240.0f },
		{ 360.0f, 200.0f }, { 400.0f, 200.0f }, { 360.0f, 240.0f },
	};
	uint16 idx[6] = { 0, 1, 2, 3, 4, 5 };
	BeginFrame(GREY);
	for(int pass = 0; pass < 2; pass++){
		float dy = pass*60.0f;
		Im2DVertex lines[4], tris[6];
		for(int i = 0; i < 4; i++)
			lines[i] = Vert(lineXY[i][0], lineXY[i][1] + dy, col);
		for(int i = 0; i < 6; i++)
			tris[i] = Vert(triXY[i][0], triXY[i][1] + dy, col);
		PrimitiveType lineType = pass == 0 ? PRIMTYPELINELIST : PRIMTYPEPOLYLINE;
		PrimitiveType triType = pass == 0 ? PRIMTYPETRILIST : PRIMTYPETRISTRIP;
		if(indexed){
			im2d::RenderIndexedPrimitive(lineType, lines, 4, idx, 4);
			im2d::RenderIndexedPrimitive(triType, tris, 6, idx, 6);
		}else{
			im2d::RenderPrimitive(lineType, lines, 4);
			im2d::RenderPrimitive(triType, tris, 6);
		}
	}
	EndFrame();
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= PixelIs(img, 40, 200, col);
		ok &= PixelIs(img, 40, 240, col);
		ok &= PixelIs(img, 60, 220, GREY);
		ok &= PixelIs(img, 310, 205, col);
		ok &= PixelIs(img, 370, 205, col);
		ok &= PixelIs(img, 345, 205, GREY);
		ok &= PixelIs(img, 60, 280, col);
		ok &= PixelIs(img, 345, 265, col);
		img->destroy();
	}
	return Report(ok, indexed ? "im2d indexed line and triangle lists do not draw the strip's extra segments"
	                          : "im2d line and triangle lists do not draw the strip's extra segments");
}

static bool
CheckIm2DWideFan(void)
{
	static const RGBA col = { 240, 60, 200, 255 };
	const int n = 70000;
	Im2DVertex *v = new Im2DVertex[n];
	Im2DVertex centre = Vert(500.0f, 300.0f, col);
	for(int i = 0; i < n; i++)
		v[i] = centre;
	v[1] = Vert(460.0f, 300.0f, col);
	v[2] = Vert(460.0f, 340.0f, col);
	v[n-2] = Vert(540.0f, 300.0f, col);
	v[n-1] = Vert(540.0f, 340.0f, col);
	BeginFrame(GREY);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, n);
	EndFrame();
	delete[] v;
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		ok &= PixelIs(img, 470, 310, col);
		ok &= PixelIs(img, 530, 305, col);
		ok &= PixelIs(img, 505, 330, GREY);
		ok &= PixelIs(img, 495, 330, GREY);
		ok &= PixelIs(img, 500, 350, GREY);
		img->destroy();
	}
	return Report(ok, "im2d fan of 70000 vertices draws its first and last triangles through 32-bit indices");
}

static RGBA
FrameQuadColour(int f, int q)
{
	RGBA c = { (uint8)(f*30 + 10), (uint8)(q*15), (uint8)(250 - f*20), 255 };
	return c;
}

static bool
CheckIm2DFramesInFlight(void)
{
	const int frames = 7;
	for(int f = 0; f < frames; f++){
		BeginFrame(GREY);
		for(int q = 0; q < 16; q++){
			float x = 20.0f + q*20.0f + f*2.0f, y = 100.0f + f*4.0f;
			DrawQuad(x, y, x + 10.0f, y + 10.0f, FrameQuadColour(f, q));
		}
		EndFrame();
	}
	Image *img = ReadBack();
	bool ok = img != nil;
	if(ok){
		int f = frames-1;
		for(int q = 0; q < 16; q++){
			int x = 20 + q*20 + f*2, y = 100 + f*4;
			ok &= RectIs(img, x, y, x + 10, y + 10, FrameQuadColour(f, q), GREY);
		}
		img->destroy();
	}
	uint32 early = metal::getStateStats().ringEarlyReuses;
	if(early){
		Detail("  %u ring slots were reused before their frame completed\n", early);
		ok = false;
	}
	return Report(ok, "im2d draws 7 frames without a readback and the last one reads back whole");
}

static bool
CheckNewCameraTextureIsTransparentBlack(void)
{
	const char *name = "a new camera texture samples transparent black until something is drawn or copied into it";
	static const RGBA clearBlack = { 0, 0, 0, 0 };
	Raster *fresh = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	bool ok = fresh != nil;
	if(fresh && !metal::rasterHasPendingWork(fresh)){
		Detail("  a new camera texture has no pending clear\n");
		ok = false;
	}
	Raster *copied = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	if(copied && !metal::rasterHasPendingWork(copied)){
		Detail("  the copied texture has no pending clear before its write\n");
		ok = false;
	}
	uint8 *px = copied ? copied->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH) : nil;
	if(px){
		for(int i = 0; i < 16*16; i++){
			px[i*4] = 0;
			px[i*4+1] = 0;
			px[i*4+2] = 255;
			px[i*4+3] = 255;
		}
		copied->unlock(0);
	}
	Raster *drawn = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	if(fresh == nil || copied == nil || drawn == nil || px == nil){
		Detail("  camera texture creation or write lock failed\n");
		if(fresh) fresh->destroy();
		if(copied) copied->destroy();
		if(drawn) drawn->destroy();
		return Report(false, name);
	}
	if(!metal::rasterHasPendingWork(drawn)){
		Detail("  the drawn texture has no pending clear before its draw\n");
		ok = false;
	}

	Raster *fb = ctx.camera->frameBuffer, *zb = ctx.camera->zBuffer;
	ctx.camera->frameBuffer = drawn;
	ctx.camera->zBuffer = nil;
	ctx.camera->beginUpdate();
	Set2DState();
	DrawQuad(0.0f, 0.0f, 16.0f, 16.0f, RED);
	ctx.camera->endUpdate();
	ctx.camera->frameBuffer = fb;
	ctx.camera->zBuffer = zb;

	Raster *later = nil;
	for(int frame = 0; frame < 2; frame++){
		Raster *sampled[4] = { fresh, copied, drawn, later };
		RGBA want[4] = { clearBlack, BLUE, RED, clearBlack };
		BeginFrame(GREY);
		SetRenderState(ALPHATESTREF, 0);
		SetRenderState(VERTEXALPHA, 1);
		SetRenderState(SRCBLEND, BLENDONE);
		SetRenderState(DESTBLEND, BLENDZERO);
		for(int i = 0; i < 4; i++){
			if(sampled[i] == nil)
				continue;
			Im2DVertex v[4];
			SetRenderStatePtr(TEXTURERASTER, sampled[i]);
			Quad(v, 10.0f + i*30.0f, 440.0f, 26.0f + i*30.0f, 456.0f, WHITE);
			im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
		}
		SetRenderState(SRCBLEND, BLENDSRCALPHA);
		SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
		SetRenderStatePtr(TEXTURERASTER, fresh);
		Im2DVertex v[4];
		Quad(v, 130.0f, 440.0f, 146.0f, 456.0f, WHITE);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
		EndFrame();
		Image *img = ReadBack();
		if(img == nil){
			ok = false;
			break;
		}
		for(int i = 0; i < 4; i++)
			if(sampled[i])
				ok &= RectIs(img, 10 + i*30, 440, 26 + i*30, 456, want[i], GREY);
		ok &= RectIs(img, 130, 440, 146, 456, GREY, GREY);
		if(!ok)
			Detail("  in frame %d\n", frame);
		img->destroy();
		if(later == nil)
			later = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	}
	fresh->destroy();
	copied->destroy();
	drawn->destroy();
	if(later)
		later->destroy();
	return Report(ok, name);
}

static bool
TexelIs(const uint8 *px, int stride, int x, int y, int r, int g, int b)
{
	const uint8 *p = &px[y*stride + x*3];
	if(p[0] == r && p[1] == g && p[2] == b)
		return true;
	Detail("  texel (%d,%d) is %d,%d,%d, expected %d,%d,%d\n", x, y, p[0], p[1], p[2], r, g, b);
	return false;
}

static bool32
CopyCameraInto(Raster *t, int32 x, int32 y)
{
	Raster::pushContext(t);
	bool32 r = ctx.camera->frameBuffer->renderFast(x, y);
	Raster::popContext();
	return r;
}

static bool32
RenderFastFrame(Raster *t, int32 x, int32 y)
{
	BeginFrame(GREY);
	DrawQuad(0.0f, 0.0f, 32.0f, 16.0f, RED);
	DrawQuad(600.0f, 460.0f, 640.0f, 480.0f, BLUE);
	bool32 r = CopyCameraInto(t, x, y);
	DrawQuad(40.0f, 0.0f, 72.0f, 16.0f, GREEN);
	EndFrame();
	return r;
}

static bool
CheckRenderFastCopiesCamera(void)
{
	const char *name = "renderFast copies the camera into a camera texture and the frame resumes with Load";
	Raster *t = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	if(t == nil)
		return Report(false, name);
	bool32 r = RenderFastFrame(t, 0, 0);
	bool ok = r == 1;
	if(!ok)
		Detail("  renderFast returned %d\n", r);
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 0, 0, RED);
		ok &= PixelIs(img, 45, 5, GREEN);
		img->destroy();
	}
	uint8 *px = t->lock(0, Raster::LOCKREAD);
	ok &= px != nil;
	if(px){
		ok &= TexelIs(px, t->stride, 0, 0, 255, 0, 0);
		ok &= TexelIs(px, t->stride, 31, 15, 255, 0, 0);
		ok &= TexelIs(px, t->stride, 45, 5, GREY.red, GREY.green, GREY.blue);
		ok &= TexelIs(px, t->stride, 639, 479, 0, 0, 255);
		ok &= TexelIs(px, t->stride, 640, 0, 0, 0, 0);
		ok &= TexelIs(px, t->stride, 0, 480, 0, 0, 0);
		t->unlock(0);
	}
	t->destroy();
	return Report(ok, name);
}

static bool
CheckRenderFastOffset(void)
{
	const char *name = "renderFast at an offset copies with a top-left origin";
	Raster *t = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	if(t == nil)
		return Report(false, name);
	bool ok = RenderFastFrame(t, 8, 4) == 1;
	uint8 *px = t->lock(0, Raster::LOCKREAD);
	ok &= px != nil;
	if(px){
		ok &= TexelIs(px, t->stride, 8, 4, 255, 0, 0);
		ok &= TexelIs(px, t->stride, 7, 4, 0, 0, 0);
		ok &= TexelIs(px, t->stride, 39, 19, 255, 0, 0);
		t->unlock(0);
	}
	t->destroy();
	return Report(ok, name);
}

static bool
CheckRenderFastAsFullScreenFilter(void)
{
	const char *name = "a renderFast copy drawn back over the frame keeps the frame, as a full-screen filter does";
	Raster *t = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	if(t == nil)
		return Report(false, name);
	BeginFrame(GREY);
	DrawQuad(0.0f, 0.0f, 32.0f, 16.0f, RED);
	DrawQuad(600.0f, 460.0f, 640.0f, 480.0f, BLUE);
	bool ok = CopyCameraInto(t, 0, 0) == 1;
	Im2DVertex v[4];
	Quad(v, 0.0f, 0.0f, 1024.0f, 512.0f, WHITE);
	SetRenderStatePtr(TEXTURERASTER, t);
	SetRenderState(TEXTUREFILTER, Texture::NEAREST);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(ZTESTENABLE, 0);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
	EndFrame();
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 0, 0, RED);
		ok &= PixelIs(img, 639, 479, BLUE);
		ok &= PixelIs(img, 320, 240, GREY);
		img->destroy();
	}
	t->destroy();
	return Report(ok, name);
}

static bool
CheckRenderFastRejects(void)
{
	const char *name = "renderFast rejects a texture source and a camera destination";
	Raster *tex = Raster::create(16, 16, 32, Raster::C8888 | Raster::TEXTURE);
	Raster *t = Raster::create(64, 64, 0, Raster::CAMERATEXTURE);
	Raster *cam = Raster::create(64, 64, 0, Raster::CAMERA);
	bool ok = tex && t && cam;
	if(ok){
		Raster::pushContext(t);
		bool32 r = tex->renderFast(0, 0);
		Raster::popContext();
		if(r != 0){
			Detail("  a texture source returned %d\n", r);
			ok = false;
		}
		Raster::pushContext(cam);
		r = ctx.camera->frameBuffer->renderFast(0, 0);
		Raster::popContext();
		if(r != 0){
			Detail("  a camera destination returned %d\n", r);
			ok = false;
		}
	}
	if(tex) tex->destroy();
	if(t) t->destroy();
	if(cam) cam->destroy();
	return Report(ok, name);
}

static bool
CheckDepthOnlyClearSurvivesNewTexture(void)
{
	const char *name = "a depth clear left pending by a destroyed camera raster survives a new camera texture";
	BeginFrame(GREY);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	DrawQuad(0.0f, 0.0f, 640.0f, 480.0f, RED);
	EndFrame();

	Clear(GREY, Camera::CLEARZ);
	ctx.camera->frameBuffer->destroy();
	ctx.camera->frameBuffer = nil;
	Raster *t = Raster::create(16, 16, 0, Raster::CAMERATEXTURE);
	ctx.camera->frameBuffer = Raster::create(640, 480, 0, Raster::CAMERA);
	bool ok = t != nil && ctx.camera->frameBuffer != nil;
	if(ok){
		ctx.camera->beginUpdate();
		Set2DState();
		SetRenderState(ZTESTENABLE, 1);
		Im2DVertex v[4];
		Quad(v, 0.0f, 0.0f, 640.0f, 480.0f, GREEN);
		for(int i = 0; i < 4; i++)
			v[i].setScreenZ(0.5f);
		im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
		EndFrame();
		Image *img = ReadBack();
		ok = img != nil;
		if(img){
			ok &= PixelIs(img, 320, 240, GREEN);
			img->destroy();
		}
	}
	if(t)
		t->destroy();
	return Report(ok, name);
}

static bool
CheckRingGuardHeldFrame(void)
{
	const int frames = 5;
	uint32 earlyBefore = metal::getStateStats().ringEarlyReuses;
	double t0 = Seconds();
	for(int f = 0; f < frames; f++){
		BeginFrame(GREY);
		DrawQuad(20.0f + f*20.0f, 100.0f, 30.0f + f*20.0f, 110.0f, FrameQuadColour(f, 0));
		if(f == 0)
			metal::holdFrameForTest(0.25);
		EndFrame();
	}
	double elapsed = Seconds() - t0;
	bool ok = elapsed >= 0.2;
	if(!ok)
		Detail("  %d frames took %.3f s; the held frame did not hold\n", frames, elapsed);
	uint32 early = metal::getStateStats().ringEarlyReuses - earlyBefore;
	if(early){
		Detail("  %u ring slots were reused before their frame completed\n", early);
		ok = false;
	}
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		int f = frames-1;
		ok &= RectIs(img, 20 + f*20, 100, 30 + f*20, 110, FrameQuadColour(f, 0), GREY);
		img->destroy();
	}
	return Report(ok, "a frame held on the GPU makes a later frame wait for its ring slot");
}

static bool
CheckRenderFastIntoSubRaster(void)
{
	const char *name = "renderFast into a sub-raster stays inside the sub-raster";
	Raster *t = Raster::create(64, 64, 0, Raster::CAMERATEXTURE);
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERATEXTURE | Raster::DONTALLOCATE);
	bool ok = t && sub;
	if(ok){
		Rect r = { 8, 8, 16, 16 };
		sub->subRaster(t, &r);
		ok = RenderFastFrame(sub, 0, 0) == 1;
		uint8 *px = t->lock(0, Raster::LOCKREAD);
		ok &= px != nil;
		if(px){
			ok &= TexelIs(px, t->stride, 8, 8, 255, 0, 0);
			ok &= TexelIs(px, t->stride, 23, 23, 255, 0, 0);
			ok &= TexelIs(px, t->stride, 24, 8, 0, 0, 0);
			ok &= TexelIs(px, t->stride, 8, 24, 0, 0, 0);
			t->unlock(0);
		}
	}
	if(sub) sub->destroy();
	if(t) t->destroy();
	return Report(ok, name);
}

static bool
CheckRenderFastIntoAutoMipTexture(void)
{
	const char *name = "renderFast into an automatic-mipmap texture regenerates its lower levels";
	Raster *t = Raster::create(16, 16, 32, Raster::C8888 | Raster::TEXTURE | Raster::MIPMAP | Raster::AUTOMIPMAP);
	if(t == nil)
		return Report(false, name);
	uint32 blits = metal::getRasterStats().mipmapBlits;
	bool ok = RenderFastFrame(t, 0, 0) == 1;
	blits = metal::getRasterStats().mipmapBlits - blits;
	if(blits != 1){
		Detail("  %u mipmap blits, expected 1\n", blits);
		ok = false;
	}
	uint8 *px = t->lock(4, Raster::LOCKREAD);
	ok &= px != nil;
	if(px){
		ok &= PixelNear(px, 255, 0, 0, 255, 1, "level 4");
		t->unlock(4);
	}
	px = t->lock(0, Raster::LOCKREAD);
	ok &= px != nil;
	if(px){
		ok &= PixelNear(px + 15*t->stride + 15*4, 255, 0, 0, 255, 0, "level 0 (15,15)");
		t->unlock(0);
	}
	t->destroy();
	return Report(ok, name);
}

static bool
CheckRenderFastIntoMipTextureMarksLevelZero(void)
{
	const char *name = "renderFast into a mipmapped texture leaves only level 0 marked as filled";
	Raster *t = MakeMetalMipRaster(5);
	if(t == nil)
		return Report(false, name);
	bool ok = RenderFastFrame(t, 0, 0) == 1;
	metal::MetalRaster *natras = MetalExt(t);
	if(natras->filledLevels != 1 || natras->filledMask != 1){
		Detail("  %d levels filled with mask 0x%x, expected 1 with 0x1\n", natras->filledLevels, natras->filledMask);
		ok = false;
	}
	t->destroy();
	return Report(ok, name);
}

static bool
CheckRenderFastIntoTextureMadeMidFrame(void)
{
	const char *name = "renderFast into a camera texture made mid-frame is not wiped by its own pending clear";
	BeginFrame(GREY);
	DrawQuad(0.0f, 0.0f, 32.0f, 16.0f, RED);
	Raster *t = Raster::create(64, 64, 0, Raster::CAMERATEXTURE);
	bool ok = t != nil;
	if(ok){
		if(!metal::rasterHasPendingWork(t)){
			Detail("  the new camera texture has no pending clear\n");
			ok = false;
		}
		ok &= CopyCameraInto(t, 0, 0) == 1;
	}
	DrawQuad(40.0f, 0.0f, 72.0f, 16.0f, GREEN);
	EndFrame();
	if(t){
		uint8 *px = t->lock(0, Raster::LOCKREAD);
		ok &= px != nil;
		if(px){
			ok &= TexelIs(px, t->stride, 0, 0, 255, 0, 0);
			ok &= TexelIs(px, t->stride, 45, 5, GREY.red, GREY.green, GREY.blue);
			ok &= TexelIs(px, t->stride, 63, 63, GREY.red, GREY.green, GREY.blue);
			t->unlock(0);
		}
		t->destroy();
	}
	return Report(ok, name);
}

static bool
CheckRenderFastBeforeFirstDraw(void)
{
	const char *name = "renderFast before any draw in the frame copies the camera's clear colour";
	Raster *t = Raster::create(64, 64, 0, Raster::CAMERATEXTURE);
	if(t == nil)
		return Report(false, name);
	BeginFrame(GREY);
	DrawQuad(0.0f, 0.0f, 32.0f, 16.0f, RED);
	EndFrame();
	BeginFrame(GREY);
	bool ok = true;
	if(!metal::rasterHasPendingWork(ctx.camera->frameBuffer)){
		Detail("  the camera has no pending clear\n");
		ok = false;
	}
	ok &= CopyCameraInto(t, 0, 0) == 1;
	DrawQuad(0.0f, 0.0f, 32.0f, 16.0f, RED);
	EndFrame();
	uint8 *px = t->lock(0, Raster::LOCKREAD);
	ok &= px != nil;
	if(px){
		ok &= TexelIs(px, t->stride, 0, 0, GREY.red, GREY.green, GREY.blue);
		ok &= TexelIs(px, t->stride, 63, 63, GREY.red, GREY.green, GREY.blue);
		t->unlock(0);
	}
	t->destroy();
	return Report(ok, name);
}

static bool
CheckPassAndCopyCounters(void)
{
	const char *name = "frame stats count render passes and copies";
	Raster *t = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	Raster *ct = Raster::create(16, 16, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	if(t == nil || ct == nil){
		if(t) t->destroy();
		if(ct) ct->destroy();
		return Report(false, name);
	}
	metal::resolveRasterTarget(t);
	metal::resolveRasterTarget(ct);

	metal::FrameStats before = metal::getFrameStats();
	BeginFrame(GREY);
	DrawQuad(0.0f, 0.0f, 32.0f, 16.0f, RED);
	bool ok = CopyCameraInto(t, 0, 0) == 1;
	DrawQuad(40.0f, 0.0f, 72.0f, 16.0f, GREEN);
	EndFrame();
	metal::FrameStats after = metal::getFrameStats();
	uint32 passes = after.renderPasses - before.renderPasses;
	uint32 copies = after.copies - before.copies;
	if(passes != 2 || copies != 1){
		Detail("  copy frame: %u passes, %u copies, expected 2 and 1\n", passes, copies);
		ok = false;
	}

	before = after;
	Raster *fb = ctx.camera->frameBuffer, *zb = ctx.camera->zBuffer;
	ctx.camera->frameBuffer = ct;
	ctx.camera->zBuffer = nil;
	ctx.camera->beginUpdate();
	Set2DState();
	DrawQuad(0.0f, 0.0f, 8.0f, 8.0f, RED);
	ctx.camera->endUpdate();
	ctx.camera->frameBuffer = fb;
	ctx.camera->zBuffer = zb;
	BeginFrame(GREY);
	DrawQuad(0.0f, 0.0f, 32.0f, 16.0f, GREEN);
	EndFrame();
	after = metal::getFrameStats();
	passes = after.renderPasses - before.renderPasses;
	copies = after.copies - before.copies;
	if(passes != 2 || copies != 0){
		Detail("  target frame: %u passes, %u copies, expected 2 and 0\n", passes, copies);
		ok = false;
	}
	t->destroy();
	ct->destroy();
	return Report(ok, name);
}

static Raster*
MakeSolidTexture(RGBA c)
{
	uint8 pixels[4*4*4];
	for(int i = 0; i < 16; i++){
		pixels[i*4+0] = c.red;
		pixels[i*4+1] = c.green;
		pixels[i*4+2] = c.blue;
		pixels[i*4+3] = c.alpha;
	}
	Image *img = MakeImage(32, 4, 4, pixels, nil, 0);
	Raster *ras = Raster::create(4, 4, 32, Raster::C8888 | Raster::TEXTURE);
	if(ras && ras->setFromImage(img) == nil){
		ras->destroy();
		ras = nil;
	}
	img->destroy();
	if(ras == nil)
		Detail("  the %d,%d,%d texture could not be made\n", c.red, c.green, c.blue);
	return ras;
}

static void
DrawTextured(Raster *ras, float x0, float y0, float x1, float y1)
{
	Im2DVertex v[4];
	SetRenderStatePtr(TEXTURERASTER, ras);
	Quad(v, x0, y0, x1, y1, WHITE);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
}

static bool
CountRose(const char *what, uint32 before, uint32 after, uint32 want)
{
	if(after - before == want)
		return true;
	Detail("  %s rose by %u, expected %u\n", what, after - before, want);
	return false;
}

static bool
CheckRelockInSameFrame(void)
{
	const char *name = "a texture relocked after a draw in the same frame keeps the first draw's texels";
	Raster *ras = MakeSolidTexture(RED);
	if(ras == nil)
		return Report(false, name);
	metal::RasterStats s0 = metal::getRasterStats();
	BeginFrame(GREY);
	DrawTextured(ras, 20.0f, 20.0f, 40.0f, 40.0f);
	uint8 green[4] = { GREEN.red, GREEN.green, GREEN.blue, GREEN.alpha };
	bool ok = WriteFlatLevel(ras, 0, green, 4);
	DrawTextured(ras, 60.0f, 20.0f, 80.0f, 40.0f);
	EndFrame();
	metal::RasterStats s1 = metal::getRasterStats();
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 30, 30, RED);
		ok &= PixelIs(img, 70, 30, GREEN);
		img->destroy();
	}
	ok &= CountRose("staged uploads", s0.stagedUploads, s1.stagedUploads, 1);
	ras->destroy();
	return Report(ok, name);
}

static bool
CheckRelockHeldPreviousFrame(void)
{
	const char *name = "a texture relocked while the frame that drew it is held on the GPU is uploaded staged";
	Raster *ras = MakeSolidTexture(BLUE);
	if(ras == nil)
		return Report(false, name);
	BeginFrame(GREY);
	metal::holdFrameForTest(0.2);
	DrawTextured(ras, 20.0f, 20.0f, 40.0f, 40.0f);
	EndFrame();
	metal::RasterStats s0 = metal::getRasterStats();
	uint8 red[4] = { RED.red, RED.green, RED.blue, RED.alpha };
	bool ok = WriteFlatLevel(ras, 0, red, 4);
	metal::RasterStats s1 = metal::getRasterStats();
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 30, 30, BLUE);
		img->destroy();
	}
	BeginFrame(GREY);
	DrawTextured(ras, 20.0f, 20.0f, 40.0f, 40.0f);
	EndFrame();
	img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelIs(img, 30, 30, RED);
		img->destroy();
	}
	ok &= CountRose("staged uploads", s0.stagedUploads, s1.stagedUploads, 1);
	ras->destroy();
	return Report(ok, name);
}

static bool
CheckFreshUploadIsDirect(void)
{
	const char *name = "a texture that was never bound is uploaded directly";
	metal::RasterStats s0 = metal::getRasterStats();
	Raster *ras = MakeSolidTexture(GREEN);
	metal::RasterStats s1 = metal::getRasterStats();
	bool ok = ras != nil;
	ok &= CountRose("direct uploads", s0.directUploads, s1.directUploads, 1);
	ok &= CountRose("staged uploads", s0.stagedUploads, s1.stagedUploads, 0);
	if(ras)
		ras->destroy();
	return Report(ok, name);
}

static bool
CheckAutoMipmapInFrame(void)
{
	static const RGBA col = { 10, 200, 30, 255 };
	const char *name = "automatic mipmaps made mid-frame are sampled by the same frame and read back";
	BeginFrame(GREY);
	DrawQuad(300.0f, 300.0f, 310.0f, 310.0f, RED);
	metal::RasterStats s0 = metal::getRasterStats();
	Raster *ras = Raster::create(8, 8, 32, Raster::C8888 | Raster::TEXTURE | Raster::MIPMAP | Raster::AUTOMIPMAP);
	uint8 texel[4] = { col.red, col.green, col.blue, col.alpha };
	bool ok = ras != nil && WriteFlatLevel(ras, 0, texel, 4);
	metal::RasterStats s1 = metal::getRasterStats();
	if(ras){
		SetRenderState(TEXTUREFILTER, Texture::LINEARMIPNEAREST);
		DrawTextured(ras, 100.0f, 100.0f, 102.0f, 102.0f);
	}
	metal::holdFrameForTest(0.2);
	EndFrame();
	ok &= CountRose("mipmap blits", s0.mipmapBlits, s1.mipmapBlits, 1);
	if(ras){
		metal::RasterStats s2 = metal::getRasterStats();
		uint8 *px = ras->lock(3, Raster::LOCKREAD);
		metal::RasterStats s3 = metal::getRasterStats();
		ok &= px != nil;
		if(px){
			ok &= PixelNear(px, col.red, col.green, col.blue, col.alpha, 0, "level 3");
			ras->unlock(3);
		}
		ok &= CountRose("GPU waits", s2.gpuWaits, s3.gpuWaits, 1);
	}
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelNearRGBA(img, 100, 100, col, 1);
		img->destroy();
	}
	if(ras)
		ras->destroy();
	return Report(ok, name);
}

static void
CheckIm2D(void)
{
	CheckIm2DSolidQuad();
	CheckScissorAfterSubRectClear();
	CheckDrawAfterSubRasterClearKeepsTarget();
	CheckIm2DTexturedQuad();
	CheckIm2DVertexColours();
	CheckIm2DBlending();
	CheckIm2DAlphaTest();
	CheckIm2DSubRaster();
	CheckIm2DLineAndTriangle();
	CheckIm2DAddressing();
	CheckIm2DPrimitives(false);
	CheckIm2DPrimitives(true);
	CheckIm2DCulling();
	CheckIm2DRingGrowth();
	CheckIm2DSamplesClearedCameraTexture();
	CheckIm2DFeedbackDropped();
	CheckStaleStageOneTargetNotFeedback();
	CheckDepthAttachmentFeedbackDropped();
	CheckStaleStageNeverBreaksPass();
	CheckForeignRasterDrawsWhite();
	CheckIm2DDefaultAttributes();
	CheckIm2DCustomConstants();
	CheckStatsCountUploads();
	CheckStatsFramesWithoutDrawable();
	CheckNoPeriodicStatsByDefault();
	CheckPeriodicStatsAfterInterval();
	CheckIm2DFailedPipelineDropped();
	CheckIm2DTwoUVs();
	CheckIm2DTwoUVsNeedOverride();
	CheckIm2DTwoUVLayout();
	CheckHostPrewarm();
	CheckD3DNativeFallbackLevels();
	for(int i = 0; i < (int)(sizeof(palCases)/sizeof(palCases[0])); i++)
		CheckNativePal(&palCases[i]);
	for(int i = 0; i < (int)(sizeof(d3d9PalCases)/sizeof(d3d9PalCases[0])); i++)
		CheckD3D9NativePal(&d3d9PalCases[i]);
	CheckFilledLevelsOutOfOrder();
	CheckMinifiedMipLevels();
	CheckIm2DListIsNotStrip(false);
	CheckIm2DListIsNotStrip(true);
	CheckIm2DWideFan();
	CheckIm2DFramesInFlight();
	CheckNewCameraTextureIsTransparentBlack();
	CheckRenderFastCopiesCamera();
	CheckRenderFastOffset();
	CheckRenderFastAsFullScreenFilter();
	CheckRenderFastRejects();
	CheckDepthOnlyClearSurvivesNewTexture();
	CheckRingGuardHeldFrame();
	CheckRenderFastIntoSubRaster();
	CheckRenderFastIntoAutoMipTexture();
	CheckRenderFastIntoMipTextureMarksLevelZero();
	CheckRenderFastIntoTextureMadeMidFrame();
	CheckRenderFastBeforeFirstDraw();
	CheckPassAndCopyCounters();
	CheckRelockInSameFrame();
	CheckRelockHeldPreviousFrame();
	CheckFreshUploadIsDirect();
	CheckAutoMipmapInFrame();
}

static metal::MetalHost*
Host(void)
{
	return metal::metalGlobals.host;
}

static bool
CheckHostModes(void)
{
	metal::MetalHost *host = Host();
	int32 num = 0;
	const metal::MetalHostMode *modes = host ? host->getModes(0, &num) : nil;
	bool ok = modes && num > 0 && modes[0].flags == 0;
	if(!ok)
		Detail("  %d modes, entry 0 flags %u\n", num, modes ? modes[0].flags : 0);
	for(int32 i = 1; ok && i < num; i++)
		if(modes[i].flags != VIDEOMODEEXCLUSIVE){
			Detail("  mode %d flags %u, expected exclusive\n", i, modes[i].flags);
			ok = false;
		}
	if(ok && Engine::getNumVideoModes() != num){
		Detail("  engine lists %d modes, host %d\n", Engine::getNumVideoModes(), num);
		ok = false;
	}
	for(int32 i = 0; ok && i < num; i++){
		VideoMode vm;
		Engine::getVideoModeInfo(&vm, i);
		if(vm.width != modes[i].width || vm.height != modes[i].height ||
		   vm.depth != modes[i].depth || vm.flags != modes[i].flags){
			Detail("  mode %d engine %dx%dx%d flags %u, host %dx%dx%d flags %u\n", i,
			       vm.width, vm.height, vm.depth, vm.flags,
			       modes[i].width, modes[i].height, modes[i].depth, modes[i].flags);
			ok = false;
		}
	}
	if(ok && Engine::getNumSubSystems() != host->numDisplays()){
		Detail("  engine lists %d subsystems, host %d displays\n", Engine::getNumSubSystems(), host->numDisplays());
		ok = false;
	}
	if(ok && host->numDisplays() > 0){
		SubSystemInfo info;
		Engine::getSubSystemInfo(&info, 0);
		if(strncmp(info.name, host->displayName(0), sizeof(info.name)-1) != 0){
			Detail("  subsystem 0 \"%.*s\", display 0 \"%s\"\n", (int)sizeof(info.name)-1, info.name, host->displayName(0));
			ok = false;
		}
	}
	return Report(ok, "the default host lists modes with the windowed entry first, and the engine reports them");
}

static bool
CheckHostDisplayMode(void)
{
	metal::MetalHost *host = Host();
	int32 num = 0;
	const metal::MetalHostMode *modes = host->getModes(0, &num);
	metal::MetalHostMode cur = {};
	bool ok;
	if(host->numDisplays() == 0)
		ok = !host->displayMode(0, &cur) && num == 1;
	else
		ok = host->displayMode(0, &cur) && cur.width == modes[0].width && cur.height == modes[0].height &&
		     cur.depth == modes[0].depth && cur.refresh == modes[0].refresh;
	if(!ok)
		Detail("  display 0 mode %dx%dx%d at %d, windowed entry %dx%dx%d at %d\n",
		       cur.width, cur.height, cur.depth, cur.refresh,
		       modes[0].width, modes[0].height, modes[0].depth, modes[0].refresh);
	return Report(ok, "the windowed entry is the display's current mode");
}

static bool
CheckHostSurfaceSize(void)
{
	metal::MetalHost *host = Host();
	int fw, fh, ww, wh, lw = 0, lh = 0;
	double ls = 0.0;
	int32 hw, hh;
	FramebufferSize(&fw, &fh);
	WindowSize(&ww, &wh);
	host->drawableSize(&hw, &hh);
	float32 scale = host->backingScale();
	bool ok = ww == 640 && wh == 480 && hw == fw && hh == fh;
	ok = ok && scale > 0.0f && fw == (int)(ww*scale) && fh == (int)(wh*scale);
	ok = ok && LayerDrawableSize(&lw, &lh, &ls) && lw == fw && lh == fh && ls == scale;
	if(!ok)
		Detail("  window %dx%d, framebuffer %dx%d, host %dx%d scale %.2f, layer %dx%d scale %.2f\n",
		       ww, wh, fw, fh, hw, hh, scale, lw, lh, ls);
	return Report(ok, "the host's drawable size and backing scale match the window and the layer");
}

static bool
CheckHostVisible(void)
{
	bool ok = Host()->visible() && !WindowShown();
	if(!ok)
		Detail("  host visible %d, window visible %d\n", Host()->visible(), WindowShown());
	return Report(ok, "a window created hidden counts as visible for rendering");
}

static bool
CheckHostSizeChangeOnce(void)
{
	metal::MetalHost *host = Host();
	DrawFrame(GREY);
	bool before = host->pollSizeChange();
	SetWindowSize(700, 500);
	bool first = host->pollSizeChange();
	bool second = host->pollSizeChange();
	SetWindowSize(640, 480);
	DrawFrame(GREY);
	bool ok = !before && first && !second;
	if(!ok)
		Detail("  before %d, after resize %d, again %d; expected 0, 1, 0\n", before, first, second);
	return Report(ok, "the host reports a size change once");
}

static bool
LayerMatchesWindow(void)
{
	int fw, fh, lw = 0, lh = 0;
	double ls;
	FramebufferSize(&fw, &fh);
	bool ok = LayerDrawableSize(&lw, &lh, &ls) && lw == fw && lh == fh;
	if(!ok)
		Detail("  layer %dx%d, framebuffer %dx%d\n", lw, lh, fw, fh);
	return ok;
}

static bool
CheckLayerFollowsResize(void)
{
	int fw, fh;
	float xs;
	SetWindowSize(720, 540);
	DrawFrame(GREY);
	FramebufferSize(&fw, &fh);
	xs = WindowContentScale();
	bool ok = fw == (int)(720*xs + 0.5f);
	if(!ok)
		Detail("  framebuffer %dx%d after the resize, scale %g\n", fw, fh, xs);
	ok &= LayerMatchesWindow();
	SetWindowSize(640, 480);
	DrawFrame(GREY);
	ok &= LayerMatchesWindow();
	ok &= CornersAndCentreAre(GREY);
	return Report(ok, "the layer's drawable follows a window resize at the next frame");
}

static bool
CheckNoDrawableWhileNotVisible(void)
{
	metal::FrameStats f0 = metal::getFrameStats();
	metal::setSurfaceHiddenForTest(1);
	for(int i = 0; i < 4; i++)
		DrawFrame(GREY);
	DrawFrame(RED);
	metal::FrameStats f1 = metal::getFrameStats();
	bool pixels = CornersAndCentreAre(RED);
	metal::setSurfaceHiddenForTest(0);
	DrawFrame(GREEN);
	metal::FrameStats f2 = metal::getFrameStats();
	uint32 shown = f1.framesShown - f0.framesShown;
	uint32 acquired = f1.drawablesAcquired - f0.drawablesAcquired;
	uint32 presented = f1.framesPresented - f0.framesPresented;
	uint32 after = f2.drawablesAcquired - f1.drawablesAcquired;
	bool ok = pixels && shown == 5 && acquired == 0 && presented == 0 && after == 1;
	if(!ok)
		Detail("  not visible: %u shown, %u acquired, %u presented; visible again: %u acquired; expected 5, 0, 0, 1\n",
		       shown, acquired, presented, after);
	return Report(ok, "no drawable is acquired while the surface is not visible, and the camera still renders");
}

static bool
CheckStatsFramesWhileNotVisible(void)
{
	metal::logStats();
	metal::setSurfaceHiddenForTest(1);
	for(int i = 0; i < 5; i++)
		DrawFrame(GREY);
	metal::setSurfaceHiddenForTest(0);
	uint32 waitUs = metal::getFrameStats().drawableWaitMaxUs;
	metal::logStats();
	const char *line = metal::getStatsLine();
	StatsTail t;
	bool ok = ParseStatsTail(line, &t) && t.withoutDrawable == 5 && waitUs == 0 && t.waitMaxMs == 0.0;
	if(!ok)
		Detail("  got \"%s\", wait max %u us, expected 5 frames without drawable and no drawable wait\n", line, waitUs);
	return Report(ok, "the stats line counts frames shown while not visible as frames without drawable");
}

// sleepForTimeInterval sleeps at least the interval, so the lower bound is safe
static bool
CheckNotVisibleWaitFollowsVsync(void)
{
	int32 hz = Host()->refreshRate();
	if(hz <= 0)
		hz = 60;
	double period = 1.0/hz;
	metal::setSurfaceHiddenForTest(1);
	auto t0 = std::chrono::steady_clock::now();
	for(int i = 0; i < 20; i++)
		DrawFrameFlags(GREY, Raster::FLIPWAITVSYNCH);
	auto t1 = std::chrono::steady_clock::now();
	for(int i = 0; i < 20; i++)
		DrawFrameFlags(GREY, 0);
	auto t2 = std::chrono::steady_clock::now();
	metal::setSurfaceHiddenForTest(0);
	DrawFrame(GREY);
	double waited = std::chrono::duration<double>(t1 - t0).count();
	double unpaced = std::chrono::duration<double>(t2 - t1).count();
	bool ok = waited >= 20*period*0.95 && unpaced < 10*period;
	if(!ok)
		Detail("  20 vsync frames %.1f ms, 20 no-wait frames %.1f ms, refresh %d Hz\n",
		       waited*1000.0, unpaced*1000.0, hz);
	return Report(ok, "while not visible a vsync frame sleeps one refresh and a no-wait frame does not");
}

static void
CheckHostTable(void)
{
	CheckHostModes();
	CheckHostDisplayMode();
	CheckHostSurfaceSize();
	CheckHostVisible();
	CheckHostSizeChangeOnce();
	CheckLayerFollowsResize();
}

// logStats starts a new interval, so this runs before checks that draw
static void
CheckSurfaceNotVisible(void)
{
	CheckNoDrawableWhileNotVisible();
	CheckStatsFramesWhileNotVisible();
	CheckNotVisibleWaitFollowsVsync();
}

static bool
CheckResize(void)
{
	DestroyRasters();
	bool ok = CreateRasters(800, 600);
	if(ok){
		DrawFrame(GREY);
		Image *img = ReadBack();
		ok = img && img->width == 800 && img->height == 600;
		if(img && !ok)
			Detail("  size %dx%d, expected 800x600\n", img->width, img->height);
		if(img)
			img->destroy();
		ok &= CornersAndCentreAre(GREY);
	}
	return Report(ok, "resize to 800x600");
}

static bool
CheckRestart(void)
{
	WatchObject(metal::getWhiteTexture());
	StopEngine();
	uint32 inFlight = metal::getStateStats().framesInFlightAtTerm;
	CheckStatsLineAtStop();
	bool ok = true;
	if(WatchedObjectAlive()){
		Detail("  white texture survived term\n");
		ok = false;
	}
	WatchObject(nil);
	ok &= StartEngine(640, 480);
	if(ok){
		DrawFrame(GREEN);
		ok = CornersAndCentreAre(GREEN);
		ok &= WhiteTexel(metal::getWhiteTexture());
	}
	bool restarted = Report(ok, "stop, close, term, then init, open, start again");
	if(inFlight)
		Detail("  %u frames were not finished when term released state and rasters\n", inFlight);
	Report(inFlight == 0, "device term finishes every frame before releasing state and rasters");
	return restarted;
}

int
main(void)
{
	if(!CheckOpen()){
		printf("%d failures\n", failures);
		return 1;
	}
	CheckPrewarmLine();
	CheckClearColour(RED, "clear red");
	CheckClearColour(GREEN, "clear green");
	CheckClearColour(GREY, "clear grey");
	CheckSubRectClear();
	CheckClearWithPassOpen();
	CheckDepthOnlyClear();
	CheckShowWithoutUpdate();
	CheckTwentyFrames();
	CheckCompositeColour();
	CheckCompositeOrientation();
	CheckTexturesFromImages();
	CheckRasterFormats();
	CheckDXT();
	CheckNativeTextures();
	CheckLockWriteChangesPixels();
	CheckMipmapGeneration();
	CheckCameraTexture();
	CheckWhiteTexture();
	CheckUnallocatedRasterDefaults();
	CheckImageIntoRasterWithoutTexture();
	if(!CheckShaderVariants())
		failures++;
	if(!CheckPrewarmedPipelines())
		failures++;
	if(!CheckRenderStateRoundTrip(ctx.camera->frameBuffer))
		failures++;
	if(!CheckLightingShaderBlockSizes())
		failures++;
	if(!CheckRasterAddressing())
		failures++;
	if(!CheckRingGrowthKeepsHandedOutBuffer())
		failures++;
	if(!CheckRingBytesCountPadding())
		failures++;
	if(!CheckFlushCacheDropsFailedPipeline())
		failures++;
	if(!CheckVariantPerDraw())
		failures++;
	if(!CheckOnlyReadBlocksBound())
		failures++;
	if(!CheckOnlySampledTexturesBound())
		failures++;
	if(!CheckDefaultShader())
		failures++;
	if(!CheckWorldBlocksUploadOnChange())
		failures++;
	CheckDestroyEvictsRaster();
	CheckIm2D();
	CheckSurfaceNotVisible();
	failures += RunWorldChecks(ctx.camera);
	failures += RunSkinChecks(ctx.camera);
	failures += RunMatFXChecks(ctx.camera);
	failures += RunTargetChecks(ctx.camera);
	failures += RunHostChecks(ctx.camera);
	CheckHostTable();
	CheckResize();
	metal::setCustomConstants(staleConstants, sizeof(staleConstants));
	if(CheckRestart()){
		CheckCustomConstantsGoneAfterRestart();
		CheckShortCustomConstantsReadZero();
		CheckOversizedCustomBlockDropped();
		failures += RunRestartWorldChecks(RestartEngine, CurrentCamera);
		failures += RunRestartTargetChecks(RestartEngine, CurrentCamera);
		failures += RunRestartHostChecks(RestartEngine, CurrentCamera);
		failures += RunMsaaChecks(RestartEngineSamples, CurrentCamera);
		StopEngine();
	}

	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
