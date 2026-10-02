#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include <src/metal/metalprepass.h>
#include "world_checks.h"
#include "modern_checks.h"

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

static const RGBA SKY = { 60, 90, 120, 255 };

static void
SetSettings(int32 tier, int32 upscale, int32 aa)
{
	metal::RenderSettings s = metal::getRenderSettings();
	s.tier = tier;
	s.upscale = upscale;
	s.antiAliasing = aa;
	metal::setRenderSettings(&s);
}

static void
ModernOn(int32 upscale, int32 aa)
{
	metal::TierCaps all = { true, true, true };
	metal::setTierCapsForTest(&all);
	SetSettings(metal::TIER_MODERN, upscale, aa);
}

static void
EnhancedOn(void)
{
	metal::TierCaps m1 = { true, false, false };
	metal::setTierCapsForTest(&m1);
	SetSettings(metal::TIER_ENHANCED, metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
}

static void
TiersOff(void)
{
	SetSettings(metal::TIER_CLASSIC, metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::setTierCapsForTest(nil);
}

struct Scene3D
{
	Atomic *front;
	Atomic *back;
	Atomic *blended;
	Geometry *geos[3];
	Material *mat;
};

static Atomic*
Quad3D(Scene3D *s, int i, uint32 flags, float x0, float y0, float x1, float y1, float z, RGBA c)
{
	RGBA col[4] = { c, c, c, c };
	s->geos[i] = QuadGeometry(flags, x0, y0, x1, y1, z, col, nil, s->mat);
	return MakeAtomic(s->geos[i]);
}

static void
MakeScene(Scene3D *s)
{
	RGBA red = { 220, 40, 30, 255 }, blue = { 30, 60, 220, 255 }, glass = { 40, 200, 90, 128 };
	s->mat = Material::create();
	s->front = Quad3D(s, 0, Geometry::POSITIONS | Geometry::TEXTURED | Geometry::PRELIT | Geometry::NORMALS,
		2.0f, 1.5f, -1.0f, -1.0f, 10.0f, red);
	s->back = Quad3D(s, 1, Geometry::POSITIONS | Geometry::PRELIT, 6.0f, 4.0f, -6.0f, -4.0f, 20.0f, blue);
	s->blended = Quad3D(s, 2, Geometry::POSITIONS | Geometry::PRELIT, -0.5f, 0.5f, -3.0f, -2.5f, 8.0f, glass);
}

static void
FreeScene(Scene3D *s)
{
	DestroyAtomic(s->front);
	DestroyAtomic(s->back);
	DestroyAtomic(s->blended);
	for(int i = 0; i < 3; i++)
		s->geos[i]->destroy();
	s->mat->destroy();
}

static void
DrawOpaque(Scene3D *s)
{
	SetRenderState(VERTEXALPHA, 0);
	s->back->render();
	s->front->render();
}

static void
DrawAll(Scene3D *s)
{
	DrawOpaque(s);
	SetRenderState(VERTEXALPHA, 1);
	s->blended->render();
	SetRenderState(VERTEXALPHA, 0);
}

static void
SceneFrame(Scene3D *s, bool prepass)
{
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(prepass && metal::beginPrepass()){
		DrawAll(s);
		metal::endPrepass();
	}
	DrawAll(s);
	metal::endScene(nil);
	WorldEnd();
}

static uint8*
CompositeNow(void)
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
FramesWithinOneStep(const uint8 *a, const uint8 *b, int w, int h, const char *what)
{
	int differing = 0, largest = 0, at = -1;
	if(a == nil || b == nil)
		return false;
	for(int i = 0; i < w*h*4; i++){
		if(i % 4 == 3)
			continue;
		int d = abs(a[i] - b[i]);
		if(d > 0)
			differing++;
		if(d > largest){
			largest = d;
			at = i/4;
		}
	}
	Detail("  %s: %d channel values differ, largest %d at (%d,%d)\n", what, differing, largest, at < 0 ? -1 : at % w,
		at < 0 ? -1 : at / w);
	return largest <= 1;
}

static int
PX(float x, float z)
{
	return (int)floorf(320.0f*(1.0f - x/z));
}

static int
PY(float y, float z)
{
	return (int)floorf(240.0f*(1.0f - y/(0.75f*z)));
}

static bool
CheckPrepassTargets(void)
{
	const char *name = "a modern prepass writes depth and view normals for opaque draws and skips blended ones";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	float n[3] = {}, m[2], d = -1.0f, bn[3], bm[2], bd = -1.0f;
	bool read = false;
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		DrawAll(&s);
		metal::endPrepass();
		read = metal::readPrepassForTest(PX(0.5f, 10.0f), PY(0.25f, 10.0f), n, m, &d) &&
			metal::readPrepassForTest(PX(-2.5f, 8.0f), PY(-2.4f, 8.0f), bn, bm, &bd);
	}
	DrawAll(&s);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	FreeScene(&s);
	bool ok = read && fabsf(n[0]) < 0.01f && fabsf(n[1]) < 0.01f && fabsf(n[2] + 1.0f) < 0.01f &&
		d > 0.0f && d < 1.0f && bd > d &&
		after.prepassDraws - before.prepassDraws == 2 && after.prepassSkipped - before.prepassSkipped == 1 &&
		after.prepassEqual - before.prepassEqual == 2;
	if(!ok)
		Detail("  normal %.3f,%.3f,%.3f depth %.6f; under the blended quad depth %.6f; prepass draws %u skipped %u equal %u\n",
			n[0], n[1], n[2], d, bd, after.prepassDraws - before.prepassDraws, after.prepassSkipped - before.prepassSkipped,
			after.prepassEqual - before.prepassEqual);
	return Report(ok, name);
}

static bool
PrepassIdentity(const char *name)
{
	Scene3D s;
	MakeScene(&s);
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	EnhancedOn();
	SceneFrame(&s, false);
	uint8 *enhanced = CompositeNow();
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	SceneFrame(&s, true);
	uint8 *modern = CompositeNow();
	bool depthOk = true;
	if(PLUGINOFFSET(metal::MetalRaster, cam->frameBuffer, metal::nativeRasterOffset)->numSamples > 1){
		float n[3], m[2], d = -1.0f;
		depthOk = metal::readPrepassForTest(PX(0.5f, 10.0f), PY(0.25f, 10.0f), n, m, &d) && d > 0.0f && d < 1.0f;
		if(!depthOk)
			Detail("  prepass depth on the front quad %.6f\n", d);
	}
	cam->showRaster(0);
	metal::StateStats st = metal::getStateStats();
	bool prepassRan = st.prepassDraws - before.prepassDraws == 2;
	if(!prepassRan)
		Detail("  the modern frame drew %u prepass draws, expected 2\n", st.prepassDraws - before.prepassDraws);
	TiersOff();
	FreeScene(&s);
	bool ok = FramesWithinOneStep(enhanced, modern, w, h, "modern with the prepass against enhanced") &&
		st.prepassOrphans == 0 && depthOk && prepassRan;
	delete[] enhanced;
	delete[] modern;
	return Report(ok, name);
}

static bool
CheckPrepassOrphan(void)
{
	const char *name = "a prepass draw the forward pass never draws is counted as an orphan and logged once";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	for(int f = 0; f < 2; f++){
		metal::beginScene(cam);
		WorldBegin(SKY);
		if(metal::beginPrepass()){
			DrawOpaque(&s);
			metal::endPrepass();
		}
		SetRenderState(VERTEXALPHA, 0);
		s.back->render();
		metal::endScene(nil);
		WorldEnd();
	}
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	FreeScene(&s);
	bool ok = after.prepassOrphans - before.prepassOrphans == 2 && after.prepassOrphanLogs - before.prepassOrphanLogs <= 1 &&
		after.prepassOrphanLogs >= 1;
	if(!ok)
		Detail("  orphans %u logs %u\n", after.prepassOrphans - before.prepassOrphans, after.prepassOrphanLogs);
	return Report(ok, name);
}

static bool
Sky(const float *px, const float *sky)
{
	return fabsf(px[0] - sky[0]) < 0.002f && fabsf(px[1] - sky[1]) < 0.002f && fabsf(px[2] - sky[2]) < 0.002f;
}

static bool
Red(const float *px, const float *sky)
{
	return px[0] > sky[0] + 0.2f && px[2] < px[0];
}

static bool
CheckPrepassViewChange(void)
{
	const char *name = "a forward draw under another view than the prepass's is not keyed and keeps its normal depth test";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	float px[4] = {}, sky[4] = {};
	bool began = false, read = false;
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		began = true;
		s.front->render();
		metal::endPrepass();
	}
	RawMatrix view = cam->devView;
	view.pos.x += 0.5f;
	metal::setViewMatrix((float32*)&view);
	s.front->render();
	read = metal::readScenePixel(PX(-1.0f, 10.0f) + 8, PY(0.25f, 10.0f), px) && metal::readScenePixel(5, 5, sky);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	FreeScene(&s);
	uint32 equal = after.prepassEqual - before.prepassEqual;
	bool ok = began && read && equal == 0 && Red(px, sky);
	if(!ok)
		Detail("  prepass %d read %d equal %u; newly covered pixel %.3f,%.3f,%.3f, sky %.3f,%.3f,%.3f\n", began, read, equal,
			px[0], px[1], px[2], sky[0], sky[1], sky[2]);
	return Report(ok, name);
}

static bool
ForcedEqualFrame(Scene3D *s, Atomic *forward, float px[4], float sky[4])
{
	bool read = false;
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		s->front->render();
		metal::endPrepass();
		metal::forcePrepassEqualForTest();
		forward->render();
		read = metal::readScenePixel(PX(0.5f, 10.0f), PY(0.25f, 10.0f), px) && metal::readScenePixel(5, 5, sky);
	}
	metal::endScene(nil);
	WorldEnd();
	return read;
}

static bool
CheckPrepassEqualOnGpu(void)
{
	const char *name = "a draw tested equal against the prepass depth is dropped a little in front of it and drawn on it";
	Scene3D s;
	MakeScene(&s);
	RGBA red = { 220, 40, 30, 255 }, col[4] = { red, red, red, red };
	float k = 9.9f/10.0f;
	Geometry *g = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::PRELIT | Geometry::NORMALS,
		2.0f*k, 1.5f*k, -1.0f*k, -1.0f*k, 9.9f, col, nil, s.mat);
	Atomic *nearer = MakeAtomic(g);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	float ahead[4] = {}, on[4] = {}, sky[4] = {}, sky2[4] = {};
	bool read = ForcedEqualFrame(&s, nearer, ahead, sky) && ForcedEqualFrame(&s, s.front, on, sky2);
	TiersOff();
	DestroyAtomic(nearer);
	g->destroy();
	FreeScene(&s);
	bool ok = read && Sky(ahead, sky) && Red(on, sky2);
	if(!ok)
		Detail("  read %d; in front %.3f,%.3f,%.3f, on it %.3f,%.3f,%.3f, sky %.3f,%.3f,%.3f\n", read, ahead[0], ahead[1],
			ahead[2], on[0], on[1], on[2], sky[0], sky[1], sky[2]);
	return Report(ok, name);
}

static bool
CheckPrepassKeepsSceneClearPending(void)
{
	const char *name = "a modern prepass frame clears the scene colour in its first forward pass, not in a pass of its own";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	bool began = false;
	metal::beginScene(cam);
	WorldBegin(SKY);
	uint32 p0 = metal::getFrameStats().renderPasses;
	if(metal::beginPrepass()){
		began = true;
		DrawOpaque(&s);
		metal::endPrepass();
	}
	DrawOpaque(&s);
	uint32 passes = metal::getFrameStats().renderPasses - p0;
	metal::endScene(nil);
	WorldEnd();
	TiersOff();
	FreeScene(&s);
	bool ok = began && passes == 2;
	if(!ok)
		Detail("  prepass %d, %u passes up to the forward draws, expected 2\n", began, passes);
	return Report(ok, name);
}

static bool
CheckPrepassPrewarmed(void)
{
	const char *name = "the modern tier switch builds the prepass pipelines, and a modern frame builds none late";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	SceneFrame(&s, false);
	metal::StateStats before = metal::getStateStats();
	SceneFrame(&s, true);
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	FreeScene(&s);
	bool ok = after.pipelinesLate == before.pipelinesLate && after.pipelineFailures == before.pipelineFailures;
	if(!ok)
		Detail("  late %u -> %u, failures %u -> %u\n", before.pipelinesLate, after.pipelinesLate, before.pipelineFailures,
			after.pipelineFailures);
	return Report(ok, name);
}

static bool
CheckShowClosesPrepass(void)
{
	const char *name = "a frame shown with its prepass still open leaves no keys behind for the next frame";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::beginScene(cam);
	WorldBegin(SKY);
	bool began = metal::beginPrepass();
	if(began)
		DrawOpaque(&s);
	WorldEnd();
	float px[4] = {}, sky[4] = {};
	metal::beginScene(cam);
	WorldBegin(SKY);
	DrawOpaque(&s);
	bool read = metal::readScenePixel(PX(0.5f, 10.0f), PY(0.25f, 10.0f), px) && metal::readScenePixel(5, 5, sky);
	metal::endScene(nil);
	WorldEnd();
	TiersOff();
	FreeScene(&s);
	bool ok = began && read && Red(px, sky);
	if(!ok)
		Detail("  prepass %d read %d; front quad %.3f,%.3f,%.3f, sky %.3f,%.3f,%.3f\n", began, read, px[0], px[1], px[2],
			sky[0], sky[1], sky[2]);
	return Report(ok, name);
}

static bool
CheckForcedEqualCleared(void)
{
	const char *name = "a forced equal test left unused in one frame does not reach the next frame";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::beginScene(cam);
	WorldBegin(SKY);
	bool began = metal::beginPrepass();
	if(began){
		s.front->render();
		metal::endPrepass();
	}
	s.front->render();
	metal::forcePrepassEqualForTest();
	metal::endScene(nil);
	WorldEnd();
	float px[4] = {}, sky[4] = {};
	metal::beginScene(cam);
	WorldBegin(SKY);
	s.front->render();
	bool read = metal::readScenePixel(PX(0.5f, 10.0f), PY(0.25f, 10.0f), px) && metal::readScenePixel(5, 5, sky);
	metal::endScene(nil);
	WorldEnd();
	TiersOff();
	FreeScene(&s);
	bool ok = began && read && Red(px, sky);
	if(!ok)
		Detail("  prepass %d read %d; front quad %.3f,%.3f,%.3f, sky %.3f,%.3f,%.3f\n", began, read, px[0], px[1], px[2],
			sky[0], sky[1], sky[2]);
	return Report(ok, name);
}

static void
DrawSpread(Atomic *a, int n)
{
	Frame *f = a->getFrame();
	for(int i = 0; i < n; i++){
		V3d p = { 0.0005f*i, 0.0f, 0.0f };
		f->translate(&p, COMBINEREPLACE);
		f->updateObjects();
		a->render();
	}
}

static bool
CheckPrepassUnkeyed(void)
{
	const char *name = "prepass draws past the key capacity are counted as unkeyed and their forward draws keep their depth test";
	enum { CAPACITY = 8192, EXTRA = 5 };
	RGBA red = { 220, 40, 30, 255 }, col[4] = { red, red, red, red };
	Material *mat = Material::create();
	Geometry *g = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::PRELIT | Geometry::NORMALS,
		0.1f, 0.1f, -0.1f, -0.1f, 10.0f, col, nil, mat);
	Atomic *a = MakeAtomic(g);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	metal::beginScene(cam);
	WorldBegin(SKY);
	SetRenderState(VERTEXALPHA, 0);
	bool began = metal::beginPrepass();
	if(began){
		DrawSpread(a, CAPACITY + EXTRA);
		metal::endPrepass();
	}
	DrawSpread(a, CAPACITY + EXTRA);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();
	metal::logStats();
	const char *tail = strstr(metal::getStatsLine(), " unkeyed ");
	unsigned shown = 0;
	bool printed = tail && sscanf(tail, " unkeyed %u", &shown) == 1 && shown == after.prepassUnkeyed;
	TiersOff();
	DestroyAtomic(a);
	g->destroy();
	mat->destroy();
	uint32 draws = after.prepassDraws - before.prepassDraws, unkeyed = after.prepassUnkeyed - before.prepassUnkeyed;
	uint32 equal = after.prepassEqual - before.prepassEqual, orphans = after.prepassOrphans - before.prepassOrphans;
	bool ok = began && draws == CAPACITY + EXTRA && unkeyed == EXTRA && equal == CAPACITY && orphans == 0 && printed;
	if(!ok)
		Detail("  prepass %d draws %u unkeyed %u equal %u orphans %u; stats line shows %u\n", began, draws, unkeyed, equal,
			orphans, shown);
	return Report(ok, name);
}

static bool
CheckPrepassDropMidFrame(void)
{
	const char *name = "a camera depth raster destroyed inside the prepass bracket drops the frame's keys without orphans";
	Scene3D s;
	MakeScene(&s);
	int w = cam->zBuffer->width, h = cam->zBuffer->height;
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	metal::beginScene(cam);
	WorldBegin(SKY);
	bool began = metal::beginPrepass();
	if(began)
		DrawOpaque(&s);
	cam->zBuffer->destroy();
	cam->zBuffer = Raster::create(w, h, 0, Raster::ZBUFFER);
	metal::endPrepass();
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats mid = metal::getStateStats();
	SceneFrame(&s, true);
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	FreeScene(&s);
	bool ok = began && mid.prepassOrphans == before.prepassOrphans && mid.prepassOrphanLogs == before.prepassOrphanLogs &&
		after.prepassDraws - mid.prepassDraws == 2 && after.prepassEqual - mid.prepassEqual == 2 &&
		after.prepassOrphans == before.prepassOrphans;
	if(!ok)
		Detail("  prepass %d orphans %u logs %u after the drop; next frame prepass draws %u equal %u orphans %u\n", began,
			mid.prepassOrphans - before.prepassOrphans, mid.prepassOrphanLogs - before.prepassOrphanLogs,
			after.prepassDraws - mid.prepassDraws, after.prepassEqual - mid.prepassEqual,
			after.prepassOrphans - mid.prepassOrphans);
	return Report(ok, name);
}

static void
PlaceCamera(float yawDeg, float x, float y, float z)
{
	Frame *f = cam->getFrame();
	f->matrix.setIdentity();
	V3d up = { 0.0f, 1.0f, 0.0f };
	f->rotate(&up, yawDeg, COMBINEREPLACE);
	V3d pos = { x, y, z };
	f->translate(&pos, COMBINEPOSTCONCAT);
	f->updateObjects();
}

static bool
PrepassMotionAt(int x, int y, float m[2])
{
	float n[3], d;
	return metal::readPrepassForTest(x, y, n, m, &d);
}

static void
TurnMotion(int x, int y, float sign, float deg, float m[2])
{
	float nx = 2.0f*(x + 0.5f)/640.0f - 1.0f, ny = 1.0f - 2.0f*(y + 0.5f)/480.0f;
	float a = atanf(nx), b = a + sign*deg*3.14159265f/180.0f;
	m[0] = 0.5f*(tanf(b) - nx);
	m[1] = -0.5f*ny*(cosf(a)/cosf(b) - 1.0f);
}

static bool
CheckCameraMotion(void)
{
	const char *name = "a turning camera gives a drawn surface and an uncovered far pixel the motion of their view directions";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	SceneFrame(&s, true);
	PlaceCamera(2.0f, 0.0f, 0.0f, 0.0f);
	SceneFrame(&s, true);
	float onQuad[2] = {}, far[2] = {};
	bool read = PrepassMotionAt(320, 240, onQuad) && PrepassMotionAt(20, 20, far);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	FreeScene(&s);
	float sign = onQuad[0] < 0.0f ? -1.0f : 1.0f, wantQuad[2], wantFar[2];
	TurnMotion(320, 240, sign, 2.0f, wantQuad);
	TurnMotion(20, 20, sign, 2.0f, wantFar);
	bool ok = read && fabsf(onQuad[0]) > 0.01f && fabsf(onQuad[0] - wantQuad[0]) < 0.0015f &&
		fabsf(onQuad[1] - wantQuad[1]) < 0.001f && fabsf(far[0] - wantFar[0]) < 0.0015f && fabsf(far[1] - wantFar[1]) < 0.0015f;
	if(!ok)
		Detail("  quad motion %.5f,%.5f expected %.5f,%.5f; far motion %.5f,%.5f expected %.5f,%.5f\n", onQuad[0], onQuad[1],
			wantQuad[0], wantQuad[1], far[0], far[1], wantFar[0], wantFar[1]);
	return Report(ok, name);
}

static bool
CheckMotionAfterGap(void)
{
	const char *name = "a prepass frame after a frame without the prepass starts without camera history";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	SceneFrame(&s, true);
	PlaceCamera(2.0f, 0.0f, 0.0f, 0.0f);
	SceneFrame(&s, false);
	SceneFrame(&s, true);
	float m[2] = { 1.0f, 1.0f };
	bool read = PrepassMotionAt(320, 240, m);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	FreeScene(&s);
	bool ok = read && fabsf(m[0]) < 1e-4f && fabsf(m[1]) < 1e-4f;
	if(!ok)
		Detail("  motion %.5f,%.5f, expected none\n", m[0], m[1]);
	return Report(ok, name);
}

static bool
CheckObjectMotion(void)
{
	const char *name = "an object's previous world matrix gives its motion and a static scene gives none";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	SceneFrame(&s, true);
	SceneFrame(&s, true);
	float still[2] = { 1.0f, 1.0f };
	bool readStill = PrepassMotionAt(PX(0.5f, 10.0f), PY(0.25f, 10.0f), still);
	V3d up = { 0.0f, 1.0f, 0.0f };
	s.front->getFrame()->translate(&up, COMBINEPOSTCONCAT);
	s.front->getFrame()->updateObjects();
	SceneFrame(&s, true);
	float moved[2] = {};
	bool readMoved = PrepassMotionAt(PX(0.5f, 10.0f), PY(1.25f, 10.0f), moved);
	TiersOff();
	FreeScene(&s);
	float want = 240.0f/(0.75f*10.0f)/480.0f;
	bool ok = readStill && readMoved && fabsf(still[0]) < 1e-4f && fabsf(still[1]) < 1e-4f &&
		fabsf(moved[0]) < 1e-3f && fabsf(moved[1] - want) < 0.002f;
	if(!ok)
		Detail("  still %.5f,%.5f moved %.5f,%.5f, expected y %.5f\n", still[0], still[1], moved[0], moved[1], want);
	return Report(ok, name);
}

static bool
CheckObjectMotionAfterCut(void)
{
	const char *name = "an object moved across a camera cut has no object motion";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	SceneFrame(&s, true);
	V3d up = { 0.0f, 1.0f, 0.0f };
	s.front->getFrame()->translate(&up, COMBINEPOSTCONCAT);
	s.front->getFrame()->updateObjects();
	metal::cameraCut();
	SceneFrame(&s, true);
	float m[2] = { 1.0f, 1.0f };
	bool read = PrepassMotionAt(PX(0.5f, 10.0f), PY(1.25f, 10.0f), m);
	TiersOff();
	FreeScene(&s);
	bool ok = read && fabsf(m[0]) < 1e-4f && fabsf(m[1]) < 1e-4f;
	if(!ok)
		Detail("  motion %.5f,%.5f, expected none\n", m[0], m[1]);
	return Report(ok, name);
}

static bool
CheckSkinnedMotion(void)
{
	const char *name = "a skinned draw's previous bones give its motion and its forward draw tests equal";
	RGBA red = { 220, 40, 30, 255 }, col[4] = { red, red, red, red };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::NORMALS | Geometry::PRELIT, 1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, col, nil, mat);
	Matrix ident[2];
	ident[0].setIdentity();
	ident[1].setIdentity();
	uint8 idx[4][4] = { { 0, 1, 0, 0 }, { 0, 1, 0, 0 }, { 0, 1, 0, 0 }, { 0, 1, 0, 0 } };
	float wts[4][4] = { { 0.5f, 0.5f, 0, 0 }, { 0.5f, 0.5f, 0, 0 }, { 0.5f, 0.5f, 0, 0 }, { 0.5f, 0.5f, 0, 0 } };
	AttachSkin(geo, 2, ident, idx, wts);
	Atomic *a = SkinAtomic(geo);
	V3d away = { 0.0f, 0.0f, 10.0f };
	a->getFrame()->translate(&away, COMBINEREPLACE);
	a->getFrame()->updateObjects();
	HAnimHierarchy *h = HAnimHierarchy::create(2, nil, nil, 0, 36);
	h->matrices[0] = *a->getFrame()->getLTM();
	h->matrices[1] = *a->getFrame()->getLTM();
	Skin::setHierarchy(a, h);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	float m[2] = { 0.0f, 0.0f }, px[4] = {}, sky[4] = {};
	bool read = false;
	for(int f = 0; f < 2; f++){
		if(f == 1){
			V3d right = { 1.0f, 0.0f, 0.0f };
			h->matrices[0].translate(&right, COMBINEPOSTCONCAT);
			h->matrices[1].translate(&right, COMBINEPOSTCONCAT);
		}
		metal::beginScene(cam);
		WorldBegin(SKY);
		if(metal::beginPrepass()){
			a->render();
			metal::endPrepass();
		}
		a->render();
		if(f == 1)
			read = metal::readScenePixel(PX(0.5f, 10.0f), PY(0.0f, 10.0f), px) && metal::readScenePixel(5, 5, sky);
		metal::endScene(nil);
		WorldEnd();
		if(f == 1)
			read = read && PrepassMotionAt(PX(0.5f, 10.0f), PY(0.0f, 10.0f), m);
	}
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	DestroySkinAtomic(a);
	geo->destroy();
	mat->destroy();
	float want = 0.5f/10.0f;
	bool ok = read && fabsf(m[0] - want) < 0.002f && fabsf(m[1]) < 0.001f && Red(px, sky) &&
		after.prepassEqual - before.prepassEqual == 2 && after.prepassOrphans == before.prepassOrphans;
	if(!ok)
		Detail("  motion %.5f,%.5f expected x %.5f; pixel %.3f,%.3f,%.3f; equal draws %u orphans %u\n", m[0], m[1], want,
			px[0], px[1], px[2], after.prepassEqual - before.prepassEqual, after.prepassOrphans - before.prepassOrphans);
	return Report(ok, name);
}

int
RunModernChecks(Camera *camera)
{
	failures = 0;
	cam = camera;
	WorldOpen(camera);
	CheckPrepassTargets();
	PrepassIdentity("modern frames with the prepass are within 1/255 of enhanced frames at one sample");
	CheckPrepassOrphan();
	CheckPrepassViewChange();
	CheckPrepassEqualOnGpu();
	CheckPrepassKeepsSceneClearPending();
	CheckPrepassPrewarmed();
	CheckShowClosesPrepass();
	CheckForcedEqualCleared();
	CheckPrepassUnkeyed();
	CheckPrepassDropMidFrame();
	CheckCameraMotion();
	CheckMotionAfterGap();
	CheckObjectMotion();
	CheckObjectMotionAfterCut();
	CheckSkinnedMotion();
	WorldClose();
	return failures;
}

static bool
SampleZeroPixel(int x, int y, float wantMotionY, int *onQuad)
{
	float n[3] = {}, m[2] = {}, d = -1.0f;
	if(!metal::readPrepassForTest(x, y, n, m, &d)){
		Detail("  pixel %d,%d could not be read\n", x, y);
		return false;
	}
	bool quad = d < 1.0f;
	bool ok = quad ? fabsf(n[0]) < 0.01f && fabsf(n[1]) < 0.01f && fabsf(n[2] + 1.0f) < 0.01f &&
			fabsf(m[0]) < 1e-3f && fabsf(m[1] - wantMotionY) < 0.002f :
		n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f && fabsf(m[0]) < 1e-4f && fabsf(m[1]) < 1e-4f;
	if(quad)
		(*onQuad)++;
	if(!ok)
		Detail("  pixel %d,%d depth %.6f normal %.3f,%.3f,%.3f motion %.5f,%.5f\n", x, y, d, n[0], n[1], n[2], m[0], m[1]);
	return ok;
}

static bool
CheckPrepassSampleZero(void)
{
	const char *name = "with four samples the resolved normal, motion and depth of an edge pixel all come from sample 0";
	RGBA red = { 220, 40, 30, 255 }, col[4] = { red, red, red, red };
	Material *mat = Material::create();
	Geometry *g = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::PRELIT | Geometry::NORMALS,
		0.015625f, 1.0f, -0.984375f, -1.0f, 10.0f, col, nil, mat);
	Atomic *a = MakeAtomic(g);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::cameraCut();
	bool ok = true;
	int onQuad = 0;
	for(int f = 0; f < 2; f++){
		if(f == 1){
			V3d up = { 0.0f, 0.5f, 0.0f };
			a->getFrame()->translate(&up, COMBINEPOSTCONCAT);
			a->getFrame()->updateObjects();
		}
		metal::beginScene(cam);
		WorldBegin(SKY);
		SetRenderState(VERTEXALPHA, 0);
		if(metal::beginPrepass()){
			a->render();
			metal::endPrepass();
		}
		a->render();
		metal::endScene(nil);
		if(f == 1){
			float want = 0.5f/(0.75f*10.0f)*0.5f;
			bool left = SampleZeroPixel(319, PY(0.5f, 10.0f), want, &onQuad);
			bool right = SampleZeroPixel(351, PY(0.5f, 10.0f), want, &onQuad);
			ok = left && right;
		}
		WorldEnd();
	}
	TiersOff();
	DestroyAtomic(a);
	g->destroy();
	mat->destroy();
	if(onQuad != 1)
		Detail("  %d of the two edge pixels have sample 0 on the quad, expected 1\n", onQuad);
	return Report(ok && onQuad == 1, name);
}

int
RunRestartModernChecks(bool (*restart)(uint32 samples), Camera *(*camera)(void))
{
	const char *name = "modern frames with the prepass are within 1/255 of enhanced frames at four samples";
	failures = 0;
	if(!restart(4)){
		cam = camera();
		Report(false, name);
		restart(1);
		cam = camera();
		return failures;
	}
	cam = camera();
	WorldOpen(cam);
	PrepassIdentity(name);
	CheckPrepassSampleZero();
	WorldClose();
	if(!restart(1))
		Report(false, "the engine restarts at one sample after the modern checks");
	cam = camera();
	return failures;
}
