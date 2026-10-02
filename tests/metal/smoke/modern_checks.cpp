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

static bool
CheckTaaStill(void)
{
	const char *name = "TAA at native converges a still edge on its true sub-pixel position, holds it, leaves flat colour alone and drops its history on a cut";
	Scene3D s;
	MakeScene(&s);
	float cx = 0.0125f, cy = 0.04f;
	float wantX = 320.0f*(1.0f + (1.0f + cx)/10.0f), wantY = 240.0f*(1.0f - (1.5f - cy)/7.5f);
	int rowY = PY(0.25f - cy, 10.0f), rowX0 = (int)wantX - RUN/2, colX = PX(0.5f, 10.0f), colY0 = (int)wantY - RUN/2;
	EdgeProbe plain, taa, last, cut;
	PlaceCamera(0.0f, cx, cy, 0.0f);
	metal::cameraCut();
	ModernOn(metal::UPSCALE_NATIVE, metal::ANTIALIAS_OFF);
	Frames(&s, 2);
	ProbeEdges(&plain, rowY, rowX0, colX, colY0);
	SetSettings(metal::TIER_MODERN, metal::UPSCALE_NATIVE, metal::ANTIALIAS_TAA);
	metal::setJitterIndexForTest(0);
	float drift = 0.0f, meanX = 0.0f, meanY = 0.0f;
	plain.x = AreaEdge(plain.row, plain.row, rowX0);
	plain.y = AreaEdge(plain.col, plain.col, colY0);
	for(int f = 0; f < 32; f++){
		Frames(&s, 1);
		ProbeEdges(&taa, rowY, rowX0, colX, colY0);
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
	Frames(&s, 1);
	ProbeEdges(&cut, rowY, rowX0, colX, colY0);
	PlaceCamera(0.0f, 0.0f, 0.0f, 0.0f);
	metal::cameraCut();
	TiersOff();
	FreeScene(&s);
	float dx = fabsf(meanX - wantX), dy = fabsf(meanY - wantY), px = fabsf(plain.x - wantX), py = fabsf(plain.y - wantY);
	int partialX = PartialPixels(plain.row, taa.row), partialY = PartialPixels(plain.col, taa.col);
	bool flat = SameRuns(taa, plain, 0.01f, true), cutPlain = SameRuns(cut, plain, 0.01f, false);
	bool ok = fi.antiAliasing == metal::ANTIALIAS_TAA && stats && dx < 0.05f && dy < 0.05f && dx < px && dy < py &&
		drift < 0.12f && flat && partialX >= 1 && partialX <= 3 && partialY >= 1 && partialY <= 3 && cutPlain;
	Detail("  edges at x %.3f y %.3f: one native frame x %.3f y %.3f, TAA mean over the last 8 frames x %.3f y %.3f, drift %.3f, "
		"partial pixels %d and %d\n", wantX, wantY, plain.x, plain.y, meanX, meanY, drift, partialX, partialY);
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
NoFireflyNoHalo(const HighlightRun &taa, const HighlightRun &plain, int edge, const char *what)
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
		if(bad < 0 && (i < edge - 2 || i > edge + 1))
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
	Detail("  %s: plain red %.3f to %.3f, resolved red up to %.3f, resolved luma down to %.3f (plain %.3f)\n", what, lo[0], hi[0], top,
		bottom, Luma(plain.v[RUN-1]));
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
		ok = NoFireflyNoHalo(taa, plain, RUN/2, pan ? "panning surface" : "still surface") && ok;
		ok = NoFireflyNoHalo(taaE, plainE, RUN/2, pan ? "panning emitter" : "still emitter") && ok;
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
	CheckTaaMoving();
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
	CheckPrepassSplit("a prepass split in two keeps the first segment's normal and motion at four samples");
	WorldClose();
	if(!restart(1))
		Report(false, "the engine restarts at one sample after the modern checks");
	cam = camera();
	return failures;
}
