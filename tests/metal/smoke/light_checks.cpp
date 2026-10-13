#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metallights.h>
#include <src/metal/metalshadow.h>
#include "world_checks.h"
#include "state_checks.h"
#include "light_checks.h"
#include "late_pipelines.h"

using namespace rw;

static char details[4096];
static int detailsLen = 0;
static int failures = 0;
static Camera *lightCamera;
static metal::RenderSettings lightSaved;
static Geometry *backGeo;
static Material *backMat;
static Atomic *backdrop;

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

static void
LightsOn(int32 mode)
{
	metal::TierCaps all = { true, true, true };
	metal::setTierCapsForTest(&all);
	lightSaved = metal::getRenderSettings();
	metal::RenderSettings s = lightSaved;
	s.tier = metal::TIER_MODERN;
	s.upscale = metal::UPSCALE_NATIVE;
	s.antiAliasing = metal::ANTIALIAS_OFF;
	s.shadowQuality = metal::SHADOWQUALITY_OFF;
	s.lights = mode;
	metal::setRenderSettings(&s);
	RGBA grey = { 90, 90, 90, 255 };
	RGBA col[4] = { grey, grey, grey, grey };
	backMat = Material::create();
	backGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 60.0f, 45.0f, -60.0f, -45.0f, 90.0f, col, nil, backMat);
	backdrop = MakeAtomic(backGeo);
}

static void
LightsOff(void)
{
	DestroyAtomic(backdrop);
	backGeo->destroy();
	backMat->destroy();
	metal::setRenderSettings(&lightSaved);
	metal::setTierCapsForTest(nil);
}

static void
SetMode(int32 mode)
{
	metal::RenderSettings s = metal::getRenderSettings();
	s.lights = mode;
	metal::setRenderSettings(&s);
}

enum { WHEN_NORMAL, WHEN_EARLY, WHEN_LATE };

static bool
LightFrame(const metal::LightDesc *lights, int32 n, int32 when = WHEN_NORMAL)
{
	RGBA sky = { 64, 64, 64, 255 };
	if(when == WHEN_EARLY)
		engine->currentCamera = nil;
	metal::beginScene(lightCamera);
	if(when == WHEN_EARLY)
		metal::setLights(lights, n);
	WorldBegin(sky);
	SetRenderState(VERTEXALPHA, 0);
	if(when == WHEN_NORMAL)
		metal::setLights(lights, n);
	bool began = metal::beginPrepass() != 0;
	if(began){
		backdrop->render();
		metal::endPrepass();
	}
	if(when == WHEN_LATE)
		metal::setLights(lights, n);
	backdrop->render();
	metal::endScene(nil);
	WorldEnd();
	return began;
}

static void
TwoSceneFrame(const metal::LightDesc *lights, int32 n, int32 slots[2])
{
	RGBA sky = { 64, 64, 64, 255 };
	for(int s = 0; s < 2; s++){
		metal::beginScene(lightCamera);
		WorldBegin(sky);
		SetRenderState(VERTEXALPHA, 0);
		metal::setLights(lights, n);
		if(metal::beginPrepass()){
			backdrop->render();
			metal::endPrepass();
		}
		backdrop->render();
		metal::endScene(nil);
		slots[s] = metal::lightClusterSlotForTest();
		lightCamera->endUpdate();
	}
	lightCamera->showRaster(0);
}

static metal::LightDesc
PointLight(uint32 id, float x, float y, float z, float radius)
{
	metal::LightDesc d;
	memset(&d, 0, sizeof(d));
	d.pos[0] = x;
	d.pos[1] = y;
	d.pos[2] = z;
	d.radius = radius;
	d.color[0] = d.color[1] = d.color[2] = 1.0f;
	d.cosOuter = -2.0f;
	d.cosInner = -2.0f;
	d.id = id;
	return d;
}

static uint32
ClusterOf(float x, float y, float z, int32 *cx, int32 *cy, int32 *cz)
{
	float width = 1000.0f, height = 750.0f;
	float px = (-x/z + 1.0f)*0.5f*width, py = (1.0f - y/(z*0.75f))*0.5f*height;
	uint32 c = ltCluster(px, py, z, width, height, 1.0f, 101.0f);
	*cx = (int32)(c % LT_CLUSTER_X);
	*cy = (int32)((c / LT_CLUSTER_X) % LT_CLUSTER_Y);
	*cz = (int32)(c / (LT_CLUSTER_X*LT_CLUSTER_Y));
	return c;
}

static int32
Listed(int32 cx, int32 cy, int32 cz, uint32 *ids, int32 max)
{
	int32 count = -1;
	if(!metal::readClusterForTest(cx, cy, cz, ids, max, &count))
		return -1;
	return count;
}

static bool
InCluster(uint32 id, int32 cx, int32 cy, int32 cz)
{
	uint32 ids[LT_PER_CLUSTER];
	int32 n = Listed(cx, cy, cz, ids, LT_PER_CLUSTER);
	for(int32 i = 0; i < n && i < LT_PER_CLUSTER; i++)
		if(ids[i] == id)
			return true;
	return false;
}

static bool
CheckLightCluster(void)
{
	const char *name = "a light at a known position lands in its cluster and in no cluster outside its radius";
	metal::LightDesc l = PointLight(7, 3.0f, 4.0f, 20.0f, 2.0f);
	bool began = LightFrame(&l, 1);
	int32 cx, cy, cz, dx, dy, dz, sx, sy, sz;
	ClusterOf(3.0f, 4.0f, 20.0f, &cx, &cy, &cz);
	ClusterOf(3.0f, 4.0f, 30.0f, &dx, &dy, &dz);
	ClusterOf(3.0f, 4.0f, 10.0f, &sx, &sy, &sz);
	int32 my = LT_CLUSTER_Y - 1 - cy;
	uint32 ids[LT_PER_CLUSTER];
	int32 here = Listed(cx, cy, cz, ids, LT_PER_CLUSTER);
	uint32 first = here > 0 ? ids[0] : 0;
	int32 deeper = Listed(dx, dy, dz, ids, LT_PER_CLUSTER);
	int32 nearer = Listed(sx, sy, sz, ids, LT_PER_CLUSTER);
	int32 mirrored = Listed(cx, my, cz, ids, LT_PER_CLUSTER);
	bool ok = began && here == 1 && first == 7 && deeper == 0 && nearer == 0 && mirrored == 0 && dz > cz && sz < cz && my != cy;
	if(!ok)
		Detail("  prepass %d; cluster %d %d %d lists %d (first %u); 10 m deeper %d %d %d lists %d; 10 m nearer %d %d %d lists %d; mirrored in y %d %d %d lists %d\n",
			began, cx, cy, cz, here, first, dx, dy, dz, deeper, sx, sy, sz, nearer, cx, my, cz, mirrored);
	return Report(ok, name);
}

static bool
CheckLightSpotCluster(void)
{
	const char *name = "a spot lands only in clusters inside its cone";
	metal::LightDesc l = PointLight(9, 0.0f, 3.0f, 10.0f, 8.0f);
	l.dir[1] = -1.0f;
	l.cosOuter = cosf(30.0f*3.14159265f/180.0f);
	l.cosInner = cosf(20.0f*3.14159265f/180.0f);
	l.flags = metal::LIGHT_SPOT;
	bool began = LightFrame(&l, 1);
	int32 ux, uy, uz, wx, wy, wz;
	ClusterOf(0.3f, -0.2f, 11.0f, &ux, &uy, &uz);
	ClusterOf(6.3f, -0.2f, 11.0f, &wx, &wy, &wz);
	uint32 ids[LT_PER_CLUSTER];
	int32 under = Listed(ux, uy, uz, ids, LT_PER_CLUSTER);
	uint32 first = under > 0 ? ids[0] : 0;
	int32 aside = Listed(wx, wy, wz, ids, LT_PER_CLUSTER);
	bool ok = began && under == 1 && first == 9 && aside == 0;
	if(!ok)
		Detail("  prepass %d; cluster under the spot %d %d %d lists %d (first %u); cluster 6 m aside %d %d %d lists %d\n",
			began, ux, uy, uz, under, first, wx, wy, wz, aside);
	return Report(ok, name);
}

struct Placed
{
	float x, z, dist2;
	uint32 id;
};

static bool
CheckLightDistanceCap(void)
{
	const char *name = "of 300 lights the 256 nearest are kept and 44 counted dropped";
	enum { N = 300 };
	std::vector<Placed> placed;
	for(int j = 0; j < N; j++){
		Placed p;
		p.x = -9.0f + 0.06f*(float)j;
		p.z = 10.0f + 0.0003f*(float)j;
		p.dist2 = p.x*p.x + p.z*p.z;
		p.id = (uint32)j;
		placed.push_back(p);
	}
	std::sort(placed.begin(), placed.end(), [](const Placed &a, const Placed &b){ return a.dist2 > b.dist2; });
	std::vector<metal::LightDesc> lights;
	for(const Placed &p : placed)
		lights.push_back(PointLight(p.id, p.x, 0.0f, p.z, 0.05f));
	bool began = LightFrame(lights.data(), N);
	metal::LightStats st = metal::getLightStats();
	const Placed &farthest = placed[0], &lastDropped = placed[43], &firstKept = placed[44], &nearest = placed[N - 1];
	const Placed *probe[4] = { &farthest, &lastDropped, &firstKept, &nearest };
	bool present[4];
	for(int i = 0; i < 4; i++){
		int32 cx, cy, cz;
		ClusterOf(probe[i]->x, 0.0f, probe[i]->z, &cx, &cy, &cz);
		present[i] = InCluster(probe[i]->id, cx, cy, cz);
	}
	bool ok = began && st.submitted == N && st.kept == 256 && st.dropped == 44 && !present[0] && !present[1] && present[2] && present[3];
	if(!ok)
		Detail("  prepass %d; submitted %u kept %u dropped %u (want 300 256 44); listed in own cluster: farthest %d, 44th farthest %d, 45th farthest %d, nearest %d (want 0 0 1 1)\n",
			began, st.submitted, st.kept, st.dropped, present[0], present[1], present[2], present[3]);
	return Report(ok, name);
}

static bool
CheckLightNoPass(void)
{
	const char *name = "a frame with no lights and a frame with LIGHTS off run no cluster pass";
	metal::LightDesc l = PointLight(3, 0.0f, 0.0f, 20.0f, 2.0f);
	int32 p0 = metal::lightClusterPassesForTest();
	LightFrame(nil, 0);
	int32 p1 = metal::lightClusterPassesForTest();
	LightFrame(&l, 1);
	int32 p2 = metal::lightClusterPassesForTest();
	SetMode(metal::LIGHTS_OFF);
	LightFrame(&l, 1);
	metal::LightStats off = metal::getLightStats();
	int32 p3 = metal::lightClusterPassesForTest();
	SetMode(metal::LIGHTS_ON);
	LightFrame(&l, 1);
	int32 p4 = metal::lightClusterPassesForTest();
	metal::LightStats on = metal::getLightStats();
	bool ok = p1 == p0 && p2 == p1 + 1 && p3 == p2 && p4 == p3 + 1 && off.submitted == 0 && off.kept == 0 && on.kept == 1;
	if(!ok)
		Detail("  passes %d %d %d %d %d (want n, n, n+1, n+1, n+2); LIGHTS off submitted %u kept %u; on kept %u\n",
			p0, p1, p2, p3, p4, off.submitted, off.kept, on.kept);
	return Report(ok, name);
}

static bool
CheckLightSlotPerFrame(void)
{
	const char *name = "two lit scenes in one frame encode two passes in one cluster slot and the next frame uses another";
	metal::LightDesc l = PointLight(4, 0.0f, 0.0f, 20.0f, 2.0f);
	LightFrame(&l, 1);
	int32 p0 = metal::lightClusterPassesForTest();
	int32 a[2], b[2];
	TwoSceneFrame(&l, 1, a);
	int32 p1 = metal::lightClusterPassesForTest();
	int32 cx, cy, cz;
	ClusterOf(0.0f, 0.0f, 20.0f, &cx, &cy, &cz);
	bool listed = InCluster(4, cx, cy, cz);
	TwoSceneFrame(&l, 1, b);
	bool ok = p1 == p0 + 2 && listed && a[0] >= 0 && a[0] == a[1] && b[0] >= 0 && b[0] == b[1] && b[0] != a[0];
	if(!ok)
		Detail("  passes %d then %d (want +2); light listed %d; slots %d %d then %d %d (want equal within a frame, different between)\n",
			p0, p1, listed, a[0], a[1], b[0], b[1]);
	return Report(ok, name);
}

static bool
CheckLightStatsScene(void)
{
	const char *name = "the GPU side stats and the cluster hook report only the current scene's pass";
	enum { N = 40 };
	metal::LightDesc lights[N];
	for(int i = 0; i < N; i++)
		lights[i] = PointLight((uint32)i, 0.0f, 0.0f, 20.0f, 1.0f);
	LightFrame(lights, N);
	int32 cx, cy, cz;
	ClusterOf(0.0f, 0.0f, 20.0f, &cx, &cy, &cz);
	uint32 ids[LT_PER_CLUSTER];
	int32 lit = Listed(cx, cy, cz, ids, LT_PER_CLUSTER);
	metal::LightStats a = metal::getLightStats();
	LightFrame(nil, 0);
	metal::LightStats b = metal::getLightStats();
	int32 none = Listed(cx, cy, cz, ids, LT_PER_CLUSTER);
	bool ok = lit == LT_PER_CLUSTER && a.maxPerCluster == N && a.clusterLoss >= N - LT_PER_CLUSTER &&
		b.clusterLoss == 0 && b.maxPerCluster == 0 && b.clusterMs == -1.0f && none == -1;
	if(!ok)
		Detail("  lit scene: cluster lists %d, worst %u, lost %u (want %d, %d, at least %d); unlit scene: worst %u, lost %u, ms %f, hook %d (want 0, 0, -1, -1)\n",
			lit, a.maxPerCluster, a.clusterLoss, LT_PER_CLUSTER, N, N - LT_PER_CLUSTER, b.maxPerCluster, b.clusterLoss, b.clusterMs, none);
	return Report(ok, name);
}

static bool
CheckLightClusterLoss(void)
{
	const char *name = "a cluster past 32 lights keeps its 32 brightest and counts the loss";
	enum { N = 40 };
	metal::LightDesc lights[N];
	for(int i = 0; i < N; i++){
		lights[i] = PointLight((uint32)i, 3.0f, 4.0f, 20.0f, 0.05f);
		lights[i].color[0] = lights[i].color[1] = lights[i].color[2] = 0.5f + 0.05f*(float)i;
	}
	bool began = LightFrame(lights, N);
	int32 cx, cy, cz;
	ClusterOf(3.0f, 4.0f, 20.0f, &cx, &cy, &cz);
	uint32 ids[LT_PER_CLUSTER];
	int32 listed = Listed(cx, cy, cz, ids, LT_PER_CLUSTER);
	metal::LightStats st = metal::getLightStats();
	bool present[N];
	memset(present, 0, sizeof(present));
	for(int32 i = 0; i < listed && i < LT_PER_CLUSTER; i++)
		if(ids[i] < N)
			present[ids[i]] = true;
	bool dimGone = true, brightKept = true;
	for(int i = 0; i < N; i++){
		if(i < N - LT_PER_CLUSTER && present[i])
			dimGone = false;
		if(i >= N - LT_PER_CLUSTER && !present[i])
			brightKept = false;
	}
	bool ok = began && listed == LT_PER_CLUSTER && dimGone && brightKept && st.clusterLoss == N - LT_PER_CLUSTER &&
		st.maxPerCluster == N;
	if(!ok)
		Detail("  prepass %d; cluster %d %d %d lists %d (want %d); the %d dimmest all absent %d, the %d brightest all present %d; lost %u (want %d), worst %u (want %d)\n",
			began, cx, cy, cz, listed, LT_PER_CLUSTER, N - LT_PER_CLUSTER, dimGone, LT_PER_CLUSTER, brightKept,
			st.clusterLoss, N - LT_PER_CLUSTER, st.maxPerCluster, N);
	return Report(ok, name);
}

static bool
CheckLightStatsLine(void)
{
	const char *name = "the stats line counts dropped lights and cluster loss";
	enum { STACKED = 40, FILLER = 260, N = STACKED + FILLER };
	std::vector<metal::LightDesc> lights;
	for(int i = 0; i < STACKED; i++){
		metal::LightDesc d = PointLight((uint32)i, 3.0f, 4.0f, 20.0f, 0.05f);
		d.color[0] = d.color[1] = d.color[2] = 0.5f + 0.05f*(float)i;
		lights.push_back(d);
	}
	for(int j = 0; j < FILLER; j++)
		lights.push_back(PointLight((uint32)(100 + j), -35.0f + 0.27f*(float)j, 3.0f*(float)(j % 9 - 4), 40.0f + 0.001f*(float)j, 0.05f));
	bool began = LightFrame(lights.data(), N);
	int32 cx, cy, cz;
	ClusterOf(3.0f, 4.0f, 20.0f, &cx, &cy, &cz);
	uint32 ids[LT_PER_CLUSTER];
	Listed(cx, cy, cz, ids, LT_PER_CLUSTER);
	metal::LightStats st = metal::getLightStats();
	metal::logStats();
	unsigned submitted = 0, dropped = 0, loss = 0, worst = 0;
	char ms[32] = "";
	const char *tail = strstr(metal::getStatsLine(), " lights ");
	bool parsed = tail && sscanf(tail, " lights %u dropped %u clusterloss %u percluster %u cluster %31s",
		&submitted, &dropped, &loss, &worst, ms) == 5;
	bool ok = began && parsed && submitted == N && dropped == FILLER + STACKED - 256 && loss == STACKED - LT_PER_CLUSTER &&
		worst == STACKED && strcmp(ms, "-") != 0 && st.dropped == dropped && st.clusterLoss == loss;
	if(!ok)
		Detail("  prepass %d; line \"%s\"; lights %u dropped %u clusterloss %u percluster %u cluster %s (want %d, %d, %d, %d, a time); hook: dropped %u lost %u\n",
			began, metal::getStatsLine(), submitted, dropped, loss, worst, ms, N, N - 256, STACKED - LT_PER_CLUSTER, STACKED,
			st.dropped, st.clusterLoss);
	return Report(ok, name);
}

static bool
CheckLightSceneCamera(void)
{
	const char *name = "lights submitted before the camera's update bin with the scene's camera";
	metal::LightDesc l = PointLight(11, 0.0f, 0.0f, 20.0f, 2.0f);
	bool began = LightFrame(&l, 1, WHEN_EARLY);
	int32 cx, cy, cz;
	ClusterOf(0.0f, 0.0f, 20.0f, &cx, &cy, &cz);
	uint32 ids[LT_PER_CLUSTER];
	int32 here = Listed(cx, cy, cz, ids, LT_PER_CLUSTER);
	uint32 first = here > 0 ? ids[0] : 0;
	bool ok = began && here == 1 && first == 11;
	if(!ok)
		Detail("  prepass %d; cluster %d %d %d lists %d (first %u)\n", began, cx, cy, cz, here, first);
	return Report(ok, name);
}

static bool
CheckLightLate(void)
{
	const char *name = "lights submitted after the prepass ended are ignored and counted 0";
	metal::LightDesc l = PointLight(5, 0.0f, 0.0f, 20.0f, 2.0f);
	int32 p0 = metal::lightClusterPassesForTest();
	LightFrame(&l, 1, WHEN_LATE);
	metal::LightStats st = metal::getLightStats();
	int32 p1 = metal::lightClusterPassesForTest();
	bool ok = p1 == p0 && st.submitted == 0 && st.kept == 0 && st.dropped == 0;
	if(!ok)
		Detail("  passes %d then %d (want equal); submitted %u kept %u dropped %u (want 0 0 0)\n", p0, p1, st.submitted, st.kept, st.dropped);
	return Report(ok, name);
}

static bool
CheckLightDegenerate(void)
{
	const char *name = "a spot with no direction acts as a sphere and a light with a negative or NaN radius is dropped and counted";
	metal::LightDesc lights[4];
	lights[0] = PointLight(21, 0.0f, 3.0f, 10.0f, 8.0f);
	lights[0].flags = metal::LIGHT_SPOT;
	lights[0].cosOuter = cosf(30.0f*3.14159265f/180.0f);
	lights[0].cosInner = cosf(20.0f*3.14159265f/180.0f);
	lights[1] = PointLight(22, 6.0f, 0.0f, 20.0f, -3.0f);
	lights[2] = PointLight(23, -6.0f, 0.0f, 20.0f, NAN);
	lights[3] = PointLight(24, 0.0f, 0.0f, 20.0f, 2.0f);
	bool began = LightFrame(lights, 4);
	metal::LightStats st = metal::getLightStats();
	int32 cx, cy, cz, ax, ay, az, bx, by, bz;
	ClusterOf(0.0f, 3.0f, 14.0f, &cx, &cy, &cz);
	ClusterOf(6.0f, 0.0f, 20.0f, &ax, &ay, &az);
	ClusterOf(-6.0f, 0.0f, 20.0f, &bx, &by, &bz);
	bool spot = InCluster(21, cx, cy, cz);
	bool negative = InCluster(22, ax, ay, az);
	bool nan = InCluster(23, bx, by, bz);
	int32 vx, vy, vz;
	ClusterOf(0.0f, 0.0f, 20.0f, &vx, &vy, &vz);
	bool valid = InCluster(24, vx, vy, vz);
	bool ok = began && spot && !negative && !nan && valid && st.submitted == 4 && st.kept == 2 && st.dropped == 2;
	if(!ok)
		Detail("  prepass %d; zero direction spot listed 4 m past its apex %d; negative radius listed %d; NaN radius listed %d; submitted %u kept %u dropped %u (want 1 0 0 and 4 2 2, valid light listed %d)\n",
			began, spot, negative, nan, st.submitted, st.kept, st.dropped, valid);
	return Report(ok, name);
}

static const float ORIGIN[3] = { 100.0f, 40.0f, 0.0f };
static Matrix savedCameraMatrix;

struct LitScene
{
	Material *mat, *envMat;
	Texture *white, *black;
	Geometry *geos[4];
	Atomic *atoms[4];
	int n;
};

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

static float
PixelX(int px, float z)
{
	return z*(1.0f - (px + 0.5f)/320.0f);
}

static float
PixelY(int py, float z)
{
	return 0.75f*z*(1.0f - (py + 0.5f)/240.0f);
}

static void
CameraOffset(bool on)
{
	Frame *f = lightCamera->getFrame();
	if(on){
		savedCameraMatrix = f->matrix;
		V3d d = { ORIGIN[0], ORIGIN[1], ORIGIN[2] };
		f->translate(&d, COMBINEPOSTCONCAT);
	}else
		f->matrix = savedCameraMatrix;
	f->updateObjects();
}

static void
Place(Atomic *a, float x, float y, float z)
{
	V3d d = { ORIGIN[0] + x, ORIGIN[1] + y, ORIGIN[2] + z };
	a->getFrame()->translate(&d, COMBINEREPLACE);
	a->getFrame()->updateObjects();
}

static metal::LightDesc
SceneLight(uint32 id, float x, float y, float z, float radius, float bright)
{
	metal::LightDesc d = PointLight(id, ORIGIN[0] + x, ORIGIN[1] + y, ORIGIN[2] + z, radius);
	d.color[0] = d.color[1] = d.color[2] = bright;
	return d;
}

static void
SetWeight(float w)
{
	metal::RelitParams p = metal::getRelitParams();
	p.weight = w;
	metal::setRelitParams(&p);
}

static Material*
LitMaterial(void)
{
	Material *mat = Material::create();
	SurfaceProperties sp = { 1.0f, 0.0f, 1.0f };
	mat->surfaceProps = sp;
	return mat;
}

static Texture*
OneTexel(RGBA c)
{
	return MakeTexture(1, 1, &c, Texture::NEAREST, Texture::CLAMP, Texture::CLAMP);
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
	Place(a, x, y, z);
	HAnimHierarchy *h = HAnimHierarchy::create(2, nil, nil, 0, 36);
	h->matrices[0] = *a->getFrame()->getLTM();
	h->matrices[1] = *a->getFrame()->getLTM();
	Skin::setHierarchy(a, h);
	return a;
}

enum { LIT_WALL, LIT_QUAD, LIT_SKINNED, LIT_ENV };

static void
MakeLitScene(LitScene *s, int n, Material *mat = nil, uint32 wallFlags = 0)
{
	RGBA grey = { 64, 64, 64, 255 }, greys[4] = { grey, grey, grey, grey };
	s->mat = mat ? mat : LitMaterial();
	s->envMat = nil;
	s->white = s->black = nil;
	s->n = n;
	s->geos[LIT_WALL] = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT | wallFlags, 10.0f, 7.0f, -10.0f, -7.0f, 0.0f, greys, nil, s->mat);
	s->atoms[LIT_WALL] = MakeAtomic(s->geos[LIT_WALL]);
	Place(s->atoms[LIT_WALL], 0.0f, 0.0f, 20.0f);
	if(n < 2)
		return;
	s->geos[LIT_QUAD] = QuadGeometry(Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT, -2.0f, 2.0f, -6.0f, -2.0f, 0.0f,
		nil, nil, s->mat);
	s->atoms[LIT_QUAD] = MakeAtomic(s->geos[LIT_QUAD]);
	Place(s->atoms[LIT_QUAD], 0.0f, 0.0f, 19.0f);
	s->atoms[LIT_SKINNED] = SkinnedQuad(&s->geos[LIT_SKINNED], s->mat, 4.0f, 0.0f, 19.0f);
	if(n < 4)
		return;
	RGBA white = { 255, 255, 255, 255 }, black = { 0, 0, 0, 255 }, ones[4] = { white, white, white, white };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	s->white = OneTexel(white);
	s->black = OneTexel(black);
	s->envMat = LitMaterial();
	s->envMat->setTexture(s->white);
	MatFX::setEffects(s->envMat, MatFX::ENVMAP);
	MatFX *fx = MatFX::get(s->envMat);
	fx->setEnvCoefficient(1.0f);
	fx->setEnvTexture(s->black);
	fx->setEnvFrame(nil);
	s->geos[LIT_ENV] = QuadGeometry(Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT | Geometry::TEXTURED, 1.0f, -4.5f, -1.0f, -6.5f,
		0.0f, ones, uv, s->envMat);
	s->atoms[LIT_ENV] = MakeAtomic(s->geos[LIT_ENV]);
	MatFX::enableEffects(s->atoms[LIT_ENV]);
	Place(s->atoms[LIT_ENV], 0.0f, 0.0f, 19.0f);
}

static void
FreeLitScene(LitScene *s)
{
	for(int i = 0; i < s->n; i++){
		if(i == LIT_SKINNED)
			DestroySkinAtomic(s->atoms[i]);
		else
			DestroyAtomic(s->atoms[i]);
		s->geos[i]->destroy();
	}
	if(s->envMat)
		s->envMat->destroy();
	if(s->white)
		s->white->destroy();
	if(s->black)
		s->black->destroy();
	s->mat->destroy();
}

static void
LitFrame(LitScene *s, const metal::LightDesc *lights, int32 n, bool shiftView = false)
{
	RGBA sky = { 64, 64, 64, 255 };
	metal::beginScene(lightCamera);
	WorldBegin(sky);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(CULLMODE, CULLNONE);
	metal::setLights(lights, n);
	if(metal::beginPrepass()){
		for(int i = 0; i < s->n; i++)
			s->atoms[i]->render();
		metal::endPrepass();
	}
	if(shiftView){
		RawMatrix view = lightCamera->devView;
		view.pos.x += 0.5f;
		metal::setViewMatrix((float32*)&view);
	}
	SetRenderState(CULLMODE, CULLNONE);
	for(int i = 0; i < s->n; i++)
		s->atoms[i]->render();
	metal::endScene(nil);
	WorldEnd();
}

static void
ReadAt(int x, int y, float rgb[3])
{
	float px[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
	if(!metal::readScenePixel(x, y, px))
		Detail("  pixel %d,%d could not be read\n", x, y);
	rgb[0] = px[0];
	rgb[1] = px[1];
	rgb[2] = px[2];
}

static float
FacingTerm(const metal::LightDesc &l, float x, float y, float z)
{
	float dx = l.pos[0] - ORIGIN[0] - x, dy = l.pos[1] - ORIGIN[1] - y, dz = l.pos[2] - ORIGIN[2] - z;
	float d2 = dx*dx + dy*dy + dz*dz;
	return ltFalloff(d2, l.radius)*fmaxf(-dz/sqrtf(d2), 0.0f)*l.color[0];
}

static bool
TermMatches(const float *lit, const float *base, float want, const char *what, int x, int y)
{
	bool ok = true;
	for(int c = 0; c < 3; c++)
		ok = ok && fabsf(lit[c] - base[c] - want) <= 2.0f/255.0f;
	if(!ok)
		Detail("  %s at %d,%d: lit %.4f %.4f %.4f, base %.4f %.4f %.4f, want the term %.4f added\n", what, x, y,
			lit[0], lit[1], lit[2], base[0], base[1], base[2], want);
	return ok;
}

static void
SetShadows(int32 quality)
{
	lightCamera->showRaster(0);
	metal::RenderSettings s = metal::getRenderSettings();
	s.shadowQuality = quality;
	metal::setRenderSettings(&s);
}

static bool
FalloffAtWeight(const char *name, float weight, bool shadows = false)
{
	LitScene s;
	MakeLitScene(&s, 1);
	SetWeight(weight);
	if(shadows)
		SetShadows(metal::SHADOWQUALITY_MEDIUM);
	CameraOffset(true);
	metal::LightDesc l = SceneLight(31, 0.0f, 0.0f, 18.0f, 10.0f, 2.5f);
	float ground[3] = { 0.0f, 3.0f, 6.0f }, base[3][3], lit[3][3];
	int xs[3], y = PY(0.0f, 20.0f);
	for(int i = 0; i < 3; i++)
		xs[i] = PX(ground[i], 20.0f);
	for(int f = 0; f < 3; f++)
		LitFrame(&s, nil, 0);
	for(int i = 0; i < 3; i++)
		ReadAt(xs[i], y, base[i]);
	metal::StateStats a0 = metal::getStateStats();
	LitFrame(&s, &l, 1);
	metal::StateStats a1 = metal::getStateStats();
	for(int i = 0; i < 3; i++)
		ReadAt(xs[i], y, lit[i]);
	CameraOffset(false);
	if(shadows)
		SetShadows(metal::SHADOWQUALITY_OFF);
	SetWeight(1.0f);
	FreeLitScene(&s);
	uint32 lightDraws = a1.lightDraws - a0.lightDraws, relitDraws = a1.relitDraws - a0.relitDraws;
	uint32 shadowDraws = weight >= 1.0f ? a1.plainShadowDraws - a0.plainShadowDraws : a1.relitShadowDraws - a0.relitShadowDraws;
	bool ok = lightDraws > 0 && (weight >= 1.0f || relitDraws > 0) && (!shadows || shadowDraws > 0);
	if(!ok)
		Detail("  light draws %u, relit draws %u, shadow twin draws %u (want light draws, relit draws below weight 1, and shadow twin draws with shadows on)\n",
			lightDraws, relitDraws, shadowDraws);
	for(int i = 0; i < 3; i++)
		ok = TermMatches(lit[i], base[i], FacingTerm(l, PixelX(xs[i], 20.0f), PixelY(y, 20.0f), 20.0f), "floor", xs[i], y) && ok;
	return Report(ok, name);
}

static bool
CheckLightFalloffPlain(void)
{
	return FalloffAtWeight("a prelit floor at weight 1 lit by a sphere reads the falloff at known distances", 1.0f);
}

static bool
CheckLightFalloffRelit(void)
{
	return FalloffAtWeight("at weight 0.5 the light is added on top of the relit colour", 0.5f);
}

static bool
CheckLightShadowedPlain(void)
{
	return FalloffAtWeight("with shadows on at medium, a prelit floor at weight 1 draws the plain shadow twin with the light and reads the falloff", 1.0f, true);
}

static bool
CheckLightShadowedRelit(void)
{
	return FalloffAtWeight("with shadows on at medium, a relit floor at weight 0.5 draws the shadow variant with the light and reads the falloff", 0.5f, true);
}

static bool
CheckLightVertexLit(void)
{
	const char *name = "a vertex-lit atomic, a skinned atomic and a matfx env atomic read the light per pixel";
	LitScene s;
	MakeLitScene(&s, 4);
	SetWeight(1.0f);
	CameraOffset(true);
	metal::LightDesc l[3] = { SceneLight(41, -4.0f, 0.0f, 17.0f, 5.0f, 0.4f), SceneLight(42, 4.0f, 0.0f, 17.0f, 5.0f, 0.4f),
		SceneLight(43, 0.0f, -5.5f, 17.0f, 5.0f, 0.4f) };
	float gx[3] = { -4.0f, 4.0f, 0.0f }, gy[3] = { 0.0f, 0.0f, -5.5f };
	int xs[3], ys[3];
	for(int i = 0; i < 3; i++){
		xs[i] = PX(gx[i], 19.0f);
		ys[i] = PY(gy[i], 19.0f);
	}
	float base[3][3], lit[3][3];
	for(int f = 0; f < 2; f++)
		LitFrame(&s, nil, 0);
	for(int i = 0; i < 3; i++)
		ReadAt(xs[i], ys[i], base[i]);
	metal::StateStats a0 = metal::getStateStats();
	LitFrame(&s, l, 3);
	metal::StateStats a1 = metal::getStateStats();
	for(int i = 0; i < 3; i++)
		ReadAt(xs[i], ys[i], lit[i]);
	CameraOffset(false);
	FreeLitScene(&s);
	uint32 lightDraws = a1.lightDraws - a0.lightDraws, relitDraws = a1.relitDraws - a0.relitDraws;
	bool ok = lightDraws >= 4 && relitDraws >= 3;
	if(!ok)
		Detail("  light draws %u, relit draws %u (want at least 4 and 3)\n", lightDraws, relitDraws);
	const char *what[3] = { "vertex-lit quad", "skinned quad", "matfx env quad" };
	for(int i = 0; i < 3; i++)
		ok = TermMatches(lit[i], base[i], FacingTerm(l[i], PixelX(xs[i], 19.0f), PixelY(ys[i], 19.0f), 19.0f), what[i], xs[i], ys[i]) && ok;
	return Report(ok, name);
}

static bool
CheckLightSpotCutoff(void)
{
	const char *name = "a spot lights inside its cone and leaves the floor outside unchanged";
	LitScene s;
	MakeLitScene(&s, 1);
	SetWeight(1.0f);
	CameraOffset(true);
	metal::LightDesc l = SceneLight(51, 0.0f, 0.0f, 14.0f, 12.0f, 2.0f);
	l.flags = metal::LIGHT_SPOT;
	l.dir[2] = 1.0f;
	l.cosOuter = cosf(20.0f*3.14159265f/180.0f);
	l.cosInner = cosf(15.0f*3.14159265f/180.0f);
	int inX = PX(0.0f, 20.0f), outX = PX(2.7f, 20.0f), y = PY(0.0f, 20.0f);
	float baseIn[3], baseOut[3], litIn[3], litOut[3];
	for(int f = 0; f < 2; f++)
		LitFrame(&s, nil, 0);
	ReadAt(inX, y, baseIn);
	ReadAt(outX, y, baseOut);
	LitFrame(&s, &l, 1);
	ReadAt(inX, y, litIn);
	ReadAt(outX, y, litOut);
	CameraOffset(false);
	FreeLitScene(&s);
	float px = PixelX(inX, 20.0f), py = PixelY(y, 20.0f);
	float d2 = px*px + py*py + 36.0f;
	float want = ltSpot(6.0f/sqrtf(d2), l.cosOuter, l.cosInner)*FacingTerm(l, px, py, 20.0f);
	bool inside = TermMatches(litIn, baseIn, want, "inside the cone", inX, y);
	bool outside = litOut[0] == baseOut[0] && litOut[1] == baseOut[1] && litOut[2] == baseOut[2];
	if(!outside)
		Detail("  outside the cone at %d,%d: lit %.4f %.4f %.4f, without the light %.4f %.4f %.4f\n", outX, y,
			litOut[0], litOut[1], litOut[2], baseOut[0], baseOut[1], baseOut[2]);
	return Report(inside && outside, name);
}

static bool
CheckLightViewChange(void)
{
	const char *name = "a draw under another view than the prepass's gets no light variant and reads the floor unchanged";
	LitScene s;
	MakeLitScene(&s, 1);
	SetWeight(1.0f);
	CameraOffset(true);
	metal::LightDesc l = SceneLight(81, 0.0f, 0.0f, 18.0f, 10.0f, 2.5f);
	int x = PX(0.0f, 20.0f), y = PY(0.0f, 20.0f);
	float same[3], base[3], lit[3];
	LitFrame(&s, nil, 0);
	metal::StateStats a0 = metal::getStateStats();
	LitFrame(&s, &l, 1);
	metal::StateStats a1 = metal::getStateStats();
	ReadAt(x, y, same);
	LitFrame(&s, nil, 0, true);
	ReadAt(x, y, base);
	metal::StateStats a2 = metal::getStateStats();
	LitFrame(&s, &l, 1, true);
	metal::StateStats a3 = metal::getStateStats();
	ReadAt(x, y, lit);
	CameraOffset(false);
	FreeLitScene(&s);
	uint32 same1 = a1.lightDraws - a0.lightDraws, moved = a3.lightDraws - a2.lightDraws;
	bool unchanged = lit[0] == base[0] && lit[1] == base[1] && lit[2] == base[2];
	bool ok = same1 > 0 && moved == 0 && unchanged && same[0] > base[0];
	if(!ok)
		Detail("  light draws with the prepass's view %u (want some), with another view %u (want 0); lit %.4f %.4f %.4f, without the light %.4f %.4f %.4f (want equal); with the prepass's view the pixel reads %.4f\n",
			same1, moved, lit[0], lit[1], lit[2], base[0], base[1], base[2], same[0]);
	return Report(ok, name);
}

static metal::Shader *standIn;

static void
StandInRender(Atomic *atomic, metal::InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	metal::setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = metal::lightingCB(atomic);
	metal::setupVertexInput(header);
	metal::InstanceData *inst = header->inst;
	for(int32 n = header->numMeshes; n--; inst++){
		Material *m = inst->material;
		metal::setMaterial(flags, m->color, m->surfaceProps);
		metal::setTexture(0, m->texture);
		SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		standIn->use(metal::drawVariant(vsBits));
		metal::drawInst(header, inst);
	}
	metal::teardownVertexInput(header);
}

static bool
StandInDraws(uint32 mask, uint32 *lightMask, uint32 *lightDraws)
{
	standIn = CreateHostDefaultShader(mask);
	if(standIn == nil)
		return false;
	*lightMask = standIn->variantMask & metal::VARIANT_LIGHTS;
	metal::AttribDesc attribs[metal::MAXVERTEXATTRIBS];
	int32 n = metal::defaultVertexAttribs(0, 1, 0, attribs);
	bool ok = metal::prewarmShader(standIn, attribs, n, 0, 0, 0, 0, 1) != 0;
	metal::ObjPipeline *pipe = metal::ObjPipeline::create();
	pipe->instanceCB = metal::defaultInstanceCB;
	pipe->uninstanceCB = metal::defaultUninstanceCB;
	pipe->renderCB = StandInRender;
	LitScene s;
	MakeLitScene(&s, 1);
	s.atoms[LIT_WALL]->pipeline = pipe;
	SetWeight(1.0f);
	CameraOffset(true);
	metal::LightDesc l = SceneLight(91, 0.0f, 0.0f, 18.0f, 10.0f, 2.5f);
	for(int f = 0; f < 2; f++)
		LitFrame(&s, nil, 0);
	metal::StateStats a0 = metal::getStateStats();
	LitFrame(&s, &l, 1);
	metal::StateStats a1 = metal::getStateStats();
	*lightDraws = a1.lightDraws - a0.lightDraws;
	CameraOffset(false);
	FreeLitScene(&s);
	pipe->destroy();
	standIn->destroy();
	standIn = nil;
	return ok;
}

static bool
CheckLightOptIn(void)
{
	const char *name = "a shader with the relit bits and no light bit draws no light variant, and one that opts in draws it";
	uint32 maskOff, drawsOff, maskOn, drawsOn;
	bool builtOff = StandInDraws(metal::VARIANT_ALL | metal::VARIANT_RELITALL, &maskOff, &drawsOff);
	bool builtOn = StandInDraws(metal::VARIANT_ALL | metal::VARIANT_RELITALL | metal::VARIANT_LIGHTS, &maskOn, &drawsOn);
	bool ok = builtOff && builtOn && maskOff == 0 && drawsOff == 0 && maskOn != 0 && drawsOn > 0;
	if(!ok)
		Detail("  built %d %d; light bit in the mask %u without the opt-in, %u with it (want 0, set); light draws %u without, %u with (want 0, some)\n",
			builtOff, builtOn, maskOff, maskOn, drawsOff, drawsOn);
	return Report(ok, name);
}

static bool
ChannelsMatch(const float *lit, const float *base, const float *want, const char *what, int x, int y)
{
	bool ok = true;
	for(int c = 0; c < 3; c++)
		ok = ok && fabsf(lit[c] - base[c] - want[c]) <= 2.0f/255.0f;
	if(!ok)
		Detail("  %s at %d,%d: lit %.4f %.4f %.4f, base %.4f %.4f %.4f, want the term %.4f %.4f %.4f added\n", what, x, y,
			lit[0], lit[1], lit[2], base[0], base[1], base[2], want[0], want[1], want[2]);
	return ok;
}

static void
HeaderTerm(const metal::LightDesc &l, const float P[3], float gloss, const float albedo[3], float out[3])
{
	float d[3] = { l.pos[0] - ORIGIN[0] - P[0], l.pos[1] - ORIGIN[1] - P[1], l.pos[2] - ORIGIN[2] - P[2] };
	float d2 = d[0]*d[0] + d[1]*d[1] + d[2]*d[2], dl = sqrtf(d2);
	float pl = sqrtf(P[0]*P[0] + P[1]*P[1] + P[2]*P[2]);
	float L[3] = { d[0]/dl, d[1]/dl, d[2]/dl }, V[3] = { -P[0]/pl, -P[1]/pl, -P[2]/pl };
	float H[3] = { L[0] + V[0], L[1] + V[1], L[2] + V[2] };
	float hl = sqrtf(H[0]*H[0] + H[1]*H[1] + H[2]*H[2]);
	float nl = fmaxf(L[2], 0.0f), nh = H[2]/hl;
	float spec = ltSpecular(nh, gloss);
	for(int c = 0; c < 3; c++)
		out[c] = l.color[c]*ltFalloff(d2, l.radius)*nl*(albedo[c] + spec);
}

static bool
CheckLightColourGloss(void)
{
	const char *name = "a coloured light with gloss above 0 on a prelit floor adds the colour, the albedo and the specular term of the header";
	Material *mat = LitMaterial();
	RGBA tint = { 255, 200, 120, 255 };
	mat->color = tint;
	SurfaceProperties sp = { 1.0f, 0.0f, 0.6f };
	mat->surfaceProps = sp;
	LitScene s;
	MakeLitScene(&s, 1, mat, Geometry::MODULATE);
	SetWeight(1.0f);
	CameraOffset(true);
	Frame *f = lightCamera->getFrame();
	V3d up = { 0.0f, 1.0f, 0.0f };
	f->rotate(&up, 180.0f, COMBINEPRECONCAT);
	f->updateObjects();
	Place(s.atoms[LIT_WALL], 0.0f, 0.0f, -20.0f);
	const float gloss = 0.75f;
	metal::LightFrameParams fp = { gloss, 1.0f };
	metal::setLightFrameParams(&fp);
	int px[2] = { 320, 350 }, py = 240;
	float P[2][3];
	for(int i = 0; i < 2; i++){
		P[i][0] = -PixelX(px[i], 20.0f);
		P[i][1] = PixelY(py, 20.0f);
		P[i][2] = -20.0f;
	}
	float pl = sqrtf(P[0][0]*P[0][0] + P[0][1]*P[0][1] + 400.0f);
	float V[3] = { -P[0][0]/pl, -P[0][1]/pl, 20.0f/pl };
	float R[3] = { -V[0], -V[1], V[2] };
	metal::LightDesc l = SceneLight(101, P[0][0] + 2.0f*R[0]/R[2], P[0][1] + 2.0f*R[1]/R[2], -18.0f, 10.0f, 1.0f);
	l.color[1] = 0.5f;
	l.color[2] = 0.25f;
	float base[2][3], lit[2][3];
	for(int fr = 0; fr < 3; fr++)
		LitFrame(&s, nil, 0);
	for(int i = 0; i < 2; i++)
		ReadAt(px[i], py, base[i]);
	metal::StateStats a0 = metal::getStateStats();
	LitFrame(&s, &l, 1);
	metal::StateStats a1 = metal::getStateStats();
	for(int i = 0; i < 2; i++)
		ReadAt(px[i], py, lit[i]);
	fp.wetGloss = 0.0f;
	metal::setLightFrameParams(&fp);
	CameraOffset(false);
	FreeLitScene(&s);
	uint32 lightDraws = a1.lightDraws - a0.lightDraws;
	bool ok = lightDraws > 0;
	if(!ok)
		Detail("  light draws %u (want some)\n", lightDraws);
	const float albedo[3] = { 1.0f*0.6f, 200.0f/255.0f*0.6f, 120.0f/255.0f*0.6f };
	const char *what[2] = { "the mirror pixel", "a pixel off the mirror" };
	for(int i = 0; i < 2; i++){
		float want[3];
		HeaderTerm(l, P[i], gloss, albedo, want);
		ok = ChannelsMatch(lit[i], base[i], want, what[i], px[i], py) && ok;
	}
	return Report(ok, name);
}

static void
GlowFrame(Atomic **atoms, const float *scales, int n, const metal::LightDesc *light)
{
	RGBA sky = { 64, 64, 64, 255 };
	metal::beginScene(lightCamera);
	WorldBegin(sky);
	SetRenderState(VERTEXALPHA, 0);
	SetRenderState(CULLMODE, CULLNONE);
	metal::setLights(light, 1);
	if(metal::beginPrepass()){
		for(int i = 0; i < n; i++)
			atoms[i]->render();
		metal::endPrepass();
	}
	SetRenderState(CULLMODE, CULLNONE);
	for(int i = 0; i < n; i++){
		metal::setDrawEmissiveLuma(scales[i]);
		atoms[i]->render();
		metal::setDrawEmissiveLuma(1.0f);
	}
	metal::endScene(nil);
	WorldEnd();
}

static bool
CheckLightEmissive(void)
{
	const char *name = "a bright texel goes above white at the emissive scale and a dark texel lit past the gate keeps its colour";
	RGBA white = { 255, 255, 255, 255 }, dark = { 51, 51, 51, 255 }, ones[4] = { white, white, white, white };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	Texture *tex[2] = { OneTexel(white), OneTexel(dark) };
	Material *mat[2];
	Geometry *geo[3];
	Atomic *atoms[3];
	const int which[3] = { 0, 1, 0 };
	const float xs[3] = { -4.0f, 0.0f, 4.0f };
	for(int i = 0; i < 2; i++){
		mat[i] = LitMaterial();
		mat[i]->setTexture(tex[i]);
	}
	for(int i = 0; i < 3; i++){
		geo[i] = QuadGeometry(Geometry::POSITIONS | Geometry::NORMALS | Geometry::PRELIT | Geometry::LIGHT | Geometry::TEXTURED, 1.5f, 1.5f, -1.5f, -1.5f,
			0.0f, ones, uv, mat[which[i]]);
		atoms[i] = MakeAtomic(geo[i]);
		Place(atoms[i], xs[i], 0.0f, 20.0f);
	}
	CameraOffset(true);
	metal::LightDesc l = SceneLight(111, 0.0f, 0.0f, 17.0f, 12.0f, 34.0f);
	const float plain[3] = { 1.0f, 1.0f, 1.0f }, glow[3] = { 4.0f, 4.0f, 1.0f };
	float base[3][3], lit[3][3];
	int y = PY(0.0f, 20.0f), px[3];
	for(int i = 0; i < 3; i++)
		px[i] = PX(xs[i], 20.0f);
	for(int f = 0; f < 3; f++)
		GlowFrame(atoms, plain, 3, &l);
	for(int i = 0; i < 3; i++)
		ReadAt(px[i], y, base[i]);
	metal::StateStats a0 = metal::getStateStats();
	GlowFrame(atoms, glow, 3, &l);
	metal::StateStats a1 = metal::getStateStats();
	for(int i = 0; i < 3; i++)
		ReadAt(px[i], y, lit[i]);
	CameraOffset(false);
	for(int i = 0; i < 3; i++){
		DestroyAtomic(atoms[i]);
		geo[i]->destroy();
	}
	for(int i = 0; i < 2; i++){
		mat[i]->destroy();
		tex[i]->destroy();
	}
	bool bright = true, keeps = true, unmarked = true;
	for(int c = 0; c < 3; c++){
		bright = bright && lit[0][c] >= 3.5f*base[0][c] && lit[0][c] > 1.5f;
		keeps = keeps && fabsf(lit[1][c] - base[1][c]) <= 2.0f/255.0f;
		unmarked = unmarked && fabsf(lit[2][c] - base[2][c]) <= 2.0f/255.0f;
	}
	bool strong = base[1][0] > 0.9f && base[1][0] < 1.0f;
	bool ok = bright && keeps && unmarked && strong && a1.lightDraws > a0.lightDraws;
	if(!ok)
		Detail("  bright texel %.3f %.3f %.3f (was %.3f %.3f %.3f, want 4x and above 1.5); dark texel %.3f %.3f %.3f (was %.3f %.3f %.3f, want the same); unmarked bright %.3f %.3f %.3f (was %.3f %.3f %.3f, want the same); light draws %u\n",
			lit[0][0], lit[0][1], lit[0][2], base[0][0], base[0][1], base[0][2], lit[1][0], lit[1][1], lit[1][2],
			base[1][0], base[1][1], base[1][2], lit[2][0], lit[2][1], lit[2][2], base[2][0], base[2][1], base[2][2], a1.lightDraws - a0.lightDraws);
	return Report(ok, name);
}

static bool
CheckLightPlainTwinFailure(void)
{
	const char *name = "a plain shadow twin that fails to build keeps the lights on that draw and on the frames after";
	LitScene s;
	MakeLitScene(&s, 1);
	SetWeight(1.0f);
	SetShadows(metal::SHADOWQUALITY_MEDIUM);
	CameraOffset(true);
	metal::LightDesc l = SceneLight(111, 0.0f, 0.0f, 18.0f, 10.0f, 2.5f);
	int x = PX(3.0f, 20.0f), y = PY(0.0f, 20.0f);
	float lit[2][3], base[3];
	for(int f = 0; f < 2; f++)
		LitFrame(&s, nil, 0);
	metal::setPlainTwinFailForTest(1);
	metal::StateStats a[3];
	for(int f = 0; f < 2; f++){
		a[f] = metal::getStateStats();
		LitFrame(&s, &l, 1);
		ReadAt(x, y, lit[f]);
	}
	a[2] = metal::getStateStats();
	for(int f = 0; f < 2; f++)
		LitFrame(&s, nil, 0);
	ReadAt(x, y, base);
	metal::setPlainTwinFailForTest(0);
	CameraOffset(false);
	SetShadows(metal::SHADOWQUALITY_OFF);
	FreeLitScene(&s);
	uint32 first = a[1].lightDraws - a[0].lightDraws, second = a[2].lightDraws - a[1].lightDraws;
	bool ok = first > 0 && second > 0;
	if(!ok)
		Detail("  light draws in the frame whose twin failed %u, in the next %u (want some)\n", first, second);
	float want = FacingTerm(l, PixelX(x, 20.0f), PixelY(y, 20.0f), 20.0f);
	for(int f = 0; f < 2; f++)
		ok = TermMatches(lit[f], base, want, f ? "the next frame" : "the frame whose twin failed", x, y) && ok;
	return Report(ok, name);
}

static uint8*
CompositeFrame(void)
{
	Raster *fb = lightCamera->frameBuffer;
	uint8 *px = new uint8[fb->width*fb->height*4];
	if(!metal::compositeCameraPixels(fb, px)){
		Detail("  compositeCameraPixels failed\n");
		memset(px, 0, fb->width*fb->height*4);
	}
	lightCamera->showRaster(0);
	return px;
}

static bool
FramesEqual(const uint8 *a, const uint8 *b, int w, int h, const char *what)
{
	int differing = 0, largest = 0, at = -1;
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

static bool
LightsOffIdentical(const char *what)
{
	LitScene s;
	MakeLitScene(&s, 3);
	SetWeight(1.0f);
	CameraOffset(true);
	metal::LightDesc l = SceneLight(61, 0.0f, 0.0f, 17.0f, 10.0f, 2.0f);
	int w = lightCamera->frameBuffer->width, h = lightCamera->frameBuffer->height;
	lightCamera->showRaster(0);
	SetMode(metal::LIGHTS_OFF);
	LitFrame(&s, &l, 1);
	uint8 *before = CompositeFrame();
	SetMode(metal::LIGHTS_ON);
	metal::StateStats a0 = metal::getStateStats();
	LitFrame(&s, &l, 1);
	delete[] CompositeFrame();
	metal::StateStats a1 = metal::getStateStats();
	uint8 *pair = new uint8[w*h*8], *ref = new uint8[w*h*8];
	LitFrame(&s, nil, 0);
	uint8 *empty = CompositeFrame();
	SetMode(metal::LIGHTS_OFF);
	LitFrame(&s, &l, 1);
	uint8 *off = CompositeFrame();
	metal::StateStats a2 = metal::getStateStats();
	SetMode(metal::LIGHTS_ON);
	CameraOffset(false);
	FreeLitScene(&s);
	memcpy(pair, empty, w*h*4);
	memcpy(pair + w*h*4, off, w*h*4);
	memcpy(ref, before, w*h*4);
	memcpy(ref + w*h*4, before, w*h*4);
	uint32 lit = a1.lightDraws - a0.lightDraws, unlit = a2.lightDraws - a1.lightDraws;
	bool ok = lit > 0 && unlit == 0;
	if(!ok)
		Detail("  light draws in the lit frame %u (want some), in the frames without light %u (want 0)\n", lit, unlit);
	ok = FramesEqual(ref, pair, w, 2*h, what) && ok;
	delete[] before;
	delete[] empty;
	delete[] off;
	delete[] pair;
	delete[] ref;
	return ok;
}

static bool
CheckLightsOffIdenticalOneSample(void)
{
	const char *name = "a modern frame with LIGHTS on and no light, and with LIGHTS off, is bit-equal to the frame before lights, one sample";
	bool ok = LightsOffIdentical("LIGHTS on with no light, then LIGHTS off with one, against LIGHTS off before any light, one sample");
	return Report(ok, name);
}

int
RunLightSampleChecks(Camera *camera)
{
	const char *name = "a modern frame with LIGHTS on and no light, and with LIGHTS off, is bit-equal to the frame before lights, four samples";
	failures = 0;
	lightCamera = camera;
	LightsOn(metal::LIGHTS_ON);
	bool ok = LightsOffIdentical("LIGHTS on with no light, then LIGHTS off with one, against LIGHTS off before any light, four samples");
	LightsOff();
	Report(ok, name);
	return failures;
}

static bool
CheckLightCountView(void)
{
	const char *name = "the light-count view shows a cluster's light count and leaves a frame without lights graded";
	const int hx = 330, hy = 250, cx0 = 20, cy0 = 20;
	metal::LightDesc l[4] = { PointLight(1, 0.0f, 0.0f, 90.0f, 20.0f), PointLight(2, 2.0f, 0.0f, 90.0f, 20.0f),
		PointLight(3, -2.0f, 0.0f, 90.0f, 20.0f), PointLight(4, PixelX(cx0, 90.0f), PixelY(cy0, 90.0f), 90.0f, 20.0f) };
	metal::setDebugView(metal::DEBUGVIEW_LIGHTS);
	bool began = LightFrame(l, 4);
	float heat[4] = {}, cold[4] = {};
	bool read = metal::readFrameOutput(hx, hy, heat) && metal::readFrameOutput(cx0, cy0, cold);
	int32 gx, gy, gz;
	ClusterOf(PixelX(hx, 90.0f), PixelY(hy, 90.0f), 90.0f, &gx, &gy, &gz);
	uint32 ids[LT_PER_CLUSTER];
	int32 listed = Listed(gx, gy, gz, ids, LT_PER_CLUSTER);
	int32 sx, sy, sz;
	ClusterOf(PixelX(cx0, 90.0f), PixelY(cy0, 90.0f), 90.0f, &sx, &sy, &sz);
	int32 skyListed = Listed(sx, sy, sz, ids, LT_PER_CLUSTER);
	const float want[3] = { 0.0f, 2.0f/7.0f, 5.0f/7.0f };
	bool ok = began && read && listed == 3 && skyListed > 0;
	for(int c = 0; c < 3; c++)
		ok = ok && fabsf(heat[c] - want[c]) <= 0.01f && fabsf(cold[c]) <= 0.01f;
	if(!ok)
		Detail("  prepass %d; cluster %d %d %d lists %d; sky cluster lists %d; pixel %d,%d reads %.3f %.3f %.3f, want %.3f %.3f %.3f; uncovered pixel %d,%d reads %.3f %.3f %.3f, want black (sky)\n",
			began, gx, gy, gz, listed, skyListed, hx, hy, heat[0], heat[1], heat[2], want[0], want[1], want[2], cx0, cy0, cold[0], cold[1], cold[2]);
	lightCamera->showRaster(0);
	LightFrame(nil, 0);
	uint8 *viewed = CompositeFrame();
	metal::setDebugView(metal::DEBUGVIEW_NONE);
	LightFrame(nil, 0);
	uint8 *graded = CompositeFrame();
	Raster *fb = lightCamera->frameBuffer;
	bool same = memcmp(graded, viewed, fb->width*fb->height*4) == 0;
	if(!same)
		Detail("  a frame without lights under the light-count view is not the graded frame\n");
	delete[] viewed;
	delete[] graded;
	return Report(ok && same, name);
}

static bool
CheckLightInstancesAtNight(void)
{
	const char *name = "with the sun below the horizon and a shadowed light, the instance level is built and the sun pass is not";
	metal::RenderSettings saved = metal::getRenderSettings(), s = saved;
	s.shadowQuality = metal::SHADOWQUALITY_MEDIUM;
	s.shadowedLights = 1;
	s.lights = metal::LIGHTS_SHADOWED;
	metal::setRenderSettings(&s);
	metal::RelitParams relitSaved = metal::getRelitParams(), rp = relitSaved;
	rp.sunScale = 1.0f;
	metal::setRelitParams(&rp);
	Light *sun = Light::create(Light::DIRECTIONAL);
	Frame *sunFrame = Frame::create();
	sunFrame->updateObjects();
	sun->setFrame(sunFrame);
	metal::setSunLight(sun);
	metal::LightDesc l = PointLight(61, 0.0f, 0.0f, 20.0f, 30.0f);
	RGBA sky = { 64, 64, 64, 255 };
	auto frame = [&](const metal::LightDesc *lights, int32 n){
		metal::beginScene(lightCamera);
		WorldBegin(sky);
		SetRenderState(VERTEXALPHA, 0);
		metal::setLights(lights, n);
		bool began = metal::beginPrepass() != 0;
		if(began){
			metal::addShadowCaster(backdrop, metal::SHADOWCASTER_STATIC);
			backdrop->render();
			metal::endPrepass();
		}
		backdrop->render();
		metal::endScene(nil);
		WorldEnd();
		return began;
	};
	bool began = frame(&l, 1);
	began = frame(&l, 1) && began;
	metal::RayStats on = metal::getRayStats();
	SetMode(metal::LIGHTS_ON);
	began = frame(&l, 1) && began;
	began = frame(&l, 1) && began;
	metal::RayStats off = metal::getRayStats();
	SetMode(metal::LIGHTS_SHADOWED);
	began = frame(nil, 0) && began;
	began = frame(nil, 0) && began;
	metal::RayStats none = metal::getRayStats();
	metal::setSunLight(nil);
	sun->destroy();
	sunFrame->destroy();
	metal::setRelitParams(&relitSaved);
	metal::setRenderSettings(&saved);
	bool ok = began && on.instances > 0 && on.raysPerFrame == 0 && off.instances == 0 && none.instances == 0;
	if(!ok)
		Detail("  prepass %d; shadowed light: instances %u rays %u (want some, 0); lights on: instances %u (want 0); no lights: instances %u (want 0)\n",
			began, on.instances, on.raysPerFrame, off.instances, none.instances);
	return Report(ok, name);
}

struct NightRig
{
	metal::RenderSettings saved;
	metal::RelitParams relitSaved;
	Light *sun;
	Frame *sunFrame;
};

static void
NightBegin(NightRig *r)
{
	r->saved = metal::getRenderSettings();
	metal::RenderSettings s = r->saved;
	s.shadowQuality = metal::SHADOWQUALITY_MEDIUM;
	s.shadowedLights = 1;
	s.lights = metal::LIGHTS_SHADOWED;
	metal::setRenderSettings(&s);
	r->relitSaved = metal::getRelitParams();
	metal::RelitParams rp = r->relitSaved;
	rp.sunScale = 1.0f;
	metal::setRelitParams(&rp);
	r->sun = Light::create(Light::DIRECTIONAL);
	r->sunFrame = Frame::create();
	r->sunFrame->updateObjects();
	r->sun->setFrame(r->sunFrame);
	metal::setSunLight(r->sun);
}

static void
NightEnd(NightRig *r)
{
	metal::setSunLight(nil);
	r->sun->destroy();
	r->sunFrame->destroy();
	metal::setRelitParams(&r->relitSaved);
	metal::setRenderSettings(&r->saved);
}

static bool
NightFrame(const metal::LightDesc *lights, int32 n, Atomic *blocker = nil, uint32 blockerOwner = 0)
{
	RGBA sky = { 64, 64, 64, 255 };
	metal::beginScene(lightCamera);
	WorldBegin(sky);
	SetRenderState(VERTEXALPHA, 0);
	metal::setLights(lights, n);
	bool began = metal::beginPrepass() != 0;
	if(began){
		metal::addShadowCaster(backdrop, metal::SHADOWCASTER_STATIC);
		if(blocker){
			metal::setShadowOwner(blockerOwner);
			metal::addShadowCaster(blocker, metal::SHADOWCASTER_STATIC);
			metal::setShadowOwner(0);
		}
		backdrop->render();
		metal::endPrepass();
	}
	backdrop->render();
	metal::endScene(nil);
	WorldEnd();
	return began;
}

static metal::RayStats
NightRays(const metal::LightDesc *lights, int32 n, bool *began, Atomic *blocker = nil, uint32 blockerOwner = 0)
{
	*began = true;
	for(int f = 0; f < 3; f++)
		*began = NightFrame(lights, n, blocker, blockerOwner) && *began;
	metal::rtWaitForTest();
	return metal::getRayStats();
}

static uint32
WallRaysWanted(const metal::LightDesc &l, uint32 frameNo)
{
	const float wall = 90.0f;
	metal::FrameInfo info = metal::getFrameInfo();
	int32 W = info.internalWidth, H = info.internalHeight;
	int32 rw = (W + 1)/2, rh = (H + 1)/2;
	LTVec3 pos = { l.pos[0], l.pos[1], l.pos[2] }, dir = { l.dir[0], l.dir[1], l.dir[2] };
	uint32 n = 0;
	for(int32 hy = 0; hy < rh; hy++)
		for(int32 hx = 0; hx < rw; hx++){
			int32 sx = std::min(2*hx + (int32)shPhaseX(frameNo), W - 1), sy = std::min(2*hy + (int32)shPhaseY(frameNo), H - 1);
			float x = -((sx + 0.5f)/W*2.0f - 1.0f)*wall, y = 0.75f*wall*(1.0f - 2.0f*(sy + 0.5f)/H);
			if(fabsf(x) > 60.0f || fabsf(y) > 45.0f)
				continue;
			float dist = sqrtf(x*x + y*y + wall*wall);
			LTVec3 o = { x, y, wall - shNormalOffset(dist) };
			if(ltRayWanted(o, pos, l.radius, dir, l.cosOuter, (l.flags & metal::LIGHT_SPOT) != 0))
				n++;
		}
	return n;
}

static bool
CheckLightRaysInCone(void)
{
	const char *name = "a spot's rays stay inside its cone at every quality";
	NightRig rig;
	NightBegin(&rig);
	const float deg = 3.14159265f/180.0f;
	metal::LightDesc spot = PointLight(61, 0.0f, 0.0f, 60.0f, 40.0f), ball = PointLight(62, 0.0f, 0.0f, 60.0f, 31.0f);
	spot.flags = metal::LIGHT_SPOT;
	spot.dir[2] = 1.0f;
	spot.cosOuter = cosf(25.0f*deg);
	spot.cosInner = cosf(15.0f*deg);
	metal::LightDesc sky = spot;
	sky.id = 63;
	sky.dir[1] = 1.0f;
	sky.dir[2] = 0.0f;
	bool began[3];
	metal::RayStats a = NightRays(&spot, 1, &began[0]);
	uint32 wantA = WallRaysWanted(spot, a.lightFrame);
	metal::RayStats b = NightRays(&ball, 1, &began[1]);
	uint32 wantB = WallRaysWanted(ball, b.lightFrame);
	metal::RayStats c = NightRays(&sky, 1, &began[2]);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	metal::setRenderSettings(&high);
	bool beganHigh;
	metal::RayStats h = NightRays(&spot, 1, &beganHigh);
	uint32 wantH = WallRaysWanted(spot, h.lightFrame);
	NightEnd(&rig);
	bool ok = beganHigh && h.lightSlots == 1 && wantH > 0 && h.lightRays[0] == wantH && began[0] && began[1] && began[2] && a.lightSlots == 1 && a.lightSlotId[0] == 61 && wantA > 0 && a.lightRays[0] == wantA &&
		b.lightSlots == 1 && b.lightSlotId[0] == 62 && wantB > 0 && b.lightRays[0] == wantB && wantB != wantA &&
		c.lightSlots == 1 && c.lightSlotId[0] == 63 && c.lightRays[0] == 0;
	if(!ok)
		Detail("  high prepass %d slots %u rays %u (want %u)\n", beganHigh, h.lightSlots, h.lightRays[0], wantH);
	if(!ok)
		Detail("  prepass %d %d %d; cone slots %u id %u rays %u (want %u); range slots %u id %u rays %u (want %u); sky slots %u id %u rays %u (want 0)\n",
			began[0], began[1], began[2], a.lightSlots, a.lightSlotId[0], a.lightRays[0], wantA, b.lightSlots, b.lightSlotId[0], b.lightRays[0],
			wantB, c.lightSlots, c.lightSlotId[0], c.lightRays[0]);
	return Report(ok, name);
}

static bool
CheckLightRaysOwner(void)
{
	const char *name = "a light's rays skip its own owner's body";
	NightRig rig;
	NightBegin(&rig);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 20.0f, 20.0f, -20.0f, -20.0f, 75.0f, col, nil, backMat);
	Atomic *blocker = MakeAtomic(geo);
	metal::LightDesc own = PointLight(71, 0.0f, 0.0f, 60.0f, 40.0f), other = own;
	own.owner = 1;
	other.owner = 2;
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 HX = (info.internalWidth + 1)/4, HY = (info.internalHeight + 1)/4;
	float tagged = -1.0f, untagged = -1.0f, foreign = -1.0f, open = -1.0f;
	bool began[4], read[4];
	metal::RayStats t = NightRays(&own, 1, &began[0], blocker, 1);
	read[0] = metal::readLightRawForTest(0, HX, HY, &tagged) != 0;
	metal::RayStats u = NightRays(&own, 1, &began[1], blocker, 0);
	read[1] = metal::readLightRawForTest(0, HX, HY, &untagged) != 0;
	NightRays(&other, 1, &began[2], blocker, 1);
	read[2] = metal::readLightRawForTest(0, HX, HY, &foreign) != 0;
	NightRays(&own, 1, &began[3]);
	read[3] = metal::readLightRawForTest(0, HX, HY, &open) != 0;
	DestroyAtomic(blocker);
	geo->destroy();
	NightEnd(&rig);
	bool ok = began[0] && began[1] && began[2] && began[3] && read[0] && read[1] && read[2] && read[3] && t.ownedInstances > 0 && u.ownedInstances == 0 &&
		tagged == 1.0f && untagged == 0.0f && foreign == 0.0f && open == 1.0f;
	if(!ok)
		Detail("  prepass %d %d %d %d read %d %d %d %d; owned %u/%u; tagged %.2f (want 1) untagged %.2f (want 0) other owner %.2f (want 0) no body %.2f (want 1)\n",
			began[0], began[1], began[2], began[3], read[0], read[1], read[2], read[3], t.ownedInstances, u.ownedInstances, tagged, untagged, foreign, open);
	return Report(ok, name);
}

static bool
CheckLightShadowBox(void)
{
	const char *name = "a shadowed light over a box leaves the box's shadow on the floor and a budget of 0 removes it";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 6.0f, 6.0f, -6.0f, -6.0f, 89.0f, col, nil, backMat);
	Atomic *box = MakeAtomic(geo);
	metal::LightDesc two[2] = { PointLight(81, 0.0f, 0.0f, 86.0f, 40.0f), PointLight(82, 90.0f, 0.0f, 86.0f, 150.0f) };
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 W = info.internalWidth, H = info.internalHeight, CX = W/2, CY = H/2;
	const float perPixel = 180.0f/(float)W, shadowWidth = 16.0f/perPixel;
	bool began = true;
	for(int f = 0; f < 16; f++)
		began = NightFrame(two, 2, box) && began;
	metal::rtWaitForTest();
	metal::RayStats st = metal::getRayStats();
	float centre = -1.0f, lit = -1.0f, v = 0.0f, dark = 0.0f;
	bool read = metal::readLightShadowForTest(0, CX, CY, &centre) != 0;
	read = metal::readLightShadowForTest(0, CX + (int32)(20.0f/perPixel), CY, &lit) != 0 && read;
	const int32 reach = (int32)(shadowWidth*0.5f) + 12;
	for(int32 x = CX - reach; x <= CX + reach; x++){
		read = metal::readLightShadowForTest(0, x, CY, &v) != 0 && read;
		dark += 1.0f - v;
	}
	high.shadowedLights = 0;
	metal::setRenderSettings(&high);
	for(int f = 0; f < 3; f++)
		began = NightFrame(two, 2, box) && began;
	float gone = -1.0f;
	bool readGone = metal::readLightShadowForTest(0, CX, CY, &gone) != 0;
	uint8 *budgetZero = CompositeFrame();
	SetMode(metal::LIGHTS_ON);
	for(int f = 0; f < 3; f++)
		began = NightFrame(two, 2, box) && began;
	uint8 *unshadowed = CompositeFrame();
	int largest = 0, n = lightCamera->frameBuffer->width*lightCamera->frameBuffer->height*4;
	for(int i = 0; i < n; i++)
		if(i % 4 != 3)
			largest = std::max(largest, abs(budgetZero[i] - unshadowed[i]));
	delete[] budgetZero;
	delete[] unshadowed;
	DestroyAtomic(box);
	geo->destroy();
	NightEnd(&rig);
	bool ok = began && read && st.lightSlots == 2 && st.lightSlotId[0] == 81 && lit > 0.8f && centre < 0.2f*lit && fabsf(dark - shadowWidth) <= 2.0f && !readGone && largest <= 2;
	if(!ok)
		Detail("  prepass %d read %d slots %u; shadow at centre %.2f (want below %.2f), lit %.2f; dark width %.1f pixels (want %.1f); budget 0 reads %d (want 0), frame differs by %d (want 2 at most)\n",
			began, read, st.lightSlots, centre, 0.2f*lit, lit, dark, shadowWidth, readGone, largest);
	return Report(ok, name);
}

static void
PrintClusterTiming(void)
{
	const int32 W = 2560, H = 1440;
	Raster *color = Raster::create(W, H, 0, Raster::CAMERA), *depth = Raster::create(W, H, 0, Raster::ZBUFFER);
	if(color == nil || depth == nil){
		printf("  cluster pass timing: not measured (no %dx%d target)\n", W, H);
		return;
	}
	Raster *savedColor = lightCamera->frameBuffer, *savedDepth = lightCamera->zBuffer;
	lightCamera->frameBuffer = color;
	lightCamera->zBuffer = depth;
	std::vector<metal::LightDesc> lights;
	uint32 seed = 12345;
	auto rnd = [&](){ seed = seed*1664525u + 1013904223u; return (float)(seed >> 8)/(float)(1 << 24); };
	for(int i = 0; i < LT_MAX_LIGHTS; i++){
		float z = 3.0f + 77.0f*rnd();
		metal::LightDesc d = PointLight((uint32)i, (rnd()*2.0f - 1.0f)*0.8f*z, (rnd()*2.0f - 1.0f)*0.6f*z, z, 4.0f + 8.0f*rnd());
		if(i % 4 == 0){
			d.dir[1] = -1.0f;
			d.cosOuter = cosf(35.0f*3.14159265f/180.0f);
			d.cosInner = cosf(25.0f*3.14159265f/180.0f);
			d.flags = metal::LIGHT_SPOT;
		}
		lights.push_back(d);
	}
	float ms[7];
	int32 measured = 0;
	for(int f = 0; f < 12; f++){
		LightFrame(lights.data(), (int32)lights.size());
		uint32 ids[LT_PER_CLUSTER];
		int32 n;
		metal::readClusterForTest(0, 0, 0, ids, 1, &n);
		metal::LightStats st = metal::getLightStats();
		if(f >= 5 && st.clusterMs >= 0.0f)
			ms[measured++] = st.clusterMs;
	}
	metal::FrameInfo info = metal::getFrameInfo();
	metal::LightStats st = metal::getLightStats();
	lightCamera->frameBuffer = savedColor;
	lightCamera->zBuffer = savedDepth;
	depth->destroy();
	color->destroy();
	if(measured == 0){
		printf("  cluster pass timing: not measured (no stage-boundary counter samples)\n");
		return;
	}
	std::sort(ms, ms + measured);
	printf("  cluster pass timing, 256 lights (a quarter spots), internal %dx%d, median of %d: %.3f ms (worst cluster %u lights, %u lost)\n",
		info.internalWidth, info.internalHeight, measured, ms[measured/2], st.maxPerCluster, st.clusterLoss);
}

int
RunLightChecks(Camera *camera)
{
	failures = 0;
	lightCamera = camera;
	WorldOpen(camera);
	LightsOn(metal::LIGHTS_ON);
	LightFrame(nil, 0);
	CheckLightCluster();
	CheckLightSpotCluster();
	CheckLightDistanceCap();
	CheckLightNoPass();
	CheckLightSlotPerFrame();
	CheckLightStatsScene();
	CheckLightClusterLoss();
	CheckLightStatsLine();
	CheckLightSceneCamera();
	CheckLightLate();
	CheckLightDegenerate();
	CheckLightFalloffPlain();
	CheckLightFalloffRelit();
	CheckLightVertexLit();
	CheckLightSpotCutoff();
	CheckLightColourGloss();
	CheckLightEmissive();
	CheckLightOptIn();
	CheckLightViewChange();
	CheckLightShadowedPlain();
	CheckLightShadowedRelit();
	CheckLightPlainTwinFailure();
	CheckLightsOffIdenticalOneSample();
	CheckLightCountView();
	CheckLightInstancesAtNight();
	CheckLightRaysInCone();
	CheckLightRaysOwner();
	CheckLightShadowBox();
	PrintClusterTiming();
	LightsOff();
	WorldClose();
	return failures;
}
