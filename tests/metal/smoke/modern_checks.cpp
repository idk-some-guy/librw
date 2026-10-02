#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalkeys.h>
#include <src/metal/metalprepass.h>
#include <src/metal/metalrelit.h>
#include "world_checks.h"
#include "modern_checks.h"
#include "late_pipelines.h"

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
	NoteLatePipelines(name);
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
	const char *tail = strstr(metal::getStatsLine(), " prepass ");
	unsigned shown = 0, shownDraws = 0, shownEqual = 0;
	bool printed = tail && sscanf(tail, " prepass %u equal %u unkeyed %u", &shownDraws, &shownEqual, &shown) == 3 &&
		shown == after.prepassUnkeyed && shownDraws == CAPACITY + EXTRA && shownEqual == CAPACITY;
	TiersOff();
	DestroyAtomic(a);
	g->destroy();
	mat->destroy();
	uint32 draws = after.prepassDraws - before.prepassDraws, unkeyed = after.prepassUnkeyed - before.prepassUnkeyed;
	uint32 equal = after.prepassEqual - before.prepassEqual, orphans = after.prepassOrphans - before.prepassOrphans;
	bool ok = began && draws == CAPACITY + EXTRA && unkeyed == EXTRA && equal == CAPACITY && orphans == 0 && printed;
	if(!ok)
		Detail("  prepass %d draws %u unkeyed %u equal %u orphans %u; stats line shows prepass %u equal %u unkeyed %u\n", began,
			draws, unkeyed, equal, orphans, shownDraws, shownEqual, shown);
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
	float yaw = 2.0f;
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	SceneFrame(&s, true);
	PlaceCamera(yaw, 0.0f, 0.0f, 0.0f);
	SceneFrame(&s, true);
	float onQuad[2] = {}, far[2] = {};
	bool read = PrepassMotionAt(320, 240, onQuad) && PrepassMotionAt(20, 20, far);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	FreeScene(&s);
	// A positive yaw turns the view toward +x, which this projection draws on the left: the scene moves right.
	float sign = yaw > 0.0f ? -1.0f : 1.0f, wantQuad[2], wantFar[2];
	TurnMotion(320, 240, sign, yaw, wantQuad);
	TurnMotion(20, 20, sign, yaw, wantFar);
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

static bool
SplitPrepassFrame(Scene3D *s, int x, int y, float n[3], float m[2])
{
	float d;
	bool began = false;
	metal::beginScene(cam);
	WorldBegin(SKY);
	SetRenderState(VERTEXALPHA, 0);
	if(metal::beginPrepass()){
		began = true;
		s->front->render();
		metal::readPrepassForTest(x, y, n, m, &d);
		s->back->render();
		metal::endPrepass();
	}
	DrawOpaque(s);
	metal::endScene(nil);
	WorldEnd();
	return began && metal::readPrepassForTest(x, y, n, m, &d);
}

static bool
CheckPrepassSplit(const char *name)
{
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::cameraCut();
	int x = PX(0.5f, 10.0f), y = PY(1.25f, 10.0f);
	float n[3] = {}, m[2] = {};
	bool read = SplitPrepassFrame(&s, x, y, n, m);
	V3d up = { 0.0f, 1.0f, 0.0f };
	s.front->getFrame()->translate(&up, COMBINEPOSTCONCAT);
	s.front->getFrame()->updateObjects();
	read = read && SplitPrepassFrame(&s, x, y, n, m);
	TiersOff();
	FreeScene(&s);
	float want = 240.0f/(0.75f*10.0f)/480.0f;
	bool ok = read && fabsf(n[0]) < 0.01f && fabsf(n[1]) < 0.01f && fabsf(n[2] + 1.0f) < 0.01f && fabsf(m[0]) < 1e-3f &&
		fabsf(m[1] - want) < 0.002f;
	if(!ok)
		Detail("  read %d; first segment's pixel normal %.3f,%.3f,%.3f motion %.5f,%.5f, expected y %.5f\n", read, n[0], n[1],
			n[2], m[0], m[1], want);
	return Report(ok, name);
}

static uint16 quadIndex[6] = { 0, 1, 2, 0, 2, 3 };

static void
Vertex2D(metal::Im2DVertex *v, float x, float y, RGBA c)
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
BlendMode(bool additive)
{
	SetRenderStatePtr(TEXTURERASTER, nil);
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(ZWRITEENABLE, 0);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(SRCBLEND, additive ? BLENDONE : BLENDSRCALPHA);
	SetRenderState(DESTBLEND, additive ? BLENDONE : BLENDINVSRCALPHA);
}

static void
OpaqueMode(void)
{
	SetRenderState(ZTESTENABLE, 1);
	SetRenderState(ZWRITEENABLE, 1);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
}

static void
Quad2D(float x0, float y0, float x1, float y1, RGBA c, bool additive)
{
	metal::Im2DVertex v[4];
	Vertex2D(&v[0], x0, y0, c);
	Vertex2D(&v[1], x0, y1, c);
	Vertex2D(&v[2], x1, y1, c);
	Vertex2D(&v[3], x1, y0, c);
	BlendMode(additive);
	im2d::RenderIndexedPrimitive(PRIMTYPETRILIST, v, 4, quadIndex, 6);
	OpaqueMode();
}

static void
Quad3DImmediate(float x0, float y0, float x1, float y1, float z, RGBA c)
{
	const float xy[4][2] = { { x0, y1 }, { x1, y1 }, { x1, y0 }, { x0, y0 } };
	metal::Im3DVertex v[4];
	for(int i = 0; i < 4; i++){
		v[i].setX(xy[i][0]); v[i].setY(xy[i][1]); v[i].setZ(z);
		v[i].setNormalX(0.0f); v[i].setNormalY(0.0f); v[i].setNormalZ(-1.0f);
		v[i].setColor(c.red, c.green, c.blue, c.alpha);
		v[i].setU(0.0f); v[i].setV(0.0f);
	}
	BlendMode(false);
	im3d::Transform(v, 4, nil, 0);
	im3d::RenderIndexedPrimitive(PRIMTYPETRILIST, quadIndex, 6);
	im3d::End();
	OpaqueMode();
}

static void
Frames(Scene3D *s, int n)
{
	for(int i = 0; i < n; i++)
		SceneFrame(s, true);
}

static Camera*
OtherCamera(void)
{
	Camera *other = Camera::create();
	other->setFrame(Frame::create());
	other->frameBuffer = Raster::create(64, 64, 0, Raster::CAMERATEXTURE);
	other->zBuffer = Raster::create(64, 64, 0, Raster::ZBUFFER);
	return other;
}

static void
FreeOtherCamera(Camera *other)
{
	Frame *f = other->getFrame();
	Raster *fb = other->frameBuffer, *z = other->zBuffer;
	other->destroy();
	f->destroy();
	fb->destroy();
	z->destroy();
}

static bool
CheckJitter(void)
{
	const char *name = "the scene camera's projection is jittered through eight phases and an off-scene camera's is not";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	Frames(&s, 3);
	float seen[8][2];
	bool ok = true;
	for(int f = 0; f < 9; f++){
		SceneFrame(&s, true);
		metal::FrameInfo fi = metal::getFrameInfo();
		float proj[16];
		metal::sceneProjectionForTest(proj);
		float wantX = 2.0f*fi.jitterX/fi.internalWidth, wantY = -2.0f*fi.jitterY/fi.internalHeight;
		ok = ok && fabsf(proj[8] - wantX) < 1e-6f && fabsf(proj[9] - wantY) < 1e-6f && fabsf(fi.jitterX) < 0.5f &&
			fabsf(fi.jitterY) < 0.5f && (fi.jitterX != 0.0f || fi.jitterY != 0.0f);
		if(f < 8){
			seen[f][0] = fi.jitterX;
			seen[f][1] = fi.jitterY;
			for(int g = 0; g < f; g++)
				ok = ok && (seen[g][0] != fi.jitterX || seen[g][1] != fi.jitterY);
		}else
			ok = ok && fi.jitterX == seen[0][0] && fi.jitterY == seen[0][1];
		if(!ok){
			Detail("  frame %d jitter %g,%g proj[8] %g proj[9] %g\n", f, fi.jitterX, fi.jitterY, proj[8], proj[9]);
			break;
		}
	}
	Camera *other = OtherCamera();
	float otherProj[16] = {};
	metal::beginScene(cam);
	WorldBegin(SKY);
	cam->endUpdate();
	other->beginUpdate();
	metal::sceneProjectionForTest(otherProj);
	other->endUpdate();
	cam->beginUpdate();
	metal::endScene(nil);
	WorldEnd();
	FreeOtherCamera(other);
	bool otherOk = otherProj[8] == 0.0f && otherProj[9] == 0.0f;
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_FXAA);
	SceneFrame(&s, true);
	float spatialProj[16];
	metal::sceneProjectionForTest(spatialProj);
	metal::FrameInfo sf = metal::getFrameInfo();
	bool spatialOk = spatialProj[8] == 0.0f && spatialProj[9] == 0.0f && sf.jitterX == 0.0f && sf.jitterY == 0.0f;
	TiersOff();
	FreeScene(&s);
	if(!otherOk)
		Detail("  other camera proj[8] %g proj[9] %g\n", otherProj[8], otherProj[9]);
	if(!spatialOk)
		Detail("  spatial frame proj[8] %g proj[9] %g jitter %g,%g\n", spatialProj[8], spatialProj[9], sf.jitterX, sf.jitterY);
	return Report(ok && otherOk && spatialOk, name);
}

static bool
CheckJitterKeyed(void)
{
	const char *name = "with the jitter on, an off-scene camera between the prepass and the forward pass leaves the keyed draws testing equal";
	Scene3D s;
	MakeScene(&s);
	Camera *other = OtherCamera();
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	Frames(&s, 3);
	metal::StateStats before = metal::getStateStats();
	metal::beginScene(cam);
	WorldBegin(SKY);
	bool began = metal::beginPrepass();
	if(began){
		DrawAll(&s);
		metal::endPrepass();
	}
	cam->endUpdate();
	other->beginUpdate();
	other->endUpdate();
	cam->beginUpdate();
	DrawAll(&s);
	metal::FrameInfo fi = metal::getFrameInfo();
	float sx = (float)fi.internalWidth/fi.outputWidth, sy = (float)fi.internalHeight/fi.outputHeight;
	float px[4] = {}, sky[4] = {};
	bool read = metal::readScenePixel((int)(PX(0.5f, 10.0f)*sx), (int)(PY(0.25f, 10.0f)*sy), px) &&
		metal::readScenePixel(5, 5, sky);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	FreeOtherCamera(other);
	FreeScene(&s);
	uint32 draws = after.prepassDraws - before.prepassDraws, equal = after.prepassEqual - before.prepassEqual;
	bool jittered = fi.jitterX != 0.0f || fi.jitterY != 0.0f;
	bool ok = began && read && jittered && draws == 2 && equal == 2 && after.prepassOrphans == before.prepassOrphans &&
		Red(px, sky);
	if(!ok)
		Detail("  prepass %d read %d jitter %g,%g; prepass draws %u equal %u orphans %u; front %.3f,%.3f,%.3f\n", began, read,
			fi.jitterX, fi.jitterY, draws, equal, after.prepassOrphans - before.prepassOrphans, px[0], px[1], px[2]);
	return Report(ok, name);
}

static bool
CheckTemporalScaler(void)
{
	const char *name = "modern at the quality factor runs the temporal scaler for taa and the spatial one for fxaa, and falls back to spatial";
	Scene3D s;
	MakeScene(&s);
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	Frames(&s, 2);
	metal::FrameInfo a = metal::getFrameInfo();
	metal::logStats();
	bool stats = strstr(metal::getStatsLine(), "upscaler temporal") && strstr(metal::getStatsLine(), " aa temporal");
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_QUALITY, metal::ANTIALIAS_FXAA);
	Frames(&s, 1);
	metal::FrameInfo c = metal::getFrameInfo();
	metal::setTemporalFailForTest(1);
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_BALANCED, metal::ANTIALIAS_TAA);
	Frames(&s, 1);
	metal::FrameInfo b = metal::getFrameInfo();
	metal::setTemporalFailForTest(0);
	TiersOff();
	FreeScene(&s);
	bool ok = a.upscaler == metal::UPSCALER_TEMPORAL && a.antiAliasing == metal::ANTIALIAS_TEMPORAL &&
		a.internalWidth == metal::internalSpan(w, metal::UPSCALE_QUALITY) && a.outputHeight == h && stats &&
		c.upscaler == metal::UPSCALER_SPATIAL && c.antiAliasing == metal::ANTIALIAS_FXAA &&
		b.upscaler == metal::UPSCALER_SPATIAL && b.antiAliasing == metal::ANTIALIAS_FXAA;
	if(!ok)
		Detail("  taa: upscaler %d aa %d internal %d; stats %d; fxaa: upscaler %d aa %d; fallback upscaler %d aa %d\n",
			a.upscaler, a.antiAliasing, a.internalWidth, stats, c.upscaler, c.antiAliasing, b.upscaler, b.antiAliasing);
	return Report(ok, name);
}

static bool
CheckTemporalAboveWhite(void)
{
	const char *name = "the temporal scaler keeps values above white";
	RGBA black = { 0, 0, 0, 255 }, white = { 255, 255, 255, 255 };
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	float in[4] = {}, out[4] = {};
	bool read = false;
	for(int f = 0; f < 12; f++){
		metal::beginScene(cam);
		WorldBegin(black);
		metal::beginPrepass();
		metal::endPrepass();
		metal::setEmissiveScale(5.0f);
		Quad2D(240, 160, 400, 320, white, true);
		metal::setEmissiveScale(1.0f);
		metal::endScene(nil);
		read = metal::readFrameOutput(320, 240, in) && metal::readFrameOutput(10, 10, out);
		WorldEnd();
	}
	TiersOff();
	bool ok = read && in[0] > 4.5f && in[0] < 5.5f && out[0] < 0.02f;
	if(!ok)
		Detail("  block centre %.3f corner %.4f\n", in[0], out[0]);
	return Report(ok, name);
}

static void
OutputRun(bool across, int fixed, int first, int n, float *red)
{
	for(int i = 0; i < n; i++){
		float px[4] = {};
		if(across)
			metal::readFrameOutput(first + i, fixed, px);
		else
			metal::readFrameOutput(fixed, first + i, px);
		red[i] = px[0];
	}
}

static float
Crossing(const float *v, int n, int first)
{
	float mid = 0.5f*(v[0] + v[n-1]);
	for(int i = 0; i + 1 < n; i++)
		if(v[i] != v[i+1] && (v[i] - mid)*(v[i+1] - mid) <= 0.0f)
			return first + i + 0.5f + (mid - v[i])/(v[i+1] - v[i]);
	return -100.0f;
}

enum { RUN = 12 };

struct EdgeProbe
{
	float row[RUN], col[RUN];
	float x, y;
};

static void
ProbeEdges(EdgeProbe *e, int rowY, int rowX0, int colX, int colY0)
{
	OutputRun(true, rowY, rowX0, RUN, e->row);
	OutputRun(false, colX, colY0, RUN, e->col);
	e->x = Crossing(e->row, RUN, rowX0);
	e->y = Crossing(e->col, RUN, colY0);
}

static bool
CheckTemporalConverges(void)
{
	const char *name = "a still scene through the temporal scaler converges on the true edges and holds them (jitter sign)";
	Scene3D s;
	MakeScene(&s);
	float cy = 0.04f;
	float wantX = 320.0f*(1.0f + 1.0f/10.0f), wantY = 240.0f*(1.0f - (1.5f - cy)/7.5f);
	int rowY = PY(0.25f - cy, 10.0f), rowX0 = (int)wantX - RUN/2, colX = PX(0.5f, 10.0f), colY0 = (int)wantY - RUN/2;
	EdgeProbe native, single, temporal, last;
	PlaceCamera(0.0f, 0.0f, cy, 0.0f);
	metal::cameraCut();
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	Frames(&s, 2);
	ProbeEdges(&native, rowY, rowX0, colX, colY0);
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	metal::setJitterIndexForTest(0);
	Frames(&s, 2);
	ProbeEdges(&single, rowY, rowX0, colX, colY0);
	float drift = 0.0f, meanX = 0.0f, meanY = 0.0f;
	for(int f = 0; f < 32; f++){
		Frames(&s, 1);
		ProbeEdges(&temporal, rowY, rowX0, colX, colY0);
		if(f >= 24){
			drift = std::max(drift, std::max(fabsf(temporal.x - last.x), fabsf(temporal.y - last.y)));
			meanX += temporal.x/8.0f;
			meanY += temporal.y/8.0f;
		}
		last = temporal;
	}
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	FreeScene(&s);
	float dx = fabsf(meanX - wantX), dy = fabsf(meanY - wantY);
	bool flat = true;
	for(int i = 0; i < RUN; i++)
		if(i < RUN/2 - 3 || i > RUN/2 + 2)
			flat = flat && fabsf(temporal.row[i] - native.row[i]) < 0.03f && fabsf(temporal.col[i] - native.col[i]) < 0.03f;
	bool ok = dx < 0.12f && dy < 0.12f && drift < 0.3f && flat;
	Detail("  edges at x %.3f y %.3f: native x %.3f y %.3f, one frame x %.3f y %.3f, mean over the last 8 frames x %.3f y %.3f "
		"(last x %.3f y %.3f), drift %.3f\n", wantX, wantY, native.x, native.y, single.x, single.y, meanX, meanY, temporal.x,
		temporal.y, drift);
	if(!ok && (dx >= 0.12f || dy >= 0.12f || drift >= 0.3f))
		Detail("  the 0.12 px and 0.3 drift limits depend on Apple's MetalFX; an OS update may have shifted its behaviour "
			"(measured errors x %.3f y %.3f, drift %.3f)\n", dx, dy, drift);
	if(!ok)
		for(int i = 0; i < RUN; i++)
			Detail("  %d: row native %.3f temporal %.3f; column native %.3f temporal %.3f\n", i, native.row[i],
				temporal.row[i], native.col[i], temporal.col[i]);
	return Report(ok, name);
}

static void
DrawOne(Atomic *a)
{
	metal::beginScene(cam);
	WorldBegin(SKY);
	SetRenderState(VERTEXALPHA, 0);
	if(metal::beginPrepass()){
		a->render();
		metal::endPrepass();
	}
	a->render();
	metal::endScene(nil);
	WorldEnd();
}

static float
PatchDiff(const uint8 *a, const uint8 *b, int w, int cx, int cy, int half)
{
	float sum = 0.0f;
	for(int y = cy - half; y < cy + half; y++)
		for(int x = cx - half; x < cx + half; x++)
			sum += abs(a[(y*w + x)*4] - b[(y*w + x)*4])/255.0f;
	return sum/(4*half*half);
}

static float
PanAgainstNative(Atomic *wall, float stepX, float stepY, int32 upscale)
{
	enum { STEPS = 24 };
	int w = cam->frameBuffer->width;
	ModernOn(upscale, metal::ANTIALIAS_TAA);
	DrawOne(wall);
	metal::cameraCut();
	for(int f = 0; f < STEPS; f++){
		PlaceCamera(0.0f, stepX*f, stepY*f, 0.0f);
		DrawOne(wall);
	}
	uint8 *temporal = CompositeNow();
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DrawOne(wall);
	DrawOne(wall);
	uint8 *native = CompositeNow();
	float d = temporal && native ? PatchDiff(temporal, native, w, 320, 240, 32) : 1.0f;
	delete[] temporal;
	delete[] native;
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	return d;
}

static bool
CheckTemporalMotion(const char *name, int32 upscale, float ratio, float limit)
{
	static const RGBA texels[4] = { { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, { 0, 0, 0, 255 }, { 255, 255, 255, 255 } };
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 24.0f, 0.0f }, { 24.0f, 16.0f }, { 0.0f, 16.0f } };
	Texture *tex = MakeTexture(2, 2, texels, Texture::LINEAR, Texture::WRAP, Texture::WRAP);
	Material *mat = Material::create();
	mat->setTexture(tex);
	Geometry *g = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::PRELIT | Geometry::NORMALS,
		3.0f, 2.0f, -3.0f, -2.0f, 10.0f, col, uv, mat);
	Atomic *wall = MakeAtomic(g);
	float still = PanAgainstNative(wall, 0.0f, 0.0f, upscale);
	float moving = PanAgainstNative(wall, -0.06f, 0.04f, upscale);
	TiersOff();
	DestroyAtomic(wall);
	g->destroy();
	mat->destroy();
	tex->destroy();
	bool ok = ratio > 0.0f ? moving < ratio*still : moving < limit;
	Detail("  mean red difference from the native frame: still %.4f, panning %.4f\n", still, moving);
	return Report(ok, name);
}

static bool
CheckReactiveMask(void)
{
	const char *name = "the reactive mask marks im2D and im3D draws in the bracket and emitters, and nothing else";
	Scene3D s;
	MakeScene(&s);
	RGBA white = { 255, 255, 255, 255 }, half = { 255, 255, 255, 128 };
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	Frames(&s, 1);
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		DrawOpaque(&s);
		metal::endPrepass();
	}
	DrawAll(&s);
	metal::beginReactive();
	Quad2D(20, 20, 60, 60, white, true);
	Quad2D(100, 20, 140, 60, half, false);
	Quad3DImmediate(5.0f, -2.0f, 4.0f, -3.0f, 10.0f, white);
	metal::endReactive();
	metal::setEmissiveScale(3.0f);
	Quad2D(560, 20, 600, 60, white, true);
	metal::setEmissiveScale(1.0f);
	Quad2D(560, 400, 600, 440, white, true);
	Quad3DImmediate(-4.0f, -2.0f, -5.0f, -3.0f, 10.0f, white);
	metal::endScene(nil);
	WorldEnd();
	float in = -1, partial = -1, im3 = -1, emit = -1, plain = -1, plain3 = -1, quad = -1;
	metal::FrameInfo fi = metal::getFrameInfo();
	float sx = (float)fi.internalWidth/fi.outputWidth, sy = (float)fi.internalHeight/fi.outputHeight;
	bool read = metal::readReactiveForTest((int)(40*sx), (int)(40*sy), &in) &&
		metal::readReactiveForTest((int)(120*sx), (int)(40*sy), &partial) &&
		metal::readReactiveForTest((int)(PX(4.5f, 10.0f)*sx), (int)(PY(-2.5f, 10.0f)*sy), &im3) &&
		metal::readReactiveForTest((int)(580*sx), (int)(40*sy), &emit) &&
		metal::readReactiveForTest((int)(580*sx), (int)(420*sy), &plain) &&
		metal::readReactiveForTest((int)(PX(-4.5f, 10.0f)*sx), (int)(PY(-2.5f, 10.0f)*sy), &plain3) &&
		metal::readReactiveForTest((int)(320*sx), (int)(240*sy), &quad);
	TiersOff();
	FreeScene(&s);
	bool ok = read && in > 0.99f && fabsf(partial - 128.0f/255.0f) < 0.01f && im3 > 0.99f && emit > 0.99f &&
		plain == 0.0f && plain3 == 0.0f && quad == 0.0f;
	if(!ok)
		Detail("  read %d; bracket %.3f half alpha %.3f im3D %.3f emitter %.3f plain %.3f plain im3D %.3f opaque %.3f\n", read,
			in, partial, im3, emit, plain, plain3, quad);
	return Report(ok, name);
}

static bool
CheckReactivePassReused(void)
{
	const char *name = "the reactive replay reuses one render pass descriptor from frame to frame";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	const void *seen[4];
	for(int f = 0; f < 4; f++){
		Frames(&s, 1);
		seen[f] = metal::reactivePassForTest();
	}
	TiersOff();
	FreeScene(&s);
	bool ok = seen[0] != nil;
	for(int f = 1; f < 4; f++)
		ok = ok && seen[f] == seen[0];
	if(!ok)
		Detail("  descriptors %p %p %p %p\n", seen[0], seen[1], seen[2], seen[3]);
	return Report(ok, name);
}

enum { PREPASS_NONE, PREPASS_EMPTY, PREPASS_BLENDED, PREPASS_ALL };

static void
HistoryFrames(Scene3D *s, const int *frames, int n, metal::FrameInfo *out)
{
	for(int i = 0; i < n; i++){
		metal::beginScene(cam);
		WorldBegin(SKY);
		if(frames[i] != PREPASS_NONE && metal::beginPrepass()){
			if(frames[i] == PREPASS_ALL)
				DrawAll(s);
			else if(frames[i] == PREPASS_BLENDED){
				SetRenderState(VERTEXALPHA, 1);
				s->blended->render();
				SetRenderState(VERTEXALPHA, 0);
			}
			metal::endPrepass();
		}
		DrawAll(s);
		metal::endScene(nil);
		WorldEnd();
		out[i] = metal::getFrameInfo();
	}
}

static bool
CheckPrepassValid(const char *name, int32 upscale)
{
	static const int frames[] = {
		PREPASS_ALL, PREPASS_ALL, PREPASS_NONE, PREPASS_ALL, PREPASS_ALL, PREPASS_EMPTY, PREPASS_ALL, PREPASS_ALL,
		PREPASS_BLENDED, PREPASS_ALL, PREPASS_ALL
	};
	static const int valid[] = { 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1 };
	static const int reset[] = { 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0 };
	enum { N = sizeof(frames)/sizeof(frames[0]) };
	Scene3D s;
	MakeScene(&s);
	ModernOn(upscale, metal::ANTIALIAS_TAA);
	SceneFrame(&s, false);
	metal::FrameInfo fi[N];
	HistoryFrames(&s, frames, N, fi);
	TiersOff();
	FreeScene(&s);
	bool ok = true;
	for(int i = 0; i < N; i++)
		ok = ok && (fi[i].prepassValid != 0) == (valid[i] != 0) && (fi[i].temporalReset != 0) == (reset[i] != 0);
	if(!ok)
		for(int i = 0; i < N; i++)
			Detail("  frame %d: prepass valid %d (want %d) reset %d (want %d)\n", i, fi[i].prepassValid, valid[i],
				fi[i].temporalReset, reset[i]);
	return Report(ok, name);
}

static bool
CheckTemporalResets(void)
{
	const char *name = "the temporal history resets on a camera cut, a setting change, a new factor and a tier change, and only then";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	int got[10], want[10] = { 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 };
	int n = 0;
	for(int step = 0; step < 5; step++){
		if(step == 1)
			metal::cameraCut();
		else if(step == 2){
			metal::RenderSettings rs = metal::getRenderSettings();
			rs.softness = 2.0f;
			metal::setRenderSettings(&rs);
		}else if(step == 3)
			SetSettings(metal::TIER_MODERN, metal::UPSCALE_BALANCED, metal::ANTIALIAS_TAA);
		else if(step == 4){
			EnhancedOn();
			Frames(&s, 1);
			ModernOn(metal::UPSCALE_BALANCED, metal::ANTIALIAS_TAA);
		}
		for(int f = 0; f < 2; f++){
			Frames(&s, 1);
			got[n++] = metal::getFrameInfo().temporalReset != 0;
		}
	}
	metal::RenderSettings rs = metal::getRenderSettings();
	rs.softness = SOFTNESS_DEFAULT;
	metal::setRenderSettings(&rs);
	TiersOff();
	FreeScene(&s);
	bool ok = memcmp(got, want, sizeof(got)) == 0;
	if(!ok)
		for(int i = 0; i < 10; i++)
			Detail("  frame %d reset %d want %d\n", i, got[i], want[i]);
	return Report(ok, name);
}

static uint8 slab[4096] __attribute__((aligned(16)));
static bool slabArmed, slabUsed;
static MemoryFunctions savedMemfuncs;

static void*
SlabMalloc(size_t sz, uint32 hint)
{
	if(slabArmed && hint == (MEMDUR_EVENT | ID_ATOMIC) && sz <= sizeof(slab)){
		slabArmed = false;
		slabUsed = true;
		return slab;
	}
	return savedMemfuncs.rwmalloc(sz, hint);
}

static void
SlabFree(void *p)
{
	if(p != slab)
		savedMemfuncs.rwfree(p);
}

static bool
CheckReusedAddress(void)
{
	const char *name = "an object created at a destroyed object's address starts without the old object's motion history";
	RGBA red = { 220, 40, 30, 255 }, col[4] = { red, red, red, red };
	Material *mat = Material::create();
	Geometry *g = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::PRELIT | Geometry::NORMALS,
		2.0f, 1.5f, -1.0f, -1.0f, 10.0f, col, nil, mat);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::cameraCut();
	savedMemfuncs = Engine::memfuncs;
	Engine::memfuncs.rwmalloc = SlabMalloc;
	Engine::memfuncs.rwfree = SlabFree;
	slabArmed = true;
	Atomic *a = MakeAtomic(g);
	void *first = a;
	DrawOne(a);
	DrawOne(a);
	DestroyAtomic(a);
	slabArmed = true;
	Atomic *b = MakeAtomic(g);
	void *second = b;
	slabArmed = false;
	V3d up = { 0.0f, 1.0f, 0.0f };
	b->getFrame()->translate(&up, COMBINEPOSTCONCAT);
	b->getFrame()->updateObjects();
	DrawOne(b);
	float m[2] = { 1.0f, 1.0f };
	bool read = PrepassMotionAt(PX(0.5f, 10.0f), PY(1.25f, 10.0f), m);
	DestroyAtomic(b);
	Engine::memfuncs = savedMemfuncs;
	TiersOff();
	g->destroy();
	mat->destroy();
	bool ok = slabUsed && first == second && read && fabsf(m[0]) < 1e-4f && fabsf(m[1]) < 1e-4f;
	if(!ok)
		Detail("  addresses %p %p read %d motion %.5f,%.5f, expected none\n", first, second, read, m[0], m[1]);
	return Report(ok, name);
}

static bool
Jittered(const metal::FrameInfo &f)
{
	return f.jitterX != 0.0f || f.jitterY != 0.0f;
}

static void
SteadyFrames(Scene3D *s)
{
	metal::cameraCut();
	Frames(s, 4);
}

static void
Im2DSceneFrame(Scene3D *s, float x1, float y0, const float *hud)
{
	RGBA white = { 255, 255, 255, 255 };
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		DrawOpaque(s);
		metal::endPrepass();
	}
	DrawAll(s);
	Quad2D(30.0f, y0, x1, 150.0f, white, false);
	metal::endScene(nil);
	if(hud)
		Quad2D(hud[0], hud[1], hud[2], hud[3], white, false);
	WorldEnd();
}

static bool
HudInPlace(const uint8 *px, int w, const float *hud, int *bad)
{
	int x0 = (int)hud[0], y0 = (int)hud[1], x1 = (int)hud[2], y1 = (int)hud[3], my = (y0 + y1)/2, mx = (x0 + x1)/2;
	const int in[4][2] = { { x0, my }, { x1, my }, { mx, y0 }, { mx, y1 } };
	const int out[4][2] = { { x0 - 1, my }, { x1 + 1, my }, { mx, y0 - 1 }, { mx, y1 + 1 } };
	for(int i = 0; i < 4; i++){
		if(px[(in[i][1]*w + in[i][0])*4] < 250){
			*bad = i;
			return false;
		}
		if(px[(out[i][1]*w + out[i][0])*4] > 200){
			*bad = 4 + i;
			return false;
		}
	}
	return true;
}

static bool
CheckTemporalIm2D(void)
{
	const char *name = "an im2D edge in the scene converges through the temporal scaler like a 3D edge, and im2D after the scene stays in place (im2D jitter)";
	static const float hud[4] = { 500.25f, 300.25f, 600.75f, 400.75f };
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	Frames(&s, 2);
	metal::FrameInfo fi = metal::getFrameInfo();
	float sx = (float)fi.outputWidth/fi.internalWidth, sy = (float)fi.outputHeight/fi.internalHeight;
	float wantX = (150.0f + 0.85f)*sx, wantY = (40.0f + 0.85f)*sy;
	int rowY = 105, rowX0 = (int)wantX - RUN/2, colX = 90, colY0 = (int)wantY - RUN/2;
	EdgeProbe native, temporal, last;
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	for(int f = 0; f < 2; f++)
		Im2DSceneFrame(&s, wantX, wantY, nil);
	ProbeEdges(&native, rowY, rowX0, colX, colY0);
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	float drift = 0.0f, meanX = 0.0f, meanY = 0.0f;
	int jittered = 0, hudBad = -1, hudFrame = -1;
	bool hudOk = true;
	for(int f = 0; f < 34; f++){
		bool withHud = f >= 25;
		Im2DSceneFrame(&s, wantX, wantY, withHud ? hud : nil);
		ProbeEdges(&temporal, rowY, rowX0, colX, colY0);
		if(f >= 26){
			drift = std::max(drift, std::max(fabsf(temporal.x - last.x), fabsf(temporal.y - last.y)));
			meanX += temporal.x/8.0f;
			meanY += temporal.y/8.0f;
		}
		last = temporal;
		if(withHud){
			jittered += Jittered(metal::getFrameInfo());
			uint8 *px = CompositeNow();
			if(px == nil || !HudInPlace(px, cam->frameBuffer->width, hud, &hudBad)){
				if(hudOk)
					hudFrame = f;
				hudOk = false;
			}
			delete[] px;
		}
	}
	TiersOff();
	FreeScene(&s);
	float dx = fabsf(meanX - wantX), dy = fabsf(meanY - wantY);
	bool flat = true;
	for(int i = 0; i < RUN; i++)
		if(i < RUN/2 - 3 || i > RUN/2 + 2)
			flat = flat && fabsf(temporal.row[i] - native.row[i]) < 0.03f && fabsf(temporal.col[i] - native.col[i]) < 0.03f;
	bool ok = dx < 0.12f && dy < 0.12f && drift < 1.0f && flat && jittered == 9 && hudOk;
	Detail("  im2D edges at x %.3f y %.3f: native x %.3f y %.3f, mean over the last 8 frames x %.3f y %.3f, drift %.3f; "
		"%d of 9 frames jittered\n", wantX, wantY, native.x, native.y, meanX, meanY, drift, jittered);
	if(!ok && (dx >= 0.12f || dy >= 0.12f || drift >= 1.0f))
		Detail("  the 0.12 px and 1.0 drift limits depend on Apple's MetalFX; an OS update may have shifted its behaviour "
			"(measured errors x %.3f y %.3f, drift %.3f)\n", dx, dy, drift);
	if(!hudOk)
		Detail("  im2D after the scene displaced on frame %d (probe %d: 0-3 inside, 4-7 outside the quad)\n", hudFrame, hudBad);
	if(!flat)
		for(int i = 0; i < RUN; i++)
			Detail("  %d: row native %.3f temporal %.3f; column native %.3f temporal %.3f\n", i, native.row[i],
				temporal.row[i], native.col[i], temporal.col[i]);
	return Report(ok, name);
}

static bool
Covered(int x, int y, float threshold)
{
	float px[4] = {};
	return metal::readScenePixel(x, y, px) && px[0] > threshold;
}

static bool
CheckIm2DJitterCoverage(void)
{
	const char *name = "an im2D edge and a 3D edge at the same sub-pixel position cover the same scene pixels in every jitter phase";
	RGBA white = { 255, 255, 255, 255 };
	Scene3D s;
	MakeScene(&s);
	float cy = 0.04f;
	PlaceCamera(0.0f, 0.0f, cy, 0.0f);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	SteadyFrames(&s);
	metal::FrameInfo fi = metal::getFrameInfo();
	float ix = (float)fi.internalWidth/fi.outputWidth, iy = (float)fi.internalHeight/fi.outputHeight;
	float e3x = 352.0f*ix, e3y = 240.0f*(1.0f - (1.5f - cy)/7.5f)*iy;
	int p3x = (int)e3x, p3y = (int)e3y, row3 = (int)(PY(0.25f - cy, 10.0f)*iy), col3 = (int)(PX(0.5f, 10.0f)*ix);
	float e2x = 150.0f + (e3x - p3x), e2y = 40.0f + (e3y - p3y);
	int row2 = (int)(105*iy), col2 = (int)(90*ix);
	int jittered = 0, xOff = 0, yOn = 0, bad = -1;
	bool c[4] = {}, at[4] = {};
	for(int f = 0; f < 8; f++){
		metal::beginScene(cam);
		WorldBegin(SKY);
		if(metal::beginPrepass()){
			DrawOpaque(&s);
			metal::endPrepass();
		}
		DrawAll(&s);
		Quad2D(30.0f, e2y/iy, e2x/ix, 150.0f, white, false);
		c[0] = Covered(p3x, row3, 0.5f);
		c[1] = Covered(150, row2, 0.6f);
		c[2] = Covered(col3, p3y, 0.5f);
		c[3] = Covered(col2, 40, 0.6f);
		metal::endScene(nil);
		WorldEnd();
		jittered += Jittered(metal::getFrameInfo());
		xOff += !c[0];
		yOn += c[2];
		if(bad < 0 && (c[0] != c[1] || c[2] != c[3])){
			bad = f;
			memcpy(at, c, sizeof(at));
		}
	}
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	FreeScene(&s);
	bool ok = bad < 0 && jittered == 8 && xOff > 0 && yOn > 0;
	if(!ok)
		Detail("  %d of 8 frames jittered; 3D right edge uncovered on %d, top edge covered on %d; first disagreement on frame %d "
			"(3D x %d im2D x %d, 3D y %d im2D y %d); edges 3D %.3f,%.3f im2D %.3f,%.3f\n", jittered, xOff, yOn, bad, at[0], at[1],
			at[2], at[3], e3x, e3y, e2x, e2y);
	return Report(ok, name);
}

static bool
Temporal(const metal::FrameInfo &f)
{
	return f.upscaler == metal::UPSCALER_TEMPORAL || f.antiAliasing == metal::ANTIALIAS_TAA;
}

static bool
CheckJitterStarts(const char *name, int32 upscale)
{
	static const int all[8] = { PREPASS_ALL, PREPASS_ALL, PREPASS_ALL, PREPASS_ALL, PREPASS_ALL, PREPASS_ALL, PREPASS_ALL, PREPASS_ALL };
	Scene3D s;
	MakeScene(&s);
	metal::FrameInfo made[8], cut[4];
	TiersOff();
	Frames(&s, 2);
	ModernOn(upscale, metal::ANTIALIAS_TAA);
	HistoryFrames(&s, all, 8, made);
	metal::cameraCut();
	HistoryFrames(&s, all, 4, cut);
	TiersOff();
	FreeScene(&s);
	int first = 0;
	while(first < 4 && !Temporal(made[first]))
		first++;
	bool ok = first < 4;
	const metal::FrameInfo *seq[2] = { made + first, cut };
	for(int k = 0; k < 2 && ok; k++){
		const metal::FrameInfo *f = seq[k];
		ok = f[0].temporalReset && !Jittered(f[0]) && !f[1].temporalReset && !Jittered(f[1]) && !f[2].temporalReset &&
			Jittered(f[2]) && Jittered(f[3]);
	}
	if(!ok){
		Detail("  first temporal frame %d\n", first);
		for(int i = 0; i < 4 && first + i < 8; i++)
			Detail("  frame %d: made reset %d jitter %g,%g; after a cut reset %d jitter %g,%g\n", i, made[first+i].temporalReset,
				made[first+i].jitterX, made[first+i].jitterY, cut[i].temporalReset, cut[i].jitterX, cut[i].jitterY);
	}
	return Report(ok, name);
}

static bool
CheckJitterNeedsHistory(const char *name, bool alternate)
{
	enum { N = 10 };
	int frames[N];
	for(int i = 0; i < N; i++)
		frames[i] = alternate && i % 2 == 0 ? PREPASS_ALL : PREPASS_NONE;
	Scene3D s;
	MakeScene(&s);
	metal::FrameInfo fresh[N], cut[N];
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	HistoryFrames(&s, frames, N, fresh);
	SteadyFrames(&s);
	metal::cameraCut();
	HistoryFrames(&s, frames, N, cut);
	TiersOff();
	FreeScene(&s);
	bool ok = cut[0].upscaler == metal::UPSCALER_TEMPORAL;
	for(int i = 0; i < N; i++)
		ok = ok && !Jittered(fresh[i]) && !Jittered(cut[i]);
	if(!ok)
		for(int i = 0; i < N; i++)
			Detail("  frame %d: upscaler %d jitter %g,%g; after a cut %g,%g\n", i, cut[i].upscaler, fresh[i].jitterX, fresh[i].jitterY,
				cut[i].jitterX, cut[i].jitterY);
	return Report(ok, name);
}

static bool
CheckJitterCutInFrame(void)
{
	const char *name = "a camera cut after beginScene costs one jittered reset frame, then the jitter restarts on the third frame";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	SteadyFrames(&s);
	metal::FrameInfo f[3];
	metal::beginScene(cam);
	metal::cameraCut();
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		DrawAll(&s);
		metal::endPrepass();
	}
	DrawAll(&s);
	metal::endScene(nil);
	WorldEnd();
	f[0] = metal::getFrameInfo();
	for(int i = 1; i < 3; i++){
		Frames(&s, 1);
		f[i] = metal::getFrameInfo();
	}
	TiersOff();
	FreeScene(&s);
	bool ok = f[0].temporalReset && Jittered(f[0]) && !f[1].temporalReset && !Jittered(f[1]) && Jittered(f[2]);
	if(!ok)
		for(int i = 0; i < 3; i++)
			Detail("  frame %d: reset %d jitter %g,%g\n", i, f[i].temporalReset, f[i].jitterX, f[i].jitterY);
	return Report(ok, name);
}

static bool
CheckJitterNotApplied(void)
{
	const char *name = "a scene camera updated before beginScene is not jittered and the temporal scaler is told no jitter";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	SteadyFrames(&s);
	metal::FrameInfo before = metal::getFrameInfo();
	WorldBegin(SKY);
	metal::beginScene(cam);
	if(metal::beginPrepass()){
		DrawAll(&s);
		metal::endPrepass();
	}
	DrawAll(&s);
	float proj[16];
	metal::sceneProjectionForTest(proj);
	metal::endScene(nil);
	WorldEnd();
	metal::FrameInfo fi = metal::getFrameInfo();
	TiersOff();
	FreeScene(&s);
	bool ok = Jittered(before) && proj[8] == 0.0f && proj[9] == 0.0f && !Jittered(fi);
	if(!ok)
		Detail("  steady jitter %g,%g; then proj[8] %g proj[9] %g, scaler jitter %g,%g\n", before.jitterX, before.jitterY, proj[8],
			proj[9], fi.jitterX, fi.jitterY);
	return Report(ok, name);
}

static bool
CheckIm2DJitterNotApplied(void)
{
	const char *name = "with the scene camera updated before beginScene, im2D drawn in the scene is not jittered either";
	RGBA white = { 255, 255, 255, 255 };
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	SteadyFrames(&s);
	bool steady = Jittered(metal::getFrameInfo());
	int w = cam->frameBuffer->width, moved = 0, first = -1;
	for(int f = 0; f < 8; f++){
		WorldBegin(SKY);
		metal::beginScene(cam);
		if(metal::beginPrepass()){
			DrawOpaque(&s);
			metal::endPrepass();
		}
		DrawAll(&s);
		Quad2D(30.0f, 40.15f, 150.85f, 150.0f, white, false);
		uint8 *px = CompositeNow();
		bool x = px && px[(100*w + 150)*4 + 1] > 200, y = px && px[(40*w + 90)*4 + 1] > 200;
		delete[] px;
		metal::endScene(nil);
		WorldEnd();
		if(!x || !y){
			moved++;
			if(first < 0)
				first = f;
		}
	}
	TiersOff();
	FreeScene(&s);
	bool ok = steady && moved == 0;
	if(!ok)
		Detail("  steady frames jittered %d; im2D edge moved off its unjittered pixels on %d of 8 frames, first %d\n", steady,
			moved, first);
	return Report(ok, name);
}

static bool
CheckJitterSubRaster(void)
{
	const char *name = "a scene camera on a sub-raster is jittered by the same pixels, scaled to its viewport";
	Scene3D s;
	MakeScene(&s);
	Raster *parent = cam->frameBuffer;
	Raster *sub = Raster::create(0, 0, 0, Raster::CAMERA | Raster::DONTALLOCATE);
	Rect r = { 200, 100, 320, 160 };
	sub->subRaster(parent, &r);
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	SteadyFrames(&s);
	float proj[16];
	metal::beginScene(cam);
	WorldBegin(SKY);
	cam->endUpdate();
	cam->frameBuffer = sub;
	cam->beginUpdate();
	metal::sceneProjectionForTest(proj);
	cam->endUpdate();
	cam->frameBuffer = parent;
	cam->beginUpdate();
	if(metal::beginPrepass()){
		DrawAll(&s);
		metal::endPrepass();
	}
	DrawAll(&s);
	metal::endScene(nil);
	WorldEnd();
	metal::FrameInfo fi = metal::getFrameInfo();
	TiersOff();
	FreeScene(&s);
	sub->destroy();
	float w = r.w*(float)fi.internalWidth/fi.outputWidth, h = r.h*(float)fi.internalHeight/fi.outputHeight;
	float wantX = 2.0f*fi.jitterX/w, wantY = -2.0f*fi.jitterY/h;
	bool ok = Jittered(fi) && fabsf(proj[8] - wantX) <= 0.01f*fabsf(wantX) && fabsf(proj[9] - wantY) <= 0.01f*fabsf(wantY);
	if(!ok)
		Detail("  jitter %g,%g; sub-raster proj[8] %g proj[9] %g, expected %g %g\n", fi.jitterX, fi.jitterY, proj[8], proj[9],
			wantX, wantY);
	return Report(ok, name);
}

static bool
CheckTemporalLogs(void)
{
	const char *name = "taa resolving to the spatial scaler logs one line: once without a depth raster, once without prepass targets";
	Scene3D s;
	MakeScene(&s);
	Raster *z = cam->zBuffer;
	metal::setTemporalFailForTest(false);
	uint32 start = metal::temporalLogsForTest();
	cam->zBuffer = nil;
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	Frames(&s, 2);
	int32 noDepth = metal::getFrameInfo().upscaler;
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_BALANCED, metal::ANTIALIAS_TAA);
	Frames(&s, 2);
	cam->zBuffer = z;
	TiersOff();
	Frames(&s, 1);
	uint32 depthLogs = metal::temporalLogsForTest() - start;
	metal::setPrepassTargetsFailForTest(true);
	start = metal::temporalLogsForTest();
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	Frames(&s, 2);
	int32 noTargets = metal::getFrameInfo().upscaler;
	uint32 targetLogs = metal::temporalLogsForTest() - start;
	metal::setPrepassTargetsFailForTest(false);
	TiersOff();
	Frames(&s, 1);
	FreeScene(&s);
	bool ok = noDepth == metal::UPSCALER_SPATIAL && noTargets == metal::UPSCALER_SPATIAL && depthLogs == 1 && targetLogs == 1;
	if(!ok)
		Detail("  no depth: upscaler %d, %u lines; no prepass targets: upscaler %d, %u lines\n", noDepth, depthLogs, noTargets,
			targetLogs);
	return Report(ok, name);
}

static int
PartialPixels(const float *plain, const float *v)
{
	float lo = fminf(plain[0], plain[RUN-1]), hi = fmaxf(plain[0], plain[RUN-1]);
	int n = 0;
	for(int i = 0; i < RUN; i++)
		if(v[i] > lo + 0.05f*(hi - lo) && v[i] < hi - 0.05f*(hi - lo))
			n++;
	return n;
}

static bool
SameRuns(const EdgeProbe &a, const EdgeProbe &b, float tol, bool awayOnly)
{
	for(int i = 0; i < RUN; i++){
		if(awayOnly && i >= RUN/2 - 3 && i <= RUN/2 + 2)
			continue;
		if(fabsf(a.row[i] - b.row[i]) >= tol || fabsf(a.col[i] - b.col[i]) >= tol)
			return false;
	}
	return true;
}

static float
AreaEdge(const float *v, const float *plain, int first)
{
	float sum = 0.0f;
	for(int i = 0; i < RUN; i++)
		sum += (v[i] - plain[RUN-1])/(plain[0] - plain[RUN-1]);
	return first + sum;
}

enum { TENT_X0 = 40, TENT_Y = 420 };

static void
RampQuad(float x0, float x1, RGBA a, RGBA b)
{
	metal::Im2DVertex v[4];
	Vertex2D(&v[0], x0, 400.0f, a);
	Vertex2D(&v[1], x0, 440.0f, a);
	Vertex2D(&v[2], x1, 440.0f, b);
	Vertex2D(&v[3], x1, 400.0f, b);
	BlendMode(false);
	im2d::RenderIndexedPrimitive(PRIMTYPETRILIST, v, 4, quadIndex, 6);
	OpaqueMode();
}

static void
TentFrame(Scene3D *s)
{
	RGBA white = { 255, 255, 255, 255 };
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		DrawAll(s);
		metal::endPrepass();
	}
	DrawAll(s);
	metal::beginReactive();
	RampQuad(TENT_X0, TENT_X0 + 16, SKY, white);
	RampQuad(TENT_X0 + 16, TENT_X0 + 32, white, SKY);
	metal::endReactive();
	metal::endScene(nil);
	WorldEnd();
}

static float
TentCentre(void)
{
	float sum = 0.0f, moment = 0.0f, sky = SKY.red/255.0f;
	for(int x = TENT_X0 - 4; x < TENT_X0 + 36; x++){
		float px[4] = {};
		metal::readFrameOutput(x, TENT_Y, px);
		sum += px[0] - sky;
		moment += (px[0] - sky)*(x + 0.5f);
	}
	return sum > 0.0f ? moment/sum : -100.0f;
}

static bool
CheckTaaStill(void)
{
	const char *name = "TAA at native converges a still edge on its true sub-pixel position, holds it, keeps a reactive ramp in place, leaves flat colour alone and drops its history on a cut";
	Scene3D s;
	MakeScene(&s);
	float cx = 0.0125f, cy = 0.04f;
	float wantX = 320.0f*(1.0f + (1.0f + cx)/10.0f), wantY = 240.0f*(1.0f - (1.5f - cy)/7.5f);
	int rowY = PY(0.25f - cy, 10.0f), rowX0 = (int)wantX - RUN/2, colX = PX(0.5f, 10.0f), colY0 = (int)wantY - RUN/2;
	EdgeProbe plain, taa, last, cut;
	PlaceCamera(0.0f, cx, cy, 0.0f);
	metal::cameraCut();
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	TentFrame(&s);
	TentFrame(&s);
	ProbeEdges(&plain, rowY, rowX0, colX, colY0);
	float tentPlain = TentCentre(), tentLast = 0.0f, tentDrift = 0.0f, tentErr = 0.0f;
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	metal::setJitterIndexForTest(0);
	float drift = 0.0f, meanX = 0.0f, meanY = 0.0f;
	plain.x = AreaEdge(plain.row, plain.row, rowX0);
	plain.y = AreaEdge(plain.col, plain.col, colY0);
	for(int f = 0; f < 32; f++){
		TentFrame(&s);
		ProbeEdges(&taa, rowY, rowX0, colX, colY0);
		float tent = TentCentre();
		if(f >= 24){
			tentDrift = std::max(tentDrift, fabsf(tent - tentLast));
			tentErr = std::max(tentErr, fabsf(tent - tentPlain));
		}
		tentLast = tent;
		taa.x = AreaEdge(taa.row, plain.row, rowX0);
		taa.y = AreaEdge(taa.col, plain.col, colY0);
		if(f >= 24){
			drift = std::max(drift, std::max(fabsf(taa.x - last.x), fabsf(taa.y - last.y)));
			meanX += taa.x/8.0f;
			meanY += taa.y/8.0f;
		}
		last = taa;
	}
	metal::FrameInfo fi = metal::getFrameInfo();
	metal::logStats();
	bool stats = strstr(metal::getStatsLine(), " aa taa ") != nil;
	metal::cameraCut();
	TentFrame(&s);
	ProbeEdges(&cut, rowY, rowX0, colX, colY0);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	FreeScene(&s);
	float dx = fabsf(meanX - wantX), dy = fabsf(meanY - wantY), px = fabsf(plain.x - wantX), py = fabsf(plain.y - wantY);
	int partialX = PartialPixels(plain.row, taa.row), partialY = PartialPixels(plain.col, taa.col);
	bool flat = SameRuns(taa, plain, 0.01f, true), cutPlain = SameRuns(cut, plain, 0.01f, false);
	bool ok = fi.antiAliasing == metal::ANTIALIAS_TAA && stats && dx < 0.05f && dy < 0.05f && dx < px && dy < py &&
		drift < 0.12f && flat && partialX >= 1 && partialX <= 3 && partialY >= 1 && partialY <= 3 && cutPlain &&
		tentDrift < 0.05f && tentErr < 0.05f;
	Detail("  edges at x %.3f y %.3f: one native frame x %.3f y %.3f, TAA mean over the last 8 frames x %.3f y %.3f, drift %.3f, "
		"partial pixels %d and %d\n", wantX, wantY, plain.x, plain.y, meanX, meanY, drift, partialX, partialY);
	Detail("  reactive ramp centre %.3f in the native frame; over the last 8 TAA frames it moves up to %.3f per frame and "
		"strays up to %.3f\n", tentPlain, tentDrift, tentErr);
	if(!ok){
		Detail("  aa %d stats %d flat %d cut frame equals the plain frame %d\n", fi.antiAliasing, stats, flat, cutPlain);
		for(int i = 0; i < RUN; i++)
			Detail("  %d: row plain %.3f taa %.3f cut %.3f; column plain %.3f taa %.3f cut %.3f\n", i, plain.row[i], taa.row[i],
				cut.row[i], plain.col[i], taa.col[i], cut.col[i]);
	}
	return Report(ok, name);
}

static bool
NearPixel(const float *a, const float *b, float tol)
{
	return fabsf(a[0] - b[0]) < tol && fabsf(a[1] - b[1]) < tol && fabsf(a[2] - b[2]) < tol;
}

static const char *nanTaaSrc =
"fragment float4\n"
"nanTaaFS(FragmentIn in [[stage_in]])\n"
"{\n"
"	return float4(sqrt(-1.0 - in.color.r), 0.5, 0.5, 1.0);\n"
"}\n";

static bool
CheckTaaNanHistory(void)
{
	const char *name = "coverage: the TAA history after an invalid scene pixel stays finite in that frame and in the frames after it";
	enum { FRAMES = 5 };
	const char *src[] = { metal::header_metal_src, metal::im2d_metal_src, nanTaaSrc, nil };
	RGBA white = { 255, 255, 255, 255 };
	Scene3D s;
	MakeScene(&s);
	metal::Shader *sh = metal::Shader::create(src, "im2dVS", "nanTaaFS", 0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	bool warm = sh && metal::prewarmIm2DShader(sh, 0, 0, BLENDONE, BLENDZERO, 1);
	SteadyFrames(&s);
	metal::StateStats before = metal::getStateStats();
	float scene[4] = {}, o[FRAMES][4] = {};
	bool read = true, taa = true;
	for(int f = 0; f < FRAMES; f++){
		metal::beginScene(cam);
		WorldBegin(SKY);
		if(metal::beginPrepass()){
			DrawOpaque(&s);
			metal::endPrepass();
		}
		DrawAll(&s);
		if(f == 0){
			metal::Im2DVertex v[4];
			Vertex2D(&v[0], 96.0f, 96.0f, white);
			Vertex2D(&v[1], 96.0f, 104.0f, white);
			Vertex2D(&v[2], 104.0f, 104.0f, white);
			Vertex2D(&v[3], 104.0f, 96.0f, white);
			SetRenderStatePtr(TEXTURERASTER, nil);
			SetRenderState(ZTESTENABLE, 0);
			SetRenderState(ZWRITEENABLE, 0);
			SetRenderState(VERTEXALPHA, 0);
			metal::im2dOverrideShader = sh;
			metal::setDrawEmissiveScale(3.0f);
			im2d::RenderIndexedPrimitive(PRIMTYPETRILIST, v, 4, quadIndex, 6);
			metal::setDrawEmissiveScale(1.0f);
			metal::im2dOverrideShader = nil;
			OpaqueMode();
		}
		metal::endScene(nil);
		if(f == 0)
			read = metal::readScenePixel(100, 100, scene) && read;
		read = metal::readFrameOutput(100, 100, o[f]) && read;
		taa = taa && metal::getFrameInfo().antiAliasing == metal::ANTIALIAS_TAA;
		WorldEnd();
	}
	metal::StateStats after = metal::getStateStats();
	TiersOff();
	FreeScene(&s);
	if(sh)
		sh->destroy();
	bool ok = warm && read && taa && std::isnan(scene[0]);
	for(int f = 0; f < FRAMES; f++){
		ok = ok && std::isfinite(o[f][0]) && std::isfinite(o[f][1]) && std::isfinite(o[f][2]);
		Detail("  frame %d output %.4f,%.4f,%.4f\n", f, o[f][0], o[f][1], o[f][2]);
	}
	if(!warm || !read || !taa || !std::isnan(scene[0]))
		Detail("  shader %d warm %d read %d taa %d scene red %f\n", sh != nil, warm, read, taa, scene[0]);
	if(after.pipelinesLate != before.pipelinesLate){
		Detail("  late pipelines %u -> %u\n", before.pipelinesLate, after.pipelinesLate);
		ok = false;
	}
	return Report(ok, name);
}

static bool
CheckTaaMoving(void)
{
	const char *name = "TAA follows a moving quad: the pixels it leaves take the background and the pixels it enters take the quad, with no trail";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	SteadyFrames(&s);
	V3d step = { -0.25f, 0.0f, 0.0f };
	for(int f = 0; f < 6; f++){
		s.front->getFrame()->translate(&step, COMBINEPOSTCONCAT);
		s.front->getFrame()->updateObjects();
		Frames(&s, 1);
	}
	int y = PY(0.25f, 10.0f), xs[3] = { 300, 396, 352 };
	float taa[3][4] = {}, plain[3][4] = {};
	bool read = true;
	for(int i = 0; i < 3; i++)
		read = read && metal::readFrameOutput(xs[i], y, taa[i]);
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	Frames(&s, 2);
	for(int i = 0; i < 3; i++)
		read = read && metal::readFrameOutput(xs[i], y, plain[i]);
	TiersOff();
	FreeScene(&s);
	bool ok = read;
	for(int i = 0; i < 3; i++)
		ok = ok && NearPixel(taa[i], plain[i], 0.05f);
	if(!ok)
		for(int i = 0; i < 3; i++)
			Detail("  %s pixel %d: taa %.3f,%.3f,%.3f plain %.3f,%.3f,%.3f\n", i == 0 ? "vacated" : i == 1 ? "entered" : "inside",
				xs[i], taa[i][0], taa[i][1], taa[i][2], plain[i][0], plain[i][1], plain[i][2]);
	return Report(ok, name);
}

struct EdgeSpill
{
	float near, far;
};

static void
EdgeGhost(float edge, bool fgRight, int y, EdgeSpill *spill)
{
	int ep = (int)edge;
	float fg[4], bg[4];
	metal::readFrameOutput(ep + (fgRight ? 20 : -20), y, fg);
	metal::readFrameOutput(ep + (fgRight ? -20 : 20), y, bg);
	for(int k = 1; k <= 3; k++){
		float in[4], out[4];
		metal::readFrameOutput(ep + (fgRight ? k : -k), y, in);
		metal::readFrameOutput(ep + (fgRight ? -k : k), y, out);
		float range = fabsf(fg[0] - bg[0]);
		float spillIn = fabsf(in[0] - fg[0])/range, spillOut = fabsf(out[0] - bg[0])/range;
		if(k == 1)
			spill->near = fmaxf(spill->near, fmaxf(spillIn, spillOut));
		else
			spill->far = fmaxf(spill->far, fmaxf(spillIn, spillOut));
	}
}

static bool
CheckTaaMovingEdge(void)
{
	const char *name = "TAA leaves no ghost beside the edges of a quad moving over a static background under jitter (motion of the nearest surface)";
	enum { FRAMES = 24 };
	const float step = 1.3f;
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	SteadyFrames(&s);
	V3d move = { -step*10.0f/320.0f, 0.0f, 0.0f };
	EdgeSpill trail = { 0.0f, 0.0f }, lead = { 0.0f, 0.0f };
	int jittered = 0;
	for(int f = 0; f < FRAMES; f++){
		s.front->getFrame()->translate(&move, COMBINEPOSTCONCAT);
		s.front->getFrame()->updateObjects();
		Frames(&s, 1);
		if(f < FRAMES - 8)
			continue;
		jittered += Jittered(metal::getFrameInfo());
		EdgeGhost(256.0f + step*(f + 1), true, 200, &trail);
		EdgeGhost(352.0f + step*(f + 1), false, 200, &lead);
	}
	TiersOff();
	FreeScene(&s);
	bool ok = jittered >= 6 && trail.near < 0.5f && lead.near < 0.5f && trail.far < 0.02f && lead.far < 0.02f;
	Detail("  over the last 8 frames (%d jittered), the pixels beside the edge pixel take up to %.3f (trailing) and %.3f (leading) "
		"of the other surface, the next two up to %.3f and %.3f\n", jittered, trail.near, lead.near, trail.far, lead.far);
	return Report(ok, name);
}

static void
SplitQuads(int f, RGBA a, RGBA b)
{
	Quad2D(100, 100, 150, 200, f % 2 ? b : a, false);
	Quad2D(150, 100, 200, 200, f % 2 ? a : b, false);
}

static bool
CheckTaaReactive(void)
{
	const char *name = "TAA takes the current frame where the reactive mask is set, even where the history fits the neighbourhood";
	RGBA a = { 255, 0, 0, 255 }, b = { 0, 0, 255, 255 };
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	metal::cameraCut();
	float bracketed[4] = {}, plain[4] = {};
	int f = 0, last = -1;
	bool read = false, phased = true;
	for(int pass = 0; pass < 2; pass++){
		metal::FrameInfo fi;
		do{
			metal::beginScene(cam);
			WorldBegin(SKY);
			if(metal::beginPrepass()){
				DrawOpaque(&s);
				metal::endPrepass();
			}
			DrawAll(&s);
			if(pass == 0)
				metal::beginReactive();
			SplitQuads(f, a, b);
			if(pass == 0)
				metal::endReactive();
			metal::endScene(nil);
			read = metal::readFrameOutput(149, 150, pass == 0 ? bracketed : plain);
			WorldEnd();
			fi = metal::getFrameInfo();
			f++;
		}while(f < 10 + 10*pass || ((fi.jitterX != 0.0f || !Jittered(fi)) && f < 40 + 40*pass));
		phased = phased && fi.jitterX == 0.0f && Jittered(fi);
		if(pass == 0)
			last = f - 1;
	}
	TiersOff();
	FreeScene(&s);
	bool mixed = fabsf(plain[0] - plain[2]) < 0.9f;
	bool ok = read && phased && fabsf(bracketed[0] - (last % 2 ? 0.0f : 1.0f)) < 0.02f &&
		fabsf(bracketed[2] - (last % 2 ? 1.0f : 0.0f)) < 0.02f && mixed;
	if(!ok)
		Detail("  jittered with x 0 at the end of both runs %d; last frame %d; bracketed output %.3f,%.3f,%.3f; unbracketed output %.3f,%.3f,%.3f (expected a mix)\n", phased, last,
			bracketed[0], bracketed[1], bracketed[2], plain[0], plain[1], plain[2]);
	return Report(ok, name);
}

static bool
CheckTaaFailure(void)
{
	const char *name = "TAA history that cannot be created runs FXAA with one log line, and TAA in the modern tier logs no fallback";
	Scene3D s;
	MakeScene(&s);
	uint32 allocs0, logs0, requests0, allocs, logs, requests;
	metal::taaCountsForTest(&allocs0, &logs0, &requests0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	metal::setTaaFailForTest(1);
	Frames(&s, 3);
	metal::FrameInfo fi = metal::getFrameInfo();
	metal::taaCountsForTest(&allocs, &logs, &requests);
	metal::setTaaFailForTest(0);
	TiersOff();
	FreeScene(&s);
	allocs -= allocs0;
	logs -= logs0;
	requests -= requests0;
	bool ok = fi.antiAliasing == metal::ANTIALIAS_FXAA && allocs == 1 && logs == 1 && requests == 0;
	if(!ok)
		Detail("  aa %d allocations %u logs %u fallback lines %u\n", fi.antiAliasing, allocs, logs, requests);
	return Report(ok, name);
}

static bool
CheckTaaTextures(void)
{
	const char *name = "the TAA history is dropped when TAA stops running, and the anti-aliasing source when no method needs it";
	Scene3D s;
	MakeScene(&s);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	Frames(&s, 2);
	bool taaHist = metal::taaHistoryForTest(), taaSource = metal::antiAliasTextureForTest();
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_FXAA);
	Frames(&s, 2);
	bool fxaaHist = metal::taaHistoryForTest(), fxaaSource = metal::antiAliasTextureForTest();
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	Frames(&s, 2);
	bool offHist = metal::taaHistoryForTest(), offSource = metal::antiAliasTextureForTest();
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	Frames(&s, 2);
	bool backHist = metal::taaHistoryForTest();
	TiersOff();
	FreeScene(&s);
	bool ok = taaHist && taaSource && !fxaaHist && fxaaSource && !offHist && !offSource && backHist;
	if(!ok)
		Detail("  history/source: taa %d/%d fxaa %d/%d off %d/%d, taa again %d\n", taaHist, taaSource, fxaaHist, fxaaSource,
			offHist, offSource, backHist);
	return Report(ok, name);
}

static bool
CheckTaaResets(void)
{
	const char *name = "TAA at native drops its history on a camera cut, a setting change, a resize and a tier change, and only then";
	Scene3D s;
	MakeScene(&s);
	Raster *fb = cam->frameBuffer, *zb = cam->zBuffer;
	Raster *small = Raster::create(fb->width/2, fb->height/2, 0, Raster::CAMERA);
	Raster *smallZ = Raster::create(fb->width/2, fb->height/2, 0, Raster::ZBUFFER);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	int got[12], want[12] = { 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 };
	int aa[12];
	int n = 0;
	for(int step = 0; step < 6; step++){
		if(step == 1)
			metal::cameraCut();
		else if(step == 2){
			metal::RenderSettings rs = metal::getRenderSettings();
			rs.softness = 2.0f;
			metal::setRenderSettings(&rs);
		}else if(step == 3){
			cam->frameBuffer = small;
			cam->zBuffer = smallZ;
		}else if(step == 4){
			cam->frameBuffer = fb;
			cam->zBuffer = zb;
		}else if(step == 5){
			EnhancedOn();
			Frames(&s, 1);
			ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
		}
		for(int f = 0; f < 2; f++){
			Frames(&s, 1);
			metal::FrameInfo fi = metal::getFrameInfo();
			aa[n] = fi.antiAliasing;
			got[n++] = fi.temporalReset != 0;
		}
	}
	metal::RenderSettings rs = metal::getRenderSettings();
	rs.softness = SOFTNESS_DEFAULT;
	metal::setRenderSettings(&rs);
	TiersOff();
	small->destroy();
	smallZ->destroy();
	FreeScene(&s);
	bool ok = memcmp(got, want, sizeof(got)) == 0;
	for(int i = 0; i < 12; i++)
		ok = ok && aa[i] == metal::ANTIALIAS_TAA;
	if(!ok)
		for(int i = 0; i < 12; i++)
			Detail("  frame %d reset %d want %d aa %d\n", i, got[i], want[i], aa[i]);
	return Report(ok, name);
}

static float
ColumnsDiff(const uint8 *a, const uint8 *b, int w, int x0, int x1, int y0, int y1)
{
	float sum = 0.0f;
	for(int y = y0; y < y1; y++)
		for(int x = x0; x < x1; x++)
			sum += abs(a[(y*w + x)*4] - b[(y*w + x)*4])/255.0f;
	return sum/((x1 - x0)*(y1 - y0));
}

static bool
CheckTaaOffscreen(void)
{
	const char *name = "TAA drops history that reprojects off screen: the columns a pan brings in match the native frame";
	static const RGBA texels[4] = { { 255, 255, 255, 255 }, { 0, 0, 0, 255 }, { 0, 0, 0, 255 }, { 255, 255, 255, 255 } };
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 96.0f, 0.0f }, { 96.0f, 64.0f }, { 0.0f, 64.0f } };
	Texture *tex = MakeTexture(2, 2, texels, Texture::LINEAR, Texture::WRAP, Texture::WRAP);
	Material *mat = Material::create();
	mat->setTexture(tex);
	Geometry *g = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::PRELIT | Geometry::NORMALS,
		12.0f, 8.0f, -12.0f, -8.0f, 10.0f, col, uv, mat);
	Atomic *wall = MakeAtomic(g);
	int w = cam->frameBuffer->width;
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	for(int f = 0; f < 16; f++){
		PlaceCamera(0.0f, 0.1f*f, 0.0f, 0.0f);
		DrawOne(wall);
	}
	uint8 *taa = CompositeNow();
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DrawOne(wall);
	DrawOne(wall);
	uint8 *plain = CompositeNow();
	float edge = taa && plain ? ColumnsDiff(taa, plain, w, 0, 3, 160, 320) : 1.0f;
	float inside = taa && plain ? ColumnsDiff(taa, plain, w, 300, 303, 160, 320) : 1.0f;
	delete[] taa;
	delete[] plain;
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	DestroyAtomic(wall);
	g->destroy();
	mat->destroy();
	tex->destroy();
	bool ok = edge < 0.05f;
	Detail("  mean red difference from the native frame: the three columns brought in %.4f, three columns inside %.4f\n", edge,
		inside);
	return Report(ok, name);
}

static void
HighlightFrame(Atomic *back, Atomic *front)
{
	RGBA warm = { 255, 200, 120, 255 }, night = { 8, 8, 12, 255 };
	metal::beginScene(cam);
	WorldBegin(night);
	SetRenderState(VERTEXALPHA, 0);
	if(metal::beginPrepass()){
		back->render();
		front->render();
		metal::endPrepass();
	}
	back->render();
	front->render();
	metal::setEmissiveScale(8.0f);
	Quad2D(100, 300, 160, 360, warm, true);
	metal::setEmissiveScale(1.0f);
	metal::endScene(nil);
	WorldEnd();
}

struct HighlightRun
{
	float v[RUN][4];
};

static void
ReadRun(HighlightRun *r, int x0, int y)
{
	for(int i = 0; i < RUN; i++)
		metal::readFrameOutput(x0 + i, y, r->v[i]);
}

static float
Luma(const float *c)
{
	return 0.299f*c[0] + 0.587f*c[1] + 0.114f*c[2];
}

static bool
NoFireflyNoHalo(const HighlightRun &taa, const HighlightRun &plain, int edgeLo, int edgeHi, const char *what)
{
	float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
	for(int i = 0; i < RUN; i++)
		for(int c = 0; c < 3; c++){
			lo[c] = fminf(lo[c], plain.v[i][c]);
			hi[c] = fmaxf(hi[c], plain.v[i][c]);
		}
	int bad = -1;
	const char *why = "";
	for(int i = 0; i < RUN && bad < 0; i++){
		for(int c = 0; c < 3; c++)
			if(taa.v[i][c] > hi[c]*1.01f + 0.005f || taa.v[i][c] < lo[c] - 0.005f){
				bad = i;
				why = taa.v[i][c] > hi[c] ? "above the brightest" : "below the darkest";
			}
		if(bad < 0 && i > 0 && Luma(taa.v[i]) > Luma(taa.v[i-1]) + 0.01f){
			bad = i;
			why = "luma rises away from the highlight";
		}
		if(bad < 0 && (i < edgeLo || i > edgeHi))
			for(int c = 0; c < 3; c++)
				if(fabsf(taa.v[i][c] - plain.v[i][c]) > 0.01f + 0.01f*plain.v[i][c]){
					bad = i;
					why = "away from the edge, differs from the plain frame";
				}
	}
	float top = -1e9f, bottom = 1e9f;
	for(int i = 0; i < RUN; i++){
		top = fmaxf(top, taa.v[i][0]);
		bottom = fminf(bottom, Luma(taa.v[i]));
	}
	Detail("  %s: plain red %.3f to %.3f, resolved red up to %.3f, resolved luma down to %.3f (plain %.3f), beside the edge red %.3f "
		"(plain %.3f)\n", what, lo[0], hi[0], top, bottom, Luma(plain.v[RUN-1]), taa.v[edgeHi+1][0], plain.v[edgeHi+1][0]);
	if(bad >= 0)
		for(int i = 0; i < RUN; i++)
			Detail("  %s %d%s: taa %.3f,%.3f,%.3f plain %.3f,%.3f,%.3f%s%s\n", what, i, i == bad ? " (bad)" : "", taa.v[i][0],
				taa.v[i][1], taa.v[i][2], plain.v[i][0], plain.v[i][1], plain.v[i][2], i == bad ? ", " : "", i == bad ? why : "");
	return bad < 0;
}

static bool
CheckTaaHighlight(void)
{
	const char *name = "TAA leaves no firefly and no dark halo where an emitter above white or a white surface meets a dark one, still and under a pan";
	RGBA dark = { 12, 12, 16, 255 }, white = { 255, 255, 255, 255 };
	RGBA dc[4] = { dark, dark, dark, dark }, wc[4] = { white, white, white, white };
	Material *mat = Material::create();
	Geometry *bg = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 8.0f, 6.0f, -8.0f, -6.0f, 20.0f, dc, nil, mat);
	Geometry *fg = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 1.0f, 1.0f, -1.0f, -1.0f, 10.0f, wc, nil, mat);
	Atomic *back = MakeAtomic(bg), *front = MakeAtomic(fg);
	bool ok = true;
	for(int pan = 0; pan < 2; pan++){
		float cx = 0.0125f;
		PlaceCamera(0.0f, cx, 0.0f, 0.0f);
		metal::cameraCut();
		ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
		metal::setJitterIndexForTest(0);
		for(int f = 0; f < 24; f++){
			if(pan){
				cx += 0.031f;
				PlaceCamera(0.0f, cx, 0.0f, 0.0f);
			}
			HighlightFrame(back, front);
		}
		int edge = (int)(320.0f*(1.0f + (1.0f + cx)/10.0f));
		HighlightRun taa, plain, taaE, plainE;
		ReadRun(&taa, edge - RUN/2, 240);
		ReadRun(&taaE, 160 - RUN/2, 330);
		SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
		HighlightFrame(back, front);
		HighlightFrame(back, front);
		ReadRun(&plain, edge - RUN/2, 240);
		ReadRun(&plainE, 160 - RUN/2, 330);
		ok = NoFireflyNoHalo(taa, plain, RUN/2 - 1, RUN/2, pan ? "panning surface" : "still surface") && ok;
		ok = NoFireflyNoHalo(taaE, plainE, RUN/2 - 1, RUN/2 - 1, pan ? "panning emitter" : "still emitter") && ok;
	}
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	DestroyAtomic(back);
	DestroyAtomic(front);
	bg->destroy();
	fg->destroy();
	mat->destroy();
	return Report(ok, name);
}

static float
FringeEdgeSwing(Atomic *back, Atomic *front, bool fringe, int *jittered)
{
	enum { FRAMES = 24, SPAN = 2 };
	RGBA faint = { 16, 16, 16, 16 }, night = { 8, 8, 12, 255 };
	PlaceCamera(0.0f, 0.0125f, 0.0f, 0.0f);
	metal::cameraCut();
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	int edge = (int)(320.0f*(1.0f + 1.0125f/10.0f));
	float lo[2*SPAN + 1], hi[2*SPAN + 1];
	for(int i = 0; i <= 2*SPAN; i++){
		lo[i] = 1e9f;
		hi[i] = -1e9f;
	}
	*jittered = 0;
	for(int f = 0; f < FRAMES; f++){
		metal::beginScene(cam);
		WorldBegin(night);
		SetRenderState(VERTEXALPHA, 0);
		if(metal::beginPrepass()){
			back->render();
			front->render();
			metal::endPrepass();
		}
		back->render();
		front->render();
		if(fringe){
			metal::beginReactive();
			Quad2D(edge - 40, 200, edge + 40, 280, faint, true);
			metal::endReactive();
		}
		metal::endScene(nil);
		WorldEnd();
		if(f < FRAMES - 8)
			continue;
		*jittered += Jittered(metal::getFrameInfo());
		for(int i = 0; i <= 2*SPAN; i++){
			float px[4];
			metal::readFrameOutput(edge - SPAN + i, 240, px);
			lo[i] = fminf(lo[i], px[0]);
			hi[i] = fmaxf(hi[i], px[0]);
		}
	}
	TiersOff();
	float swing = 0.0f;
	for(int i = 0; i <= 2*SPAN; i++)
		swing = fmaxf(swing, hi[i] - lo[i]);
	return swing;
}

static const RGBA GREY = { 128, 128, 128, 255 };

static Geometry*
MeshGeometry(const float (*p)[3], int numVerts, const uint16 (*t)[3], int numTris, bool strips)
{
	Geometry *geo = Geometry::create(numVerts, numTris, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT |
		(strips ? Geometry::TRISTRIP : 0));
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i < numVerts; i++){
		v[i].set(p[i][0], p[i][1], p[i][2]);
		geo->colors[i] = GREY;
	}
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	for(int i = 0; i < numTris; i++){
		geo->triangles[i].v[0] = t[i][0];
		geo->triangles[i].v[1] = t[i][1];
		geo->triangles[i].v[2] = t[i][2];
		geo->triangles[i].matId = 0;
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static Geometry*
RidgeGeometry(void)
{
	static const float p[6][3] = { { -1, -1, 10 }, { -1, 1, 10 }, { 0, -1, 9 }, { 0, 1, 9 }, { 1, -1, 10 }, { 1, 1, 10 } };
	static const uint16 t[4][3] = { { 0, 1, 3 }, { 0, 3, 2 }, { 2, 3, 5 }, { 2, 5, 4 } };
	return MeshGeometry(p, 6, t, 4, false);
}

static Geometry*
BoxGeometry(float cx, float cy, float cz)
{
	float p[8][3];
	for(int i = 0; i < 8; i++){
		p[i][0] = cx + (i & 1 ? 1.0f : -1.0f);
		p[i][1] = cy + (i & 2 ? 1.0f : -1.0f);
		p[i][2] = cz + (i & 4 ? 1.0f : -1.0f);
	}
	static const uint16 t[12][3] = {
		{ 0, 2, 3 }, { 0, 3, 1 }, { 4, 5, 7 }, { 4, 7, 6 }, { 0, 4, 6 }, { 0, 6, 2 },
		{ 1, 3, 7 }, { 1, 7, 5 }, { 0, 1, 5 }, { 0, 5, 4 }, { 2, 6, 7 }, { 2, 7, 3 } };
	return MeshGeometry(p, 8, t, 12, false);
}

static Geometry*
TwoMaterialBox(float cx, float cy, float cz)
{
	static const uint16 t[12][3] = {
		{ 0, 4, 6 }, { 0, 6, 2 }, { 1, 3, 7 }, { 1, 7, 5 }, { 0, 1, 5 }, { 0, 5, 4 },
		{ 2, 6, 7 }, { 2, 7, 3 }, { 4, 5, 7 }, { 4, 7, 6 }, { 0, 2, 3 }, { 0, 3, 1 } };
	Geometry *geo = Geometry::create(8, 12, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i < 8; i++){
		v[i].set(cx + (i & 1 ? 1.0f : -1.0f), cy + (i & 2 ? 1.0f : -1.0f), cz + (i & 4 ? 1.0f : -1.0f));
		geo->colors[i] = GREY;
	}
	for(int k = 0; k < 2; k++){
		Material *mat = Material::create();
		geo->matList.appendMaterial(mat);
		mat->destroy();
	}
	for(int i = 0; i < 12; i++){
		geo->triangles[i].v[0] = t[i][0];
		geo->triangles[i].v[1] = t[i][1];
		geo->triangles[i].v[2] = t[i][2];
		geo->triangles[i].matId = i >= 10 ? 1 : 0;
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static Geometry*
CylinderGeometry(float r, float cz, bool cap)
{
	enum { SIDES = 8, COLS = SIDES + 1 };
	float p[2*COLS + 1][3];
	for(int ring = 0; ring < 2; ring++)
		for(int j = 0; j < COLS; j++){
			float a = 2.0f*3.14159265f*(j % SIDES)/SIDES;
			p[ring*COLS + j][0] = r*sinf(a);
			p[ring*COLS + j][1] = ring ? r : -r;
			p[ring*COLS + j][2] = cz - r*cosf(a);
		}
	p[2*COLS][0] = 0.0f;
	p[2*COLS][1] = r;
	p[2*COLS][2] = cz;
	uint16 t[3*SIDES][3];
	for(int j = 0; j < SIDES; j++){
		t[2*j][0] = j; t[2*j][1] = COLS + j + 1; t[2*j][2] = j + 1;
		t[2*j+1][0] = j; t[2*j+1][1] = COLS + j; t[2*j+1][2] = COLS + j + 1;
		t[2*SIDES + j][0] = 2*COLS; t[2*SIDES + j][1] = COLS + j + 1; t[2*SIDES + j][2] = COLS + j;
	}
	return MeshGeometry(p, cap ? 2*COLS + 1 : 2*COLS, t, cap ? 3*SIDES : 2*SIDES, true);
}

static void
DerivedFrame(Atomic *a)
{
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(metal::beginPrepass()){
		SetRenderState(VERTEXALPHA, 0);
		a->render();
		metal::endPrepass();
	}
	a->render();
	metal::endScene(nil);
	WorldEnd();
}

static bool
ReadNormal(float x, float y, float z, float n[3])
{
	float m[2], d;
	return metal::readPrepassForTest(PX(x, z), PY(y, z), n, m, &d);
}

static bool
NearNormal(const float *n, float x, float y, float z, float tol)
{
	return fabsf(n[0] - x) < tol && fabsf(n[1] - y) < tol && fabsf(n[2] - z) < tol;
}

static bool
CheckDerivedBox(void)
{
	const char *name = "a box with shared corners reads its face normal in the prepass everywhere, corners included";
	Geometry *geo = BoxGeometry(2.5f, 0.0f, 10.0f);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::StateStats before = metal::getStateStats();
	DerivedFrame(a);
	metal::StateStats after = metal::getStateStats();
	int bad = 0, read = 0;
	float worst[3] = { 0, 0, 0 }, at[3] = { 0, 0, 0 };
	for(int i = 0; i < 5; i++)
		for(int j = 0; j < 5; j++){
			float n[3];
			float x = 1.55f + 1.9f*i/4, y = -0.95f + 1.9f*j/4;
			if(!ReadNormal(x, y, 9.0f, n))
				continue;
			read++;
			if(!NearNormal(n, 0.0f, 0.0f, -1.0f, 0.02f)){
				bad++;
				memcpy(worst, n, sizeof(worst));
				at[0] = x; at[1] = y; at[2] = 9.0f;
			}
		}
	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++){
			float n[3];
			float z = 9.1f + 1.8f*i/2, y = -0.9f + 1.8f*j/2;
			if(!ReadNormal(1.5f, y, z, n))
				continue;
			read++;
			if(!NearNormal(n, -1.0f, 0.0f, 0.0f, 0.02f) && !NearNormal(n, 1.0f, 0.0f, 0.0f, 0.02f)){
				bad++;
				memcpy(worst, n, sizeof(worst));
				at[0] = 1.5f; at[1] = y; at[2] = z;
			}
		}
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	uint32 draws = after.prepassDraws - before.prepassDraws, equal = after.prepassEqual - before.prepassEqual;
	bool ok = read == 34 && bad == 0 && draws == 1 && equal == draws;
	if(!ok)
		Detail("  %d of %d samples off the face normal, e.g. %.3f,%.3f,%.3f at %.2f,%.2f,%.2f; prepass %u equal %u\n", bad, read,
			worst[0], worst[1], worst[2], at[0], at[1], at[2], draws, equal);
	return Report(ok, name);
}

static bool
CheckDerivedCrease(const char *name)
{
	Geometry *geo = BoxGeometry(2.5f, 0.0f, 10.0f);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	int bad = 0, read = 0, at = -1;
	float worst[3] = { 0, 0, 0 };
	int front = PX(1.5f, 9.0f);
	for(int y = PY(0.9f, 9.0f); y <= PY(-0.9f, 9.0f); y++)
		for(int side = 0; side < 2; side++){
			float n[3], m[2], d;
			if(!metal::readPrepassForTest(front + side, y, n, m, &d))
				continue;
			read++;
			if(side ? !NearNormal(n, 1.0f, 0.0f, 0.0f, 0.02f) : !NearNormal(n, 0.0f, 0.0f, -1.0f, 0.02f)){
				bad++;
				memcpy(worst, n, sizeof(worst));
				at = y*1000 + front + side;
			}
		}
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	bool ok = read > 100 && bad == 0;
	if(!ok)
		Detail("  %d of %d pixels off their face normal, e.g. %.3f,%.3f,%.3f at %d,%d\n", bad, read, worst[0], worst[1], worst[2],
			at % 1000, at / 1000);
	return Report(ok, name);
}

static bool
CheckDerivedSecondMesh(void)
{
	const char *name = "a box whose front face is its second mesh reads that face's own normal in the prepass";
	Geometry *geo = TwoMaterialBox(2.5f, 0.0f, 10.0f);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	int bad = 0, read = 0;
	float worst[3] = { 0, 0, 0 };
	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++){
			float n[3];
			if(!ReadNormal(1.7f + 1.6f*i/2, -0.8f + 1.6f*j/2, 9.0f, n))
				continue;
			read++;
			if(!NearNormal(n, 0.0f, 0.0f, -1.0f, 0.02f)){
				bad++;
				memcpy(worst, n, sizeof(worst));
			}
		}
	metal::InstanceDataHeader *h = (metal::InstanceDataHeader*)geo->instData;
	bool twoMeshes = h && h->numMeshes == 2 && h->inst[1].offset > 0;
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	bool ok = twoMeshes && read == 9 && bad == 0;
	if(!ok)
		Detail("  two meshes %d, %d of %d samples off (0,0,-1), e.g. %.3f,%.3f,%.3f\n", twoMeshes, bad, read, worst[0], worst[1],
			worst[2]);
	return Report(ok, name);
}

static bool
CheckSmoothInterpolated(void)
{
	const char *name = "a quad with valid vertex normals reads the normalised interpolated normal, no face normal mixed in";
	Geometry *geo = Geometry::create(4, 2, Geometry::POSITIONS | Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED);
	float s = sinf(30.0f*3.14159265f/180.0f), c = cosf(30.0f*3.14159265f/180.0f);
	for(int i = 0; i < 4; i++){
		float x = i & 1 ? 1.0f : -1.0f;
		geo->morphTargets[0].vertices[i].set(x, i & 2 ? 1.0f : -1.0f, 10.0f);
		geo->morphTargets[0].normals[i].set(x*s, 0.0f, -c);
		geo->colors[i] = GREY;
	}
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	static const uint16 t[2][3] = { { 0, 2, 3 }, { 0, 3, 1 } };
	for(int i = 0; i < 2; i++){
		for(int k = 0; k < 3; k++)
			geo->triangles[i].v[k] = t[i][k];
		geo->triangles[i].matId = 0;
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	float n[3] = {};
	bool read = ReadNormal(-0.5f, 0.0f, 10.0f, n);
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	float at = 0.5f*(1.0f + 10.0f*(1.0f - (PX(-0.5f, 10.0f) + 0.5f)/320.0f));
	float ix = (1.0f - 2.0f*at)*s, iz = c, il = sqrtf(ix*ix + iz*iz);
	bool ok = read && fabsf(fabsf(n[0]) - ix/il) < 0.005f && fabsf(fabsf(n[2]) - iz/il) < 0.005f;
	if(!ok)
		Detail("  read %.4f,%.4f,%.4f; the interpolated normal is |x| %.4f |z| %.4f\n", n[0], n[1], n[2], ix/il, iz/il);
	return Report(ok, name);
}

static bool
CheckDerivedCylinder(const char *name, float scale)
{
	Geometry *geo = CylinderGeometry(1.5f/scale, 10.0f/scale, false);
	Atomic *a = MakeAtomic(geo);
	V3d s = { scale, scale, scale };
	a->getFrame()->scale(&s, COMBINEREPLACE);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	float l[3] = {}, r[3] = {}, m[2], d;
	bool read = metal::readPrepassForTest(317, 240, l, m, &d) && metal::readPrepassForTest(323, 240, r, m, &d);
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	bool ok = read && fabsf(l[0] - r[0]) < 0.15f && l[2] < -0.9f && r[2] < -0.9f;
	if(!ok)
		Detail("  left of the seam %.3f,%.3f,%.3f right %.3f,%.3f,%.3f (flat facets would differ by 0.77 in x)\n", l[0], l[1], l[2],
			r[0], r[1], r[2]);
	return Report(ok, name);
}

static bool
CheckDerivedRim(void)
{
	const char *name = "a cylinder whose rim is shared with its cap shades its side toward the flat facet at the rim";
	Geometry *geo = CylinderGeometry(1.5f, 10.0f, true);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	float s = sinf(10.0f*3.14159265f/180.0f), c = cosf(10.0f*3.14159265f/180.0f);
	float top[3] = {}, low[3] = {};
	bool read = ReadNormal(1.5f*s, 1.45f, 10.0f - 1.5f*c, top) && ReadNormal(1.5f*s, -1.2f, 10.0f - 1.5f*c, low);
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	float facet = sinf(22.5f*3.14159265f/180.0f);
	bool ok = read && fabsf(fabsf(top[0]) - facet) < 0.03f && fabsf(fabsf(low[0]) - facet) > 0.1f;
	if(!ok)
		Detail("  by the rim %.3f,%.3f,%.3f lower down %.3f,%.3f,%.3f (the facet is |x| %.3f)\n", top[0], top[1], top[2],
			low[0], low[1], low[2], facet);
	return Report(ok, name);
}

static bool
CheckDerivedRidge(void)
{
	const char *name = "a ridge between two slopes keeps both slope normals up to the ridge in the prepass";
	Geometry *geo = RidgeGeometry();
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	float half[3] = {}, l[3] = {}, r[3] = {};
	bool read = ReadNormal(-0.5f, 0.0f, 9.5f, half) && ReadNormal(-0.05f, 0.0f, 9.05f, l) && ReadNormal(0.05f, 0.0f, 9.05f, r);
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	bool ok = read && NearNormal(l, half[0], half[1], half[2], 0.02f) && fabsf(l[0]) > 0.69f && l[2] < -0.69f &&
		NearNormal(r, -l[0], l[1], l[2], 0.02f);
	if(!ok)
		Detail("  halfway %.3f,%.3f,%.3f by the ridge %.3f,%.3f,%.3f and %.3f,%.3f,%.3f (slopes are |x| 0.707)\n", half[0], half[1],
			half[2], l[0], l[1], l[2], r[0], r[1], r[2]);
	return Report(ok, name);
}

static bool
CheckNormalStreamModernOnly(void)
{
	const char *name = "classic and enhanced frames never build the derived normal stream; the first modern prepass builds it once";
	Geometry *geo = RidgeGeometry();
	Atomic *a = MakeAtomic(geo);
	uint32 start = metal::getInstanceStats().normalStreams;
	cam->showRaster(0);
	EnhancedOn();
	metal::beginScene(cam);
	WorldBegin(SKY);
	int32 enhancedTier = metal::getFrameInfo().tier;
	a->render();
	metal::endScene(nil);
	WorldEnd();
	SetSettings(metal::TIER_CLASSIC, metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	WorldBegin(SKY);
	int32 classicTier = metal::getFrameInfo().tier;
	a->render();
	WorldEnd();
	uint32 afterOld = metal::getInstanceStats().normalStreams;
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	DerivedFrame(a);
	uint32 afterModern = metal::getInstanceStats().normalStreams;
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	bool ok = afterOld == start && afterModern == start + 1 && enhancedTier == metal::TIER_ENHANCED &&
		classicTier == metal::TIER_CLASSIC;
	if(!ok)
		Detail("  streams %u -> %u (classic and enhanced) -> %u (two modern frames); tiers %d and %d\n", start, afterOld,
			afterModern, enhancedTier, classicTier);
	return Report(ok, name);
}

static bool
CheckNormalStreamRebuilt(void)
{
	const char *name = "a vertex change drops the derived normal stream and the next modern prepass rebuilds it from the new positions";
	Geometry *geo = RidgeGeometry();
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	DerivedFrame(a);
	uint32 built = metal::getInstanceStats().normalStreams;
	geo->lock(Geometry::LOCKVERTICES);
	geo->morphTargets[0].vertices[2].z = 10.0f;
	geo->morphTargets[0].vertices[3].z = 10.0f;
	geo->unlock();
	DerivedFrame(a);
	uint32 rebuilt = metal::getInstanceStats().normalStreams;
	float n[3] = {};
	bool read = ReadNormal(-0.5f, 0.0f, 10.0f, n);
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	bool ok = read && rebuilt == built + 1 && NearNormal(n, 0.0f, 0.0f, -1.0f, 0.02f);
	if(!ok)
		Detail("  streams %u -> %u; normal after the change %.3f,%.3f,%.3f\n", built, rebuilt, n[0], n[1], n[2]);
	return Report(ok, name);
}

static bool
SeamReads(float l[3], float r[3])
{
	float m[2], d;
	return metal::readPrepassForTest(317, 240, l, m, &d) && metal::readPrepassForTest(323, 240, r, m, &d);
}

static void
CheckNormalBudget(void)
{
	const char *flat = "with the derivation budget at zero an instance without a stream draws flat and builds nothing";
	const char *later = "restoring the budget builds the waiting stream in the next modern frame, which then reads smooth";
	Geometry *geo = CylinderGeometry(1.5f, 10.0f, false);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::setNormalBudgetForTest(0.0f);
	metal::InstanceStats s0 = metal::getInstanceStats();
	metal::StateStats p0 = metal::getStateStats();
	DerivedFrame(a);
	metal::InstanceStats s1 = metal::getInstanceStats();
	metal::StateStats p1 = metal::getStateStats();
	float l[3] = {}, r[3] = {};
	bool read = SeamReads(l, r);
	float facet = sinf(22.5f*3.14159265f/180.0f);
	uint32 draws = p1.prepassDraws - p0.prepassDraws, equal = p1.prepassEqual - p0.prepassEqual;
	bool ok = read && s1.normalStreams == s0.normalStreams && s1.normalPending == s0.normalPending + 1 &&
		fabsf(fabsf(l[0]) - facet) < 0.02f && fabsf(fabsf(r[0]) - facet) < 0.02f && l[0]*r[0] < 0.0f && draws == 1 && equal == 1;
	if(!ok)
		Detail("  built %u -> %u pending %u -> %u; seam %.3f,%.3f,%.3f | %.3f,%.3f,%.3f (facets |x| %.3f); prepass %u equal %u\n",
			s0.normalStreams, s1.normalStreams, s0.normalPending, s1.normalPending, l[0], l[1], l[2], r[0], r[1], r[2], facet,
			draws, equal);
	Report(ok, flat);
	metal::setNormalBudgetForTest(-1.0f);
	DerivedFrame(a);
	metal::InstanceStats s2 = metal::getInstanceStats();
	read = SeamReads(l, r);
	ok = read && s2.normalStreams == s1.normalStreams + 1 && s2.normalPending == s0.normalPending &&
		fabsf(l[0] - r[0]) < 0.15f && l[2] < -0.9f && r[2] < -0.9f;
	if(!ok)
		Detail("  built %u -> %u pending %u; seam %.3f,%.3f,%.3f | %.3f,%.3f,%.3f\n", s1.normalStreams, s2.normalStreams,
			s2.normalPending, l[0], l[1], l[2], r[0], r[1], r[2]);
	Report(ok, later);
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
}

static bool
CheckNormalBudgetCut(void)
{
	const char *name = "a reported camera cut builds every waiting stream in that frame, whatever the budget";
	Geometry *geos[3] = { RidgeGeometry(), BoxGeometry(2.5f, 0.0f, 10.0f), CylinderGeometry(1.5f, 10.0f, true) };
	Atomic *as[3];
	for(int i = 0; i < 3; i++)
		as[i] = MakeAtomic(geos[i]);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::setNormalBudgetForTest(0.0f);
	metal::InstanceStats s0 = metal::getInstanceStats();
	for(int i = 0; i < 3; i++)
		DerivedFrame(as[i]);
	metal::InstanceStats s1 = metal::getInstanceStats();
	metal::cameraCut();
	DerivedFrame(as[0]);
	metal::InstanceStats s2 = metal::getInstanceStats();
	DerivedFrame(as[1]);
	metal::InstanceStats s3 = metal::getInstanceStats();
	metal::setNormalBudgetForTest(-1.0f);
	TiersOff();
	for(int i = 0; i < 3; i++){
		DestroyAtomic(as[i]);
		geos[i]->destroy();
	}
	bool ok = s1.normalStreams == s0.normalStreams && s1.normalPending == s0.normalPending + 3 &&
		s2.normalStreams == s0.normalStreams + 3 && s2.normalPending == s0.normalPending && s3.normalStreams == s2.normalStreams;
	if(!ok)
		Detail("  built %u -> %u -> %u (cut) -> %u; pending %u -> %u -> %u\n", s0.normalStreams, s1.normalStreams, s2.normalStreams,
			s3.normalStreams, s0.normalPending, s1.normalPending, s2.normalPending);
	return Report(ok, name);
}

static bool
CheckNormalQueueFree(void)
{
	const char *name = "a waiting instance freed before its build leaves the queue, and a built stream leaves the live count with its instance";
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::setNormalBudgetForTest(0.0f);
	Geometry *geo = RidgeGeometry();
	Atomic *a = MakeAtomic(geo);
	metal::InstanceStats s0 = metal::getInstanceStats();
	DerivedFrame(a);
	metal::InstanceStats s1 = metal::getInstanceStats();
	DestroyAtomic(a);
	geo->destroy();
	metal::InstanceStats s2 = metal::getInstanceStats();
	metal::setNormalBudgetForTest(-1.0f);
	geo = RidgeGeometry();
	a = MakeAtomic(geo);
	DerivedFrame(a);
	metal::InstanceStats s3 = metal::getInstanceStats();
	DestroyAtomic(a);
	geo->destroy();
	metal::InstanceStats s4 = metal::getInstanceStats();
	TiersOff();
	bool ok = s1.normalPending == s0.normalPending + 1 && s2.normalPending == s0.normalPending &&
		s3.normalLive == s0.normalLive + 1 && s3.normalBytes == s0.normalBytes + 6*4 &&
		s4.normalLive == s0.normalLive && s4.normalBytes == s0.normalBytes;
	if(!ok)
		Detail("  pending %u -> %u -> %u (freed); live %u -> %u -> %u, bytes %u -> %u -> %u\n", s0.normalPending, s1.normalPending,
			s2.normalPending, s0.normalLive, s3.normalLive, s4.normalLive, s0.normalBytes, s3.normalBytes, s4.normalBytes);
	return Report(ok, name);
}

static bool
CheckNormalStatsLine(void)
{
	const char *name = "the stats line shows normal stream builds, the waiting queue, live streams and their bytes";
	metal::InstanceStats s = metal::getInstanceStats();
	metal::logStats();
	const char *tail = strstr(metal::getStatsLine(), " normals built ");
	unsigned built = 0, pending = 0, live = 0, bytes = 0;
	bool ok = tail && sscanf(tail, " normals built %u pending %u live %u bytes %u", &built, &pending, &live, &bytes) == 4 &&
		pending == s.normalPending && live == s.normalLive && bytes == s.normalBytes;
	if(!ok)
		Detail("  got \"%s\"; pending %u live %u bytes %u\n", metal::getStatsLine(), s.normalPending, s.normalLive, s.normalBytes);
	return Report(ok, name);
}

static const RGBA BLACK = { 0, 0, 0, 255 };
static const RGBA STONE = { 140, 120, 100, 255 };
static const RGBA STONE_SEETHROUGH = { 140, 120, 100, 254 };

struct Sun
{
	Light *sun;
	Light *amb;
	Frame *frame;
};

static void
MakeSun(Sun *s, float ambient)
{
	s->sun = Light::create(Light::DIRECTIONAL);
	s->sun->setColor(0.8f, 0.6f, 0.5f);
	s->frame = Frame::create();
	V3d axis = { 0.0f, 1.0f, 0.0f };
	s->frame->rotate(&axis, 30.0f, COMBINEREPLACE);
	s->frame->updateObjects();
	s->sun->setFrame(s->frame);
	TestWorld()->addLight(s->sun);
	s->amb = Light::create(Light::AMBIENT);
	s->amb->setColor(ambient, ambient, ambient);
	TestWorld()->addLight(s->amb);
	metal::setSunLight(s->sun);
}

static void
FreeSun(Sun *s)
{
	metal::setSunLight(nil);
	TestWorld()->removeLight(s->sun);
	TestWorld()->removeLight(s->amb);
	s->sun->destroy();
	s->frame->destroy();
	s->amb->destroy();
}

static Material*
TestMaterial(void)
{
	Material *mat = Material::create();
	SurfaceProperties sp = { 1.0f, 0.0f, 1.0f };
	mat->surfaceProps = sp;
	return mat;
}

static void
RelitSet(float weight, float hemiScale, float sunScale)
{
	metal::RelitParams p = metal::getRelitParams();
	p.weight = weight;
	p.hemiScale = hemiScale;
	p.hemiSaturation = 1.0f;
	p.sunScale = sunScale;
	metal::setRelitParams(&p);
}

static void
RelitSky(float tr, float tg, float tb, float br, float bg, float bb)
{
	metal::RelitParams p = metal::getRelitParams();
	p.skyTop[0] = tr; p.skyTop[1] = tg; p.skyTop[2] = tb;
	p.skyBottom[0] = br; p.skyBottom[1] = bg; p.skyBottom[2] = bb;
	metal::setRelitParams(&p);
}

static void
RelitReset(void)
{
	metal::RelitParams p = { 1.0f, { 0.0f, 0.0f, 1.0f }, { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f }, 0.5f, 0.35f, 1.0f };
	metal::setRelitParams(&p);
	metal::setShadowFactorForTest(-1.0f);
	metal::setFaceFetchForTest(1);
}

static float
FacingTilt(const Sun *s)
{
	V3d at = s->frame->getLTM()->at;
	return atan2f(-at.x, at.z);
}

static float
AwayTilt(const Sun *s)
{
	float t = FacingTilt(s);
	return t - copysignf(1.72f, t);
}

static V3d
TiltNormal(float t)
{
	V3d n = { sinf(t), 0.0f, -cosf(t) };
	return n;
}

static Geometry*
TiltedQuad(uint32 flags, float cx, float z, float tilt, RGBA c, Material *mat)
{
	RGBA col[4] = { c, c, c, c };
	Geometry *geo = QuadGeometry(flags, cx - 1.0f, -1.0f, cx + 1.0f, 1.0f, 0.0f, col, nil, mat);
	geo->lock(Geometry::LOCKVERTICES | Geometry::LOCKNORMALS);
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i < 4; i++){
		float x = v[i].x - cx;
		v[i].x = cx + x*cosf(tilt);
		v[i].z = z + x*sinf(tilt);
		if(flags & Geometry::NORMALS)
			geo->morphTargets[0].normals[i] = TiltNormal(tilt);
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static float
Expect(int ch, float prelit, float ambient, float w, V3d n, const Sun *s, bool twoSided, float shadow)
{
	metal::RelitParams p = metal::getRelitParams();
	float wc = relitClampWeight(w);
	float classic = fminf(fmaxf(relitWeighted(prelit, wc) + ambient, 0.0f), 1.0f);
	RLColor top = { p.skyTop[0], p.skyTop[1], p.skyTop[2] }, bottom = { p.skyBottom[0], p.skyBottom[1], p.skyBottom[2] };
	RLColor h = relitDesaturate(relitHemisphere(n.z, top, bottom), p.hemiSaturation);
	float hc = ch == 0 ? h.r : ch == 1 ? h.g : h.b;
	float sc = ch == 0 ? s->sun->color.red : ch == 1 ? s->sun->color.green : s->sun->color.blue;
	V3d at = s->frame->getLTM()->at;
	float live = 1.0f - wc;
	float c = relitSunCos(-(n.x*at.x + n.y*at.y + n.z*at.z), twoSided);
	RLSplit sp = relitSplitAdd(classic, hc*live*p.hemiScale, sc*c*live*p.sunScale, 1.0f);
	return relitShadowed(sp.full, sp.sun, shadow);
}

static void
LitBegin(Atomic **atoms, int n, int32 cull)
{
	metal::beginScene(cam);
	metal::setRelitTwoSided(cull == CULLNONE);
	WorldBegin(SKY);
	SetRenderState(CULLMODE, cull);
	if(metal::beginPrepass()){
		for(int i = 0; i < n; i++)
			atoms[i]->render();
		metal::endPrepass();
	}
	SetRenderState(CULLMODE, cull);
	for(int i = 0; i < n; i++)
		atoms[i]->render();
}

static void
LitEnd(void)
{
	metal::endScene(nil);
	WorldEnd();
}

static float
ScenePixel(int x, int y, int ch)
{
	float px[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
	if(!metal::readScenePixel(x, y, px))
		Detail("  pixel %d,%d could not be read\n", x, y);
	return px[ch];
}

static bool
IsSky(int x, int y)
{
	float px[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
	if(!metal::readScenePixel(x, y, px))
		return true;
	return fabsf(px[0] - SKY.red/255.0f) < 0.004f && fabsf(px[1] - SKY.green/255.0f) < 0.004f &&
		fabsf(px[2] - SKY.blue/255.0f) < 0.004f;
}

static bool
FramesEqual(const uint8 *a, const uint8 *b, int w, int h, const char *what)
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
	return differing == 0;
}

static void
MoveAtomic(Atomic *a, float x, float y, float z)
{
	V3d d = { x, y, z };
	a->getFrame()->translate(&d, COMBINEPOSTCONCAT);
	a->getFrame()->updateObjects();
}

static Atomic*
SkinnedQuad(Geometry **geo, Material *mat, float x, float y, float z)
{
	RGBA red = { 220, 40, 30, 255 }, col[4] = { red, red, red, red };
	*geo = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::NORMALS | Geometry::PRELIT | Geometry::LIGHT,
		1.0f, 1.0f, -1.0f, -1.0f, 0.0f, col, nil, mat);
	Matrix ident[2];
	ident[0].setIdentity();
	ident[1].setIdentity();
	uint8 idx[4][4] = { { 0, 1, 0, 0 }, { 0, 1, 0, 0 }, { 0, 1, 0, 0 }, { 0, 1, 0, 0 } };
	float wts[4][4] = { { 0.5f, 0.5f, 0, 0 }, { 0.5f, 0.5f, 0, 0 }, { 0.5f, 0.5f, 0, 0 }, { 0.5f, 0.5f, 0, 0 } };
	AttachSkin(*geo, 2, ident, idx, wts);
	Atomic *a = SkinAtomic(*geo);
	V3d at = { x, y, z };
	a->getFrame()->translate(&at, COMBINEREPLACE);
	a->getFrame()->updateObjects();
	HAnimHierarchy *h = HAnimHierarchy::create(2, nil, nil, 0, 36);
	h->matrices[0] = *a->getFrame()->getLTM();
	h->matrices[1] = *a->getFrame()->getLTM();
	Skin::setHierarchy(a, h);
	return a;
}

enum { RELIT_SKINNED = 6, RELIT_ATOMS = 8 };

struct RelitScene
{
	Atomic *atoms[RELIT_ATOMS];
	Geometry *geos[RELIT_ATOMS];
	Material *mat;
	Sun sun;
	int n;
};

static void
MakeRelitScene(RelitScene *s)
{
	const uint32 prelit = Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT;
	MakeSun(&s->sun, 0.1f);
	s->mat = TestMaterial();
	float xs[3] = { 3.0f, 0.0f, -3.0f }, tilts[3] = { 0.0f, FacingTilt(&s->sun), AwayTilt(&s->sun) };
	int n = 0;
	for(int i = 0; i < 3; i++, n++){
		s->geos[n] = TiltedQuad(prelit, xs[i], 12.0f, tilts[i], STONE, s->mat);
		s->atoms[n] = MakeAtomic(s->geos[n]);
	}
	s->geos[n] = TiltedQuad(Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT, 0.0f, 30.0f, 0.0f, BLACK, s->mat);
	s->atoms[n] = MakeAtomic(s->geos[n]);
	MoveAtomic(s->atoms[n++], 0.0f, 3.0f, 0.0f);
	s->geos[n] = BoxGeometry(-4.0f, 2.5f, 16.0f);
	s->atoms[n] = MakeAtomic(s->geos[n]);
	n++;
	s->geos[n] = CylinderGeometry(1.0f, 18.0f, false);
	s->atoms[n] = MakeAtomic(s->geos[n]);
	MoveAtomic(s->atoms[n++], 4.0f, 2.5f, 0.0f);
	s->atoms[n] = SkinnedQuad(&s->geos[n], s->mat, 4.0f, -2.5f, 16.0f);
	n++;
	RGBA glass = { 40, 200, 90, 128 }, col[4] = { glass, glass, glass, glass };
	s->geos[n] = QuadGeometry(prelit, -1.0f, -3.5f, 1.0f, -2.0f, 8.0f, col, nil, s->mat);
	s->atoms[n] = MakeAtomic(s->geos[n]);
	n++;
	s->n = n;
}

static void
FreeRelitScene(RelitScene *s)
{
	for(int i = 0; i < s->n; i++){
		if(i == RELIT_SKINNED)
			DestroySkinAtomic(s->atoms[i]);
		else
			DestroyAtomic(s->atoms[i]);
		s->geos[i]->destroy();
	}
	s->mat->destroy();
	FreeSun(&s->sun);
}

static void
DrawRelitScene(RelitScene *s)
{
	for(int i = 0; i < s->n; i++){
		SetRenderState(VERTEXALPHA, i == s->n - 1);
		s->atoms[i]->render();
	}
	SetRenderState(VERTEXALPHA, 0);
}

static int32
RelitSceneFrame(RelitScene *s, int32 cull)
{
	metal::beginScene(cam);
	metal::setRelitTwoSided(cull == CULLNONE);
	WorldBegin(SKY);
	int32 tier = metal::getFrameInfo().tier;
	SetRenderState(CULLMODE, cull);
	if(metal::beginPrepass()){
		DrawRelitScene(s);
		metal::endPrepass();
	}
	SetRenderState(CULLMODE, cull);
	DrawRelitScene(s);
	metal::endScene(nil);
	WorldEnd();
	return tier;
}

static uint8*
RelitModernFrame(RelitScene *s, float weight, bool forced, uint32 *relit, uint32 *face, int32 *tier)
{
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(weight, 0.5f, 1.0f);
	metal::setRelitAtFullWeightForTest(forced);
	metal::StateStats before = metal::getStateStats();
	*tier = RelitSceneFrame(s, CULLNONE);
	uint8 *px = CompositeNow();
	cam->showRaster(0);
	metal::StateStats after = metal::getStateStats();
	metal::setRelitAtFullWeightForTest(0);
	*relit = after.relitDraws - before.relitDraws;
	*face = after.relitFaceDraws - before.relitFaceDraws;
	return px;
}

static bool
RelitIdentity(const char *name, float weight, bool forced)
{
	RelitScene s;
	MakeRelitScene(&s);
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	cam->showRaster(0);
	EnhancedOn();
	int32 enhancedTier = RelitSceneFrame(&s, CULLNONE);
	uint8 *enhanced = CompositeNow();
	cam->showRaster(0);
	uint32 relit, face;
	int32 modernTier;
	uint8 *modern = RelitModernFrame(&s, weight, forced, &relit, &face, &modernTier);
	RelitReset();
	TiersOff();
	FreeRelitScene(&s);
	bool drawn = forced ? face >= 6 && relit >= face + 2 : face == 0 && relit == 2;
	bool ran = drawn && enhancedTier == metal::TIER_ENHANCED && modernTier == metal::TIER_MODERN;
	if(!ran)
		Detail("  relit draws %u, of them with the face fetch %u (want %s); tiers %d and %d\n", relit, face,
			forced ? "at least 6 and 2 more" : "2 and none", enhancedTier, modernTier);
	bool ok = FramesEqual(enhanced, modern, w, h, "modern relit against enhanced") && ran;
	delete[] enhanced;
	delete[] modern;
	return Report(ok, name);
}

static bool
CheckRelitContinuity(void)
{
	const char *name = "a prelit weight of 0.999 draws within one step of the weight-1 frame, so a weight reaching 1 does not pop";
	RelitScene s;
	MakeRelitScene(&s);
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	cam->showRaster(0);
	uint32 relit1, face1, relit, face;
	int32 tier1, tier;
	uint8 *full = RelitModernFrame(&s, 1.0f, false, &relit1, &face1, &tier1);
	uint8 *near = RelitModernFrame(&s, 0.999f, false, &relit, &face, &tier);
	RelitReset();
	TiersOff();
	FreeRelitScene(&s);
	bool ran = face1 == 0 && face >= 6 && tier1 == metal::TIER_MODERN && tier == metal::TIER_MODERN;
	if(!ran)
		Detail("  face fetch draws %u at weight 1 and %u at 0.999 (want 0 and at least 6); tiers %d and %d\n", face1, face, tier1, tier);
	bool ok = FramesWithinOneStep(full, near, w, h, "weight 0.999 against weight 1") && ran;
	delete[] full;
	delete[] near;
	return Report(ok, name);
}

static bool
CheckRelitLightsUntagged(void)
{
	const char *name = "an enhanced frame after a modern one uploads the light block it uploaded before, without the sun tag";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	Geometry *geo = TiltedQuad(Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT, 0.0f, 12.0f, 0.0f, STONE, mat);
	Atomic *a = MakeAtomic(geo);
	uint8 before[1024] = {}, modern[1024] = {}, after[1024] = {};
	uint32 size = 0;
	cam->showRaster(0);
	metal::setSunLight(nil);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	LitBegin(&a, 1, CULLNONE);
	LitEnd();
	cam->showRaster(0);
	metal::setSunLight(sun.sun);
	EnhancedOn();
	LitBegin(&a, 1, CULLNONE);
	LitEnd();
	size = metal::lightsBlockForTest(before, sizeof(before));
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	LitBegin(&a, 1, CULLNONE);
	LitEnd();
	metal::lightsBlockForTest(modern, sizeof(modern));
	cam->showRaster(0);
	EnhancedOn();
	LitBegin(&a, 1, CULLNONE);
	LitEnd();
	metal::lightsBlockForTest(after, sizeof(after));
	cam->showRaster(0);
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	mat->destroy();
	FreeSun(&sun);
	bool tagged = size > 0 && memcmp(modern, before, size) != 0;
	bool same = size > 0 && memcmp(after, before, size) == 0;
	if(!tagged || !same)
		Detail("  block %u bytes; the modern block differs from the enhanced one %d (want 1); the enhanced block after it equals the one before %d (want 1)\n",
			size, tagged, same);
	return Report(tagged && same, name);
}

static bool
CheckRelitTiersUntouched(void)
{
	const char *name = "classic and enhanced frames draw no relit variant, bind no relit block and build no normal stream";
	RelitScene s;
	MakeRelitScene(&s);
	RelitSet(0.5f, 0.5f, 1.0f);
	metal::StateStats before = metal::getStateStats();
	metal::InstanceStats ib = metal::getInstanceStats();
	cam->showRaster(0);
	int32 classicTier = RelitSceneFrame(&s, CULLNONE);
	EnhancedOn();
	int32 enhancedTier = RelitSceneFrame(&s, CULLNONE);
	metal::StateStats after = metal::getStateStats();
	metal::InstanceStats ia = metal::getInstanceStats();
	TiersOff();
	RelitReset();
	FreeRelitScene(&s);
	uint32 relit = after.relitDraws - before.relitDraws, face = after.relitFaceDraws - before.relitFaceDraws;
	uint32 uploads = after.blockUploads[metal::BUFFER_RELIT] - before.blockUploads[metal::BUFFER_RELIT];
	uint32 vbinds = after.vertexBlockBinds[metal::BUFFER_RELIT] - before.vertexBlockBinds[metal::BUFFER_RELIT];
	uint32 fbinds = after.fragmentBlockBinds[metal::BUFFER_RELIT] - before.fragmentBlockBinds[metal::BUFFER_RELIT];
	uint32 streams = ia.normalStreams - ib.normalStreams;
	bool ok = relit == 0 && face == 0 && uploads == 0 && vbinds == 0 && fbinds == 0 && streams == 0 &&
		classicTier == metal::TIER_CLASSIC && enhancedTier == metal::TIER_ENHANCED;
	if(!ok)
		Detail("  relit draws %u face %u, relit block uploads %u vertex binds %u fragment binds %u, streams %u; tiers %d and %d\n",
			relit, face, uploads, vbinds, fbinds, streams, classicTier, enhancedTier);
	return Report(ok, name);
}

static bool
CheckRelitWeight(void)
{
	const char *name = "at weight 0.5 a prelit surface facing the sun gains the sun and one facing away keeps half its prelit colour";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	float xs[3] = { 3.0f, 0.0f, -3.0f }, tilts[3] = { 0.0f, FacingTilt(&sun), AwayTilt(&sun) };
	Geometry *geos[3];
	Atomic *as[3];
	for(int i = 0; i < 3; i++){
		geos[i] = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, xs[i], 12.0f, tilts[i], STONE, mat);
		as[i] = MakeAtomic(geos[i]);
	}
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.5f, 0.0f, 1.0f);
	LitBegin(as, 3, CULLBACK);
	float red[3], want[3];
	bool sky = false, near = true;
	for(int i = 0; i < 3; i++){
		red[i] = ScenePixel(PX(xs[i], 12.0f), PY(0.0f, 12.0f), 0);
		sky = sky || IsSky(PX(xs[i], 12.0f), PY(0.0f, 12.0f));
		want[i] = Expect(0, 140/255.0f, 0.1f, 0.5f, TiltNormal(tilts[i]), &sun, false, 1.0f);
		near = near && fabsf(red[i] - want[i]) < 2.0f/255.0f;
	}
	LitEnd();
	RelitReset();
	TiersOff();
	for(int i = 0; i < 3; i++){
		DestroyAtomic(as[i]);
		geos[i]->destroy();
	}
	mat->destroy();
	FreeSun(&sun);
	bool awayHalf = fabsf(want[2] - (0.5f*140/255.0f + 0.1f)) < 1e-4f;
	bool ok = !sky && near && awayHalf && red[1] > red[2] + 0.1f;
	if(!ok)
		Detail("  red %.4f %.4f %.4f (tilt 0, facing, away) want %.4f %.4f %.4f; sky %d\n", red[0], red[1], red[2], want[0], want[1],
			want[2], sky);
	return Report(ok, name);
}

static bool
CheckRelitFlatBox(void)
{
	const char *name = "a prelit box with shared corners is lit flat on each face up to its corners";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Geometry *geo = TwoMaterialBox(2.5f, 0.0f, 10.0f);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 1.0f);
	V3d front = { 0.0f, 0.0f, -1.0f }, side = { -1.0f, 0.0f, 0.0f };
	float wantFront = Expect(0, 128/255.0f, 0.0f, 0.0f, front, &sun, true, 1.0f);
	float wantSide = Expect(0, 128/255.0f, 0.0f, 0.0f, side, &sun, true, 1.0f);
	LitBegin(&a, 1, CULLNONE);
	int bad = 0, read = 0;
	float worst = 0.0f, wantWorst = 0.0f;
	for(int i = 0; i < 5; i++)
		for(int j = 0; j < 5; j++){
			float x = 1.55f + 1.9f*i/4, y = -0.95f + 1.9f*j/4;
			float r = ScenePixel(PX(x, 9.0f), PY(y, 9.0f), 0);
			read += r >= 0.0f;
			if(fabsf(r - wantFront) >= 2.0f/255.0f){
				bad++;
				worst = r;
				wantWorst = wantFront;
			}
		}
	for(int i = 0; i < 3; i++)
		for(int j = 0; j < 3; j++){
			float z = 9.1f + 1.8f*i/2, y = -0.9f + 1.8f*j/2;
			float r = ScenePixel(PX(1.5f, z), PY(y, z), 0);
			read += r >= 0.0f;
			if(fabsf(r - wantSide) >= 2.0f/255.0f){
				bad++;
				worst = r;
				wantWorst = wantSide;
			}
		}
	LitEnd();
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	FreeSun(&sun);
	bool ok = read == 34 && bad == 0;
	if(!ok)
		Detail("  %d of %d samples off, e.g. red %.4f where %.4f was wanted (front %.4f, side %.4f)\n", bad, read, worst, wantWorst,
			wantFront, wantSide);
	return Report(ok, name);
}

static bool
CheckRelitCrease(void)
{
	const char *name = "both pixels beside a lit crease shared by two triangles take their own face's light";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Geometry *geo = BoxGeometry(2.5f, 0.0f, 10.0f);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 1.0f);
	V3d front = { 0.0f, 0.0f, -1.0f }, side = { -1.0f, 0.0f, 0.0f };
	float want[2] = { Expect(0, 128/255.0f, 0.0f, 0.0f, front, &sun, true, 1.0f),
		Expect(0, 128/255.0f, 0.0f, 0.0f, side, &sun, true, 1.0f) };
	LitBegin(&a, 1, CULLNONE);
	int bad = 0, read = 0, at = -1;
	float worst = 0.0f;
	int col = PX(1.5f, 9.0f);
	for(int y = PY(0.9f, 9.0f); y <= PY(-0.9f, 9.0f); y++)
		for(int s = 0; s < 2; s++){
			float r = ScenePixel(col + s, y, 0);
			if(r < 0.0f)
				continue;
			read++;
			if(fabsf(r - want[s]) >= 2.0f/255.0f){
				bad++;
				worst = r;
				at = y*1000 + col + s;
			}
		}
	LitEnd();
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	FreeSun(&sun);
	bool ok = read > 100 && bad == 0;
	if(!ok)
		Detail("  %d of %d pixels off their face's light, e.g. red %.4f at %d,%d (front %.4f, side %.4f)\n", bad, read, worst,
			at % 1000, at / 1000, want[0], want[1]);
	return Report(ok, name);
}

static bool
CheckRelitBlended(void)
{
	const char *name = "a blended prelit quad, which the prepass skips, is lit like the same quad drawn opaque";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	Geometry *geos[2];
	Atomic *as[2];
	for(int i = 0; i < 2; i++){
		geos[i] = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, i ? -2.5f : 2.5f, 12.0f, -0.3f,
			i ? STONE_SEETHROUGH : STONE, mat);
		as[i] = MakeAtomic(geos[i]);
	}
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.5f, 0.5f, 1.0f);
	float want = Expect(0, 140/255.0f, 0.1f, 0.5f, TiltNormal(-0.3f), &sun, false, 1.0f);
	metal::StateStats before = metal::getStateStats();
	metal::beginScene(cam);
	WorldBegin(SKY);
	SetRenderState(CULLMODE, CULLBACK);
	for(int pass = 0; pass < 2; pass++){
		if(pass == 0 && !metal::beginPrepass())
			continue;
		SetRenderState(CULLMODE, CULLBACK);
		SetRenderState(VERTEXALPHA, 0);
		as[0]->render();
		SetRenderState(VERTEXALPHA, 1);
		as[1]->render();
		SetRenderState(VERTEXALPHA, 0);
		if(pass == 0)
			metal::endPrepass();
	}
	float opaque = ScenePixel(PX(2.5f, 12.0f), PY(0.0f, 12.0f), 0), blended = ScenePixel(PX(-2.5f, 12.0f), PY(0.0f, 12.0f), 0);
	bool sky = IsSky(PX(2.5f, 12.0f), PY(0.0f, 12.0f)) || IsSky(PX(-2.5f, 12.0f), PY(0.0f, 12.0f));
	LitEnd();
	metal::StateStats after = metal::getStateStats();
	RelitReset();
	TiersOff();
	for(int i = 0; i < 2; i++){
		DestroyAtomic(as[i]);
		geos[i]->destroy();
	}
	mat->destroy();
	FreeSun(&sun);
	uint32 prepass = after.prepassDraws - before.prepassDraws;
	bool ok = !sky && fabsf(opaque - blended) < 1.0f/255.0f && fabsf(opaque - want) < 2.0f/255.0f && prepass == 1;
	if(!ok)
		Detail("  opaque red %.4f blended %.4f want %.4f; prepass draws %u; sky %d\n", opaque, blended, want, prepass, sky);
	return Report(ok, name);
}

static bool
CheckRelitWaiting(void)
{
	const char *name = "a draw whose normal stream is not built yet is lit by its face normal, never left unlit";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Geometry *geo = CylinderGeometry(1.0f, 10.0f, false);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::setNormalBudgetForTest(0.0f);
	RelitSet(0.0f, 0.0f, 1.0f);
	V3d left = { sinf(0.3927f), 0.0f, -cosf(0.3927f) }, right = { -sinf(0.3927f), 0.0f, -cosf(0.3927f) };
	float wantL = Expect(0, 128/255.0f, 0.0f, 0.0f, left, &sun, true, 1.0f);
	float wantR = Expect(0, 128/255.0f, 0.0f, 0.0f, right, &sun, true, 1.0f);
	metal::InstanceStats s0 = metal::getInstanceStats();
	LitBegin(&a, 1, CULLNONE);
	float l = ScenePixel(PX(0.354f, 9.146f), PY(0.0f, 9.146f), 0), r = ScenePixel(PX(-0.354f, 9.146f), PY(0.0f, 9.146f), 0);
	LitEnd();
	metal::InstanceStats s1 = metal::getInstanceStats();
	metal::setNormalBudgetForTest(-1.0f);
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	FreeSun(&sun);
	bool ok = fabsf(l - wantL) < 3.0f/255.0f && fabsf(r - wantR) < 3.0f/255.0f && fabsf(l - r) > 0.05f &&
		s1.normalStreams == s0.normalStreams;
	if(!ok)
		Detail("  facets red %.4f %.4f want %.4f %.4f; streams %u -> %u\n", l, r, wantL, wantR, s0.normalStreams, s1.normalStreams);
	return Report(ok, name);
}

static Atomic*
FacingQuad(Geometry **geo, const Sun *sun, RGBA c, Material *mat)
{
	*geo = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 0.0f, 12.0f, FacingTilt(sun), c, mat);
	return MakeAtomic(*geo);
}

static void
ReadCentre(Atomic *a, float px[4])
{
	LitBegin(&a, 1, CULLBACK);
	if(!metal::readScenePixel(PX(0.0f, 12.0f), PY(0.0f, 12.0f), px))
		Detail("  the centre could not be read\n");
	LitEnd();
}

static bool
CheckRelitShadowSun(void)
{
	const char *name = "a shadow factor of 0 removes only the sun's part and keeps the prelit base and the hemisphere";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	Geometry *geo;
	Atomic *a = FacingQuad(&geo, &sun, STONE, mat);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	float A[4] = {}, B[4] = {}, C[4] = {};
	RelitSet(0.5f, 1.0f, 1.0f);
	RelitSky(0.3f, 0.3f, 0.3f, 0.1f, 0.1f, 0.1f);
	metal::setShadowFactorForTest(0.0f);
	ReadCentre(a, A);
	metal::setShadowFactorForTest(-1.0f);
	RelitSet(0.5f, 1.0f, 0.0f);
	ReadCentre(a, B);
	RelitSet(0.5f, 1.0f, 1.0f);
	ReadCentre(a, C);
	float want = Expect(0, 140/255.0f, 0.1f, 0.5f, TiltNormal(FacingTilt(&sun)), &sun, false, 1.0f);
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	mat->destroy();
	FreeSun(&sun);
	bool same = true;
	for(int i = 0; i < 3; i++)
		same = same && fabsf(A[i] - B[i]) < 1.0f/255.0f;
	bool ok = same && C[0] > A[0] + 0.1f && fabsf(C[0] - want) < 2.0f/255.0f;
	if(!ok)
		Detail("  shadowed %.4f,%.4f,%.4f, no sun %.4f,%.4f,%.4f, lit %.4f,%.4f,%.4f; lit red wants %.4f\n", A[0], A[1], A[2],
			B[0], B[1], B[2], C[0], C[1], C[2], want);
	return Report(ok, name);
}

static bool
CheckRelitSunClamp(void)
{
	const char *name = "near white the shadow factor scales only what the clamp left of the sun";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	Geometry *geo;
	RGBA pale = { 250, 250, 250, 255 };
	Atomic *a = FacingQuad(&geo, &sun, pale, mat);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.5f, 0.6f, 1.0f);
	RelitSky(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
	V3d n = TiltNormal(FacingTilt(&sun));
	const float factors[3] = { 0.0f, 0.5f, 1.0f };
	float red[3], want[3];
	bool near = true;
	for(int i = 0; i < 3; i++){
		float px[4] = {};
		metal::setShadowFactorForTest(factors[i]);
		ReadCentre(a, px);
		red[i] = px[0];
		want[i] = Expect(0, 250/255.0f, 0.1f, 0.5f, n, &sun, false, factors[i]);
		near = near && fabsf(red[i] - want[i]) < 2.0f/255.0f;
	}
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	mat->destroy();
	FreeSun(&sun);
	bool ok = near && red[2] > 1.0f - 2.0f/255.0f && red[1] < 0.99f && red[1] > red[0] + 0.02f && red[2] > red[1] + 0.02f;
	if(!ok)
		Detail("  red at shadow 0, 0.5, 1: %.4f %.4f %.4f, want %.4f %.4f %.4f\n", red[0], red[1], red[2], want[0], want[1],
			want[2]);
	return Report(ok, name);
}

static bool
CheckRelitOwnNormals(void)
{
	const char *name = "geometry with its own normals draws as in the enhanced tier at every weight, and a shadow factor of 0 removes its sun";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	Geometry *geos[2];
	Atomic *as[2];
	geos[0] = TiltedQuad(Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT, 0.0f, 12.0f, 0.0f, BLACK, mat);
	as[0] = MakeAtomic(geos[0]);
	as[1] = SkinnedQuad(&geos[1], mat, 3.0f, 0.0f, 12.0f);
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	cam->showRaster(0);
	EnhancedOn();
	LitBegin(as, 2, CULLNONE);
	int32 enhancedTier = metal::getFrameInfo().tier;
	LitEnd();
	uint8 *enhanced = CompositeNow();
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.3f, 0.5f, 1.0f);
	metal::StateStats before = metal::getStateStats();
	LitBegin(as, 2, CULLNONE);
	int32 modernTier = metal::getFrameInfo().tier;
	LitEnd();
	uint8 *modern = CompositeNow();
	cam->showRaster(0);
	metal::StateStats after = metal::getStateStats();
	metal::setShadowFactorForTest(0.0f);
	LitBegin(as, 2, CULLNONE);
	float shadowed = ScenePixel(PX(0.0f, 12.0f), PY(0.0f, 12.0f), 0);
	LitEnd();
	metal::setShadowFactorForTest(1.0f);
	LitBegin(as, 2, CULLNONE);
	float lit = ScenePixel(PX(0.0f, 12.0f), PY(0.0f, 12.0f), 0);
	LitEnd();
	V3d at = sun.frame->getLTM()->at;
	float wantLit = fminf(0.1f + 0.8f*at.z, 1.0f);
	RelitReset();
	TiersOff();
	DestroyAtomic(as[0]);
	DestroySkinAtomic(as[1]);
	geos[0]->destroy();
	geos[1]->destroy();
	mat->destroy();
	FreeSun(&sun);
	uint32 relit = after.relitDraws - before.relitDraws, face = after.relitFaceDraws - before.relitFaceDraws;
	bool same = FramesEqual(enhanced, modern, w, h, "modern relit own normals against enhanced");
	bool ok = same && relit >= 2 && face == 0 && fabsf(shadowed - 0.1f) < 2.0f/255.0f && fabsf(lit - wantLit) < 2.0f/255.0f &&
		enhancedTier == metal::TIER_ENHANCED && modernTier == metal::TIER_MODERN;
	if(!ok)
		Detail("  relit draws %u face %u; red shadowed %.4f (want 0.1), lit %.4f (want %.4f); tiers %d and %d\n", relit, face, shadowed,
			lit, wantLit, enhancedTier, modernTier);
	delete[] enhanced;
	delete[] modern;
	return Report(ok, name);
}

static bool
CheckRelitTwoSided(void)
{
	const char *name = "a thin prelit surface marked two-sided takes the sun from either side; unmarked only from its front";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	float tilt = AwayTilt(&sun);
	Geometry *geo = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 0.0f, 12.0f, tilt, STONE, mat);
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 1.0f);
	float red[2];
	bool sky = false;
	const int32 culls[2] = { CULLNONE, CULLBACK };
	for(int i = 0; i < 2; i++){
		LitBegin(&a, 1, culls[i]);
		red[i] = ScenePixel(PX(0.0f, 12.0f), PY(0.0f, 12.0f), 0);
		sky = sky || IsSky(PX(0.0f, 12.0f), PY(0.0f, 12.0f));
		LitEnd();
	}
	float want[2] = { Expect(0, 140/255.0f, 0.1f, 0.0f, TiltNormal(tilt), &sun, true, 1.0f),
		Expect(0, 140/255.0f, 0.1f, 0.0f, TiltNormal(tilt), &sun, false, 1.0f) };
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	mat->destroy();
	FreeSun(&sun);
	bool ok = !sky && fabsf(red[0] - want[0]) < 2.0f/255.0f && fabsf(red[1] - want[1]) < 2.0f/255.0f && red[0] - red[1] > 0.05f;
	if(!ok)
		Detail("  red culling off %.4f (want %.4f), culling on %.4f (want %.4f); sky %d\n", red[0], want[0], red[1], want[1], sky);
	return Report(ok, name);
}

static bool
CheckRelitWorldUp(void)
{
	const char *name = "the hemisphere follows the world's z axis from a turned and moved camera";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Material *mat = TestMaterial();
	Geometry *geo = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 0.0f, 10.0f, 0.5f, BLACK, mat);
	Atomic *a = MakeAtomic(geo);
	PlaceCamera(90.0f, 0.0f, 0.0f, 0.0f);
	V3d look = cam->getFrame()->getLTM()->at;
	PlaceCamera(90.0f, 3.0f - 12.0f*look.x, 0.0f, -2.0f - 12.0f*look.z);
	a->getFrame()->matrix = cam->getFrame()->matrix;
	a->getFrame()->updateObjects();
	Matrix *ltm = a->getFrame()->getLTM();
	V3d local = TiltNormal(0.5f), n;
	n.x = ltm->right.x*local.x + ltm->up.x*local.y + ltm->at.x*local.z;
	n.y = ltm->right.y*local.x + ltm->up.y*local.y + ltm->at.y*local.z;
	n.z = ltm->right.z*local.x + ltm->up.z*local.y + ltm->at.z*local.z;
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 1.0f, 0.0f);
	RelitSky(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
	LitBegin(&a, 1, CULLNONE);
	float red = ScenePixel(320, 240, 0), blue = ScenePixel(320, 240, 2);
	LitEnd();
	float want = 0.5f*n.z + 0.5f;
	RelitReset();
	TiersOff();
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	DestroyAtomic(a);
	geo->destroy();
	mat->destroy();
	FreeSun(&sun);
	bool ok = fabsf(want - 0.5f) > 0.15f && fabsf(red - want) < 2.0f/255.0f && fabsf(blue - (1.0f - want)) < 2.0f/255.0f;
	if(!ok)
		Detail("  red %.3f blue %.3f, world normal z %.3f wants red %.3f\n", red, blue, n.z, want);
	return Report(ok, name);
}

static Geometry*
RestartStripGeometry(void)
{
	static const float p[10][3] = { { 3.5f, -1, 10 }, { 3.5f, 1, 10 }, { 1.5f, -1, 10 }, { 1.5f, 1, 10 }, { -3.5f, -1, 10 },
		{ -3.5f, 1, 10 }, { -2, -1, 9 }, { -2, 1, 9 }, { -0.5f, -1, 10 }, { -0.5f, 1, 10 } };
	static const uint16 strip[11] = { 0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7, 8, 9 };
	Geometry *geo = Geometry::create(0x10000, 1, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT | Geometry::TRISTRIP);
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i < 0x10000; i++){
		v[i].set(i < 10 ? p[i][0] : 0.0f, i < 10 ? p[i][1] : 0.0f, i < 10 ? p[i][2] : 10.0f);
		geo->colors[i] = GREY;
	}
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	geo->triangles[0].v[0] = 0;
	geo->triangles[0].v[1] = 1;
	geo->triangles[0].v[2] = 2;
	geo->triangles[0].matId = 0;
	geo->calculateBoundingSphere();
	geo->unlock();
	MeshHeader *mh = geo->allocateMeshes(1, 11, 0);
	mh->flags = MeshHeader::TRISTRIP;
	Mesh *m = mh->getMeshes();
	m->material = mat;
	m->numIndices = 11;
	memcpy(m->indices, strip, sizeof(strip));
	return geo;
}

static bool
CheckRelitRestart(void)
{
	const char *name = "a strip with a restart index stays off the face fetch, lit from its stream where valid and classic where it has no normal";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Geometry *geo = RestartStripGeometry();
	Atomic *a = MakeAtomic(geo);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 1.0f);
	V3d flat = { 0.0f, 0.0f, -1.0f }, slope = { -1.0f, 0.0f, -1.5f };
	float len = sqrtf(slope.x*slope.x + slope.z*slope.z);
	slope.x /= len;
	slope.z /= len;
	float c1 = 128/255.0f, v = 0.1f/1.5f;
	float wantA = Expect(0, c1, 0.0f, 0.0f, flat, &sun, true, 1.0f);
	float wantB = c1 + (Expect(0, c1, 0.0f, 0.0f, slope, &sun, true, 1.0f) - c1)*v;
	metal::StateStats before = metal::getStateStats();
	LitBegin(&a, 1, CULLNONE);
	float ra = ScenePixel(PX(2.5f, 10.0f), PY(0.0f, 10.0f), 0);
	float rb = ScenePixel(PX(-2.1f, 9.0f + 0.1f/1.5f), PY(0.0f, 9.0f + 0.1f/1.5f), 0);
	LitEnd();
	metal::StateStats after = metal::getStateStats();
	metal::InstanceDataHeader *h = (metal::InstanceDataHeader*)geo->instData;
	bool restart = h && h->stripRestart;
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	FreeSun(&sun);
	uint32 pface = after.prepassFaceDraws - before.prepassFaceDraws, rface = after.relitFaceDraws - before.relitFaceDraws;
	uint32 relit = after.relitDraws - before.relitDraws;
	bool ok = restart && pface == 0 && rface == 0 && relit > 0 && fabsf(ra - wantA) < 2.0f/255.0f &&
		fabsf(rb - wantB) < 4.0f/255.0f;
	if(!ok)
		Detail("  restart %d; prepass face draws %u, relit face draws %u, relit draws %u; flat red %.4f (want %.4f), slope red %.4f (want %.4f)\n",
			restart, pface, rface, relit, ra, wantA, rb, wantB);
	return Report(ok, name);
}

static bool
CheckRelitStatsLine(void)
{
	const char *name = "the stats line shows the relit weight and the relit draws per frame";
	metal::logStats();
	const char *tail = strstr(metal::getStatsLine(), " relit weight ");
	float weight = -1.0f, draws = -1.0f;
	bool ok = tail && sscanf(tail, " relit weight %f relit draws/frame %f", &weight, &draws) == 2 && weight == 1.0f;
	if(!ok)
		Detail("  got \"%s\"\n", metal::getStatsLine());
	return Report(ok, name);
}

static double
GpuFrameMs(Atomic **layers, int n)
{
	double t[5];
	for(int f = 0; f < 6; f++){
		metal::beginScene(cam);
		WorldBegin(SKY);
		if(metal::beginPrepass())
			metal::endPrepass();
		SetRenderState(VERTEXALPHA, 1);
		for(int i = 0; i < n; i++)
			layers[i]->render();
		SetRenderState(VERTEXALPHA, 0);
		metal::endScene(nil);
		uint64 id = metal::getFrameId();
		WorldEnd();
		for(int spin = 0; spin < 2000 && metal::getCompletedFrameId() < id; spin++)
			usleep(1000);
		if(f > 0)
			t[f - 1] = metal::lastFrameGpuMsForTest();
	}
	std::sort(t, t + 5);
	return t[2];
}

static bool
CheckRelitCost(const char *name, int samples)
{
	enum { LAYERS = 32 };
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	Geometry *geos[LAYERS];
	Atomic *as[LAYERS];
	RGBA glass = { 128, 128, 128, 254 }, col[4] = { glass, glass, glass, glass };
	for(int i = 0; i < LAYERS; i++){
		float z = 33.0f - i;
		geos[i] = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, -1.2f*z, -0.9f*z, 1.2f*z, 0.9f*z, z, col, nil,
			mat);
		as[i] = MakeAtomic(geos[i]);
	}
	EnhancedOn();
	double enhanced = GpuFrameMs(as, LAYERS);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(1.0f, 0.5f, 1.0f);
	double full = GpuFrameMs(as, LAYERS);
	RelitSet(0.5f, 0.5f, 1.0f);
	metal::setFaceFetchForTest(1);
	double fetch = GpuFrameMs(as, LAYERS);
	metal::setFaceFetchForTest(0);
	double plain = GpuFrameMs(as, LAYERS);
	RelitReset();
	TiersOff();
	for(int i = 0; i < LAYERS; i++){
		DestroyAtomic(as[i]);
		geos[i]->destroy();
	}
	mat->destroy();
	FreeSun(&sun);
	Detail("  GPU ms per frame: enhanced %.3f, modern at weight 1 %.3f, modern at weight 0.5 with the face fetch %.3f, without %.3f "
		"(32 full-screen layers at %dx%d, %d samples)\n", enhanced, full, fetch, plain, cam->frameBuffer->width,
		cam->frameBuffer->height, samples);
	return Report(enhanced > 0.0 && full > 0.0 && fetch > 0.0 && plain > 0.0, name);
}

static Geometry*
SplitBoxGeometry(void)
{
	static const int faces[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 } };
	float p[24][3];
	uint16 t[12][3];
	for(int f = 0; f < 6; f++){
		for(int k = 0; k < 4; k++){
			int c = faces[f][k];
			p[f*4 + k][0] = c & 1 ? 1.0f : -1.0f;
			p[f*4 + k][1] = c & 2 ? 1.0f : -1.0f;
			p[f*4 + k][2] = c & 4 ? 1.0f : -1.0f;
		}
		uint16 b = f*4;
		uint16 t0[3] = { b, (uint16)(b + 1), (uint16)(b + 2) }, t1[3] = { b, (uint16)(b + 2), (uint16)(b + 3) };
		memcpy(t[f*2], t0, sizeof(t0));
		memcpy(t[f*2 + 1], t1, sizeof(t1));
	}
	return MeshGeometry(p, 24, t, 12, false);
}

struct CreaseBox
{
	Sun sun;
	Geometry *geo;
	Atomic *atomic;
};

static void
MakeCreaseBox(CreaseBox *b, bool split)
{
	MakeSun(&b->sun, 0.0f);
	b->geo = split ? SplitBoxGeometry() : BoxGeometry(0.0f, 0.0f, 0.0f);
	b->atomic = MakeAtomic(b->geo);
	V3d zAxis = { 0.0f, 0.0f, 1.0f }, pos = { 2.5f, 0.0f, 10.0f };
	b->atomic->getFrame()->rotate(&zAxis, 10.0f, COMBINEREPLACE);
	b->atomic->getFrame()->translate(&pos, COMBINEPOSTCONCAT);
	b->atomic->getFrame()->updateObjects();
	RelitSet(0.0f, 0.0f, 1.0f);
}

static void
FreeCreaseBox(CreaseBox *b)
{
	RelitReset();
	DestroyAtomic(b->atomic);
	b->geo->destroy();
	FreeSun(&b->sun);
}

struct CreaseRead
{
	float front, side;
	int pixels, between;
	float got, want;
};

static void
ReadCrease(const float *red, int w, Atomic *a, CreaseRead *r)
{
	V3d ends[2], local[2] = { { -1.0f, -1.0f, -1.0f }, { -1.0f, 1.0f, -1.0f } };
	V3d::transformPoints(ends, local, 2, a->getFrame()->getLTM());
	float sx[2], sy[2];
	for(int i = 0; i < 2; i++){
		sx[i] = 320.0f*(1.0f - ends[i].x/ends[i].z);
		sy[i] = 240.0f*(1.0f - ends[i].y/(0.75f*ends[i].z));
	}
	r->front = red[PY(0.0f, 9.0f)*w + PX(2.5f, 9.0f)];
	r->side = red[PY(0.0f, 10.0f)*w + PX(1.5f, 10.0f)];
	float lo = fminf(r->front, r->side), hi = fmaxf(r->front, r->side);
	r->pixels = r->between = 0;
	r->got = r->want = 0.0f;
	int y0 = (int)ceilf(fminf(sy[0], sy[1])) + 4, y1 = (int)floorf(fmaxf(sy[0], sy[1])) - 4;
	for(int y = y0; y <= y1; y++){
		float xc = sx[0] + (sx[1] - sx[0])*(y + 0.5f - sy[0])/(sy[1] - sy[0]);
		int xi = (int)floorf(xc);
		float frac = xc - xi;
		if(frac < 0.2f || frac > 0.8f)
			continue;
		float v = red[y*w + xi];
		r->pixels++;
		if(v > lo + 3.0f/255.0f && v < hi - 3.0f/255.0f)
			r->between++;
		r->got += v;
		r->want += r->front*frac + r->side*(1.0f - frac);
	}
	if(r->pixels){
		r->got /= r->pixels;
		r->want /= r->pixels;
	}
}

static void
CompositeRed(const uint8 *px, int n, float *red, float scale)
{
	for(int i = 0; i < n; i++)
		red[i] += px[i*4 + 2]/255.0f*scale;
}

static bool
CreaseFrame(CreaseBox *b, float *red, int n)
{
	LitBegin(&b->atomic, 1, CULLNONE);
	LitEnd();
	uint8 *px = CompositeNow();
	cam->showRaster(0);
	if(px == nil)
		return false;
	CompositeRed(px, n, red, 1.0f);
	delete[] px;
	return true;
}

static bool
SplitEdge(bool stream, CreaseRead *r, uint32 *built)
{
	int w = cam->frameBuffer->width, n = w*cam->frameBuffer->height;
	float *red = new float[n]();
	CreaseBox b;
	MakeCreaseBox(&b, true);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	if(!stream)
		metal::setNormalBudgetForTest(0.0f);
	metal::InstanceStats s0 = metal::getInstanceStats();
	bool read = CreaseFrame(&b, red, n);
	metal::InstanceStats s1 = metal::getInstanceStats();
	metal::setNormalBudgetForTest(-1.0f);
	*built = s1.normalStreams - s0.normalStreams;
	ReadCrease(red, w, b.atomic, r);
	TiersOff();
	FreeCreaseBox(&b);
	delete[] red;
	return read;
}

static bool
SplitEdgeBlends(const CreaseRead &r)
{
	return fabsf(r.front - r.side) > 30.0f/255.0f && r.pixels >= 20 && r.between == r.pixels &&
		fabsf(r.got - r.want) < 0.05f*fabsf(r.front - r.side);
}

static bool
CheckRelitSplitEdge(void)
{
	const char *name = "at four samples two lit faces built with their own vertices blend both faces' light by coverage along their edge";
	CreaseRead lit, faced;
	uint32 litBuilt, facedBuilt;
	bool read = SplitEdge(true, &lit, &litBuilt);
	read = SplitEdge(false, &faced, &facedBuilt) && read;
	bool ok = read && litBuilt == 1 && facedBuilt == 0 && SplitEdgeBlends(lit) && SplitEdgeBlends(faced);
	Detail("  lit from the derived stream: front red %.1f side red %.1f; %d of %d crease pixels lie between them, mean %.2f against %.2f by coverage (of 255)\n",
		lit.front*255.0f, lit.side*255.0f, lit.between, lit.pixels, lit.got*255.0f, lit.want*255.0f);
	Detail("  lit by the face fetch alone: front red %.1f side red %.1f; %d of %d between, mean %.2f against %.2f; streams built %u and %u\n",
		faced.front*255.0f, faced.side*255.0f, faced.between, faced.pixels, faced.got*255.0f, faced.want*255.0f, litBuilt, facedBuilt);
	return Report(ok, name);
}

static bool
CreaseTaa(bool split, CreaseRead *msaa, CreaseRead *taa, int *jittered, int32 *aa)
{
	enum { FRAMES = 32, LAST = 8 };
	int w = cam->frameBuffer->width, n = w*cam->frameBuffer->height;
	float *red = new float[n]();
	CreaseBox b;
	MakeCreaseBox(&b, split);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	bool read = CreaseFrame(&b, red, n);
	ReadCrease(red, w, b.atomic, msaa);
	memset(red, 0, n*sizeof(float));
	metal::cameraCut();
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	metal::setJitterIndexForTest(0);
	*jittered = 0;
	for(int f = 0; f < FRAMES && read; f++){
		LitBegin(&b.atomic, 1, CULLNONE);
		LitEnd();
		if(f < FRAMES - LAST)
			continue;
		*jittered += Jittered(metal::getFrameInfo());
		uint8 *px = CompositeNow();
		read = px != nil;
		if(px)
			CompositeRed(px, n, red, 1.0f/LAST);
		delete[] px;
	}
	*aa = metal::getFrameInfo().antiAliasing;
	ReadCrease(red, w, b.atomic, taa);
	metal::cameraCut();
	TiersOff();
	FreeCreaseBox(&b);
	delete[] red;
	return read;
}

static bool
CreaseScaler(CreaseRead *r, int *jittered, int32 *upscaler)
{
	enum { FRAMES = 32, LAST = 8 };
	int w = cam->frameBuffer->width, n = w*cam->frameBuffer->height;
	float *red = new float[n]();
	CreaseBox b;
	MakeCreaseBox(&b, false);
	cam->showRaster(0);
	metal::cameraCut();
	ModernOn(metal::UPSCALE_QUALITY, metal::ANTIALIAS_TAA);
	metal::setJitterIndexForTest(0);
	*jittered = 0;
	bool read = true;
	for(int f = 0; f < FRAMES && read; f++){
		LitBegin(&b.atomic, 1, CULLNONE);
		LitEnd();
		if(f < FRAMES - LAST)
			continue;
		*jittered += Jittered(metal::getFrameInfo());
		uint8 *px = CompositeNow();
		read = px != nil;
		if(px)
			CompositeRed(px, n, red, 1.0f/LAST);
		delete[] px;
	}
	*upscaler = metal::getFrameInfo().upscaler;
	ReadCrease(red, w, b.atomic, r);
	metal::cameraCut();
	TiersOff();
	FreeCreaseBox(&b);
	delete[] red;
	return read;
}

static bool
CheckRelitCreaseTaa(void)
{
	const char *name = "at four samples TAA at native resolves the crease between two lit faces that share their vertices to a blend of both by coverage";
	CreaseRead msaa, taa, splitMsaa, splitTaa;
	int jittered, splitJittered;
	int32 aa, splitAa;
	bool read = CreaseTaa(false, &msaa, &taa, &jittered, &aa);
	read = CreaseTaa(true, &splitMsaa, &splitTaa, &splitJittered, &splitAa) && read;
	Detail("  multisampling alone: %d of %d crease pixels lie between the faces' red %.1f and %.1f, mean %.2f against %.2f by coverage (of 255)\n",
		msaa.between, msaa.pixels, msaa.front*255.0f, msaa.side*255.0f, msaa.got*255.0f, msaa.want*255.0f);
	Detail("  TAA over the last 8 frames (%d jittered): %d of %d crease pixels lie between %.1f and %.1f, mean %.2f against %.2f by coverage\n",
		jittered, taa.between, taa.pixels, taa.front*255.0f, taa.side*255.0f, taa.got*255.0f, taa.want*255.0f);
	Detail("  the same faces with their own vertices under TAA (%d jittered): %d of %d between, mean %.2f against %.2f\n",
		splitJittered, splitTaa.between, splitTaa.pixels, splitTaa.got*255.0f, splitTaa.want*255.0f);
	CreaseRead scaled;
	int scaledJittered;
	int32 upscaler;
	if(CreaseScaler(&scaled, &scaledJittered, &upscaler))
		Detail("  temporal scaler (%s, %d jittered): %d of %d crease pixels between the faces (by more than 3 of 255), mean %.2f against coverage %.2f\n",
			upscaler == metal::UPSCALER_TEMPORAL ? "temporal" : "not temporal", scaledJittered, scaled.between, scaled.pixels,
			scaled.got*255.0f, scaled.want*255.0f);
	else
		Detail("  temporal scaler: the composite could not be read\n");
	bool ok = read && aa == metal::ANTIALIAS_TAA && jittered >= 6 && fabsf(taa.front - taa.side) > 30.0f/255.0f &&
		taa.pixels >= 20 && taa.between == taa.pixels && fabsf(taa.got - taa.want) < 0.25f*fabsf(taa.front - taa.side);
	return Report(ok, name);
}

static bool
CheckTaaFaintReactive(void)
{
	const char *name = "TAA keeps anti-aliasing a surface edge under a faint reactive fringe, as the soft edge of a glow lays down";
	RGBA dark = { 12, 12, 16, 255 }, white = { 255, 255, 255, 255 };
	RGBA dc[4] = { dark, dark, dark, dark }, wc[4] = { white, white, white, white };
	Material *mat = Material::create();
	Geometry *bg = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 8.0f, 6.0f, -8.0f, -6.0f, 20.0f, dc, nil, mat);
	Geometry *fg = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 1.0f, 1.0f, -1.0f, -1.0f, 10.0f, wc, nil, mat);
	Atomic *back = MakeAtomic(bg), *front = MakeAtomic(fg);
	int jPlain, jFringe;
	float plain = FringeEdgeSwing(back, front, false, &jPlain);
	float fringe = FringeEdgeSwing(back, front, true, &jFringe);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	DestroyAtomic(back);
	DestroyAtomic(front);
	bg->destroy();
	fg->destroy();
	mat->destroy();
	bool ok = jPlain >= 6 && jFringe >= 6 && fringe < 3.0f*plain + 0.02f;
	Detail("  over the last 8 frames (%d and %d jittered) the edge pixels swing by up to %.3f in red without the fringe and %.3f under it\n",
		jPlain, jFringe, plain, fringe);
	return Report(ok, name);
}

struct KeptFrame
{
	int32 tier;
	uint32 relit, face, prepass, equal;
};

static void
RelitKeptFrame(RelitScene *s, bool32 relit, KeptFrame *k)
{
	metal::StateStats before = metal::getStateStats();
	metal::beginScene(cam);
	metal::setRelitTwoSided(1);
	WorldBegin(SKY);
	metal::setRelitDraw(relit);
	k->tier = metal::getFrameInfo().tier;
	SetRenderState(CULLMODE, CULLNONE);
	if(metal::beginPrepass()){
		DrawRelitScene(s);
		metal::endPrepass();
	}
	SetRenderState(CULLMODE, CULLNONE);
	DrawRelitScene(s);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats after = metal::getStateStats();
	k->relit = after.relitDraws - before.relitDraws;
	k->face = after.relitFaceDraws - before.relitFaceDraws;
	k->prepass = after.prepassDraws - before.prepassDraws;
	k->equal = after.prepassEqual - before.prepassEqual;
}

static bool
CheckRelitKeptBaked(void)
{
	const char *name = "a draw the host keeps baked draws exactly its enhanced frame at weight 0.5 and still takes part in the prepass";
	RelitScene s;
	MakeRelitScene(&s);
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	cam->showRaster(0);
	EnhancedOn();
	int32 enhancedTier = RelitSceneFrame(&s, CULLNONE);
	uint8 *enhanced = CompositeNow();
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.5f, 0.5f, 1.0f);
	KeptFrame kept, lit;
	RelitKeptFrame(&s, 0, &kept);
	uint8 *modern = CompositeNow();
	cam->showRaster(0);
	RelitKeptFrame(&s, 1, &lit);
	cam->showRaster(0);
	RelitReset();
	TiersOff();
	FreeRelitScene(&s);
	bool same = FramesEqual(enhanced, modern, w, h, "kept baked against enhanced");
	bool ok = same && kept.relit == 0 && kept.face == 0 && lit.relit > 0 && enhancedTier == metal::TIER_ENHANCED &&
		kept.tier == metal::TIER_MODERN && lit.tier == metal::TIER_MODERN && kept.prepass > 0 && kept.prepass == lit.prepass &&
		kept.equal == lit.equal;
	if(!ok)
		Detail("  kept: relit %u face %u prepass %u equal %u; relit: relit %u face %u prepass %u equal %u; tiers %d %d %d\n",
			kept.relit, kept.face, kept.prepass, kept.equal, lit.relit, lit.face, lit.prepass, lit.equal, enhancedTier, kept.tier,
			lit.tier);
	delete[] enhanced;
	delete[] modern;
	return Report(ok, name);
}

static int32
TwoQuadFrame(Atomic **as, metal::StateStats *before, metal::StateStats *after)
{
	*before = metal::getStateStats();
	LitBegin(as, 2, CULLBACK);
	int32 tier = metal::getFrameInfo().tier;
	LitEnd();
	*after = metal::getStateStats();
	return tier;
}

static bool
CheckRelitUnflagged(void)
{
	const char *name = "the default render path keeps prelit geometry without the light flag baked below weight 1";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Material *mat = TestMaterial();
	float tilt = FacingTilt(&sun);
	Geometry *geos[2] = { TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT, -2.5f, 12.0f, tilt, STONE, mat),
		TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 2.5f, 12.0f, tilt, STONE, mat) };
	Atomic *as[2] = { MakeAtomic(geos[0]), MakeAtomic(geos[1]) };
	int w = cam->frameBuffer->width, y = PY(0.0f, 12.0f), xs[2] = { PX(-2.5f, 12.0f), PX(2.5f, 12.0f) };
	metal::StateStats b0, b1, m0, m1;
	cam->showRaster(0);
	EnhancedOn();
	int32 enhancedTier = TwoQuadFrame(as, &b0, &b1);
	uint8 *enhanced = CompositeNow();
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.5f, 1.0f);
	int32 modernTier = TwoQuadFrame(as, &m0, &m1);
	uint8 *modern = CompositeNow();
	cam->showRaster(0);
	RelitReset();
	TiersOff();
	for(int i = 0; i < 2; i++){
		DestroyAtomic(as[i]);
		geos[i]->destroy();
	}
	mat->destroy();
	FreeSun(&sun);
	bool ok = false;
	if(enhanced && modern){
		const uint8 *e0 = enhanced + (y*w + xs[0])*4, *n0 = modern + (y*w + xs[0])*4;
		const uint8 *e1 = enhanced + (y*w + xs[1])*4, *n1 = modern + (y*w + xs[1])*4;
		uint32 relit = m1.relitDraws - m0.relitDraws;
		ok = e0[0] == n0[0] && e0[1] == n0[1] && e0[2] == n0[2] && n1[2] > e1[2] + 13 && relit == 1 &&
			enhancedTier == metal::TIER_ENHANCED && modernTier == metal::TIER_MODERN;
		if(!ok)
			Detail("  unflagged %u,%u,%u against enhanced %u,%u,%u; flagged red %u against %u; relit draws %u; tiers %d %d\n",
				n0[2], n0[1], n0[0], e0[2], e0[1], e0[0], n1[2], e1[2], relit, enhancedTier, modernTier);
	}
	delete[] enhanced;
	delete[] modern;
	return Report(ok, name);
}

static void
HostLoopDraw(Atomic *a)
{
	a->getPipeline()->instance(a);
	metal::InstanceDataHeader *h = (metal::InstanceDataHeader*)a->geometry->instData;
	Geometry *geo = a->geometry;
	metal::setWorldMatrix(a->getFrame()->getLTM(), a);
	metal::setupVertexInput(h);
	Material *m = h->inst->material;
	metal::setMaterial(geo->flags, m->color, m->surfaceProps);
	metal::setTexture(0, nil);
	SetRenderState(VERTEXALPHA, 0);
	metal::defaultShader->use(metal::drawVariant(0));
	metal::drawInst(h, h->inst);
	metal::teardownVertexInput(h);
}

static bool
CheckRelitOptOutScoped(void)
{
	const char *name = "the relit opt-out does not carry to the next object or the next frame";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	float away = AwayTilt(&sun);
	Geometry *plain = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT, -3.0f, 12.0f, away, STONE, mat);
	Geometry *flagged = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 0.0f, 12.0f, away, STONE, mat);
	Atomic *pa = MakeAtomic(plain), *fa = MakeAtomic(flagged);
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 1.0f);
	metal::StateStats s0 = metal::getStateStats();
	metal::beginScene(cam);
	WorldBegin(SKY);
	int32 tier1 = metal::getFrameInfo().tier;
	SetRenderState(CULLMODE, CULLNONE);
	pa->render();
	HostLoopDraw(fa);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats s1 = metal::getStateStats();
	metal::beginScene(cam);
	WorldBegin(SKY);
	metal::setRelitDraw(0);
	metal::setRelitTwoSided(1);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats s2 = metal::getStateStats();
	metal::beginScene(cam);
	WorldBegin(SKY);
	int32 tier2 = metal::getFrameInfo().tier;
	SetRenderState(CULLMODE, CULLNONE);
	fa->render();
	float red = ScenePixel(PX(0.0f, 12.0f), PY(0.0f, 12.0f), 0);
	metal::endScene(nil);
	WorldEnd();
	metal::StateStats s3 = metal::getStateStats();
	float want = Expect(0, 140/255.0f, 0.1f, 0.0f, TiltNormal(away), &sun, false, 1.0f);
	float both = Expect(0, 140/255.0f, 0.1f, 0.0f, TiltNormal(away), &sun, true, 1.0f);
	RelitReset();
	TiersOff();
	DestroyAtomic(pa);
	DestroyAtomic(fa);
	plain->destroy();
	flagged->destroy();
	mat->destroy();
	FreeSun(&sun);
	uint32 objectRelit = s1.relitDraws - s0.relitDraws, frameRelit = s3.relitDraws - s2.relitDraws;
	bool object = objectRelit == 1 && tier1 == metal::TIER_MODERN;
	bool frame = frameRelit == 1 && tier2 == metal::TIER_MODERN && fabsf(red - want) < 2.0f/255.0f && fabsf(both - want) > 0.05f;
	if(!object)
		Detail("  next object: relit draws %u (want 1), tier %d\n", objectRelit, tier1);
	if(!frame)
		Detail("  next frame: relit draws %u (want 1), red %.4f (want %.4f, two-sided %.4f), tier %d\n", frameRelit, red, want,
			both, tier2);
	return Report(object && frame, name);
}

static float
HostSidedFrame(Atomic *a, int32 cull, bool32 twoSided, bool *sky)
{
	metal::beginScene(cam);
	WorldBegin(SKY);
	metal::setRelitTwoSided(twoSided);
	SetRenderState(CULLMODE, cull);
	if(metal::beginPrepass()){
		a->render();
		metal::endPrepass();
	}
	SetRenderState(CULLMODE, cull);
	a->render();
	float red = ScenePixel(PX(0.0f, 12.0f), PY(0.0f, 12.0f), 0);
	*sky = *sky || IsSky(PX(0.0f, 12.0f), PY(0.0f, 12.0f));
	metal::endScene(nil);
	WorldEnd();
	return red;
}

static bool
CheckRelitTwoSidedHost(void)
{
	const char *name = "the sun lights a thin prelit surface from behind only when the host marks the draw two-sided, whatever the cull state";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	float tilt = AwayTilt(&sun);
	Geometry *geo = TiltedQuad(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 0.0f, 12.0f, tilt, STONE, mat);
	Atomic *a = MakeAtomic(geo);
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 1.0f);
	bool sky = false;
	float off = HostSidedFrame(a, CULLNONE, 0, &sky);
	float on = HostSidedFrame(a, CULLBACK, 1, &sky);
	float wantOff = Expect(0, 140/255.0f, 0.1f, 0.0f, TiltNormal(tilt), &sun, false, 1.0f);
	float wantOn = Expect(0, 140/255.0f, 0.1f, 0.0f, TiltNormal(tilt), &sun, true, 1.0f);
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	mat->destroy();
	FreeSun(&sun);
	bool ok = !sky && fabsf(off - wantOff) < 2.0f/255.0f && fabsf(on - wantOn) < 2.0f/255.0f && on - off > 0.05f;
	if(!ok)
		Detail("  culling off unmarked %.4f (want %.4f), culling on marked %.4f (want %.4f); sky %d\n", off, wantOff, on, wantOn, sky);
	return Report(ok, name);
}

static bool
CheckRelitShadowConstant(void)
{
	const char *name = "no relit draw reads the shadow factor until a shadow source exists";
	RelitScene s;
	MakeRelitScene(&s);
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.5f, 0.5f, 1.0f);
	metal::setShadowFactorForTest(-1.0f);
	metal::StateStats a0 = metal::getStateStats();
	int32 tier1 = RelitSceneFrame(&s, CULLNONE);
	cam->showRaster(0);
	metal::StateStats a1 = metal::getStateStats();
	metal::setShadowFactorForTest(0.5f);
	int32 tier2 = RelitSceneFrame(&s, CULLNONE);
	cam->showRaster(0);
	metal::StateStats a2 = metal::getStateStats();
	RelitReset();
	TiersOff();
	FreeRelitScene(&s);
	uint32 relit1 = a1.relitDraws - a0.relitDraws, shadow1 = a1.relitShadowDraws - a0.relitShadowDraws;
	uint32 relit2 = a2.relitDraws - a1.relitDraws, shadow2 = a2.relitShadowDraws - a1.relitShadowDraws;
	uint32 late = a2.pipelinesLate - a1.pipelinesLate;
	bool ok = relit1 > 0 && shadow1 == 0 && relit2 > 0 && shadow2 == relit2 && late == 0 && tier1 == metal::TIER_MODERN &&
		tier2 == metal::TIER_MODERN;
	if(!ok)
		Detail("  without a source: relit %u shadow %u; with one: relit %u shadow %u; late %u; tiers %d %d\n", relit1, shadow1,
			relit2, shadow2, late, tier1, tier2);
	return Report(ok, name);
}

static uint8*
SkipFrame(Atomic **as, int n, bool32 skip, uint32 *face)
{
	metal::setFaceSkipForTest(skip);
	metal::StateStats before = metal::getStateStats();
	LitBegin(as, n, CULLNONE);
	LitEnd();
	metal::StateStats after = metal::getStateStats();
	*face = after.relitFaceDraws - before.relitFaceDraws;
	uint8 *px = CompositeNow();
	cam->showRaster(0);
	return px;
}

static bool
CheckRelitFaceSkip(void)
{
	const char *name = "where the derived normal is fully valid the forward light skips the face fetch and draws exactly the values the fetch gives";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Geometry *geos[2] = { CylinderGeometry(1.0f, 10.0f, false), CylinderGeometry(1.0f, 10.0f, true) };
	Atomic *as[2] = { MakeAtomic(geos[0]), MakeAtomic(geos[1]) };
	MoveAtomic(as[0], -1.5f, 0.0f, 0.0f);
	MoveAtomic(as[1], 1.5f, 0.0f, 0.0f);
	int w = cam->frameBuffer->width, h = cam->frameBuffer->height;
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.5f, 1.0f);
	uint32 warmFace, faceA, faceB;
	delete[] SkipFrame(as, 2, 1, &warmFace);
	uint32 pending = metal::getInstanceStats().normalPending;
	uint8 *a = SkipFrame(as, 2, 1, &faceA);
	uint8 *b = SkipFrame(as, 2, 0, &faceB);
	metal::setFaceSkipForTest(1);
	RelitReset();
	TiersOff();
	for(int i = 0; i < 2; i++){
		DestroyAtomic(as[i]);
		geos[i]->destroy();
	}
	FreeSun(&sun);
	bool same = FramesEqual(a, b, w, h, "skipping against fetching");
	bool ok = same && pending == 0 && faceA == faceB && faceA > 0;
	if(!ok)
		Detail("  pending streams %u; face draws %u and %u\n", pending, faceA, faceB);
	delete[] a;
	delete[] b;
	return Report(ok, name);
}

static bool
CheckRelitFetchTransform(void)
{
	const char *name = "a prelit box under a turned, mirrored and unevenly scaled world matrix is lit by its own face normal through the face fetch";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Geometry *geo = BoxGeometry(0.0f, 0.0f, 0.0f);
	Atomic *a = MakeAtomic(geo);
	V3d scale = { 1.5f, -0.75f, 1.25f }, yAxis = { 0.0f, 1.0f, 0.0f }, pos = { 0.0f, 0.0f, 10.0f };
	a->getFrame()->scale(&scale, COMBINEREPLACE);
	a->getFrame()->rotate(&yAxis, 25.0f, COMBINEPOSTCONCAT);
	a->getFrame()->translate(&pos, COMBINEPOSTCONCAT);
	a->getFrame()->updateObjects();
	V3d local[3] = { { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, -1.0f } }, p[3];
	V3d::transformPoints(p, local, 3, a->getFrame()->getLTM());
	V3d e1 = sub(p[1], p[0]), e2 = sub(p[2], p[0]);
	V3d n = normalize(cross(e1, e2));
	int x = PX(p[0].x, p[0].z), y = PY(p[0].y, p[0].z);
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 1.0f);
	float want = Expect(0, 128/255.0f, 0.0f, 0.0f, n, &sun, true, 1.0f);
	V3d turned = { -n.x, n.y, n.z };
	float other = Expect(0, 128/255.0f, 0.0f, 0.0f, turned, &sun, true, 1.0f);
	metal::StateStats before = metal::getStateStats();
	LitBegin(&a, 1, CULLNONE);
	float red = ScenePixel(x, y, 0);
	bool sky = IsSky(x, y);
	LitEnd();
	metal::StateStats after = metal::getStateStats();
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	FreeSun(&sun);
	uint32 face = after.relitFaceDraws - before.relitFaceDraws;
	bool ok = !sky && face == 1 && fabsf(red - want) < 2.0f/255.0f && fabsf(other - want) > 0.05f;
	if(!ok)
		Detail("  red %.4f at %d,%d (want %.4f, the mirrored turn would give %.4f); face draws %u; sky %d\n", red, x, y, want, other,
			face, sky);
	return Report(ok, name);
}

static bool
CheckRelitNoFetchFallback(void)
{
	const char *name = "without the face fetch the forward pass lights only where the derived normal is valid and the prepass still writes a geometric normal";
	Sun sun;
	MakeSun(&sun, 0.0f);
	Geometry *geo = BoxGeometry(2.5f, 0.0f, 10.0f);
	Atomic *a = MakeAtomic(geo);
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.5f, 1.0f);
	metal::setFaceFetchForTest(0);
	V3d front = { 0.0f, 0.0f, -1.0f };
	float want = Expect(0, 128/255.0f, 0.0f, 1.0f, front, &sun, true, 1.0f);
	float lit = Expect(0, 128/255.0f, 0.0f, 0.0f, front, &sun, true, 1.0f);
	int x = PX(2.5f, 9.0f), y = PY(0.0f, 9.0f);
	LitBegin(&a, 1, CULLNONE);
	int32 tier = metal::getFrameInfo().tier;
	float red = ScenePixel(x, y, 0);
	metal::endScene(nil);
	float n[3] = { -9.0f, -9.0f, -9.0f }, m[2] = {}, d = -1.0f;
	bool read = metal::readPrepassForTest(x, y, n, m, &d);
	WorldEnd();
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	FreeSun(&sun);
	bool normal = read && fabsf(n[0]) < 0.02f && fabsf(n[1]) < 0.02f && fabsf(n[2] + 1.0f) < 0.02f;
	bool ok = normal && fabsf(red - want) < 1.0f/255.0f && fabsf(lit - want) > 0.05f && tier == metal::TIER_MODERN;
	if(!ok)
		Detail("  front red %.4f (want %.4f, lit would be %.4f); prepass normal %.3f,%.3f,%.3f read %d; tier %d\n", red, want, lit,
			n[0], n[1], n[2], read, tier);
	return Report(ok, name);
}

static float
SunStartFrame(Atomic *a, Sun *sun, bool blacken)
{
	metal::beginScene(cam);
	WorldBegin(SKY);
	if(blacken)
		sun->sun->setColor(0.0f, 0.0f, 0.0f);
	SetRenderState(CULLMODE, CULLBACK);
	if(metal::beginPrepass()){
		a->render();
		metal::endPrepass();
	}
	SetRenderState(CULLMODE, CULLBACK);
	a->render();
	float red = ScenePixel(PX(0.0f, 12.0f), PY(0.0f, 12.0f), 0);
	metal::endScene(nil);
	WorldEnd();
	return red;
}

static bool
CheckRelitSunAtStart(void)
{
	const char *name = "a relit prelit draw takes the sun as it was when the scene began";
	Sun sun;
	MakeSun(&sun, 0.1f);
	Material *mat = TestMaterial();
	Geometry *geo;
	Atomic *a = FacingQuad(&geo, &sun, STONE, mat);
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	RelitSet(0.0f, 0.0f, 0.5f);
	float want = Expect(0, 140/255.0f, 0.1f, 0.0f, TiltNormal(FacingTilt(&sun)), &sun, false, 1.0f);
	float started = SunStartFrame(a, &sun, true);
	float black = SunStartFrame(a, &sun, false);
	sun.sun->setColor(0.8f, 0.6f, 0.5f);
	RelitReset();
	TiersOff();
	DestroyAtomic(a);
	geo->destroy();
	mat->destroy();
	FreeSun(&sun);
	bool ok = fabsf(started - want) < 2.0f/255.0f && started > black + 0.05f;
	if(!ok)
		Detail("  red %.4f (want %.4f), with a black sun from the start %.4f\n", started, want, black);
	return Report(ok, name);
}

static bool
StatsLineEnds(const char *line, const char *end)
{
	size_t n = strlen(line), e = strlen(end);
	while(n > 0 && line[n-1] == '\n')
		n--;
	return n >= e && strncmp(line + n - e, end, e) == 0;
}

static bool
CheckRelitStatsSun(void)
{
	const char *name = "the stats line names the relit weight and whether a sun lights the frame";
	metal::setSunLight(nil);
	metal::logStats();
	bool none = StatsLineEnds(metal::getStatsLine(), " sun off");
	if(!none)
		Detail("  no sun: \"%s\"\n", metal::getStatsLine());
	Sun sun;
	MakeSun(&sun, 0.0f);
	RelitSet(0.5f, 0.5f, 1.0f);
	metal::logStats();
	const char *tail = strstr(metal::getStatsLine(), " relit weight ");
	float weight = -1.0f;
	bool lit = tail && sscanf(tail, " relit weight %f", &weight) == 1 && fabsf(weight - 0.5f) < 1e-6f &&
		strstr(tail, " relit weight 0.50 ") && StatsLineEnds(metal::getStatsLine(), " sun on");
	if(!lit)
		Detail("  sun at 0.5: \"%s\"\n", metal::getStatsLine());
	RelitSet(0.5f, 0.5f, 0.0f);
	metal::logStats();
	bool scaled = StatsLineEnds(metal::getStatsLine(), " sun off");
	if(!scaled)
		Detail("  sun scale 0: \"%s\"\n", metal::getStatsLine());
	RelitReset();
	FreeSun(&sun);
	return Report(none && lit && scaled, name);
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
	CheckPrepassSplit("a prepass split in two keeps the first segment's normal and motion at one sample");
	CheckReusedAddress();
	CheckJitter();
	CheckJitterKeyed();
	CheckTemporalScaler();
	CheckTemporalAboveWhite();
	CheckTemporalConverges();
	CheckTemporalMotion("a camera panning over a textured wall through the temporal scaler matches the native frame as a still one does (motion sign)",
		metal::UPSCALE_QUALITY, 2.5f, 0.0f);
	CheckReactiveMask();
	CheckReactivePassReused();
	CheckPrepassValid("a modern frame without prepass draws is marked invalid and resets the temporal history on it and the next frame",
		metal::UPSCALE_QUALITY);
	CheckTemporalResets();
	CheckTemporalIm2D();
	CheckIm2DJitterCoverage();
	CheckJitterStarts("after a reset the jitter starts on the third frame: a reset frame and a frame with history are unjittered",
		metal::UPSCALE_QUALITY);
	CheckJitterNeedsHistory("a modern frame sequence without the prepass bracket is never jittered", false);
	CheckJitterNeedsHistory("a host that brackets the prepass on alternate frames is never jittered", true);
	CheckJitterCutInFrame();
	CheckJitterNotApplied();
	CheckIm2DJitterNotApplied();
	CheckJitterSubRaster();
	CheckTemporalLogs();
	CheckTaaStill();
	CheckTaaMovingEdge();
	CheckTaaMoving();
	CheckTaaNanHistory();
	CheckTemporalMotion("a camera panning over a textured wall through TAA at native matches the native frame as a still one does (reprojection)",
		metal::UPSCALE_NATIVE, 0.0f, 0.04f);
	CheckTaaReactive();
	CheckTaaFailure();
	CheckTaaTextures();
	CheckPrepassValid("with TAA at native, a frame without prepass draws and the frame after it resolve without history",
		metal::UPSCALE_NATIVE);
	CheckTaaResets();
	CheckJitterStarts("with TAA at native the jitter follows the same rule: a reset frame and a frame with history are unjittered",
		metal::UPSCALE_NATIVE);
	CheckTaaOffscreen();
	CheckTaaHighlight();
	CheckTaaFaintReactive();
	CheckDerivedBox();
	CheckDerivedCrease("both pixels beside a crease edge shared by two triangles read their own face normal");
	CheckDerivedSecondMesh();
	CheckSmoothInterpolated();
	CheckDerivedCylinder("a cylinder split at a seam reads a continuous normal across the seam in the prepass", 1.0f);
	CheckDerivedCylinder("a cylinder drawn at half scale still reads a continuous normal across its seam", 0.5f);
	CheckDerivedRim();
	CheckDerivedRidge();
	CheckNormalStreamModernOnly();
	CheckNormalStreamRebuilt();
	CheckNormalBudget();
	CheckNormalBudgetCut();
	CheckNormalQueueFree();
	CheckNormalStatsLine();
	RelitIdentity("modern relit frames at weight 1 draw exactly the enhanced frame at one sample", 1.0f, true);
	RelitIdentity("a NaN prelit weight is clamped on the CPU and draws exactly the enhanced frame", NAN, true);
	RelitIdentity("at weight 1 a modern frame draws prelit geometry with the plain variant and only geometry with its own normals relit",
		1.0f, false);
	RelitIdentity("a prelit weight of 0.9999999 counts as 1: prelit geometry takes the plain variant and the frame equals the enhanced one",
		0.9999999f, false);
	CheckRelitContinuity();
	CheckRelitLightsUntagged();
	CheckRelitTiersUntouched();
	CheckRelitWeight();
	CheckRelitFlatBox();
	CheckRelitCrease();
	CheckRelitBlended();
	CheckRelitWaiting();
	CheckRelitShadowSun();
	CheckRelitSunClamp();
	CheckRelitOwnNormals();
	CheckRelitTwoSided();
	CheckRelitWorldUp();
	CheckRelitRestart();
	CheckRelitStatsLine();
	CheckRelitKeptBaked();
	CheckRelitUnflagged();
	CheckRelitOptOutScoped();
	CheckRelitTwoSidedHost();
	CheckRelitShadowConstant();
	CheckRelitFaceSkip();
	CheckRelitFetchTransform();
	CheckRelitNoFetchFallback();
	CheckRelitSunAtStart();
	CheckRelitStatsSun();
	CheckRelitCost("the GPU time of a modern frame of relit layers is measured with the face fetch on and off", 1);
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

static int32
ExitStatsFrame(metal::FrameInfo *f)
{
	cam->showRaster(0);
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	metal::beginScene(cam);
	WorldBegin(SKY);
	*f = metal::getFrameInfo();
	if(metal::beginPrepass())
		metal::endPrepass();
	metal::endScene(nil);
	WorldEnd();
	TiersOff();
	return f->tier;
}

static bool
CheckStatsAtExit(int32 tier, const metal::FrameInfo &f)
{
	const char *name = "the stats line logged at shutdown carries the last scene frame's values";
	const char *line = metal::getStatsLine();
	char size[64];
	snprintf(size, sizeof(size), " internal %dx%d ", f.internalWidth, f.internalHeight);
	bool ok = tier == metal::TIER_MODERN && f.internalWidth > 0 && strstr(line, " at exit") && strstr(line, " tier modern ") &&
		strstr(line, size);
	if(!ok)
		Detail("  tier %d, internal %dx%d; got \"%s\"\n", tier, f.internalWidth, f.internalHeight, line);
	return Report(ok, name);
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
	bool reactive = metal::reactivePassForTest() != nil;
	if(!reactive)
		Detail("  no reactive pass after the restart, before any frame\n");
	Report(reactive, "an engine start builds the reactive mask pipelines before any temporal frame needs them");
	WorldOpen(cam);
	PrepassIdentity(name);
	CheckPrepassSampleZero();
	CheckPrepassSplit("a prepass split in two keeps the first segment's normal and motion at four samples");
	CheckDerivedCrease("both pixels beside a crease edge shared by two triangles read their own face normal at four samples");
	RelitIdentity("modern relit frames at weight 1 draw exactly the enhanced frame at four samples", 1.0f, true);
	CheckRelitSplitEdge();
	CheckRelitCreaseTaa();
	CheckRelitCost("the GPU time of a modern frame of relit layers is measured with the face fetch on and off at four samples", 4);
	metal::FrameInfo exitFrame;
	int32 exitTier = ExitStatsFrame(&exitFrame);
	WorldClose();
	if(!restart(1))
		Report(false, "the engine restarts at one sample after the modern checks");
	cam = camera();
	CheckStatsAtExit(exitTier, exitFrame);
	return failures;
}
