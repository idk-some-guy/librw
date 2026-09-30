#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include "world_checks.h"
#include "target_checks.h"

using namespace rw;

static char details[8192];
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

static const RGBA RED = { 255, 0, 0, 255 };
static const RGBA GREEN = { 0, 255, 0, 255 };
static const RGBA BLUE = { 0, 0, 255, 255 };
static const RGBA YELLOW = { 255, 255, 0, 255 };
static const RGBA GREY = { 128, 128, 128, 255 };
static const RGBA WHITE = { 255, 255, 255, 255 };

static Camera *scene;

struct Target { Camera *cam; Frame *frame; Raster *color; Raster *depth; Texture *tex; };

static bool
MakeTarget(Target *t, int32 w, int32 h, int32 format, int32 depthSize, int32 projection,
           float window, float nearPlane, float farPlane)
{
	memset(t, 0, sizeof(*t));
	t->color = Raster::create(w, h, 0, Raster::CAMERATEXTURE | (format ? Raster::C8888 : 0));
	if(depthSize)
		t->depth = Raster::create(depthSize, depthSize, 0, Raster::ZBUFFER);
	if(t->color == nil || (depthSize && t->depth == nil)){
		Detail("  a %dx%d target could not be made\n", w, h);
		if(t->color) t->color->destroy();
		if(t->depth) t->depth->destroy();
		memset(t, 0, sizeof(*t));
		return false;
	}
	t->cam = Camera::create();
	t->frame = Frame::create();
	t->cam->setFrame(t->frame);
	t->cam->frameBuffer = t->color;
	t->cam->zBuffer = t->depth;
	V2d vw = { window, window };
	t->cam->setViewWindow(&vw);
	t->cam->setNearPlane(nearPlane);
	t->cam->setFarPlane(farPlane);
	t->cam->fogPlane = nearPlane;
	t->cam->setProjection(projection);
	if(TestWorld())
		TestWorld()->addCamera(t->cam);
	t->tex = Texture::create(t->color);
	metal::resolveRasterTarget(t->color);
	return true;
}

static void
DestroyTarget(Target *t)
{
	if(t->cam == nil)
		return;
	if(t->cam->world)
		t->cam->world->removeCamera(t->cam);
	t->tex->raster = nil;
	t->tex->destroy();
	t->cam->frameBuffer = nil;
	t->cam->zBuffer = nil;
	t->cam->setFrame(nil);
	t->cam->destroy();
	t->frame->destroy();
	t->color->destroy();
	if(t->depth)
		t->depth->destroy();
	memset(t, 0, sizeof(*t));
}

static void
BeginTarget(Target *t, RGBA col, uint32 clearMode)
{
	if(clearMode)
		t->cam->clear(&col, clearMode);
	t->cam->beginUpdate();
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(FOGENABLE, 0);
	SetRenderState(CULLMODE, CULLNONE);
	SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
	SetRenderState(ALPHATESTREF, 3);
	SetRenderState(GSALPHATEST, 0);
	SetRenderStatePtr(TEXTURERASTER, nil);
}

static Image*
ReadTarget(Target *t)
{
	Image *img = t->color->toImage();
	if(img == nil)
		Detail("  the target could not be read\n");
	return img;
}

static bool
TexelNear(Image *img, int x, int y, int r, int g, int b, int tol)
{
	uint8 *p = &img->pixels[y*img->stride + x*img->bpp];
	int w[3] = { r, g, b };
	for(int i = 0; i < 3; i++){
		int d = p[i] - w[i];
		if(d < -tol || d > tol){
			Detail("  texel (%d,%d) is %d,%d,%d, expected %d,%d,%d within %d\n", x, y,
			       p[0], p[1], p[2], r, g, b, tol);
			return false;
		}
	}
	return true;
}

static bool
PixelNear(Image *img, int x, int y, RGBA want, int tol)
{
	uint8 *p = &img->pixels[y*img->stride + x*4];
	int w[4] = { want.red, want.green, want.blue, want.alpha };
	bool ok = true;
	for(int i = 0; i < 4; i++)
		ok &= p[i] - w[i] >= -tol && p[i] - w[i] <= tol;
	if(ok)
		return true;
	Detail("  pixel (%d,%d) is %d,%d,%d,%d, expected %d,%d,%d,%d within %d\n", x, y,
	       p[0], p[1], p[2], p[3], want.red, want.green, want.blue, want.alpha, tol);
	return false;
}

static bool
DepthNear(Raster *zb, int x, int y, float want, float tol)
{
	float d = -1.0f;
	if(!metal::readDepthPixel(zb, x, y, &d)){
		Detail("  depth at (%d,%d) could not be read\n", x, y);
		return false;
	}
	if(!(fabsf(d - want) <= tol)){
		Detail("  depth at (%d,%d) is %.7f, expected %.7f within %g\n", x, y, d, want, tol);
		return false;
	}
	return true;
}

static float
DepthOf(float z)
{
	const float n = 1.0f, f = 101.0f;
	return 0.5f*(((f+n)*z - 2.0f*f*n)/((f-n)*z) + 1.0f);
}

static void
DrawFlatQuad(float x0, float y0, float x1, float y1, float z, RGBA c)
{
	RGBA col[4] = { c, c, c, c };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::POSITIONS, x0, y0, x1, y1, z, col, nil, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	atomic->render();
	DestroyAtomic(atomic);
	geo->destroy();
}

struct SavedDrawState { void *raster; uint32 filter, src, dst; };

static SavedDrawState
SetDrawState(Raster *ras, int32 filter, int32 src, int32 dst)
{
	SavedDrawState s;
	s.raster = GetRenderStatePtr(TEXTURERASTER);
	s.filter = GetRenderState(TEXTUREFILTER);
	s.src = GetRenderState(SRCBLEND);
	s.dst = GetRenderState(DESTBLEND);
	SetRenderStatePtr(TEXTURERASTER, ras);
	SetRenderState(TEXTUREFILTER, filter);
	SetRenderState(SRCBLEND, src);
	SetRenderState(DESTBLEND, dst);
	return s;
}

static void
RestoreDrawState(const SavedDrawState &s)
{
	SetRenderStatePtr(TEXTURERASTER, s.raster);
	SetRenderState(TEXTUREFILTER, s.filter);
	SetRenderState(SRCBLEND, s.src);
	SetRenderState(DESTBLEND, s.dst);
}

static void
DrawIm2D(Raster *ras, float x0, float y0, float x1, float y1,
         float u0, float v0, float u1, float v1, int32 filter, int32 src, int32 dst, RGBA c)
{
	Camera *cam = (Camera*)engine->currentCamera;
	float recipz = 1.0f/cam->nearPlane;
	const float xy[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
	const float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
	metal::Im2DVertex v[4];
	for(int i = 0; i < 4; i++){
		v[i].setScreenX(xy[i][0]);
		v[i].setScreenY(xy[i][1]);
		v[i].setScreenZ(im2d::GetNearZ());
		v[i].setCameraZ(cam->nearPlane);
		v[i].setRecipCameraZ(recipz);
		v[i].setColor(c.red, c.green, c.blue, c.alpha);
		v[i].setU(uv[i][0], recipz);
		v[i].setV(uv[i][1], recipz);
	}
	SavedDrawState s = SetDrawState(ras, filter, src, dst);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
	RestoreDrawState(s);
}

static void
DrawRasterIm2D(Raster *ras, float x0, float y0, float x1, float y1,
               float u0, float v0, float u1, float v1, int32 filter, int32 src, int32 dst)
{
	DrawIm2D(ras, x0, y0, x1, y1, u0, v0, u1, v1, filter, src, dst, WHITE);
}

static void
DrawRasterIm3D(Raster *ras, float x0, float y0, float x1, float y1, float z,
               int32 filter, int32 src, int32 dst, RGBA c)
{
	static uint16 indices[6] = { 0, 1, 2, 0, 2, 3 };
	const float xyuv[4][4] = {
		{ x1, y1, 0.0f, 0.0f }, { x0, y1, 1.0f, 0.0f }, { x0, y0, 1.0f, 1.0f }, { x1, y0, 0.0f, 1.0f } };
	metal::Im3DVertex v[4];
	for(int i = 0; i < 4; i++){
		v[i].setX(xyuv[i][0]); v[i].setY(xyuv[i][1]); v[i].setZ(z);
		v[i].setNormalX(0.0f); v[i].setNormalY(0.0f); v[i].setNormalZ(-1.0f);
		v[i].setColor(c.red, c.green, c.blue, c.alpha);
		v[i].setU(xyuv[i][2]); v[i].setV(xyuv[i][3]);
	}
	SavedDrawState s = SetDrawState(ras, filter, src, dst);
	im3d::Transform(v, 4, nil, im3d::VERTEXUV);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, indices, 6);
	im3d::End();
	RestoreDrawState(s);
}

static uint32
Passes(void)
{
	return metal::getFrameStats().renderPasses;
}

static uint32
Copies(void)
{
	return metal::getFrameStats().copies;
}

static bool
CheckOffscreenSceneWithOwnDepth(void)
{
	const char *name = "an offscreen scene with its own depth is drawn, then sampled by im2d and im3d in the same frame";
	Target t;
	if(!MakeTarget(&t, 64, 64, 1, 64, Camera::PERSPECTIVE, 1.0f, 1.0f, 101.0f))
		return Report(false, name);
	uint32 passes = Passes();
	BeginTarget(&t, GREY, Camera::CLEARIMAGE | Camera::CLEARZ);
	DrawFlatQuad(-1.0f, -1.0f, 1.0f, 1.0f, 2.0f, RED);
	DrawFlatQuad(1.0f, 1.0f, 1.75f, 1.75f, 2.0f, GREEN);
	DrawFlatQuad(-8.0f, -8.0f, 8.0f, 8.0f, 8.0f, BLUE);
	t.cam->endUpdate();

	WorldBegin(GREY);
	DrawRasterIm2D(t.color, 100.0f, 100.0f, 164.0f, 164.0f, 0.0f, 0.0f, 1.0f, 1.0f,
	               Texture::NEAREST, BLENDONE, BLENDZERO);
	DrawRasterIm3D(t.color, -2.0f, -1.5f, 2.0f, 1.5f, 10.0f, Texture::NEAREST, BLENDONE, BLENDZERO, WHITE);
	WorldEnd();
	uint32 passCount = Passes() - passes;

	bool ok = true;
	Image *img = ReadTarget(&t);
	ok &= img != nil;
	if(img){
		ok &= TexelNear(img, 8, 8, 0, 255, 0, 0);
		ok &= TexelNear(img, 32, 32, 255, 0, 0, 0);
		ok &= TexelNear(img, 56, 56, 0, 0, 255, 0);
		ok &= TexelNear(img, 56, 8, 0, 0, 255, 0);
		img->destroy();
	}
	ok &= DepthNear(t.depth, 32, 32, DepthOf(2.0f), 1e-5f);
	ok &= DepthNear(t.depth, 56, 56, DepthOf(8.0f), 1e-5f);
	img = ReadCamera();
	ok &= img != nil;
	if(img){
		ok &= PixelNear(img, 108, 108, GREEN, 0);
		ok &= PixelNear(img, 132, 132, RED, 0);
		ok &= PixelNear(img, 156, 156, BLUE, 0);
		ok &= PixelNear(img, 266, 202, GREEN, 0);
		ok &= PixelNear(img, 320, 240, RED, 0);
		ok &= PixelNear(img, 380, 284, BLUE, 0);
		img->destroy();
	}
	if(passCount != 2){
		Detail("  %u render passes, expected 2\n", passCount);
		ok = false;
	}
	DestroyTarget(&t);
	return Report(ok, name);
}

static bool
CheckMidFrameSwitchResumesScene(void)
{
	const char *name = "ending the scene for a masked offscreen pass and resuming it keeps its colour and depth";
	static const RGBA maskTexel = { 128, 128, 128, 255 };
	Target t;
	if(!MakeTarget(&t, 128, 128, 1, 128, Camera::PERSPECTIVE, 1.0f, 1.0f, 101.0f))
		return Report(false, name);
	Texture *mask = MakeTexture(1, 1, &maskTexel, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	if(mask == nil){
		DestroyTarget(&t);
		return Report(false, name);
	}
	uint32 passes = Passes();
	uint32 copies = Copies();
	uint32 late = metal::getStateStats().pipelinesLate;

	WorldBegin(GREY);
	DrawFlatQuad(-1.0f, -1.0f, 1.0f, 1.0f, 10.0f, RED);
	scene->endUpdate();

	BeginTarget(&t, BLUE, Camera::CLEARIMAGE | Camera::CLEARZ);
	DrawFlatQuad(-1.0f, -1.0f, 1.0f, 1.0f, 2.0f, YELLOW);
	SetRenderState(VERTEXALPHA, 1);
	DrawRasterIm2D(mask->raster, 0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 0.0f, 1.0f, 1.0f,
	               Texture::NEAREST, BLENDZERO, BLENDSRCCOLOR);
	SetRenderState(VERTEXALPHA, 0);
	t.cam->endUpdate();

	scene->beginUpdate();
	DrawFlatQuad(-4.0f, -3.0f, 4.0f, 3.0f, 20.0f, BLUE);
	SetRenderState(ZTESTENABLE, 0);
	DrawRasterIm2D(t.color, 0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 0.0f, 1.0f, 1.0f,
	               Texture::NEAREST, BLENDONE, BLENDZERO);
	DrawIm2D(nil, 600.0f, 440.0f, 640.0f, 480.0f, 0.0f, 0.0f, 1.0f, 1.0f,
	         Texture::NEAREST, BLENDSRCALPHA, BLENDINVSRCALPHA, GREEN);
	SetRenderState(ZTESTENABLE, 1);
	WorldEnd();
	uint32 passCount = Passes() - passes;
	uint32 copyCount = Copies() - copies;
	uint32 lateCount = metal::getStateStats().pipelinesLate - late;

	bool ok = true;
	Image *img = ReadCamera();
	ok &= img != nil;
	if(img){
		ok &= PixelNear(img, 320, 240, RED, 0);
		ok &= PixelNear(img, 270, 240, BLUE, 0);
		ok &= PixelNear(img, 500, 380, GREY, 0);
		ok &= PixelNear(img, 64, 64, RGBA{ 128, 128, 0, 255 }, 1);
		ok &= PixelNear(img, 10, 10, RGBA{ 0, 0, 128, 255 }, 1);
		ok &= PixelNear(img, 620, 460, GREEN, 0);
		img->destroy();
	}
	if(passCount != 3 || copyCount != 0){
		Detail("  %u render passes and %u copies, expected 3 and 0\n", passCount, copyCount);
		ok = false;
	}
	if(lateCount > 1){
		Detail("  %u pipelines built late, expected at most 1\n", lateCount);
		ok = false;
	}
	mask->destroy();
	DestroyTarget(&t);
	return Report(ok, name);
}

static bool
CheckCopyThenSampleSameFrame(void)
{
	const char *name = "a renderFast copy of the scene is sampled by im2d in the same frame";
	Raster *copy = Raster::create(1024, 512, 0, Raster::CAMERATEXTURE);
	if(copy == nil)
		return Report(false, name);
	metal::resolveRasterTarget(copy);
	uint32 passes = Passes();
	uint32 copies = Copies();

	WorldBegin(GREY);
	DrawIm2D(nil, 40.0f, 40.0f, 80.0f, 80.0f, 0.0f, 0.0f, 1.0f, 1.0f,
	         Texture::NEAREST, BLENDSRCALPHA, BLENDINVSRCALPHA, RED);
	Raster::pushContext(copy);
	bool32 r = scene->frameBuffer->renderFast(0, 0);
	Raster::popContext();
	DrawRasterIm2D(copy, 320.0f, 0.0f, 640.0f, 480.0f, 0.0f, 0.0f, 320.0f/1024.0f, 480.0f/512.0f,
	               Texture::NEAREST, BLENDONE, BLENDZERO);
	WorldEnd();
	uint32 passCount = Passes() - passes;
	uint32 copyCount = Copies() - copies;

	bool ok = r == 1;
	if(!ok)
		Detail("  renderFast returned %d\n", r);
	Image *img = ReadCamera();
	ok &= img != nil;
	if(img){
		ok &= PixelNear(img, 380, 60, RED, 0);
		ok &= PixelNear(img, 500, 300, GREY, 0);
		img->destroy();
	}
	if(passCount != 2 || copyCount != 1){
		Detail("  %u render passes and %u copies, expected 2 and 1\n", passCount, copyCount);
		ok = false;
	}
	copy->destroy();
	return Report(ok, name);
}

struct ShadowRig { Target main; Target resample; };

static bool
MakeShadowRig(ShadowRig *rig)
{
	memset(rig, 0, sizeof(*rig));
	if(!MakeTarget(&rig->main, 64, 64, 0, 64, Camera::PARALLEL, 2.0f, 0.1f, 20.0f))
		return false;
	if(!MakeTarget(&rig->resample, 32, 32, 0, 32, Camera::PARALLEL, 2.0f, 0.1f, 20.0f)){
		DestroyTarget(&rig->main);
		return false;
	}
	Target *t[2] = { &rig->main, &rig->resample };
	for(Target *target : t){
		target->tex->setFilter(Texture::LINEAR);
		target->tex->setAddressU(Texture::CLAMP);
		target->tex->setAddressV(Texture::CLAMP);
	}
	return true;
}

static void
DestroyShadowRig(ShadowRig *rig)
{
	DestroyTarget(&rig->resample);
	DestroyTarget(&rig->main);
}

static const uint8 casterBoneIndices[4][4] = { { 0, 3, 1, 2 }, { 1, 2, 1, 2 }, { 2, 1, 1, 2 }, { 3, 0, 1, 2 } };
static const float casterBoneWeights[4][4] = {
	{ 0.5f, 0.25f, 0.125f, 0.125f }, { 0.5f, 0.25f, 0.125f, 0.125f },
	{ 0.5f, 0.25f, 0.125f, 0.125f }, { 0.5f, 0.25f, 0.125f, 0.125f } };

static Geometry*
ShadowCasterQuad(void)
{
	static const RGBA col[4] = { RED, GREEN, BLUE, WHITE };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED | Geometry::POSITIONS,
	                             -1.0f, -0.5f, 1.5f, 0.75f, 10.0f, col, uv, mat);
	mat->destroy();
	AttachSkin(geo, 4, nil, casterBoneIndices, casterBoneWeights);
	return geo;
}

static void
Im2DVertexAt(metal::Im2DVertex *v, float x, float y, float recipZ)
{
	v->setScreenX(x);
	v->setScreenY(y);
	v->setScreenZ(im2d::GetNearZ());
	v->setRecipCameraZ(recipZ);
	v->setColor(255, 255, 255, 255);
}

static void
Im2DRenderQuad(float x1, float y1, float x2, float y2, float recipCamZ, float uvOffset)
{
	const float xyuv[4][4] = {
		{ x1, y1, uvOffset, uvOffset }, { x1, y2, uvOffset, 1.0f + uvOffset },
		{ x2, y1, 1.0f + uvOffset, uvOffset }, { x2, y2, 1.0f + uvOffset, 1.0f + uvOffset } };
	metal::Im2DVertex v[4];
	for(int i = 0; i < 4; i++){
		Im2DVertexAt(&v[i], xyuv[i][0], xyuv[i][1], recipCamZ);
		v[i].setU(xyuv[i][2], recipCamZ);
		v[i].setV(xyuv[i][3], recipCamZ);
	}
	im2d::RenderPrimitive(PRIMTYPETRISTRIP, v, 4);
}

static void
InvertRaster(Target *t)
{
	float crw = (float)t->color->width, crh = (float)t->color->height;
	float recipZ = 1.0f/t->cam->nearPlane;
	metal::Im2DVertex v[4];
	Im2DVertexAt(&v[0], 0.0f, 0.0f, recipZ);
	Im2DVertexAt(&v[1], 0.0f, crh, recipZ);
	Im2DVertexAt(&v[2], crw, 0.0f, recipZ);
	Im2DVertexAt(&v[3], crw, crh, recipZ);
	SetRenderState(ZTESTENABLE, 0);
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, BLENDINVDESTCOLOR);
	SetRenderState(DESTBLEND, BLENDZERO);
	im2d::RenderPrimitive(PRIMTYPETRISTRIP, v, 4);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
}

static void
ShadowUpdate(ShadowRig *rig, Atomic *atomic)
{
	RGBA bgColor = { 255, 255, 255, 0 };
	rig->main.cam->clear(&bgColor, Camera::CLEARZ | Camera::CLEARIMAGE);
	rig->main.cam->beginUpdate();
	Geometry *geo = atomic->geometry;
	uint32 flags = geo->flags;
	geo->flags = flags & ~(Geometry::PRELIT | Geometry::LIGHT | Geometry::TEXTURED | Geometry::TEXTURED2 |
	                       Geometry::MODULATE);
	atomic->render();
	geo->flags = flags;
	InvertRaster(&rig->main);
	rig->main.cam->endUpdate();
}

static void
ShadowResample(ShadowRig *rig)
{
	float size = (float)rig->resample.color->width;
	float uvOffset = 0.5f/size;
	float recipCamZ = 1.0f/rig->resample.cam->nearPlane;
	rig->resample.cam->beginUpdate();
	SetRenderState(SRCBLEND, BLENDONE);
	SetRenderState(DESTBLEND, BLENDZERO);
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(TEXTUREFILTER, Texture::LINEAR);
	SetRenderStatePtr(TEXTURERASTER, rig->main.color);
	Im2DRenderQuad(0.0f, 0.0f, size, size, recipCamZ, uvOffset);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	rig->resample.cam->endUpdate();
}

static void
ShadowBorder(Target *t)
{
	const RGBA color = { 0, 0, 0, 0 };
	float size = (float)t->color->width - 1.0f;
	float recipCamZ = 1.0f/t->cam->nearPlane;
	const float xy[4][2] = { { 0.0f, 0.0f }, { size, 0.0f }, { size, size }, { 0.0f, size } };
	static uint16 ix[5] = { 0, 1, 2, 3, 0 };
	metal::Im2DVertex v[4];
	for(int i = 0; i < 4; i++){
		Im2DVertexAt(&v[i], xy[i][0], xy[i][1], recipCamZ);
		v[i].setColor(color.red, color.green, color.blue, color.alpha);
	}
	t->cam->beginUpdate();
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderStatePtr(TEXTURERASTER, nil);
	im2d::RenderIndexedPrimitive(PRIMTYPEPOLYLINE, v, 4, ix, 5);
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(VERTEXALPHA, 1);
	t->cam->endUpdate();
}

static void
ShadowBlur(Target *blur, Raster *dst, int32 passes)
{
	Raster *raster = blur->color;
	float size = (float)dst->width;
	float recipCamZ = 1.0f/blur->cam->nearPlane;
	for(int32 i = 0; i < passes; i++){
		blur->cam->frameBuffer = raster;
		blur->cam->beginUpdate();
		if(i == 0){
			SetRenderState(SRCBLEND, BLENDONE);
			SetRenderState(DESTBLEND, BLENDZERO);
			SetRenderState(ZTESTENABLE, 0);
			SetRenderState(TEXTUREFILTER, Texture::LINEAR);
		}
		SetRenderStatePtr(TEXTURERASTER, dst);
		Im2DRenderQuad(0.0f, 0.0f, size, size, recipCamZ, 1.0f/size);
		blur->cam->endUpdate();

		blur->cam->frameBuffer = dst;
		blur->cam->beginUpdate();
		SetRenderStatePtr(TEXTURERASTER, raster);
		Im2DRenderQuad(0.0f, 0.0f, size, size, recipCamZ, 0.0f);
		if(i == passes - 1){
			SetRenderState(ZTESTENABLE, 1);
			SetRenderState(SRCBLEND, BLENDSRCALPHA);
			SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
		}
		blur->cam->endUpdate();
	}
	blur->cam->frameBuffer = raster;
}

static void
ExpectedShadow(uint8 out[32*32])
{
	uint8 src[64*64];
	for(int y = 0; y < 64; y++)
		for(int x = 0; x < 64; x++)
			src[y*64 + x] = x >= 8 && x < 48 && y >= 20 && y < 40 ? 255 : 0;
	for(int j = 0; j < 32; j++)
		for(int i = 0; i < 32; i++){
			int sum = 0;
			for(int b = 1; b <= 2; b++)
				for(int a = 1; a <= 2; a++)
					sum += src[std::min(2*j + b, 63)*64 + std::min(2*i + a, 63)];
			out[j*32 + i] = (uint8)((sum + 2)/4);
		}
}

struct SavedSampling { uint32 filter, addressU, addressV; };

static SavedSampling
ClampSampling(void)
{
	SavedSampling s;
	SetRenderStatePtr(TEXTURERASTER, nil);
	s.filter = GetRenderState(TEXTUREFILTER);
	s.addressU = GetRenderState(TEXTUREADDRESSU);
	s.addressV = GetRenderState(TEXTUREADDRESSV);
	SetRenderState(TEXTUREADDRESS, Texture::CLAMP);
	return s;
}

static void
RestoreSampling(const SavedSampling &s)
{
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(TEXTUREFILTER, s.filter);
	SetRenderState(TEXTUREADDRESSU, s.addressU);
	SetRenderState(TEXTUREADDRESSV, s.addressV);
}

static void
DetailBorderRing(Raster *ras)
{
	uint8 px[32*32*4];
	if(!metal::readRasterPixels(ras, px)){
		Detail("  the border ring could not be read\n");
		return;
	}
	int rows[32] = {}, cols[32] = {}, total = 0;
	for(int y = 0; y < 32; y++)
		for(int x = 0; x < 32; x++)
			if(px[(y*32 + x)*4 + 3] == 0){
				rows[y]++;
				cols[x]++;
				total++;
			}
	Detail("  the border polyline wrote %d texels; rows", total);
	for(int i = 0; i < 32; i++)
		if(rows[i] >= 16)
			Detail(" %d (%d)", i, rows[i]);
	Detail(", columns");
	for(int i = 0; i < 32; i++)
		if(cols[i] >= 16)
			Detail(" %d (%d)", i, cols[i]);
	Detail("\n");
}

static bool
CheckShadowCameraChain(void)
{
	const char *name = "a skinned caster's shadow is rendered, inverted, resampled, bordered and darkens the scene";
	ShadowRig rig;
	if(!MakeShadowRig(&rig))
		return Report(false, name);
	Geometry *geo = ShadowCasterQuad();
	Atomic *atomic = SkinAtomic(geo);
	uint32 passes = Passes();
	uint32 late = metal::getStateStats().pipelinesLate;
	SavedSampling sampling = ClampSampling();
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(FOGENABLE, 0);
	SetRenderState(CULLMODE, CULLNONE);
	SetRenderState(GSALPHATEST, 1);

	ShadowUpdate(&rig, atomic);
	ShadowResample(&rig);
	ShadowBorder(&rig.resample);

	bool ok = true;
	uint8 want[32*32];
	ExpectedShadow(want);
	Image *img = ReadTarget(&rig.resample);
	ok &= img != nil;
	if(img){
		int bad = 0;
		for(int i = 0; i < 32*32; i++)
			if(!TexelNear(img, i%32, i/32, want[i], want[i], want[i], 1) && ++bad >= 8)
				break;
		ok &= bad == 0;
		img->destroy();
	}
	DetailBorderRing(rig.resample.color);

	WorldBegin(GREY);
	SetRenderState(GSALPHATEST, 1);
	SetRenderState(VERTEXALPHA, 1);
	DrawRasterIm3D(rig.resample.color, -2.0f, -2.0f, 2.0f, 2.0f, 10.0f, Texture::NEAREST, BLENDZERO, BLENDINVSRCCOLOR, WHITE);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(GSALPHATEST, 0);
	WorldEnd();
	uint32 passCount = Passes() - passes;
	uint32 lateCount = metal::getStateStats().pipelinesLate - late;
	RestoreSampling(sampling);

	img = ReadCamera();
	ok &= img != nil;
	if(img){
		const RGBA core = { 0, 0, 0, 0 }, half = { 64, 64, 64, 0 }, corner = { 96, 96, 96, 0 };
		const RGBA background = { 128, 128, 128, 0 };
		ok &= PixelNear(img, 258 + 4*10, 178 + 4*14, core, 0);
		ok &= PixelNear(img, 258 + 4*4, 178 + 4*10, core, 0);
		ok &= PixelNear(img, 258 + 4*22, 178 + 4*18, core, 0);
		ok &= PixelNear(img, 258 + 4*3, 178 + 4*14, half, 1);
		ok &= PixelNear(img, 258 + 4*23, 178 + 4*14, half, 1);
		ok &= PixelNear(img, 258 + 4*10, 178 + 4*9, half, 1);
		ok &= PixelNear(img, 258 + 4*10, 178 + 4*19, half, 1);
		ok &= PixelNear(img, 258 + 4*3, 178 + 4*9, corner, 1);
		ok &= PixelNear(img, 258 + 4*23, 178 + 4*19, corner, 1);
		ok &= PixelNear(img, 258 + 4*1, 178 + 4*1, background, 0);
		ok &= PixelNear(img, 258 + 4*26, 178 + 4*26, background, 0);
		ok &= PixelNear(img, 100, 100, GREY, 0);
		img->destroy();
	}
	if(passCount != 3){
		Detail("  %u render passes, expected 3\n", passCount);
		ok = false;
	}
	if(lateCount != 0){
		Detail("  %u pipelines built late, expected 0\n", lateCount);
		ok = false;
	}
	DestroySkinAtomic(atomic);
	geo->destroy();
	DestroyShadowRig(&rig);
	return Report(ok, name);
}

static bool
CheckBlurPingPong(void)
{
	const char *name = "a blur ping-pongs between its target and the source raster, offsetting by one texel per pass";
	Target blur;
	if(!MakeTarget(&blur, 32, 32, 0, 32, Camera::PARALLEL, 2.0f, 0.1f, 20.0f))
		return Report(false, name);
	Raster *dst = Raster::create(32, 32, 0, Raster::CAMERATEXTURE);
	uint8 *px = dst ? dst->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH) : nil;
	if(px == nil){
		Detail("  the source raster could not be made or locked\n");
		if(dst)
			dst->destroy();
		DestroyTarget(&blur);
		return Report(false, name);
	}
	int32 bpp = dst->stride/32;
	for(int y = 0; y < 32; y++)
		for(int x = 0; x < 32; x++){
			uint8 *p = &px[y*dst->stride + x*bpp];
			p[0] = 8*x;
			p[1] = 8*y;
			p[2] = 0;
			if(bpp == 4)
				p[3] = 255;
		}
	dst->unlock(0);
	uint32 passes = Passes();
	uint32 detached = metal::getFrameStats().depthDetached;
	SavedSampling sampling = ClampSampling();
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(GSALPHATEST, 0);
	ShadowBlur(&blur, dst, 2);
	RestoreSampling(sampling);
	uint32 passCount = Passes() - passes;
	detached = metal::getFrameStats().depthDetached - detached;

	bool ok = true;
	Image *img = dst->toImage();
	ok &= img != nil;
	if(img){
		int bad = 0;
		for(int i = 0; i < 32*32; i++){
			int x = i%32, y = i/32;
			if(!TexelNear(img, x, y, 8*std::min(x + 2, 31), 8*std::min(y + 2, 31), 0, 1) && ++bad >= 8)
				break;
		}
		ok &= bad == 0;
		img->destroy();
	}
	if(passCount != 4 || detached != 0){
		Detail("  %u render passes and %u detached depths, expected 4 and 0\n", passCount, detached);
		ok = false;
	}
	dst->destroy();
	DestroyTarget(&blur);
	return Report(ok, name);
}

static bool
CheckSubRasterCameraTextureTarget(void)
{
	const char *name = "a sub raster of a camera texture is a 3D target with depth, offset and clipped to it";
	Target t;
	if(!MakeTarget(&t, 64, 64, 1, 64, Camera::PERSPECTIVE, 1.0f, 1.0f, 101.0f))
		return Report(false, name);
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERATEXTURE | Raster::DONTALLOCATE);
	Rect r = { 16, 8, 32, 32 };
	sub->subRaster(t.color, &r);
	RGBA outside = BLUE;
	uint32 detached = metal::getFrameStats().depthDetached;
	t.cam->clear(&outside, Camera::CLEARIMAGE | Camera::CLEARZ);
	t.cam->frameBuffer = sub;
	BeginTarget(&t, GREEN, Camera::CLEARIMAGE | Camera::CLEARZ);
	DrawFlatQuad(-1.0f, -1.0f, 1.0f, 1.0f, 2.0f, RED);
	t.cam->endUpdate();
	t.cam->frameBuffer = t.color;
	sub->destroy();
	detached = metal::getFrameStats().depthDetached - detached;

	bool ok = true;
	if(detached != 0){
		Detail("  %u detached depths, expected 0\n", detached);
		ok = false;
	}
	Image *img = ReadTarget(&t);
	ok &= img != nil;
	if(img){
		ok &= PixelNear(img, 4, 4, BLUE, 0);
		ok &= PixelNear(img, 20, 10, GREEN, 0);
		ok &= PixelNear(img, 30, 20, RED, 0);
		uint8 *p = &img->pixels[24*img->stride + 8*4];
		if(p[0] == 255 && p[1] == 0 && p[2] == 0){
			Detail("  pixel (8,24) is RED outside the sub raster\n");
			ok = false;
		}
		img->destroy();
	}
	DestroyTarget(&t);
	return Report(ok, name);
}

static bool
CheckDepthOfAnotherSizeDetached(void)
{
	const char *name = "a depth raster of another size is detached, counted and reported, and the draw still lands";
	Target t;
	if(!MakeTarget(&t, 64, 64, 1, 32, Camera::PERSPECTIVE, 1.0f, 1.0f, 101.0f))
		return Report(false, name);
	Error err;
	getError(&err);
	uint32 detached = metal::getFrameStats().depthDetached;
	BeginTarget(&t, GREY, Camera::CLEARIMAGE | Camera::CLEARZ);
	DrawFlatQuad(-1.0f, -1.0f, 1.0f, 1.0f, 2.0f, RED);
	t.cam->endUpdate();
	getError(&err);
	detached = metal::getFrameStats().depthDetached - detached;

	bool ok = true;
	Image *img = ReadTarget(&t);
	ok &= img != nil;
	if(img){
		ok &= PixelNear(img, 32, 32, RED, 0);
		img->destroy();
	}
	if(detached != 1){
		Detail("  %u detached depths, expected 1\n", detached);
		ok = false;
	}
	if(err.code != ERR_GENERAL){
		Detail("  error code %u, expected ERR_GENERAL\n", err.code);
		ok = false;
	}
	DestroyTarget(&t);
	return Report(ok, name);
}

static bool
CheckIm3DStaleStageOneSceneNotFeedback(void)
{
	const char *name = "a scene colour raster left on stage 1 under the env shader does not drop an im3d draw into it";
	Texture *t = Texture::create(scene->frameBuffer);
	if(t == nil || metal::matfxEnvShader == nil){
		if(t){
			t->raster = nil;
			t->destroy();
		}
		return Report(false, name);
	}
	metal::setTexture(1, t);
	metal::StateStats before = metal::getStateStats();
	WorldBegin(GREY);
	metal::matfxEnvShader->use();
	DrawRasterIm3D(nil, -2.0f, -2.0f, 2.0f, 2.0f, 10.0f, Texture::NEAREST, BLENDSRCALPHA, BLENDINVSRCALPHA, RED);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();
	metal::setTexture(1, nil);
	t->raster = nil;
	t->destroy();

	bool ok = true;
	if(after.feedbackDraws != before.feedbackDraws || after.droppedDraws != before.droppedDraws){
		Detail("  %u feedback and %u dropped draws counted, expected none\n",
		       after.feedbackDraws - before.feedbackDraws, after.droppedDraws - before.droppedDraws);
		ok = false;
	}
	Image *img = ReadCamera();
	ok &= img != nil;
	if(img){
		ok &= PixelNear(img, 320, 240, RED, 0);
		ok &= PixelNear(img, 100, 100, GREY, 0);
		img->destroy();
	}
	return Report(ok, name);
}

static Light*
AddLight(int32 type, float c)
{
	Light *l = Light::create(type);
	l->setFrame(Frame::create());
	l->setColor(c, c, c);
	TestWorld()->addLight(l);
	return l;
}

static void
RemoveLight(Light *l)
{
	Frame *f = l->getFrame();
	TestWorld()->removeLight(l);
	l->destroy();
	f->destroy();
}

static bool
CheckEnvMapRenderStatesPrewarmed(void)
{
	const char *name = "the env map pass draws a lit world atomic and its ZERO/SRCCOLOR mask without a pipeline built after init";
	static const RGBA maskTexel = { 128, 128, 128, 255 };
	static const RGBA white[4] = { WHITE, WHITE, WHITE, WHITE };
	static const TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	const RGBA sky = { 64, 128, 200, 255 };
	Target t;
	if(!MakeTarget(&t, 128, 128, 0, 128, Camera::PERSPECTIVE, 2.0f, 0.1f, 250.0f))
		return Report(false, name);
	Texture *mask = MakeTexture(1, 1, &maskTexel, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	Texture *tex = MakeTexture(1, 1, &WHITE, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
	if(mask == nil || tex == nil){
		if(mask) mask->destroy();
		if(tex) tex->destroy();
		DestroyTarget(&t);
		return Report(false, name);
	}
	t.frame->matrix.right.x = -1.0f;
	t.frame->matrix.up.y = -1.0f;
	t.frame->matrix.update();
	Material *mat = Material::create();
	mat->setTexture(tex);
	Geometry *geo = QuadGeometry(Geometry::PRELIT | Geometry::TEXTURED | Geometry::LIGHT | Geometry::POSITIONS,
	                             -1.0f, -1.0f, 1.0f, 1.0f, 10.0f, white, uv, mat);
	mat->destroy();
	Atomic *atomic = MakeAtomic(geo);
	Light *amb = AddLight(Light::AMBIENT, 0.3f);
	Light *dir = AddLight(Light::DIRECTIONAL, 0.5f);
	metal::StateStats before = metal::getStateStats();

	WorldBegin(GREY);
	DrawFlatQuad(-1.0f, -1.0f, 1.0f, 1.0f, 10.0f, RED);
	scene->endUpdate();
	BeginTarget(&t, sky, Camera::CLEARIMAGE | Camera::CLEARZ);
	atomic->render();
	SetRenderState(VERTEXALPHA, 1);
	DrawRasterIm2D(mask->raster, 0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 0.0f, 1.0f, 1.0f,
	               Texture::NEAREST, BLENDZERO, BLENDSRCCOLOR);
	SetRenderState(VERTEXALPHA, 0);
	t.cam->endUpdate();
	scene->beginUpdate();
	DrawFlatQuad(2.0f, -1.0f, 3.0f, 1.0f, 10.0f, GREEN);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();

	bool ok = true;
	if(after.pipelinesLate != before.pipelinesLate || after.droppedDraws != before.droppedDraws){
		Detail("  %u pipelines built late and %u draws dropped, expected none\n",
		       after.pipelinesLate - before.pipelinesLate, after.droppedDraws - before.droppedDraws);
		ok = false;
	}
	Image *img = ReadTarget(&t);
	ok &= img != nil;
	if(img){
		ok &= TexelNear(img, 4, 4, 32, 64, 100, 1);
		img->destroy();
	}
	RemoveLight(dir);
	RemoveLight(amb);
	DestroyAtomic(atomic);
	geo->destroy();
	tex->destroy();
	mask->destroy();
	DestroyTarget(&t);
	return Report(ok, name);
}

static bool
CheckShadowFrameBudget(void)
{
	const char *name = "eight skinned caster shadows and the scene take 17 passes, no copy and at most 12 KB of ring per skinned caster, with no pipeline built late";
	const int N = 8;
	ShadowRig rigs[N];
	Geometry *geos[N];
	Atomic *atomics[N];
	int made = 0;
	for(; made < N; made++){
		if(!MakeShadowRig(&rigs[made]))
			break;
		geos[made] = ShadowCasterQuad();
		atomics[made] = SkinAtomic(geos[made]);
		V3d pos = { 3.0f*made, 0.0f, 0.0f };
		atomics[made]->getFrame()->translate(&pos);
		rigs[made].main.frame->translate(&pos);
	}
	bool ok = made == N;
	uint32 late = metal::getStateStats().pipelinesLate;
	uint32 passCount = 0, copyCount = 0, bytes = 0;
	SavedSampling sampling = ClampSampling();
	for(int frame = 0; ok && frame < 2; frame++){
		metal::RingSpace probe;
		if(frame == 1)
			ok &= metal::ringAlloc(1, 256, &probe) != 0;
		uint32 passes = Passes(), copies = Copies();
		uint32 ring = metal::getStateStats().frameRingBytes;
		SetRenderState(ZTESTENABLE, 1);
		SetRenderState(ZWRITEENABLE, 1);
		SetRenderState(VERTEXALPHA, 0);
		SetRenderState(FOGENABLE, 0);
		SetRenderState(CULLMODE, CULLNONE);
		SetRenderState(GSALPHATEST, 1);
		for(int k = 0; k < N; k++){
			ShadowUpdate(&rigs[k], atomics[k]);
			ShadowResample(&rigs[k]);
			ShadowBorder(&rigs[k].resample);
		}
		bytes = metal::getStateStats().frameRingBytes - ring;
		WorldBegin(GREY);
		SetRenderState(GSALPHATEST, 1);
		SetRenderState(VERTEXALPHA, 1);
		for(int k = 0; k < N; k++){
			float x = 2.0f*k - 8.0f;
			DrawRasterIm3D(rigs[k].resample.color, x, -1.0f, x + 1.5f, 1.0f, 10.0f, Texture::NEAREST,
			               BLENDZERO, BLENDINVSRCCOLOR, WHITE);
		}
		SetRenderState(VERTEXALPHA, 0);
		SetRenderState(GSALPHATEST, 0);
		WorldEnd();
		passCount = Passes() - passes;
		copyCount = Copies() - copies;
	}
	RestoreSampling(sampling);
	late = metal::getStateStats().pipelinesLate - late;

	if(passCount != 2*N + 1 || copyCount != 0){
		Detail("  %u render passes and %u copies, expected %d and 0\n", passCount, copyCount, 2*N + 1);
		ok = false;
	}
	if(bytes > (uint32)N*12288){
		Detail("  %u shadow ring bytes, expected at most %d\n", bytes, N*12288);
		ok = false;
	}
	if(late != 0){
		Detail("  %u pipelines built late, expected 0\n", late);
		ok = false;
	}
	for(int k = 0; k < made; k++){
		DestroySkinAtomic(atomics[k]);
		geos[k]->destroy();
		DestroyShadowRig(&rigs[k]);
	}
	return Report(ok, name);
}

int
RunTargetChecks(Camera *camera)
{
	failures = 0;
	scene = camera;
	WorldOpen(camera);
	CheckEnvMapRenderStatesPrewarmed();
	CheckOffscreenSceneWithOwnDepth();
	CheckMidFrameSwitchResumesScene();
	CheckCopyThenSampleSameFrame();
	CheckShadowCameraChain();
	CheckShadowFrameBudget();
	CheckBlurPingPong();
	CheckSubRasterCameraTextureTarget();
	CheckDepthOfAnotherSizeDetached();
	CheckIm3DStaleStageOneSceneNotFeedback();
	WorldClose();
	scene = nil;
	return failures;
}

static bool
DrawAndSampleTarget(Camera *main, const char *when)
{
	Target t;
	if(!MakeTarget(&t, 128, 128, 0, 128, Camera::PERSPECTIVE, 1.0f, 1.0f, 101.0f)){
		Detail("  %s the restart\n", when);
		return false;
	}
	BeginTarget(&t, BLUE, Camera::CLEARIMAGE | Camera::CLEARZ);
	SetRenderState(ZTESTENABLE, 0);
	DrawIm2D(nil, 32.0f, 32.0f, 96.0f, 96.0f, 0.0f, 0.0f, 1.0f, 1.0f, Texture::NEAREST, BLENDONE, BLENDZERO, RED);
	t.cam->endUpdate();

	RGBA grey = GREY;
	main->clear(&grey, Camera::CLEARIMAGE | Camera::CLEARZ);
	main->beginUpdate();
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(VERTEXALPHA, 0);
	DrawRasterIm2D(t.color, 0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 0.0f, 1.0f, 1.0f,
	               Texture::NEAREST, BLENDONE, BLENDZERO);
	SetRenderState(ZTESTENABLE, 1);
	main->endUpdate();
	main->showRaster(0);

	bool ok = true;
	Image *img = main->frameBuffer->toImage();
	if(img == nil || img->bpp != 4){
		Detail("  %s the restart the main camera could not be read\n", when);
		ok = false;
	}else{
		bool px = PixelNear(img, 64, 64, RED, 0);
		px &= PixelNear(img, 10, 10, BLUE, 0);
		if(!px)
			Detail("  in the frame %s the restart\n", when);
		ok &= px;
	}
	if(img)
		img->destroy();
	BeginTarget(&t, BLUE, 0);
	DrawIm2D(nil, 0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 0.0f, 1.0f, 1.0f, Texture::NEAREST, BLENDONE, BLENDZERO, RED);
	t.cam->endUpdate();
	bool open = metal::passIsOpen();
	DestroyTarget(&t);
	if(!open || metal::passIsOpen()){
		Detail("  %s the restart the pass into the target was %s before and %s after destroying it, expected open and closed\n",
		       when, open ? "open" : "closed", metal::passIsOpen() ? "open" : "closed");
		ok = false;
	}
	return ok;
}

static bool
CheckTargetAcrossRestart(bool (*restart)(void), Camera *(*camera)(void))
{
	const char *name = "a camera texture target drawn and sampled before a restart is drawn and sampled again after it";
	bool ok = DrawAndSampleTarget(camera(), "before");
	if(!ok)
		return Report(false, name);
	if(!restart()){
		Detail("  the restart failed\n");
		return Report(false, name);
	}
	uint32 late = metal::getStateStats().pipelinesLate;
	ok &= DrawAndSampleTarget(camera(), "after");
	late = metal::getStateStats().pipelinesLate - late;
	if(late != 0){
		Detail("  %u pipelines built late after the restart, expected 0\n", late);
		ok = false;
	}
	return Report(ok, name);
}

int
RunRestartTargetChecks(bool (*restart)(void), Camera *(*camera)(void))
{
	failures = 0;
	CheckTargetAcrossRestart(restart, camera);
	return failures;
}
