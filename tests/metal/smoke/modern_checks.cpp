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
	WorldClose();
	return failures;
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
	WorldClose();
	if(!restart(1))
		Report(false, "the engine restarts at one sample after the modern checks");
	cam = camera();
	return failures;
}
