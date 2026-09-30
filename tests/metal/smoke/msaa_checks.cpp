#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include "msaa_checks.h"
#include "world_checks.h"
#include "objc_checks.h"
#include "state_checks.h"

using namespace rw;

static char details[4096];
static int detailsLen = 0;
static int failures = 0;

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

static const RGBA BLACK = { 0, 0, 0, 255 };
static const RGBA RED = { 255, 0, 0, 255 };
static const RGBA GREEN = { 0, 255, 0, 255 };
static const RGBA BLUE = { 0, 0, 255, 255 };
static const RGBA GREY = { 128, 128, 128, 255 };

static Camera *cam;

using metal::Im2DVertex;
using metal::MetalRaster;

static Im2DVertex
Vert(float x, float y, RGBA c, float u = 0.0f, float v = 0.0f)
{
	Im2DVertex vt;
	float recipz = 1.0f/cam->nearPlane;
	vt.setScreenX(x);
	vt.setScreenY(y);
	vt.setScreenZ(im2d::GetNearZ());
	vt.setCameraZ(cam->nearPlane);
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
RestoreState(void)
{
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
}

static void
BeginFrame(RGBA col)
{
	cam->clear(&col, Camera::CLEARIMAGE | Camera::CLEARZ | Camera::CLEARSTENCIL);
	cam->beginUpdate();
	Set2DState();
}

static void
EndFrame(void)
{
	Set2DState();
	cam->endUpdate();
	cam->showRaster(0);
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

static bool
BytesNear(const uint8 *p, const int *want, int n, int tol, const char *what, int x, int y)
{
	for(int i = 0; i < n; i++){
		int d = p[i] - want[i];
		if(d < -tol || d > tol){
			Detail("  %s (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,%d within %d\n", what, x, y,
			       p[0], p[1], p[2], n > 3 ? p[3] : 0, want[0], want[1], want[2], n > 3 ? want[3] : 0, tol);
			return false;
		}
	}
	return true;
}

static bool32
CopyCameraInto(Raster *t, int32 x, int32 y)
{
	Raster::pushContext(t);
	bool32 r = cam->frameBuffer->renderFast(x, y);
	Raster::popContext();
	return r;
}

static Image*
ReadBack(void)
{
	Image *img = cam->frameBuffer->toImage();
	if(img == nil)
		Detail("  toImage returned nil\n");
	else if(img->depth != 32 || img->bpp != 4 || img->stride != img->width*4){
		Detail("  image layout depth %d bpp %d stride %d\n", img->depth, img->bpp, img->stride);
		img->destroy();
		return nil;
	}
	return img;
}

static uint32
CameraSamples(void)
{
	return GETMETALRASTEREXT(cam->frameBuffer)->numSamples;
}

static void
EdgeFrame(void)
{
	BeginFrame(BLACK);
	DrawQuad(50.0f, 50.0f, 100.25f, 150.0f, RED);
	DrawQuad(150.0f, 50.0f, 200.75f, 150.0f, GREEN);
	EndFrame();
}

static void
Measure(const char *what, void (*frame)(void))
{
	metal::FrameStats f0 = metal::getFrameStats();
	frame();
	metal::FrameStats f1 = metal::getFrameStats();
	printf("msaa: %s at %u samples: %u passes, %u copies\n", what, CameraSamples(),
	       f1.renderPasses - f0.renderPasses, f1.copies - f0.copies);
}

static bool
CheckMsaaPremise(void)
{
	const char *name = "four samples: the camera raster is multisampled and the device uses the standard sample positions";
	static const float want[8] = { 0.375f, 0.125f, 0.875f, 0.375f, 0.125f, 0.625f, 0.625f, 0.875f };
	float xy[8];
	uint32 maxSamples = Engine::getMaxMultiSamplingLevels();
	uint32 samples = Engine::getMultiSamplingLevels();
	bool ok = maxSamples >= 4 && samples == 4 && CameraSamples() == 4;
	if(!ok)
		Detail("  max %u, engine %u, camera raster %u samples\n", maxSamples, samples, CameraSamples());
	if(!DefaultSamplePositions(4, xy)){
		Detail("  no sample positions\n");
		return Report(false, name);
	}
	for(int i = 0; i < 8; i++)
		if(fabsf(xy[i] - want[i]) > 1.0f/32.0f){
			Detail("  sample %d %c is %.4f, expected %.4f\n", i/2, i & 1 ? 'y' : 'x', xy[i], want[i]);
			ok = false;
		}
	return Report(ok, name);
}

static bool
CheckMsaaEdges(uint32 samples)
{
	const char *name = samples > 1 ? "four samples: edges resolve to their coverage" :
		"one sample: the same edges are not multisampled";
	RGBA e1 = samples > 1 ? RGBA{ 64, 0, 0, 255 } : BLACK;
	RGBA e2 = samples > 1 ? RGBA{ 0, 191, 0, 255 } : GREEN;
	int tol = samples > 1 ? 1 : 0;
	Measure("edge frame", EdgeFrame);
	Image *img = ReadBack();
	if(img == nil)
		return Report(false, name);
	bool ok = PixelNearRGBA(img, 100, 100, e1, tol);
	ok &= PixelNearRGBA(img, 200, 100, e2, tol);
	ok &= PixelNearRGBA(img, 75, 100, RED, 0);
	ok &= PixelNearRGBA(img, 101, 100, BLACK, 0);
	ok &= PixelNearRGBA(img, 201, 100, BLACK, 0);
	img->destroy();
	return Report(ok, name);
}

static Raster *copyTarget;
static bool32 copied;

static void
CopyFrame(void)
{
	BeginFrame(BLACK);
	DrawQuad(50.0f, 50.0f, 100.25f, 150.0f, RED);
	copied = CopyCameraInto(copyTarget, 0, 0);
	DrawQuad(100.25f, 50.0f, 150.0f, 150.0f, GREEN);
	EndFrame();
}

static void
MeasureCopyFrame(void)
{
	copyTarget = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	if(copyTarget == nil)
		return;
	Measure("copy frame", CopyFrame);
	copyTarget->destroy();
	copyTarget = nil;
}

static void
CheckMsaaCopyAndResume(void)
{
	const char *copyName = "four samples: a copy from the camera reads the resolved pixels";
	const char *resumeName = "four samples: a pass resumed after a copy keeps its samples";
	copyTarget = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	if(copyTarget == nil){
		Report(false, copyName);
		Report(false, resumeName);
		return;
	}
	Measure("copy frame", CopyFrame);
	bool ok = copied == 1;
	if(!ok)
		Detail("  renderFast returned %d\n", copied);
	uint8 *px = copyTarget->lock(0, Raster::LOCKREAD);
	ok &= px != nil;
	if(px){
		static const int e[3] = { 64, 0, 0 }, r[3] = { 255, 0, 0 }, k[3] = { 0, 0, 0 };
		int stride = copyTarget->stride;
		ok &= BytesNear(&px[100*stride + 100*3], e, 3, 1, "texel", 100, 100);
		ok &= BytesNear(&px[100*stride + 75*3], r, 3, 0, "texel", 75, 100);
		ok &= BytesNear(&px[100*stride + 125*3], k, 3, 0, "texel", 125, 100);
		copyTarget->unlock(0);
	}
	Report(ok, copyName);

	Image *img = ReadBack();
	ok = img != nil;
	if(img){
		ok &= PixelNearRGBA(img, 100, 100, RGBA{ 64, 191, 0, 255 }, 1);
		ok &= PixelNearRGBA(img, 75, 100, RED, 0);
		ok &= PixelNearRGBA(img, 125, 100, GREEN, 0);
		img->destroy();
	}
	Report(ok, resumeName);
	copyTarget->destroy();
	copyTarget = nil;
}

static bool
DepthNear(int x, int y, float want, float tol)
{
	float d = -1.0f;
	if(!metal::readDepthPixel(cam->zBuffer, x, y, &d)){
		Detail("  depth at (%d,%d) could not be read\n", x, y);
		return false;
	}
	if(!(fabsf(d - want) <= tol)){
		Detail("  depth at (%d,%d) is %.7f, expected %.7f within %g\n", x, y, d, want, tol);
		return false;
	}
	return true;
}

static bool
CheckMsaaDepth(void)
{
	const char *name = "four samples: depth reads back from the multisampled depth raster";
	RGBA prelit[4] = { BLUE, BLUE, BLUE, BLUE };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	WorldOpen(cam);
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::TEXTURED,
		-1.0f, -1.0f, 1.0f, 1.0f, 10.0f, prelit, uv, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	WorldBegin(GREY);
	atomic->render();
	WorldEnd();
	bool ok = DepthNear(320, 240, 0.909f, 1e-5f);
	ok &= DepthNear(100, 100, 1.0f, 0.0f);
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelNearRGBA(img, 320, 240, BLUE, 0);
		img->destroy();
	}
	WorldClose();
	DestroyAtomic(atomic);
	geo->destroy();
	return Report(ok, name);
}

static bool
CheckMsaaComposite(void)
{
	const char *name = "four samples: the composite shows the resolved camera";
	Raster *fb = cam->frameBuffer;
	EdgeFrame();
	uint8 *px = new uint8[fb->width*fb->height*4];
	bool ok = metal::compositeCameraPixels(fb, px);
	if(!ok)
		Detail("  compositeCameraPixels failed\n");
	else{
		static const int want[4] = { 0, 0, 64, 255 };
		ok = BytesNear(&px[(100*fb->width + 100)*4], want, 4, 1, "composite BGRA", 100, 100);
	}
	delete[] px;
	return Report(ok, name);
}

static bool
CheckMsaaCameraTextureStaysSingle(void)
{
	const char *name = "four samples: a camera texture and its depth raster stay single-sample";
	Raster *color = Raster::create(128, 128, 0, Raster::CAMERATEXTURE | Raster::C8888);
	Raster *depth = Raster::create(128, 128, 0, Raster::ZBUFFER);
	if(color == nil || depth == nil){
		Detail("  the target could not be made\n");
		if(color) color->destroy();
		if(depth) depth->destroy();
		return Report(false, name);
	}
	Camera *tcam = Camera::create();
	Frame *frame = Frame::create();
	tcam->setFrame(frame);
	tcam->frameBuffer = color;
	tcam->zBuffer = depth;
	tcam->setNearPlane(cam->nearPlane);
	tcam->setFarPlane(cam->farPlane);
	metal::resolveRasterTarget(color);

	uint32 detached = metal::getFrameStats().depthDetached;
	uint32 dropped = metal::getStateStats().droppedDraws;
	RGBA clearCol = BLUE;
	tcam->clear(&clearCol, Camera::CLEARIMAGE | Camera::CLEARZ);
	tcam->beginUpdate();
	Set2DState();
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	DrawQuad(32.0f, 32.0f, 96.0f, 96.0f, RED);
	tcam->endUpdate();
	EdgeFrame();

	bool ok = true;
	uint8 *px = color->lock(0, Raster::LOCKREAD);
	ok &= px != nil;
	if(px){
		static const int r[4] = { 255, 0, 0, 255 }, b[4] = { 0, 0, 255, 255 };
		ok &= BytesNear(&px[64*color->stride + 64*4], r, 4, 0, "texel", 64, 64);
		ok &= BytesNear(&px[8*color->stride + 8*4], b, 4, 0, "texel", 8, 8);
		color->unlock(0);
	}
	uint32 dd = metal::getFrameStats().depthDetached - detached;
	uint32 dr = metal::getStateStats().droppedDraws - dropped;
	if(dd || dr){
		Detail("  depth detached %u, dropped draws %u, expected 0 and 0\n", dd, dr);
		ok = false;
	}
	MetalRaster *zr = GETMETALRASTEREXT(depth);
	if(zr->numSamples != 1 || zr->msaaTexture != nil){
		Detail("  depth raster has %u samples and %s multisampled texture, expected 1 and none\n",
		       zr->numSamples, zr->msaaTexture ? "a" : "no");
		ok = false;
	}
	RestoreState();
	tcam->frameBuffer = nil;
	tcam->zBuffer = nil;
	tcam->setFrame(nil);
	tcam->destroy();
	frame->destroy();
	color->destroy();
	depth->destroy();
	return Report(ok, name);
}

static bool
CheckMsaaDepthAfterOneSamplePass(void)
{
	const char *name = "four samples: a depth raster used by a four-sample pass and then a one-sample pass reads the one-sample depth";
	int32 w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	Raster *color = Raster::create(w, h, 0, Raster::CAMERATEXTURE | Raster::C8888);
	Raster *depth = Raster::create(w, h, 0, Raster::ZBUFFER);
	if(color == nil || depth == nil){
		Detail("  the target could not be made\n");
		if(color) color->destroy();
		if(depth) depth->destroy();
		return Report(false, name);
	}
	Raster *mainZ = cam->zBuffer;
	cam->zBuffer = depth;
	BeginFrame(BLACK);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	DrawQuad(100.0f, 100.0f, 200.0f, 200.0f, RED);
	EndFrame();
	cam->zBuffer = mainZ;

	bool ok = GETMETALRASTEREXT(depth)->msaaTexture != nil;
	if(!ok)
		Detail("  the depth raster has no multisampled texture after a four-sample pass\n");
	float d = -1.0f;
	if(!metal::readDepthPixel(depth, 150, 150, &d) || !(d < 1.0f)){
		Detail("  depth after the four-sample pass is %.7f, expected below 1\n", d);
		ok = false;
	}

	Camera *tcam = Camera::create();
	Frame *frame = Frame::create();
	tcam->setFrame(frame);
	tcam->frameBuffer = color;
	tcam->zBuffer = depth;
	tcam->setNearPlane(cam->nearPlane);
	tcam->setFarPlane(cam->farPlane);
	metal::resolveRasterTarget(color);
	RGBA clearCol = BLUE;
	tcam->clear(&clearCol, Camera::CLEARIMAGE | Camera::CLEARZ);
	tcam->beginUpdate();
	Set2DState();
	DrawQuad(0.0f, 0.0f, 10.0f, 10.0f, GREEN);
	tcam->endUpdate();

	d = -1.0f;
	if(!metal::readDepthPixel(depth, 150, 150, &d) || !(d == 1.0f)){
		Detail("  depth after the one-sample pass is %.7f, expected 1\n", d);
		ok = false;
	}
	RestoreState();
	tcam->frameBuffer = nil;
	tcam->zBuffer = nil;
	tcam->setFrame(nil);
	tcam->destroy();
	frame->destroy();
	color->destroy();
	depth->destroy();
	return Report(ok, name);
}

static bool
CheckMsaaSubRectClear(void)
{
	const char *name = "four samples: a sub-rectangle clear in an open pass draws at the pass's sample count";
	Raster *parent = cam->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 10, 20, 16, 10 };
	RGBA red = RED;
	sub->subRaster(parent, &r);
	BeginFrame(BLACK);
	DrawQuad(200.0f, 200.0f, 210.0f, 210.0f, GREEN);
	cam->frameBuffer = sub;
	cam->clear(&red, Camera::CLEARIMAGE);
	cam->frameBuffer = parent;
	EndFrame();
	sub->destroy();
	Image *img = ReadBack();
	if(img == nil)
		return Report(false, name);
	bool ok = PixelNearRGBA(img, 10, 20, RED, 0);
	ok &= PixelNearRGBA(img, 25, 29, RED, 0);
	ok &= PixelNearRGBA(img, 26, 20, BLACK, 0);
	ok &= PixelNearRGBA(img, 10, 30, BLACK, 0);
	ok &= PixelNearRGBA(img, 205, 205, GREEN, 0);
	img->destroy();
	return Report(ok, name);
}

static bool
PrewarmLineReports(unsigned want)
{
	const char *line = metal::getPrewarmLine();
	unsigned n = 0;
	double ms = -1.0;
	char expect[128];
	bool ok = sscanf(line, "rw::metal: prewarm %u pipelines in %lf ms", &n, &ms) == 2;
	if(ok){
		snprintf(expect, sizeof(expect), "rw::metal: prewarm %u pipelines in %.1f ms\n", n, ms);
		ok = strcmp(line, expect) == 0 && ms >= 0.0;
	}
	if(!ok || n != want || metal::getStateStats().pipelinesAtInit != want){
		Detail("  got \"%s\", %u pipelines at init, expected %u\n", line, metal::getStateStats().pipelinesAtInit, want);
		return false;
	}
	return true;
}

static bool
CheckMsaaPrewarmLine(void)
{
	return Report(PrewarmLineReports(102), "four samples: the init prewarm builds its 51 rows at one and at four samples");
}

static bool
CheckOneSamplePrewarmAfterMsaa(void)
{
	return Report(PrewarmLineReports(51), "one sample after a multisampled start: the init prewarm builds 51 pipelines");
}

static bool
CheckMsaaHostPrewarm(uint32 samples)
{
	const char *name = samples > 1 ? "four samples: a host prewarm builds its pipeline at one and at four samples, once" :
		"one sample: a host prewarm builds its pipeline once";
	uint32 want = samples > 1 ? 2 : 1;
	metal::Shader *sh = CreateHostDefaultShader();
	if(sh == nil){
		Detail("  the host shader could not be built\n");
		return Report(false, name);
	}
	metal::AttribDesc attribs[metal::MAXVERTEXATTRIBS];
	int32 n = metal::defaultVertexAttribs(0, 1, 1, attribs);
	uint32 h0 = metal::getStateStats().pipelinesHost;
	bool ok = metal::prewarmShader(sh, attribs, n, 0, 0, 0, 0, 1) == 1;
	uint32 h1 = metal::getStateStats().pipelinesHost;
	ok &= metal::prewarmShader(sh, attribs, n, 0, 0, 0, 0, 1) == 1;
	uint32 h2 = metal::getStateStats().pipelinesHost;
	if(!ok || h1 - h0 != want || h2 != h1){
		Detail("  prewarm %s, host pipelines +%u then +%u, expected +%u then +0\n", ok ? "succeeded" : "failed",
		       h1 - h0, h2 - h1, want);
		ok = false;
	}
	sh->destroy();
	return Report(ok, name);
}

static bool
CheckMsaaFramesBuildNothingLate(uint32 lateBefore)
{
	uint32 late = metal::getStateStats().pipelinesLate - lateBefore;
	if(late)
		Detail("  %u pipelines built after init, expected 0\n", late);
	return Report(late == 0, "four samples: the multisampled and single-sample frames build no pipeline after init");
}

static void
DefaultStencil(void)
{
	SetRenderState(STENCILENABLE, 0);
	SetRenderState(STENCILFAIL, STENCILKEEP);
	SetRenderState(STENCILZFAIL, STENCILKEEP);
	SetRenderState(STENCILPASS, STENCILKEEP);
	SetRenderState(STENCILFUNCTION, STENCILALWAYS);
	SetRenderState(STENCILFUNCTIONREF, 0);
	SetRenderState(STENCILFUNCTIONMASK, 0xFFFFFFFF);
	SetRenderState(STENCILFUNCTIONWRITEMASK, 0xFFFFFFFF);
}

static bool
CheckStencilAcrossBreak(uint32 samples)
{
	const char *name = samples > 1 ? "four samples: stencil written before a pass break masks draws after it" :
		"one sample: stencil written before a pass break masks draws after it";
	Raster *t = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	if(t == nil){
		Detail("  the camera texture could not be made\n");
		return Report(false, name);
	}
	BeginFrame(GREY);
	SetRenderState(STENCILENABLE, 1);
	SetRenderState(STENCILFUNCTION, STENCILALWAYS);
	SetRenderState(STENCILFUNCTIONREF, 1);
	SetRenderState(STENCILFUNCTIONMASK, 0xFF);
	SetRenderState(STENCILFUNCTIONWRITEMASK, 0xFF);
	SetRenderState(STENCILFAIL, STENCILKEEP);
	SetRenderState(STENCILZFAIL, STENCILKEEP);
	SetRenderState(STENCILPASS, STENCILREPLACE);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, BLENDZERO);
	SetRenderState(DESTBLEND, BLENDONE);
	SetRenderState(ZTESTENABLE, 0);
	DrawQuad(100.0f, 100.0f, 200.0f, 200.0f, GREEN);
	bool ok = CopyCameraInto(t, 0, 0) == 1;
	if(!ok)
		Detail("  the copy failed\n");
	SetRenderState(STENCILFUNCTION, STENCILEQUAL);
	SetRenderState(STENCILPASS, STENCILKEEP);
	SetRenderState(VERTEXALPHA, 0);
	DrawQuad(0.0f, 0.0f, 640.0f, 480.0f, RED);
	EndFrame();
	DefaultStencil();
	RestoreState();
	Image *img = ReadBack();
	ok &= img != nil;
	if(img){
		ok &= PixelNearRGBA(img, 150, 150, RED, 0);
		ok &= PixelNearRGBA(img, 199, 150, RED, 0);
		ok &= PixelNearRGBA(img, 50, 50, GREY, 0);
		ok &= PixelNearRGBA(img, 200, 150, GREY, 0);
		img->destroy();
	}
	t->destroy();
	return Report(ok, name);
}

static bool
CheckUnsupportedSampleCount(bool (*restart)(uint32 samples), Camera *(*camera)(void))
{
	const char *name = "a request for three samples uses two, the largest supported count below it";
	bool ok = restart(3);
	cam = camera();
	if(!ok){
		Detail("  the engine did not restart at three samples\n");
		return Report(false, name);
	}
	uint32 want = metal::supportedSampleCount(3, Engine::getMaxMultiSamplingLevels());
	ok = want == 2 && Engine::getMultiSamplingLevels() == want && CameraSamples() == want;
	if(!ok)
		Detail("  rule %u, engine %u, camera raster %u, expected 2\n", want,
		       Engine::getMultiSamplingLevels(), CameraSamples());
	return Report(ok, name);
}

int
RunMsaaChecks(bool (*restart)(uint32 samples), Camera *(*camera)(void))
{
	failures = 0;
	if(!restart(4)){
		cam = camera();
		Report(false, "the engine restarts at four samples");
		restart(1);
		return failures;
	}
	cam = camera();
	CheckMsaaPrewarmLine();
	CheckMsaaHostPrewarm(4);
	uint32 late = metal::getStateStats().pipelinesLate;
	CheckMsaaPremise();
	CheckMsaaEdges(4);
	CheckMsaaCopyAndResume();
	CheckMsaaDepth();
	CheckMsaaComposite();
	CheckMsaaCameraTextureStaysSingle();
	CheckMsaaDepthAfterOneSamplePass();
	CheckMsaaSubRectClear();
	CheckStencilAcrossBreak(4);
	CheckMsaaFramesBuildNothingLate(late);
	RestoreState();
	CheckUnsupportedSampleCount(restart, camera);
	if(!restart(1)){
		cam = camera();
		Report(false, "the engine restarts at one sample");
		return failures;
	}
	cam = camera();
	CheckOneSamplePrewarmAfterMsaa();
	CheckMsaaHostPrewarm(1);
	CheckMsaaEdges(1);
	CheckStencilAcrossBreak(1);
	MeasureCopyFrame();
	RestoreState();
	return failures;
}
