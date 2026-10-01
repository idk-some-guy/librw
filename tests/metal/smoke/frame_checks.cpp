#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include <src/metal/metaloutput.h>
#include "frame_checks.h"

using namespace rw;

static char details[8192];
static int detailsLen = 0;
static int failures = 0;
static Camera *cam;

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

static void
SetTier(int32 tier)
{
	metal::RenderSettings s = metal::getRenderSettings();
	s.tier = tier;
	metal::setRenderSettings(&s);
}

static const RGBA SKY = { 60, 90, 120, 255 };
static const RGBA RED = { 255, 0, 0, 255 };
static const RGBA GREEN = { 0, 255, 0, 255 };
static const RGBA BLUE = { 0, 0, 255, 255 };
static uint16 quadIndex[6] = { 0, 1, 2, 0, 2, 3 };

enum { DRAW_OPAQUE, DRAW_ALPHA, DRAW_ADD };

static void
EnhancedOn(void)
{
	metal::TierCaps m1 = { true, false, false };
	metal::setTierCapsForTest(&m1);
	SetTier(metal::TIER_ENHANCED);
}

static void
EnhancedOff(void)
{
	SetTier(metal::TIER_CLASSIC);
	metal::setTierCapsForTest(nil);
}

static void
Vertex(metal::Im2DVertex *v, float x, float y, RGBA c)
{
	v->setScreenX(x);
	v->setScreenY(y);
	v->setScreenZ(im2d::GetNearZ());
	v->setCameraZ(cam->nearPlane);
	v->setRecipCameraZ(1.0f/cam->nearPlane);
	v->setU(0.0f, 1.0f/cam->nearPlane);
	v->setV(0.0f, 1.0f/cam->nearPlane);
	v->setColor(c.red, c.green, c.blue, c.alpha);
}

static void
DrawMode(int mode)
{
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(ZWRITEENABLE, 0);
	SetRenderState(VERTEXALPHA, mode != DRAW_OPAQUE);
	SetRenderState(SRCBLEND, mode == DRAW_ADD ? BLENDONE : BLENDSRCALPHA);
	SetRenderState(DESTBLEND, mode == DRAW_ADD ? BLENDONE : BLENDINVSRCALPHA);
}

static void
Quad(float x0, float y0, float x1, float y1, RGBA c, int mode)
{
	metal::Im2DVertex v[4];
	Vertex(&v[0], x0, y0, c);
	Vertex(&v[1], x0, y1, c);
	Vertex(&v[2], x1, y1, c);
	Vertex(&v[3], x1, y0, c);
	DrawMode(mode);
	im2d::RenderIndexedPrimitive(PRIMTYPETRILIST, v, 4, quadIndex, 6);
}

static void
Tri(float x0, float y0, float x1, float y1, float x2, float y2, RGBA c, int mode)
{
	metal::Im2DVertex v[3];
	Vertex(&v[0], x0, y0, c);
	Vertex(&v[1], x1, y1, c);
	Vertex(&v[2], x2, y2, c);
	DrawMode(mode);
	im2d::RenderPrimitive(PRIMTYPETRILIST, v, 3);
}

static void
Begin(RGBA clear)
{
	cam->clear(&clear, Camera::CLEARIMAGE | Camera::CLEARZ | Camera::CLEARSTENCIL);
	cam->beginUpdate();
}

static void
Finish(void)
{
	cam->endUpdate();
	cam->showRaster(0);
}

static uint8*
Composite(void)
{
	Raster *fb = cam->frameBuffer;
	uint8 *px = new uint8[fb->width*fb->height*4];
	if(!metal::compositeCameraPixels(fb, px)){
		Detail("  compositeCameraPixels failed\n");
		delete[] px;
		return nil;
	}
	return px;
}

static bool
CompositeNear(uint8 *px, int x, int y, int r, int g, int b, int tol)
{
	if(px == nil)
		return false;
	uint8 *p = &px[(y*cam->frameBuffer->width + x)*4];
	if(abs(p[2] - r) <= tol && abs(p[1] - g) <= tol && abs(p[0] - b) <= tol)
		return true;
	Detail("  composite (%d,%d) is %d,%d,%d, expected %d,%d,%d within %d\n", x, y, p[2], p[1], p[0], r, g, b, tol);
	return false;
}

static bool
ImageNear(Image *img, int x, int y, int r, int g, int b, int a, int tol)
{
	if(img == nil)
		return false;
	uint8 *p = &img->pixels[y*img->stride + x*4];
	if(abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && (a < 0 || abs(p[3] - a) <= tol))
		return true;
	Detail("  image (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,%d within %d\n", x, y, p[0], p[1], p[2], p[3], r, g, b, a, tol);
	return false;
}

static bool
CheckClassicBeginScene(void)
{
	const char *name = "beginScene does nothing in the classic tier";
	SetTier(metal::TIER_CLASSIC);
	metal::beginScene(cam);
	bool active = metal::enhancedFrame() != 0;
	Begin(RED);
	Quad(10, 10, 40, 40, GREEN, DRAW_OPAQUE);
	metal::endScene(nil);
	Finish();
	uint8 *px = Composite();
	bool ok = !active && CompositeNear(px, 5, 5, 255, 0, 0, 0) && CompositeNear(px, 20, 20, 0, 255, 0, 0) &&
		metal::getFrameInfo().internalWidth == 0;
	delete[] px;
	return Report(ok, name);
}

static bool
CheckSceneRouting(void)
{
	const char *name = "an enhanced scene draws into the scene target and leaves the camera raster as a clear overlay";
	SetTier(metal::TIER_CLASSIC);
	Begin(BLUE);
	Finish();
	EnhancedOn();
	metal::beginScene(cam);
	bool active = metal::enhancedFrame() != 0;
	Begin(RED);
	Quad(10, 10, 40, 40, GREEN, DRAW_OPAQUE);
	metal::endScene(nil);
	Image *overlay = cam->frameBuffer->toImage();
	Finish();
	uint8 *px = Composite();
	metal::FrameInfo f = metal::getFrameInfo();
	bool ok = active && ImageNear(overlay, 20, 20, 0, 0, 0, 0, 0);
	ok = ok && CompositeNear(px, 5, 5, 255, 0, 0, 0) && CompositeNear(px, 20, 20, 0, 255, 0, 0);
	ok = ok && f.internalWidth == cam->frameBuffer->width && f.outputWidth == cam->frameBuffer->width &&
		f.internalHeight == cam->frameBuffer->height;
	if(overlay)
		overlay->destroy();
	delete[] px;
	EnhancedOff();
	return Report(ok, name);
}

static bool
CheckOverlayComposite(void)
{
	const char *name = "the overlay composites over the graded scene with coverage alpha";
	RGBA white50 = { 255, 255, 255, 128 };
	RGBA addRed = { 100, 0, 0, 255 };
	EnhancedOn();
	metal::beginScene(cam);
	Begin(SKY);
	metal::endScene(nil);
	Quad(0, 0, 50, 50, white50, DRAW_ALPHA);
	Quad(100, 0, 150, 50, addRed, DRAW_ADD);
	Image *overlay = cam->frameBuffer->toImage();
	Finish();
	uint8 *px = Composite();
	float a = 128/255.0f;
	int er = (int)lroundf((60/255.0f*(1.0f - a) + a)*255.0f);
	int eg = (int)lroundf((90/255.0f*(1.0f - a) + a)*255.0f);
	int eb = (int)lroundf((120/255.0f*(1.0f - a) + a)*255.0f);
	bool ok = CompositeNear(px, 25, 25, er, eg, eb, 1);
	ok = CompositeNear(px, 125, 25, 160, 90, 120, 0) && ok;
	ok = CompositeNear(px, 300, 300, 60, 90, 120, 0) && ok;
	ok = ImageNear(overlay, 25, 25, 128, 128, 128, 128, 1) && ok;
	ok = ImageNear(overlay, 125, 25, 100, 0, 0, 0, 0) && ok;
	if(overlay)
		overlay->destroy();
	delete[] px;
	EnhancedOff();
	return Report(ok, name);
}

static bool
CheckShownReadback(void)
{
	const char *name = "a shown enhanced frame reads back from the camera raster as the composite";
	EnhancedOn();
	metal::beginScene(cam);
	Begin(SKY);
	metal::endScene(nil);
	Quad(10, 10, 40, 40, GREEN, DRAW_OPAQUE);
	Finish();
	Image *img = cam->frameBuffer->toImage();
	bool ok = ImageNear(img, 60, 60, 60, 90, 120, 255, 0);
	ok = ImageNear(img, 20, 20, 0, 255, 0, 255, 0) && ok;
	if(img)
		img->destroy();
	EnhancedOff();
	return Report(ok, name);
}

static bool
CheckSceneCopy(void)
{
	const char *name = "a copy of the camera raster reads the scene during a scene and the composite after it";
	RGBA white50 = { 255, 255, 255, 128 };
	Raster *dst = Raster::create(cam->frameBuffer->width, cam->frameBuffer->height, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	EnhancedOn();
	metal::beginScene(cam);
	Begin(RED);
	Quad(10, 10, 40, 40, GREEN, DRAW_OPAQUE);
	Raster::pushContext(dst);
	cam->frameBuffer->renderFast(0, 0);
	Raster::popContext();
	Image *a = dst->toImage();
	metal::endScene(nil);
	Quad(0, 0, 50, 50, white50, DRAW_ALPHA);
	Raster::pushContext(dst);
	cam->frameBuffer->renderFast(0, 0);
	Raster::popContext();
	Image *b = dst->toImage();
	Finish();
	bool ok = ImageNear(a, 5, 5, 255, 0, 0, -1, 0) && ImageNear(a, 20, 20, 0, 255, 0, -1, 0);
	ok = ImageNear(b, 60, 60, 255, 0, 0, -1, 0) && ok;
	ok = ImageNear(b, 20, 20, 128, 255, 128, -1, 1) && ok;
	ok = ImageNear(b, 45, 45, 255, 128, 128, -1, 1) && ok;
	if(a)
		a->destroy();
	if(b)
		b->destroy();
	dst->destroy();
	EnhancedOff();
	return Report(ok, name);
}

static bool
CheckShowWithoutEnd(void)
{
	const char *name = "a scene shown without endScene is ended with a passthrough grade";
	EnhancedOn();
	metal::beginScene(cam);
	Begin(RED);
	Quad(10, 10, 40, 40, GREEN, DRAW_OPAQUE);
	Finish();
	bool after = metal::enhancedFrame() != 0;
	uint8 *px = Composite();
	bool ok = !after && CompositeNear(px, 5, 5, 255, 0, 0, 0) && CompositeNear(px, 20, 20, 0, 255, 0, 0);
	delete[] px;
	EnhancedOff();
	return Report(ok, name);
}

static void
IdentityContent(void)
{
	Begin(SKY);
	Quad(20, 20, 300, 200, RGBA{ 200, 120, 40, 255 }, DRAW_OPAQUE);
	Quad(100, 80, 500, 400, RGBA{ 30, 200, 90, 128 }, DRAW_ALPHA);
	Quad(250, 50, 600, 300, RGBA{ 90, 40, 160, 255 }, DRAW_ADD);
	Tri(320, 30, 620, 460, 40, 380, RGBA{ 250, 250, 20, 200 }, DRAW_ALPHA);
	Tri(50, 450, 200, 260, 330, 470, RGBA{ 10, 60, 230, 255 }, DRAW_OPAQUE);
	cam->endUpdate();
}

static bool
WithinOneStep(const uint8 *classic, const uint8 *enhanced)
{
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	int differing = 0, largest = 0, at = -1;
	for(int p = 0; p < w*h; p++){
		int d = 0;
		for(int c = 0; c < 3; c++)
			d = std::max(d, abs((int)classic[p*4+c] - (int)enhanced[p*4+c]));
		if(d > 0)
			differing++;
		if(d > largest){
			largest = d;
			at = p;
		}
	}
	Detail("  %d of %d pixels differ, largest difference %d/255", differing, w*h, largest);
	if(at >= 0)
		Detail(" at (%d,%d): classic %d,%d,%d enhanced %d,%d,%d", at % w, at / w, classic[at*4+2], classic[at*4+1],
			classic[at*4], enhanced[at*4+2], enhanced[at*4+1], enhanced[at*4]);
	Detail("\n");
	return largest <= 1;
}

static bool
Identity(const char *name)
{
	SetTier(metal::TIER_CLASSIC);
	IdentityContent();
	cam->showRaster(0);
	uint8 *classic = Composite();
	cam->showRaster(0);
	EnhancedOn();
	metal::beginScene(cam);
	bool active = metal::enhancedFrame() != 0;
	if(!active)
		Detail("  the enhanced tier was not active at beginScene\n");
	IdentityContent();
	metal::endScene(nil);
	cam->showRaster(0);
	uint8 *enhanced = Composite();
	EnhancedOff();
	bool ok = active && classic && enhanced && WithinOneStep(classic, enhanced);
	delete[] classic;
	delete[] enhanced;
	return Report(ok, name);
}

static const char *emitTestSrc =
"#ifndef BUFFER_EMISSIVE\n"
"#define BUFFER_EMISSIVE 9\n"
"#endif\n"
"fragment float4\n"
"emitTestFS(FragmentIn in [[stage_in]],\n"
"           constant float &emissiveScale [[buffer(BUFFER_EMISSIVE)]])\n"
"{\n"
"	return float4(in.color.rgb*emissiveScale, in.color.a);\n"
"}\n";

static bool
SceneNear(const float *got, const float *want, float tol, const char *what)
{
	for(int c = 0; c < 3; c++)
		if(fabsf(got[c] - want[c]) > tol){
			Detail("  %s is %.4f,%.4f,%.4f, expected %.4f,%.4f,%.4f within %.4f\n", what, got[0], got[1], got[2],
				want[0], want[1], want[2], tol);
			return false;
		}
	return true;
}

static bool
CheckSceneEmission(void)
{
	const char *name = "an emissive blend rises above white in the scene and a plain blend over it saturates as in 8 bits";
	const char *src[] = { metal::header_metal_src, metal::im2d_metal_src, emitTestSrc, nil };
	RGBA glow = { 90, 40, 160, 255 };
	RGBA yellow = { 250, 250, 20, 200 };
	metal::Shader *emit = metal::Shader::create(src, "im2dVS", "emitTestFS", 0);
	EnhancedOn();
	bool warm = emit && metal::prewarmIm2DShader(emit, 0, 1, BLENDONE, BLENDONE, 1);
	metal::StateStats before = metal::getStateStats();
	metal::beginScene(cam);
	Begin(SKY);
	metal::im2dOverrideShader = emit;
	metal::setDrawEmissiveScale(3.0f);
	Quad(10, 10, 60, 60, glow, DRAW_ADD);
	metal::setDrawEmissiveScale(1.0f);
	metal::im2dOverrideShader = nil;
	Quad(10, 10, 30, 60, yellow, DRAW_ALPHA);
	Quad(100, 10, 150, 60, glow, DRAW_ADD);
	cam->endUpdate();
	float e[4], over[4], plain[4];
	bool read = metal::readScenePixel(50, 30, e) && metal::readScenePixel(20, 30, over) &&
		metal::readScenePixel(120, 30, plain);
	metal::endScene(nil);
	cam->showRaster(0);
	metal::StateStats after = metal::getStateStats();
	EnhancedOff();
	if(emit)
		emit->destroy();
	float a = 200/255.0f;
	float wantE[3] = { 330/255.0f, 210/255.0f, 600/255.0f };
	float wantOver[3] = { 250/255.0f*a + (1.0f - a), 250/255.0f*a + 210/255.0f*(1.0f - a), 20/255.0f*a + (1.0f - a) };
	float wantPlain[3] = { 150/255.0f, 130/255.0f, 1.0f };
	bool ok = warm && read && SceneNear(e, wantE, 0.004f, "the emitter");
	ok = read && SceneNear(over, wantOver, 0.002f, "the translucent draw over the emitter") && ok;
	ok = read && SceneNear(plain, wantPlain, 0.002f, "the plain additive draw") && ok;
	ok = read && e[2] > 2.0f && std::max(over[0], std::max(over[1], over[2])) <= 1.0f && plain[2] <= 1.0f && ok;
	if(!warm || !read)
		Detail("  shader %d warm %d read %d\n", emit != nil, warm, read);
	if(after.pipelinesLate != before.pipelinesLate){
		Detail("  late pipelines %u -> %u\n", before.pipelinesLate, after.pipelinesLate);
		ok = false;
	}
	return Report(ok, name);
}

static const char *brightTestSrc =
"fragment float4\n"
"brightTestFS(FragmentIn in [[stage_in]])\n"
"{\n"
"	return float4(in.color.rgb*1.5, in.color.a);\n"
"}\n";

static bool
BrightDraw(metal::Shader *sh, float emissive, int mode, RGBA c, float out[4])
{
	metal::beginScene(cam);
	Begin(BLUE);
	Quad(0, 0, 80, 80, RGBA{ 0, 0, 0, 255 }, DRAW_OPAQUE);
	metal::im2dOverrideShader = sh;
	metal::setDrawEmissiveScale(emissive);
	Quad(10, 10, 60, 60, c, mode);
	metal::setDrawEmissiveScale(1.0f);
	metal::im2dOverrideShader = nil;
	cam->endUpdate();
	bool read = metal::readScenePixel(30, 30, out);
	metal::endScene(nil);
	cam->showRaster(0);
	return read;
}

static bool
CheckSceneOpaqueClamp(void)
{
	const char *name = "an opaque scene draw clamps a shader output above white unless it is emissive";
	const char *src[] = { metal::header_metal_src, metal::im2d_metal_src, brightTestSrc, nil };
	metal::Shader *sh = metal::Shader::create(src, "im2dVS", "brightTestFS", 0);
	EnhancedOn();
	bool warm = sh && metal::prewarmIm2DShader(sh, 0, 0, BLENDONE, BLENDZERO, 1);
	metal::StateStats before = metal::getStateStats();
	float plain[4], emit[4];
	RGBA c = { 200, 100, 255, 255 };
	bool read = warm && BrightDraw(sh, 1.0f, DRAW_OPAQUE, c, plain) && BrightDraw(sh, 3.0f, DRAW_OPAQUE, c, emit);
	metal::StateStats after = metal::getStateStats();
	EnhancedOff();
	if(sh)
		sh->destroy();
	float wantPlain[3] = { 1.0f, 150/255.0f, 1.0f };
	float wantEmit[3] = { 300/255.0f, 150/255.0f, 1.5f };
	bool ok = read && SceneNear(plain, wantPlain, 0.002f, "the plain opaque draw");
	ok = read && SceneNear(emit, wantEmit, 0.004f, "the emissive opaque draw") && ok;
	if(!read)
		Detail("  shader %d warm %d read %d\n", sh != nil, warm, read);
	if(after.pipelinesLate != before.pipelinesLate){
		Detail("  late pipelines %u -> %u\n", before.pipelinesLate, after.pipelinesLate);
		ok = false;
	}
	return Report(ok, name);
}

static bool
CheckSceneSourceSaturates(void)
{
	const char *name = "a blended scene draw saturates a shader output above white before blending unless it is emissive";
	const char *src[] = { metal::header_metal_src, metal::im2d_metal_src, brightTestSrc, nil };
	metal::Shader *sh = metal::Shader::create(src, "im2dVS", "brightTestFS", 0);
	EnhancedOn();
	bool warm = sh && metal::prewarmIm2DShader(sh, 0, 1, BLENDSRCALPHA, BLENDINVSRCALPHA, 1);
	metal::StateStats before = metal::getStateStats();
	float plain[4], emit[4];
	RGBA c = { 200, 100, 255, 128 };
	bool read = warm && BrightDraw(sh, 1.0f, DRAW_ALPHA, c, plain) && BrightDraw(sh, 3.0f, DRAW_ALPHA, c, emit);
	metal::StateStats after = metal::getStateStats();
	EnhancedOff();
	if(sh)
		sh->destroy();
	float sa = 128/255.0f;
	float wantPlain[3] = { sa, 150/255.0f*sa, sa };
	float wantEmit[3] = { 300/255.0f*sa, 150/255.0f*sa, 1.5f*sa };
	bool ok = read && SceneNear(plain, wantPlain, 0.003f, "the plain blended draw");
	ok = read && SceneNear(emit, wantEmit, 0.004f, "the emissive blended draw") && ok;
	if(!read)
		Detail("  shader %d warm %d read %d\n", sh != nil, warm, read);
	if(after.pipelinesLate != before.pipelinesLate){
		Detail("  late pipelines %u -> %u\n", before.pipelinesLate, after.pipelinesLate);
		ok = false;
	}
	return Report(ok, name);
}

static uint64 Im2DKey(uint32 fmt);

static bool
CheckSceneBlendInShader(void)
{
	const char *name = "scene pipelines blend in the fragment shader with fixed-function blending off";
	EnhancedOn();
	metal::PipelineBlendInfo scene = metal::pipelineBlendInfo(Im2DKey(metal::COLORFMT_SCENE));
	metal::PipelineBlendInfo rgba8 = metal::pipelineBlendInfo(Im2DKey(metal::COLORFMT_RGBA8));
	metal::PipelineBlendInfo overlay = metal::pipelineBlendInfo(Im2DKey(metal::COLORFMT_OVERLAY));
	EnhancedOff();
	bool ok = scene.cached && !scene.fixedBlend && scene.shaderBlend;
	ok = ok && rgba8.cached && rgba8.fixedBlend && !rgba8.shaderBlend;
	ok = ok && overlay.cached && overlay.fixedBlend && !overlay.shaderBlend;
	if(!ok)
		Detail("  cached/fixed/shader: scene %d/%d/%d rgba8 %d/%d/%d overlay %d/%d/%d\n",
			scene.cached, scene.fixedBlend, scene.shaderBlend, rgba8.cached, rgba8.fixedBlend, rgba8.shaderBlend,
			overlay.cached, overlay.fixedBlend, overlay.shaderBlend);
	return Report(ok, name);
}

static bool
NearF(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static bool
CheckEmission(void)
{
	const char *name = "an emissive draw writes its colour times the scale into the scene";
	RGBA dark = { 20, 20, 20, 255 };
	RGBA glow = { 60, 30, 15, 255 };
	Raster *dst = Raster::create(cam->frameBuffer->width, cam->frameBuffer->height, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	EnhancedOn();
	metal::beginScene(cam);
	Begin(dark);
	metal::setEmissiveScale(3.0f);
	Quad(10, 10, 60, 60, glow, DRAW_ADD);
	metal::setEmissiveScale(1.0f);
	Quad(100, 10, 150, 60, glow, DRAW_ADD);
	Raster::pushContext(dst);
	cam->frameBuffer->renderFast(0, 0);
	Raster::popContext();
	Image *scene = dst->toImage();
	metal::endScene(nil);
	float e[4], p[4];
	bool read = metal::readFrameOutput(30, 30, e) && metal::readFrameOutput(120, 30, p);
	Finish();
	EnhancedOff();
	bool ok = read && ImageNear(scene, 30, 30, 200, 110, 65, -1, 1) && ImageNear(scene, 120, 30, 80, 50, 35, -1, 1);
	ok = ok && NearF(e[0], 200/255.0f, 0.002f) && NearF(e[1], 110/255.0f, 0.002f) && NearF(e[2], 65/255.0f, 0.002f);
	ok = ok && NearF(p[0], 80/255.0f, 0.002f) && NearF(p[1], 50/255.0f, 0.002f) && NearF(p[2], 35/255.0f, 0.002f);
	if(read && !ok)
		Detail("  output: emitter %.4f,%.4f,%.4f plain %.4f,%.4f,%.4f\n", e[0], e[1], e[2], p[0], p[1], p[2]);
	if(scene)
		scene->destroy();
	dst->destroy();
	return Report(ok, name);
}

static bool
CheckEmissionClassic(void)
{
	const char *name = "the emissive scale changes nothing in the classic tier";
	RGBA dark = { 20, 20, 20, 255 };
	RGBA glow = { 60, 30, 15, 255 };
	SetTier(metal::TIER_CLASSIC);
	metal::beginScene(cam);
	Begin(dark);
	metal::setEmissiveScale(3.0f);
	Quad(10, 10, 60, 60, glow, DRAW_ADD);
	metal::setEmissiveScale(1.0f);
	metal::endScene(nil);
	Finish();
	uint8 *px = Composite();
	cam->showRaster(0);
	bool ok = CompositeNear(px, 30, 30, 80, 50, 35, 0);
	delete[] px;
	return Report(ok, name);
}

static bool
CheckShoulder(void)
{
	const char *name = "the SDR composite and a copy after endScene are unity below white and pull a pixel above white toward white";
	RGBA black = { 0, 0, 0, 255 };
	RGBA glow = { 200, 100, 40, 255 };
	RGBA grey = { 77, 77, 77, 255 };
	Raster *dst = Raster::create(cam->frameBuffer->width, cam->frameBuffer->height, 0, Raster::C8888 | Raster::CAMERATEXTURE);
	EnhancedOn();
	metal::beginScene(cam);
	Begin(black);
	metal::setEmissiveScale(3.0f);
	Quad(10, 10, 60, 60, glow, DRAW_ADD);
	metal::setEmissiveScale(1.0f);
	Quad(100, 10, 150, 60, grey, DRAW_OPAQUE);
	metal::endScene(nil);
	float o[4];
	bool read = metal::readFrameOutput(30, 30, o);
	Raster::pushContext(dst);
	cam->frameBuffer->renderFast(0, 0);
	Raster::popContext();
	Image *copy = dst->toImage();
	Finish();
	uint8 *px = Composite();
	EnhancedOff();
	bool ok = read && NearF(o[0], 600/255.0f, 0.01f) && NearF(o[1], 300/255.0f, 0.01f) && NearF(o[2], 120/255.0f, 0.002f);
	if(!read)
		Detail("  the frame output was not read\n");
	else if(!ok)
		Detail("  output: emitter %.4f,%.4f,%.4f\n", o[0], o[1], o[2]);
	if(read){
		float m = fmaxf(o[0], fmaxf(o[1], o[2]));
		int er = (int)lroundf(sdrShoulder(o[0], m)*255.0f);
		int eg = (int)lroundf(sdrShoulder(o[1], m)*255.0f);
		int eb = (int)lroundf(sdrShoulder(o[2], m)*255.0f);
		ok = CompositeNear(px, 30, 30, er, eg, eb, 1) && ok;
		ok = ImageNear(copy, 30, 30, er, eg, eb, -1, 1) && ok;
		ok = eb > 150 && ok;
	}
	ok = CompositeNear(px, 120, 30, 77, 77, 77, 0) && ok;
	ok = ImageNear(copy, 120, 30, 77, 77, 77, -1, 0) && ok;
	delete[] px;
	if(copy)
		copy->destroy();
	dst->destroy();
	return Report(ok, name);
}

static bool
CheckEmissiveReset(void)
{
	const char *name = "a plain draw after an emitter saturates, and a new scene starts with the emissive scale at 1.0";
	RGBA grey = { 100, 100, 100, 255 };
	RGBA glow = { 60, 30, 15, 255 };
	RGBA bright = { 200, 200, 200, 255 };
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	float after[4], next[4];
	EnhancedOn();
	metal::beginScene(cam);
	Begin(grey);
	metal::setEmissiveScale(3.0f);
	Quad(10, 10, 60, 60, glow, DRAW_ADD);
	metal::setEmissiveScale(1.0f);
	Quad(100, 10, 150, 60, bright, DRAW_ADD);
	metal::setEmissiveScale(3.0f);
	cam->endUpdate();
	bool read = metal::readScenePixel(120, 30, after);
	metal::endScene(nil);
	cam->showRaster(0);
	metal::beginScene(cam);
	float scale = metal::getEmissiveScale();
	Begin(grey);
	Quad(100, 10, 150, 60, bright, DRAW_ADD);
	cam->endUpdate();
	read = metal::readScenePixel(120, 30, next) && read;
	metal::endScene(nil);
	cam->showRaster(0);
	metal::setEmissiveScale(1.0f);
	EnhancedOff();
	bool ok = read && SceneNear(after, white, 0.0f, "the plain draw after the emitter");
	ok = read && SceneNear(next, white, 0.0f, "the plain draw in the next scene") && ok;
	if(!read)
		Detail("  the scene was not read\n");
	if(scale != 1.0f){
		Detail("  the emissive scale is %.2f after beginScene\n", scale);
		ok = false;
	}
	return Report(ok, name);
}

static bool
CheckSceneResize(void)
{
	const char *name = "the scene targets follow a camera raster resize without a restart";
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	EnhancedOn();
	metal::beginScene(cam);
	Begin(RED);
	metal::endScene(nil);
	Finish();
	cam->frameBuffer->destroy();
	cam->zBuffer->destroy();
	cam->frameBuffer = Raster::create(w/2, h/2, 0, Raster::CAMERA);
	cam->zBuffer = Raster::create(w/2, h/2, 0, Raster::ZBUFFER);
	metal::beginScene(cam);
	Begin(GREEN);
	metal::endScene(nil);
	Finish();
	metal::FrameInfo f = metal::getFrameInfo();
	uint8 *px = Composite();
	bool ok = f.outputWidth == w/2 && f.outputHeight == h/2 && f.internalWidth == w/2 && f.internalHeight == h/2 &&
		CompositeNear(px, w/4, h/4, 0, 255, 0, 0);
	if(f.outputWidth != w/2 || f.internalWidth != w/2)
		Detail("  output %dx%d internal %dx%d after resize to %dx%d\n", f.outputWidth, f.outputHeight,
			f.internalWidth, f.internalHeight, w/2, h/2);
	delete[] px;
	cam->frameBuffer->destroy();
	cam->zBuffer->destroy();
	cam->frameBuffer = Raster::create(w, h, 0, Raster::CAMERA);
	cam->zBuffer = Raster::create(w, h, 0, Raster::ZBUFFER);
	EnhancedOff();
	return Report(ok, name);
}

static bool
CheckSettingsDefaults(void)
{
	const char *name = "render settings default to the classic tier and clamp their ranges";
	metal::RenderSettings s = metal::getRenderSettings();
	bool ok = s.tier == metal::TIER_CLASSIC && s.upscale == metal::UPSCALE_NATIVE && !s.dynamicResolution &&
		s.hdrOutput == metal::HDROUTPUT_AUTO && s.emitterPeakNits == 1000.0f && s.softness == 1.0f;
	if(!ok)
		Detail("  defaults tier %d upscale %d dynamic %d hdr %d peak %.1f softness %.2f\n", s.tier, s.upscale,
			s.dynamicResolution, s.hdrOutput, s.emitterPeakNits, s.softness);
	metal::RenderSettings bad = s;
	bad.tier = 7;
	bad.upscale = -2;
	bad.dynamicResolution = 1;
	bad.hdrOutput = 5;
	bad.emitterPeakNits = 99999.0f;
	bad.softness = -1.0f;
	metal::setRenderSettings(&bad);
	metal::RenderSettings got = metal::getRenderSettings();
	bool clamped = got.tier == metal::TIER_CLASSIC && got.upscale == metal::UPSCALE_NATIVE && !got.dynamicResolution &&
		got.hdrOutput == metal::HDROUTPUT_AUTO && got.emitterPeakNits == 4000.0f && got.softness == 0.0f &&
		metal::getFrameInfo().tier == metal::TIER_CLASSIC;
	if(!clamped)
		Detail("  clamped tier %d upscale %d dynamic %d hdr %d peak %.1f softness %.2f\n", got.tier, got.upscale,
			got.dynamicResolution, got.hdrOutput, got.emitterPeakNits, got.softness);
	metal::setRenderSettings(&s);
	return Report(ok && clamped, name);
}

static bool
CheckTierFallback(void)
{
	const char *name = "a tier above the device's capability falls back with a reason";
	metal::TierCaps noRT = { true, false, false };
	metal::TierCaps noGPU = { false, false, false };
	metal::setTierCapsForTest(&noRT);
	SetTier(metal::TIER_MODERN);
	metal::FrameInfo a = metal::getFrameInfo();
	int32 kept = metal::getRenderSettings().tier;
	metal::setTierCapsForTest(&noGPU);
	SetTier(metal::TIER_ENHANCED);
	metal::FrameInfo b = metal::getFrameInfo();
	metal::setTierCapsForTest(nil);
	SetTier(metal::TIER_MODERN);
	metal::FrameInfo c = metal::getFrameInfo();
	SetTier(metal::TIER_CLASSIC);
	bool ok = a.tier == metal::TIER_ENHANCED && a.tierReason == metal::TIERREASON_NORAYTRACING && kept == metal::TIER_MODERN;
	ok = ok && b.tier == metal::TIER_CLASSIC && b.tierReason == metal::TIERREASON_NOAPPLEGPU;
	ok = ok && c.tier == metal::getSupportedTier() && (c.tierReason == metal::TIERREASON_NONE) == (c.tier == metal::TIER_MODERN);
	ok = ok && metal::getFrameInfo().tier == metal::TIER_CLASSIC;
	if(!ok)
		Detail("  no ray tracing: tier %d reason %d kept %d; no Apple GPU: tier %d reason %d; device: tier %d reason %d supported %d\n",
			a.tier, a.tierReason, kept, b.tier, b.tierReason, c.tier, c.tierReason, metal::getSupportedTier());
	return Report(ok, name);
}

static uint64
Im2DHostKey(uint32 fmt)
{
	metal::PipelineDesc d;
	memset(&d, 0, sizeof(d));
	d.shader = metal::im2dShader->shaderId;
	d.variant = metal::VARIANT_ALPHATEST & metal::im2dShader->variantMask;
	d.vertexLayout = metal::im2dVertexLayout;
	d.blendEnable = true;
	d.srcBlend = BLENDINVDESTALPHA;
	d.destBlend = BLENDSRCALPHASAT;
	d.writeMask = 15;
	d.colorFormat = fmt;
	d.depthFormat = metal::DEPTHFMT_D32S8;
	d.sampleCount = 1;
	return metal::pipelineKey(d);
}

static uint64
Im2DKey(uint32 fmt)
{
	metal::PipelineDesc d;
	memset(&d, 0, sizeof(d));
	d.shader = metal::im2dShader->shaderId;
	d.variant = metal::VARIANT_ALPHATEST & metal::im2dShader->variantMask;
	d.vertexLayout = metal::im2dVertexLayout;
	d.blendEnable = true;
	d.srcBlend = BLENDSRCALPHA;
	d.destBlend = BLENDINVSRCALPHA;
	d.writeMask = 15;
	d.colorFormat = fmt;
	d.depthFormat = metal::DEPTHFMT_NONE;
	d.sampleCount = 1;
	return metal::pipelineKey(d);
}

static bool
CheckClassicPrewarm(void)
{
	const char *name = "the classic tier prewarms only the camera format";
	bool rgba8 = metal::pipelineCached(Im2DKey(metal::COLORFMT_RGBA8)) != 0;
	bool scene = metal::pipelineCached(Im2DKey(metal::COLORFMT_SCENE)) != 0;
	bool overlay = metal::pipelineCached(Im2DKey(metal::COLORFMT_OVERLAY)) != 0;
	if(!rgba8 || scene || overlay)
		Detail("  cached: rgba8 %d scene %d overlay %d\n", rgba8, scene, overlay);
	return Report(rgba8 && !scene && !overlay, name);
}

static bool
CheckHostPrewarmAcrossTiers(void)
{
	const char *name = "a host pipeline prewarmed in the classic tier gains the scene and overlay formats at the first switch";
	metal::TierCaps m1 = { true, false, false };
	bool32 built = metal::prewarmIm2DShader(metal::im2dShader, 0, 1, BLENDINVDESTALPHA, BLENDSRCALPHASAT, 1);
	bool rgba8 = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_RGBA8)) != 0;
	bool sceneBefore = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_SCENE)) != 0;
	metal::setTierCapsForTest(&m1);
	SetTier(metal::TIER_ENHANCED);
	bool scene = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_SCENE)) != 0;
	bool overlay = metal::pipelineCached(Im2DHostKey(metal::COLORFMT_OVERLAY)) != 0;
	SetTier(metal::TIER_CLASSIC);
	metal::setTierCapsForTest(nil);
	bool ok = built && rgba8 && !sceneBefore && scene && overlay;
	if(!ok)
		Detail("  built %d; classic: rgba8 %d scene %d; enhanced: scene %d overlay %d\n", built, rgba8, sceneBefore, scene, overlay);
	return Report(ok, name);
}

static bool
CheckEnhancedPrewarm(void)
{
	const char *name = "an enhanced tier has the scene and overlay formats warm; switching tiers builds nothing again";
	metal::TierCaps m1 = { true, false, false };
	metal::setTierCapsForTest(&m1);
	SetTier(metal::TIER_ENHANCED);
	bool scene = metal::pipelineCached(Im2DKey(metal::COLORFMT_SCENE)) != 0;
	bool overlay = metal::pipelineCached(Im2DKey(metal::COLORFMT_OVERLAY)) != 0;
	metal::StateStats before = metal::getStateStats();
	SetTier(metal::TIER_CLASSIC);
	SetTier(metal::TIER_ENHANCED);
	SetTier(metal::TIER_CLASSIC);
	metal::setTierCapsForTest(nil);
	metal::StateStats after = metal::getStateStats();
	bool ok = scene && overlay && after.pipelinesAtInit == before.pipelinesAtInit && after.pipelinesLate == before.pipelinesLate &&
		after.formatPrewarms == before.formatPrewarms;
	if(!ok)
		Detail("  scene %d overlay %d; pipelines at init %u -> %u, late %u -> %u, format prewarms %u -> %u\n", scene, overlay,
			before.pipelinesAtInit, after.pipelinesAtInit, before.pipelinesLate, after.pipelinesLate,
			before.formatPrewarms, after.formatPrewarms);
	return Report(ok, name);
}

static bool
CheckTierLatchedMidFrame(void)
{
	const char *name = "a tier written mid-frame applies when the frame ends";
	RGBA col = { 0, 0, 0, 255 };
	metal::TierCaps m1 = { true, false, false };
	metal::setTierCapsForTest(&m1);
	cam->clear(&col, Camera::CLEARIMAGE);
	cam->beginUpdate();
	metal::startFrame();
	SetTier(metal::TIER_ENHANCED);
	int32 during = metal::getFrameInfo().tier;
	int32 written = metal::getRenderSettings().tier;
	cam->endUpdate();
	cam->beginUpdate();
	int32 update = metal::getFrameInfo().tier;
	cam->endUpdate();
	cam->showRaster(0);
	int32 after = metal::getFrameInfo().tier;
	SetTier(metal::TIER_CLASSIC);
	metal::setTierCapsForTest(nil);
	bool ok = during == metal::TIER_CLASSIC && written == metal::TIER_ENHANCED && update == metal::TIER_CLASSIC &&
		after == metal::TIER_ENHANCED && metal::getFrameInfo().tier == metal::TIER_CLASSIC;
	if(!ok)
		Detail("  during the frame %d, written %d, at the next update %d, after the frame %d, reset %d\n", during, written,
			update, after, metal::getFrameInfo().tier);
	return Report(ok, name);
}

static bool
CheckStatsTail(void)
{
	const char *name = "the stats line ends with the tier, frame sizes, upscaler, EDR state and bloom weight";
	metal::logStats();
	const char *line = metal::getStatsLine();
	const char *tail = strstr(line, " samples ");
	char tier[16], ups[16], edr[8];
	int iw, ih, ow, oh;
	unsigned samples;
	float hr, ph, peak, bloom;
	int n = tail ? sscanf(tail, " samples %u tier %15s internal %dx%d output %dx%d upscaler %15s edr %7s headroom %f/%f peak %f bloom %f",
		&samples, tier, &iw, &ih, &ow, &oh, ups, edr, &hr, &ph, &peak, &bloom) : 0;
	bool ok = n == 12 && strcmp(tier, "classic") == 0 && strcmp(ups, "none") == 0 && strcmp(edr, "off") == 0 &&
		hr == 1.0f && ph == 1.0f && peak == 1.0f && bloom == 0.0f && line[strlen(line)-1] == '\n';
	if(!ok)
		Detail("  parsed %d fields from \"%s\"\n", n, line);
	return Report(ok, name);
}

int
RunFrameChecks(Camera *camera)
{
	failures = 0;
	cam = camera;
	CheckSettingsDefaults();
	CheckClassicPrewarm();
	CheckHostPrewarmAcrossTiers();
	CheckTierFallback();
	CheckEnhancedPrewarm();
	CheckTierLatchedMidFrame();
	CheckStatsTail();
	CheckClassicBeginScene();
	CheckSceneRouting();
	CheckOverlayComposite();
	CheckShownReadback();
	CheckSceneCopy();
	CheckShowWithoutEnd();
	Identity("enhanced frames are within 1/255 of classic frames at one sample");
	CheckSceneBlendInShader();
	CheckSceneEmission();
	CheckSceneOpaqueClamp();
	CheckSceneSourceSaturates();
	CheckSceneResize();
	CheckEmission();
	CheckEmissionClassic();
	CheckShoulder();
	CheckEmissiveReset();
	SetTier(metal::TIER_CLASSIC);
	return failures;
}

int
RunRestartFrameChecks(bool (*restart)(uint32 samples), Camera *(*camera)(void))
{
	const char *name = "enhanced frames are within 1/255 of classic frames at four samples";
	failures = 0;
	if(!restart(4)){
		cam = camera();
		Report(false, name);
		restart(1);
		cam = camera();
		return failures;
	}
	cam = camera();
	Identity(name);
	if(!restart(1))
		Report(false, "the engine restarts at one sample after the frame checks");
	cam = camera();
	return failures;
}
