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

struct NightRead
{
	metal::LightStats lights;
	metal::RayStats rays;
	char line[2048];
};

static bool
NightFrame(const metal::LightDesc *lights, int32 n, Atomic *blocker = nil, uint32 blockerOwner = 0, Atomic *wall = nil, NightRead *read = nil, bool prepass = true, bool show = true)
{
	RGBA sky = { 64, 64, 64, 255 };
	Atomic *surface = wall ? wall : backdrop;
	metal::beginScene(lightCamera);
	WorldBegin(sky);
	SetRenderState(VERTEXALPHA, 0);
	if(wall)
		SetRenderState(CULLMODE, CULLNONE);
	metal::setLights(lights, n);
	bool began = prepass && metal::beginPrepass() != 0;
	if(began){
		metal::addShadowCaster(surface, metal::SHADOWCASTER_STATIC);
		if(blocker){
			metal::setShadowOwner(blockerOwner);
			metal::addShadowCaster(blocker, metal::SHADOWCASTER_STATIC);
			metal::setShadowOwner(0);
		}
		surface->render();
		metal::endPrepass();
	}
	surface->render();
	metal::endScene(nil);
	if(read){
		metal::logStats();
		read->lights = metal::getLightStats();
		read->rays = metal::getRayStats();
		snprintf(read->line, sizeof(read->line), "%s", metal::getStatsLine());
	}
	if(show)
		WorldEnd();
	else
		lightCamera->endUpdate();
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

static bool
CheckLightShadowTermOnly(void)
{
	const char *name = "a light's shadow factor darkens only that light's term";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 1;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 6.0f, 6.0f, -6.0f, -6.0f, 89.0f, col, nil, backMat);
	Atomic *box = MakeAtomic(geo);
	RGBA grey = { 90, 90, 90, 255 }, greys[4] = { grey, grey, grey, grey };
	Material *wallMat = LitMaterial();
	Geometry *wallGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 60.0f, 45.0f, -60.0f, -45.0f, 90.0f, greys, nil, wallMat);
	Atomic *wall = MakeAtomic(wallGeo);
	metal::LightDesc shadowed = PointLight(91, 0.0f, 0.0f, 86.0f, 40.0f), plain = PointLight(92, 12.0f, 0.0f, 86.0f, 100.0f);
	shadowed.flags = metal::LIGHT_PINNED;
	shadowed.color[0] = shadowed.color[1] = shadowed.color[2] = 20.0f;
	plain.color[0] = plain.color[1] = plain.color[2] = 30.0f;
	metal::LightDesc both[2] = { shadowed, plain };
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 W = info.internalWidth, H = info.internalHeight, CX = W/2, CY = H/2;
	const float perPixel = 180.0f/(float)W;
	const int32 litX = CX + (int32)(10.0f/perPixel);
	bool began = true;
	for(int f = 0; f < 16; f++)
		began = NightFrame(both, 2, box, 0, wall) && began;
	metal::rtWaitForTest();
	metal::RayStats st = metal::getRayStats();
	float inShadow[3], inLight[3], factorDark = -1.0f, factorLit = -1.0f;
	ReadAt(CX, CY, inShadow);
	ReadAt(litX, CY, inLight);
	bool read = metal::readLightShadowForTest(0, CX, CY, &factorDark) != 0;
	read = metal::readLightShadowForTest(0, litX, CY, &factorLit) != 0 && read;
	SetMode(metal::LIGHTS_ON);
	float base[2][3], onlyPlain[2][3], onlyShadowed[2][3];
	const int32 xs[2] = { CX, litX };
	const metal::LightDesc *sets[3] = { nil, &plain, &shadowed };
	for(int k = 0; k < 3; k++){
		for(int f = 0; f < 4; f++)
			began = NightFrame(sets[k], k ? 1 : 0, box, 0, wall) && began;
		for(int i = 0; i < 2; i++)
			ReadAt(xs[i], CY, k == 0 ? base[i] : k == 1 ? onlyPlain[i] : onlyShadowed[i]);
	}
	DestroyAtomic(wall);
	wallGeo->destroy();
	wallMat->destroy();
	DestroyAtomic(box);
	geo->destroy();
	NightEnd(&rig);
	bool ok = began && read && st.lightSlots == 1 && st.lightSlotId[0] == 91 && factorDark < 0.05f && factorLit > 0.9f;
	float factors[2] = { factorDark, factorLit };
	for(int i = 0; i < 2; i++)
		for(int c = 0; c < 3; c++){
			float *got = i ? inLight : inShadow;
			float term = onlyShadowed[i][c] - base[i][c], other = onlyPlain[i][c];
			ok = ok && term > 0.02f && (i != 0 || other - base[i][c] > 0.02f) && fabsf(got[c] - (other + factors[i]*term)) <= 2.0f/255.0f;
			if(!ok)
				Detail("  pixel %d channel %d: reads %.4f, want %.4f (other light and base %.4f, shadowed term %.4f, factor %.3f)\n",
					i, c, got[c], other + factors[i]*term, other, term, factors[i]);
		}
	if(!ok)
		Detail("  prepass %d read %d slots %u id %u; factor in the shadow %.3f (want 0), beside it %.3f (want 1)\n",
			began, read, st.lightSlots, st.lightSlotId[0], factorDark, factorLit);
	return Report(ok, name);
}

static bool
CheckLightGroupedPair(void)
{
	const char *name = "two pinned lights in one group share one slot and both their terms take that slot's factor";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 6.0f, 6.0f, -6.0f, -6.0f, 89.0f, col, nil, backMat);
	Atomic *box = MakeAtomic(geo);
	RGBA grey = { 90, 90, 90, 255 }, greys[4] = { grey, grey, grey, grey };
	Material *wallMat = LitMaterial();
	Geometry *wallGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 60.0f, 45.0f, -60.0f, -45.0f, 90.0f, greys, nil, wallMat);
	Atomic *wall = MakeAtomic(wallGeo);
	metal::LightDesc pair[2] = { PointLight(93, -3.0f, 0.0f, 86.0f, 40.0f), PointLight(94, 3.0f, 0.0f, 86.0f, 40.0f) };
	metal::LightDesc third = PointLight(95, -40.0f, 0.0f, 86.0f, 14.0f);
	for(int i = 0; i < 2; i++){
		pair[i].flags = metal::LIGHT_PINNED;
		pair[i].group = 5;
		pair[i].color[0] = pair[i].color[1] = pair[i].color[2] = 20.0f;
	}
	third.color[0] = third.color[1] = third.color[2] = 10.0f;
	metal::LightDesc all[3] = { pair[0], pair[1], third };
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 W = info.internalWidth, H = info.internalHeight, CX = W/2, CY = H/2;
	const float perPixel = 180.0f/(float)W;
	const int32 litX = CX + (int32)(10.0f/perPixel);
	bool began = true;
	for(int f = 0; f < 16; f++)
		began = NightFrame(all, 3, box, 0, wall) && began;
	metal::rtWaitForTest();
	metal::RayStats st = metal::getRayStats();
	float inShadow[3], inLight[3], factorDark = -1.0f, factorLit = -1.0f;
	ReadAt(CX, CY, inShadow);
	ReadAt(litX, CY, inLight);
	bool read = metal::readLightShadowForTest(0, CX, CY, &factorDark) != 0;
	read = metal::readLightShadowForTest(0, litX, CY, &factorLit) != 0 && read;
	float midLeft = -1.0f, midRight = -1.0f;
	read = metal::readLightShadowForTest(0, CX - (int32)(8.5f/perPixel), CY, &midLeft) != 0 && read;
	read = metal::readLightShadowForTest(0, CX + (int32)(8.5f/perPixel), CY, &midRight) != 0 && read;
	SetMode(metal::LIGHTS_ON);
	float base[2][3], terms[2][2][3];
	const int32 xs[2] = { CX, litX };
	const metal::LightDesc *sets[3] = { nil, &pair[0], &pair[1] };
	for(int k = 0; k < 3; k++){
		for(int f = 0; f < 4; f++)
			began = NightFrame(sets[k], k ? 1 : 0, box, 0, wall) && began;
		for(int i = 0; i < 2; i++)
			ReadAt(xs[i], CY, k == 0 ? base[i] : terms[k - 1][i]);
	}
	DestroyAtomic(wall);
	wallGeo->destroy();
	wallMat->destroy();
	DestroyAtomic(box);
	geo->destroy();
	NightEnd(&rig);
	bool slots = st.lightSlots == 2 && st.lightSlotId[0] == 93 && st.lightSlotId[1] == 95;
	bool ok = began && read && slots && factorDark < 0.05f && factorLit > 0.9f && midLeft > 0.9f && midRight > 0.9f;
	const float factors[2] = { factorDark, factorLit };
	for(int i = 0; i < 2; i++)
		for(int c = 0; c < 3; c++){
			float *got = i ? inLight : inShadow;
			float a = terms[0][i][c] - base[i][c], b = terms[1][i][c] - base[i][c];
			float want = base[i][c] + factors[i]*(a + b);
			ok = ok && a > 0.02f && b > 0.02f && fabsf(got[c] - want) <= 2.0f/255.0f;
			if(!ok)
				Detail("  pixel %d channel %d: reads %.4f, want %.4f (base %.4f, terms %.4f and %.4f, factor %.3f)\n", i, c, got[c], want,
					base[i][c], a, b, factors[i]);
		}
	if(!ok)
		Detail("  prepass %d read %d slots %u ids %u %u (want 2, 93 and 95); factor in the shadow %.3f (want 0), beside it %.3f (want 1); past either member's own umbra edge %.3f and %.3f (want 1, the midpoint's umbra ends first)\n",
			began, read, st.lightSlots, st.lightSlotId[0], st.lightSlotId[1], factorDark, factorLit, midLeft, midRight);
	return Report(ok, name);
}

static void
CameraSlide(const Matrix &home, float x)
{
	Frame *f = lightCamera->getFrame();
	f->matrix = home;
	V3d d = { x, 0.0f, 0.0f };
	f->translate(&d, COMBINEPOSTCONCAT);
	f->updateObjects();
}

static bool
CheckLightGroupedPairFlip(void)
{
	const char *name = "a grouped pair keeps one slot identity and its history when the other member becomes the nearer one";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc pair[2] = { PointLight(161, -3.0f, 0.0f, 70.0f, 60.0f), PointLight(162, 3.0f, 0.0f, 70.0f, 60.0f) };
	for(int i = 0; i < 2; i++){
		pair[i].flags = metal::LIGHT_PINNED;
		pair[i].group = 7;
	}
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 HX = (info.internalWidth + 1)/4, HY = (info.internalHeight + 1)/4;
	Matrix home = lightCamera->getFrame()->matrix;
	bool began = true, ids = true;
	for(int f = 0; f < 12; f++){
		CameraSlide(home, f < 8 ? 2.0f : -2.0f);
		began = NightFrame(pair, 2) && began;
		metal::RayStats st = metal::getRayStats();
		ids = ids && (f < 3 || (st.lightSlots == 1 && st.lightSlotId[0] == 161));
	}
	float value = 0.0f, length = 0.0f;
	bool read = metal::readLightHistoryForTest(0, HX, HY, &value, &length) != 0;
	lightCamera->getFrame()->matrix = home;
	lightCamera->getFrame()->updateObjects();
	NightEnd(&rig);
	bool ok = began && read && ids && length >= 8.5f;
	if(!ok)
		Detail("  prepass %d read %d; the slot kept the identity 161 on every frame %d; the history length after four frames with the other member nearer is %.1f (want 9, a restart reads 4)\n",
			began, read, ids, length);
	return Report(ok, name);
}

static bool
CheckLightSparseSlots(void)
{
	const char *name = "a freed slot index stays a hole, the lights above it keep their index and history, and a new light takes the hole";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 3;
	metal::setRenderSettings(&high);
	metal::LightDesc all[3] = { PointLight(171, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(172, 0.0f, 0.0f, 70.0f, 60.0f), PointLight(173, 12.0f, 0.0f, 70.0f, 60.0f) };
	all[0].color[0] = all[0].color[1] = all[0].color[2] = 30.0f;
	all[1].color[0] = all[1].color[1] = all[1].color[2] = 20.0f;
	all[2].color[0] = all[2].color[1] = all[2].color[2] = 10.0f;
	metal::LightDesc gone[2] = { all[0], all[2] };
	metal::LightDesc again[3] = { all[0], PointLight(174, 6.0f, 0.0f, 70.0f, 60.0f), all[2] };
	again[1].color[0] = again[1].color[1] = again[1].color[2] = 20.0f;
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 HX = (info.internalWidth + 1)/4, HY = (info.internalHeight + 1)/4;
	bool began = true, order = true, holeKept = true, taken = true;
	metal::RayStats st;
	for(int f = 0; f < 6; f++){
		began = NightFrame(all, 3) && began;
		st = metal::getRayStats();
		order = order && (f < 3 || (st.lightSlots == 3 && st.lightSlotId[0] == 171 && st.lightSlotId[1] == 172 && st.lightSlotId[2] == 173));
	}
	for(int f = 0; f < 3; f++){
		began = NightFrame(gone, 2) && began;
		st = metal::getRayStats();
		holeKept = holeKept && st.lightSlots == 3 && st.lightSlotId[0] == 171 && st.lightSlotId[1] == 0 && st.lightSlotId[2] == 173;
	}
	for(int f = 0; f < 3; f++){
		began = NightFrame(again, 3) && began;
		st = metal::getRayStats();
		taken = taken && st.lightSlots == 3 && st.lightSlotId[0] == 171 && st.lightSlotId[1] == 174 && st.lightSlotId[2] == 173;
	}
	float value, top = 0.0f, fresh = 0.0f;
	bool read = metal::readLightHistoryForTest(2, HX, HY, &value, &top) != 0;
	read = metal::readLightHistoryForTest(1, HX, HY, &value, &fresh) != 0 && read;
	for(int f = 0; f < 6; f++)
		began = NightFrame(all, 3) && began;
	for(int f = 0; f < 4; f++)
		began = NightFrame(gone, 2) && began;
	metal::rtWaitForTest();
	st = metal::getRayStats();
	bool noRays = st.lightRays[0] > 0 && st.lightRays[1] == 0 && st.lightRays[2] > 0;
	NightEnd(&rig);
	bool ok = began && read && order && holeKept && taken && top >= 9.0f && fresh <= 3.5f && noRays;
	if(!ok)
		Detail("  prepass %d read %d; ids 171 172 173 at first %d; with the middle gone the ids hold %d; a new light takes the hole %d; the top slot's history length %.1f (want 9 or more, a move reads at most 6), the hole's new light %.1f (want 3); the hole traced rays %u %u %u (want some, 0, some)\n",
			began, read, order, holeKept, taken, top, fresh, st.lightRays[0], st.lightRays[1], st.lightRays[2]);
	return Report(ok, name);
}

static bool
CheckLightHeldSlots(void)
{
	const char *name = "held lights whose scores cross keep their slots";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc ab[2] = { PointLight(101, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(102, 12.0f, 0.0f, 70.0f, 60.0f) };
	bool began = true, stable = true;
	uint32 firstId[2] = { 0, 0 };
	for(int f = 0; f < 8; f++){
		int bright = f < 3 ? 0 : (f & 1);
		for(int i = 0; i < 2; i++)
			ab[i].color[0] = ab[i].color[1] = ab[i].color[2] = i == bright ? 30.0f : 10.0f;
		began = NightFrame(ab, 2) && began;
		metal::RayStats st = metal::getRayStats();
		if(f == 2){
			firstId[0] = st.lightSlotId[0];
			firstId[1] = st.lightSlotId[1];
		}
		if(f >= 2)
			stable = stable && st.lightSlots == 2 && st.lightSlotId[0] == firstId[0] && st.lightSlotId[1] == firstId[1];
	}
	NightEnd(&rig);
	bool ok = began && stable && firstId[0] == 101 && firstId[1] == 102;
	if(!ok)
		Detail("  prepass %d; first frames hold %u %u (want 101 102), stable through the crossings %d\n", began, firstId[0], firstId[1], stable);
	return Report(ok, name);
}

static bool
CheckLightReplacedSlot(void)
{
	const char *name = "a slot given a new light starts without the old light's shadow";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 6.0f, 6.0f, -6.0f, -6.0f, 89.0f, col, nil, backMat);
	Atomic *box = MakeAtomic(geo);
	RGBA grey = { 90, 90, 90, 255 }, greys[4] = { grey, grey, grey, grey };
	Material *wallMat = LitMaterial();
	Geometry *wallGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 60.0f, 45.0f, -60.0f, -45.0f, 90.0f, greys, nil, wallMat);
	Atomic *wall = MakeAtomic(wallGeo);
	metal::LightDesc oldLight = PointLight(111, 0.0f, 0.0f, 86.0f, 40.0f), other = PointLight(112, 40.0f, 30.0f, 86.0f, 14.0f), fresh = PointLight(113, 6.0f, 0.0f, 86.0f, 40.0f);
	for(metal::LightDesc *l : { &oldLight, &other, &fresh })
		l->color[0] = l->color[1] = l->color[2] = 20.0f;
	metal::LightDesc before[2] = { oldLight, other }, after[2] = { fresh, other };
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 W = info.internalWidth, CX = W/2, CY = info.internalHeight/2;
	const float perPixel = 180.0f/(float)W;
	bool began = true;
	for(int f = 0; f < 16; f++)
		began = NightFrame(before, 2, box, 0, wall) && began;
	began = NightFrame(after, 2, box, 0, wall) && began;
	metal::rtWaitForTest();
	float v = 0.0f;
	bool read = true;
	metal::RayStats st = metal::getRayStats();
	float trace = 1.0f;
	for(float x : { 6.7f, 7.0f }){
		read = metal::readLightShadowForTest(0, CX - (int32)(x/perPixel), CY, &v) != 0 && read;
		trace = std::min(trace, v);
	}
	DestroyAtomic(wall);
	wallGeo->destroy();
	wallMat->destroy();
	DestroyAtomic(box);
	geo->destroy();
	NightEnd(&rig);
	bool ok = began && read && st.lightSlots == 2 && st.lightSlotId[0] == 113 && st.lightSlotId[1] == 112 && trace > 0.9f;
	if(!ok)
		Detail("  prepass %d read %d; slots %u ids %u %u (want 113 112); lowest factor on the old umbra's edge %.2f (want 1)\n",
			began, read, st.lightSlots, st.lightSlotId[0], st.lightSlotId[1], trace);
	return Report(ok, name);
}

static bool
CheckLightShadowTime(void)
{
	const char *name = "the stats carry the light shadow time and the slot count when the pass ran and zero when it did not";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc two[2] = { PointLight(121, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(122, 12.0f, 0.0f, 70.0f, 60.0f) };
	bool began = true;
	for(int f = 0; f < 4; f++)
		began = NightFrame(two, 2) && began;
	metal::rtWaitForTest();
	metal::LightStats ran = metal::getLightStats();
	metal::logStats();
	unsigned slots = 0;
	char ms[32] = "";
	const char *tail = strstr(metal::getStatsLine(), " lslots ");
	bool parsed = tail && sscanf(tail, " lslots %u lshadow %31s", &slots, ms) == 2;
	bool lineRan = parsed && slots == 2 && strcmp(ms, "-") != 0;
	for(int f = 0; f < 3; f++)
		began = NightFrame(nil, 0) && began;
	metal::rtWaitForTest();
	metal::LightStats idle = metal::getLightStats();
	metal::logStats();
	tail = strstr(metal::getStatsLine(), " lslots ");
	parsed = tail && sscanf(tail, " lslots %u lshadow %31s", &slots, ms) == 2;
	bool lineIdle = parsed && slots == 0 && strcmp(ms, "-") == 0;
	NightEnd(&rig);
	bool ok = began && ran.shadowed == 2 && ran.lightShadowMs > 0.0f && lineRan && idle.shadowed == 0 && idle.lightShadowMs == 0.0f && lineIdle;
	if(!ok)
		Detail("  prepass %d; with the pass: slots %u time %.3f ms (want 2, above 0), line ok %d; without: slots %u time %.3f (want 0, 0), line ok %d; line \"%s\"\n",
			began, ran.shadowed, ran.lightShadowMs, lineRan, idle.shadowed, idle.lightShadowMs, lineIdle, metal::getStatsLine());
	return Report(ok, name);
}

static bool
CheckLightStatsNoWait(void)
{
	const char *name = "the stats read as a host reads them, before the frame commits and with no wait, carry the light shadow time, the slot count and the owner counts and read zero once the pass stops";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 25.0f, 25.0f, -25.0f, -25.0f, 40.0f, col, nil, backMat);
	Atomic *blocker = MakeAtomic(geo);
	metal::LightDesc pair[2] = { PointLight(151, -1.0f, 0.0f, 10.0f, 95.0f), PointLight(152, 1.0f, 0.0f, 10.0f, 95.0f) };
	pair[0].owner = pair[1].owner = 1;
	bool began = true;
	NightRead ran, idle1, idle2;
	for(int f = 0; f < 8; f++)
		began = NightFrame(pair, 2, blocker, 1, nil, &ran) && began;
	unsigned slots = 0, listed = 0, overflow = 0;
	char ms[32] = "";
	const char *tail = strstr(ran.line, " lslots ");
	bool lineRan = tail && sscanf(tail, " lslots %u lshadow %31s lowner %u loverflow %u", &slots, ms, &listed, &overflow) == 4 && slots == 2 &&
		strcmp(ms, "-") != 0 && listed > 0;
	bool ranOk = ran.lights.shadowed == 2 && ran.lights.lightShadowMs > 0.0f && ran.lights.ownerListed > 0 && ran.rays.lightSlots == 2 &&
		ran.rays.lightMs > 0.0f && ran.rays.lightOwnerListed > 0 && ran.rays.lightSlotId[0] == 151 && ran.rays.lightSlotId[1] == 152;
	began = NightFrame(nil, 0, nil, 0, nil, &idle1) && began;
	began = NightFrame(nil, 0, nil, 0, nil, &idle2) && began;
	unsigned islots = 9, ilisted = 9;
	char ims[32] = "";
	tail = strstr(idle2.line, " lslots ");
	bool lineIdle = tail && sscanf(tail, " lslots %u lshadow %31s lowner %u loverflow %u", &islots, ims, &ilisted, &overflow) == 4 && islots == 0 &&
		strcmp(ims, "-") == 0 && ilisted == 0;
	auto quiet = [](const NightRead &r){
		return r.lights.shadowed == 0 && r.lights.lightShadowMs == 0.0f && r.lights.ownerListed == 0 && r.rays.lightSlots == 0 &&
			r.rays.lightMs == 0.0f && r.rays.lightOwnerListed == 0 && r.rays.lightOwnerOverflow == 0 && r.rays.lightSlotId[0] == 0 &&
			r.rays.lightRays[0] == 0;
	};
	bool idleOk = quiet(idle1) && quiet(idle2);
	bool alternate = true;
	for(int f = 0; f < 6; f++){
		NightRead r;
		bool on = f % 2 == 0;
		bool b = NightFrame(pair, 2, blocker, 1, nil, &r, on);
		began = (b == on) && began;
		alternate = alternate && (f == 0 ? true : on ? r.rays.lightSlots == 2 && r.lights.shadowed == 2 && r.rays.lightSlotId[0] == 151 : quiet(r));
	}
	DestroyAtomic(blocker);
	geo->destroy();
	NightEnd(&rig);
	bool ok = began && lineRan && ranOk && lineIdle && idleOk && alternate;
	if(!ok)
		Detail("  prepass %d; alternating frames read as each frame ran %d; with the pass: line ok %d, lights %u slots, %.3f ms, owner %u; rays %u slots, %.3f ms, owner %u (want 2, above 0, above 0); after it stopped: line ok %d, first idle frame %u slots %.3f ms, second %u slots %.3f ms, lights %u slots %.3f ms owner %u, rays %u slots %.3f ms owner %u (want 0); line \"%s\"\n",
			began, alternate, lineRan, ran.lights.shadowed, ran.lights.lightShadowMs, ran.lights.ownerListed, ran.rays.lightSlots, ran.rays.lightMs,
			ran.rays.lightOwnerListed, lineIdle, idle1.rays.lightSlots, idle1.rays.lightMs, idle2.rays.lightSlots, idle2.rays.lightMs, idle2.lights.shadowed, idle2.lights.lightShadowMs, idle2.lights.ownerListed, idle2.rays.lightSlots,
			idle2.rays.lightMs, idle2.rays.lightOwnerListed, ran.line);
	return Report(ok, name);
}

static bool
CheckLightStatsRestart(void)
{
	const char *name = "after the pass restarts the stats do not pair its slots with an older pass's time and ray counts";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc two[2] = { PointLight(181, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(182, 12.0f, 0.0f, 70.0f, 60.0f) };
	bool began = true;
	NightRead before, restart, later;
	for(int f = 0; f < 6; f++)
		began = NightFrame(two, 2, nil, 0, nil, &before) && began;
	metal::rtWaitForTest();
	metal::RayStats settled = metal::getRayStats();
	for(int f = 0; f < 3; f++)
		began = NightFrame(nil, 0) && began;
	began = NightFrame(two, 2) && began;
	began = NightFrame(two, 2, nil, 0, nil, &restart) && began;
	metal::rtWaitForTest();
	later.rays = metal::getRayStats();
	bool settledOk = settled.lightSlots == 2 && settled.lightMs > 0.0f && settled.lightRays[0] > 0 && settled.lightRays[1] > 0;
	bool restartOk = restart.rays.lightSlots == 2 && restart.rays.lightSlotId[0] == 181 && restart.rays.lightSlotId[1] == 182 &&
		restart.rays.lightMs == 0.0f && restart.rays.lightRays[0] == 0 && restart.rays.lightRays[1] == 0 && restart.lights.lightShadowMs == 0.0f;
	bool laterOk = later.rays.lightSlots == 2 && later.rays.lightMs > 0.0f && later.rays.lightRays[0] > 0 && later.rays.lightRays[1] > 0;
	NightEnd(&rig);
	bool ok = began && settledOk && restartOk && laterOk;
	if(!ok)
		Detail("  prepass %d; settled: %u slots %.3f ms rays %u %u (want 2, above 0); first frame after the restart: %u slots ids %u %u, %.3f ms, rays %u %u, lights %.3f ms (want 2 slots, 0 and 0 results); once it completed: %u slots %.3f ms rays %u %u (want 2, above 0)\n",
			began, settled.lightSlots, settled.lightMs, settled.lightRays[0], settled.lightRays[1], restart.rays.lightSlots, restart.rays.lightSlotId[0],
			restart.rays.lightSlotId[1], restart.rays.lightMs, restart.rays.lightRays[0], restart.rays.lightRays[1], restart.lights.lightShadowMs,
			later.rays.lightSlots, later.rays.lightMs, later.rays.lightRays[0], later.rays.lightRays[1]);
	return Report(ok, name);
}

static bool
CheckLightHoleWork(void)
{
	const char *name = "a hole in the slot span runs neither the accumulate nor the upsample, so the pass's work is that of the active slots";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 3;
	metal::setRenderSettings(&high);
	metal::LightDesc all[3] = { PointLight(201, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(202, 0.0f, 0.0f, 70.0f, 60.0f), PointLight(203, 12.0f, 0.0f, 70.0f, 60.0f) };
	all[0].color[0] = all[0].color[1] = all[0].color[2] = 30.0f;
	all[1].color[0] = all[1].color[1] = all[1].color[2] = 20.0f;
	all[2].color[0] = all[2].color[1] = all[2].color[2] = 10.0f;
	metal::LightDesc gone[2] = { all[0], all[2] };
	bool began = true;
	for(int f = 0; f < 5; f++)
		began = NightFrame(all, 3) && began;
	metal::RayStats full = metal::getRayStats();
	for(int f = 0; f < 3; f++)
		began = NightFrame(gone, 2) && began;
	metal::RayStats holed = metal::getRayStats();
	for(int f = 0; f < 3; f++)
		began = NightFrame(all, 3) && began;
	metal::RayStats back = metal::getRayStats();
	NightEnd(&rig);
	bool ok = began && full.lightSlots == 3 && full.lightSlices == 6 && holed.lightSlots == 3 && holed.lightSlices == 4 && holed.lightSlotId[1] == 0 &&
		back.lightSlots == 3 && back.lightSlices == 6;
	if(!ok)
		Detail("  prepass %d; three lights: span %u, accumulate and upsample dispatches %u (want 3, 6); the middle one gone: span %u, dispatches %u, id %u (want 3, 4, 0); back: span %u, dispatches %u (want 3, 6)\n",
			began, full.lightSlots, full.lightSlices, holed.lightSlots, holed.lightSlices, holed.lightSlotId[1], back.lightSlots, back.lightSlices);
	return Report(ok, name);
}

static bool
CheckLightHoleMarkerId(void)
{
	const char *name = "a light whose id equals the empty slot marker is a light, not a hole, and a light taking a hole always restarts";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 3;
	metal::setRenderSettings(&high);
	const uint32 marker = 0xFFFFFFFFu;
	metal::LightDesc all[3] = { PointLight(211, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(marker, 0.0f, 0.0f, 70.0f, 60.0f), PointLight(213, 12.0f, 0.0f, 70.0f, 60.0f) };
	all[0].color[0] = all[0].color[1] = all[0].color[2] = 30.0f;
	all[1].color[0] = all[1].color[1] = all[1].color[2] = 20.0f;
	all[2].color[0] = all[2].color[1] = all[2].color[2] = 10.0f;
	metal::LightDesc gone[2] = { all[0], all[2] };
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 HX = (info.internalWidth + 1)/4, HY = (info.internalHeight + 1)/4;
	bool began = true;
	for(int f = 0; f < 6; f++)
		began = NightFrame(all, 3) && began;
	metal::RayStats first = metal::getRayStats();
	float value, grown = 0.0f, fresh = 0.0f, top = 0.0f;
	bool read = metal::readLightHistoryForTest(1, HX, HY, &value, &grown) != 0;
	bool idsOk = first.lightSlots == 3 && first.lightSlotId[0] == 211 && first.lightSlotId[1] == marker && first.lightSlotId[2] == 213 && first.lightSlices == 6;
	for(int f = 0; f < 4; f++)
		began = NightFrame(gone, 2) && began;
	metal::RayStats holed = metal::getRayStats();
	bool holeOk = holed.lightSlots == 3 && holed.lightSlotId[1] == 0 && holed.lightSlices == 4;
	for(int f = 0; f < 3; f++)
		began = NightFrame(all, 3) && began;
	read = metal::readLightHistoryForTest(1, HX, HY, &value, &fresh) != 0 && read;
	read = metal::readLightHistoryForTest(2, HX, HY, &value, &top) != 0 && read;
	NightEnd(&rig);
	bool ok = began && read && idsOk && holeOk && grown >= 4.5f && fresh <= 3.5f && top >= 11.0f;
	if(!ok)
		Detail("  prepass %d read %d; slots %u, ids %u %u %u, dispatches %u (want 3, 211 4294967295 213, 6); with it gone the hole reads id %u and %u dispatches (want 0, 4); the history of the slot with the marker id after six frames %.1f (want 5 or more), after it left and came back into the hole %.1f (want 3, a restart), the slot above %.1f (want 12)\n",
			began, read, first.lightSlots, first.lightSlotId[0], first.lightSlotId[1], first.lightSlotId[2], first.lightSlices, holed.lightSlotId[1], holed.lightSlices, grown, fresh, top);
	return Report(ok, name);
}

static bool
CheckLightGroupMembership(void)
{
	const char *name = "a grouped slot restarts its history when a member leaves or joins";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc pair[2] = { PointLight(221, -3.0f, 0.0f, 70.0f, 60.0f), PointLight(222, 3.0f, 0.0f, 70.0f, 60.0f) };
	for(int i = 0; i < 2; i++){
		pair[i].flags = metal::LIGHT_PINNED;
		pair[i].group = 9;
	}
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 HX = (info.internalWidth + 1)/4, HY = (info.internalHeight + 1)/4;
	bool began = true, ids = true;
	float value, together = 0.0f, left = 0.0f, joined = 0.0f;
	for(int f = 0; f < 8; f++)
		began = NightFrame(pair, 2) && began;
	bool read = metal::readLightHistoryForTest(0, HX, HY, &value, &together) != 0;
	for(int f = 0; f < 3; f++){
		began = NightFrame(pair, 1) && began;
		metal::RayStats st = metal::getRayStats();
		ids = ids && st.lightSlots == 1 && st.lightSlotId[0] == 221;
	}
	read = metal::readLightHistoryForTest(0, HX, HY, &value, &left) != 0 && read;
	for(int f = 0; f < 3; f++){
		began = NightFrame(pair, 2) && began;
		metal::RayStats st = metal::getRayStats();
		ids = ids && st.lightSlots == 1 && st.lightSlotId[0] == 221;
	}
	read = metal::readLightHistoryForTest(0, HX, HY, &value, &joined) != 0 && read;
	NightEnd(&rig);
	bool ok = began && read && ids && together >= 6.5f && left <= 3.5f && joined <= 3.5f;
	if(!ok)
		Detail("  prepass %d read %d; the identity stayed 221 %d; history length with both members %.1f (want 7 or more), three frames after the higher one left %.1f and three after it came back %.1f (want 3 and 3, a restart each)\n",
			began, read, ids, together, left, joined);
	return Report(ok, name);
}

static bool
CheckLightGroupReplaced(void)
{
	const char *name = "a grouped slot restarts its history when a member is replaced at the same count or a member's radius moves between zero and positive";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc pair[2] = { PointLight(231, -3.0f, 0.0f, 70.0f, 60.0f), PointLight(232, 3.0f, 0.0f, 70.0f, 60.0f) };
	for(int i = 0; i < 2; i++){
		pair[i].flags = metal::LIGHT_PINNED;
		pair[i].group = 9;
	}
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 HX = (info.internalWidth + 1)/4, HY = (info.internalHeight + 1)/4;
	bool began = true, ids = true, read = true;
	float value, together = 0.0f, replaced = 0.0f, settled = 0.0f, dark = 0.0f, lit = 0.0f;
	auto history = [&](float *len){
		read = metal::readLightHistoryForTest(0, HX, HY, &value, len) != 0 && read;
	};
	auto run = [&](int frames){
		for(int f = 0; f < frames; f++){
			began = NightFrame(pair, 2) && began;
			metal::RayStats st = metal::getRayStats();
			ids = ids && st.lightSlots == 1 && st.lightSlotId[0] == 231;
		}
	};
	run(8);
	history(&together);
	pair[1].id = 233;
	pair[1].pos[0] = 4.0f;
	run(3);
	history(&replaced);
	run(5);
	history(&settled);
	pair[1].radius = 0.0f;
	run(3);
	history(&dark);
	pair[1].radius = 60.0f;
	run(3);
	history(&lit);
	NightEnd(&rig);
	bool ok = began && read && ids && together >= 6.5f && replaced <= 3.5f && settled >= 6.5f && dark <= 3.5f && lit <= 3.5f;
	if(!ok)
		Detail("  prepass %d read %d; the identity stayed 231 %d; history length with the pair %.1f (want 7 or more), three frames after the higher member was replaced by another id %.1f (want 3, a restart), five frames on %.1f (want 7 or more), three after its radius went to 0 %.1f and three after it came back %.1f (want 3 and 3, a restart each)\n",
			began, read, ids, together, replaced, settled, dark, lit);
	return Report(ok, name);
}

static bool
CheckLightPinnedZeroRadius(void)
{
	const char *name = "a pinned light of radius 0 takes a slot that runs like any other and is not taken for a hole";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc two[2] = { PointLight(241, -12.0f, 0.0f, 70.0f, 0.0f), PointLight(242, 12.0f, 0.0f, 70.0f, 60.0f) };
	two[0].flags = metal::LIGHT_PINNED;
	bool began = true;
	for(int f = 0; f < 6; f++)
		began = NightFrame(two, 2) && began;
	metal::RayStats st = metal::getRayStats();
	bool have241 = st.lightSlotId[0] == 241 || st.lightSlotId[1] == 241, have242 = st.lightSlotId[0] == 242 || st.lightSlotId[1] == 242;
	NightEnd(&rig);
	bool ok = began && st.lightSlots == 2 && st.lightSlices == 4 && have241 && have242;
	if(!ok)
		Detail("  prepass %d; span %u, dispatches %u, ids %u %u (want 2, 4, and 241 with 242)\n", began, st.lightSlots, st.lightSlices, st.lightSlotId[0], st.lightSlotId[1]);
	return Report(ok, name);
}

static bool
CheckLightStatsTwoScenes(void)
{
	const char *name = "a host with two scenes in each frame still reads the light time and ray counts after the lit scene, and zero after the unlit one";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc two[2] = { PointLight(251, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(252, 12.0f, 0.0f, 70.0f, 60.0f) };
	bool began = true, litOk = true, unlitOk = true;
	NightRead lit, unlit;
	for(int f = 0; f < 8; f++){
		began = NightFrame(two, 2, nil, 0, nil, &lit, true, false) && began;
		began = !NightFrame(nil, 0, nil, 0, nil, &unlit, false, true) && began;
		if(f >= 5){
			litOk = litOk && lit.rays.lightSlots == 2 && lit.rays.lightMs > 0.0f && lit.rays.lightRays[0] > 0 && lit.rays.lightRays[1] > 0;
			unlitOk = unlitOk && unlit.rays.lightSlots == 0 && unlit.rays.lightMs == 0.0f && unlit.rays.lightRays[0] == 0;
		}
	}
	NightEnd(&rig);
	bool ok = began && litOk && unlitOk;
	if(!ok)
		Detail("  prepass %d; after the lit scene: %u slots, %.3f ms, rays %u %u (want 2, above 0); after the unlit one: %u slots, %.3f ms, rays %u (want 0)\n",
			began, lit.rays.lightSlots, lit.rays.lightMs, lit.rays.lightRays[0], lit.rays.lightRays[1], unlit.rays.lightSlots, unlit.rays.lightMs, unlit.rays.lightRays[0]);
	return Report(ok, name);
}

static bool
CheckLightOwnerOverflow(void)
{
	const char *name = "the light owner list holds the entries of every slot and an overflow is counted";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 25.0f, 25.0f, -25.0f, -25.0f, 40.0f, col, nil, backMat);
	Atomic *blocker = MakeAtomic(geo);
	metal::LightDesc pair[2] = { PointLight(131, -1.0f, 0.0f, 10.0f, 95.0f), PointLight(132, 1.0f, 0.0f, 10.0f, 95.0f) };
	pair[0].owner = pair[1].owner = 1;
	metal::FrameInfo info = metal::getFrameInfo();
	const uint32 oneSlice = shOwnerListCapacity((uint32)(info.internalWidth + 1)/2, (uint32)(info.internalHeight + 1)/2);
	bool began = true;
	for(int f = 0; f < 4; f++)
		began = NightFrame(pair, 2, blocker, 1) && began;
	metal::rtWaitForTest();
	metal::RayStats full = metal::getRayStats();
	metal::logStats();
	unsigned listed = 0, overflow = 0;
	const char *tail = strstr(metal::getStatsLine(), " lowner ");
	bool parsed = tail && sscanf(tail, " lowner %u loverflow %u", &listed, &overflow) == 2;
	uint32 half = full.lightOwnerListed/2;
	metal::setShadowOwnerListCapForTest(half);
	for(int f = 0; f < 4; f++)
		began = NightFrame(pair, 2, blocker, 1) && began;
	metal::rtWaitForTest();
	metal::RayStats cut = metal::getRayStats();
	metal::setShadowOwnerListCapForTest(~0u);
	DestroyAtomic(blocker);
	geo->destroy();
	NightEnd(&rig);
	bool ok = began && parsed && full.lightSlots == 2 && full.lightOwnerListed > oneSlice && full.lightOwnerOverflow == 0 && listed == full.lightOwnerListed &&
		overflow == 0 && cut.lightOwnerListed == full.lightOwnerListed && cut.lightOwnerOverflow == full.lightOwnerListed - half;
	if(!ok)
		Detail("  prepass %d line %d (%u, %u); slots %u; listed %u of one slice's %u with overflow %u; cut to %u: listed %u overflow %u (want %u)\n",
			began, parsed, listed, overflow, full.lightSlots, full.lightOwnerListed, oneSlice, full.lightOwnerOverflow, half, cut.lightOwnerListed,
			cut.lightOwnerOverflow, full.lightOwnerListed - half);
	return Report(ok, name);
}

static bool
CheckLightTargetsRelease(void)
{
	const char *name = "the light targets and the shadow array's reference go with the tier and with the shadowed setting";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc two[2] = { PointLight(141, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(142, 12.0f, 0.0f, 70.0f, 60.0f) };
	bool began = true;
	auto run = [&](){
		for(int f = 0; f < 3; f++)
			began = NightFrame(two, 2) && began;
		metal::rtWaitForTest();
	};
	run();
	float live = metal::getRayStats().shadowMegabytes;
	bool held = metal::lightShadowHeldForTest() != 0;
	metal::RenderSettings s = high;
	s.tier = metal::TIER_ENHANCED;
	metal::setRenderSettings(&s);
	float enhanced = metal::getRayStats().shadowMegabytes;
	bool heldEnhanced = metal::lightShadowHeldForTest() != 0;
	metal::setRenderSettings(&high);
	run();
	float back = metal::getRayStats().shadowMegabytes;
	SetMode(metal::LIGHTS_ON);
	bool heldOff = metal::lightShadowHeldForTest() != 0;
	float off = metal::getRayStats().shadowMegabytes;
	NightEnd(&rig);
	bool ok = began && live > 0.0f && held && enhanced == 0.0f && !heldEnhanced && back > 0.0f && off == 0.0f && !heldOff;
	if(!ok)
		Detail("  prepass %d; running %.2f MB held %d (want above 0, 1); enhanced tier %.2f MB held %d (want 0, 0); back %.2f MB; lights on %.2f MB held %d (want 0, 0)\n",
			began, live, held, enhanced, heldEnhanced, back, off, heldOff);
	return Report(ok, name);
}

static bool
CheckLightCutoutAtNight(void)
{
	const char *name = "a cutout texel over a shadowed floor reads its own light factor at night as it does with a sun pass";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 1;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *boxGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 6.0f, 6.0f, -6.0f, -6.0f, 89.0f, col, nil, backMat);
	Atomic *box = MakeAtomic(boxGeo);
	RGBA grey = { 90, 90, 90, 255 }, greys[4] = { grey, grey, grey, grey };
	Material *wallMat = LitMaterial();
	Geometry *wallGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 60.0f, 45.0f, -60.0f, -45.0f, 90.0f, greys, nil, wallMat);
	Atomic *wall = MakeAtomic(wallGeo);
	RGBA fringe = { 255, 96, 32, 191 };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	RGBA texels[16];
	for(int i = 0; i < 16; i++)
		texels[i] = fringe;
	Texture *tex = MakeTexture(4, 4, texels, Texture::LINEAR, Texture::CLAMP, Texture::CLAMP);
	Material *cutMat = LitMaterial();
	cutMat->setTexture(tex);
	Geometry *cutGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT | Geometry::TEXTURED, 14.0f, 14.0f, -14.0f, -14.0f, 70.0f,
		greys, uv, cutMat);
	Atomic *cut = MakeAtomic(cutGeo);
	metal::LightDesc light = PointLight(151, 0.0f, 0.0f, 60.0f, 40.0f);
	light.color[0] = light.color[1] = light.color[2] = 20.0f;
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 CX = info.internalWidth/2, CY = info.internalHeight/2;
	RGBA sky = { 64, 64, 64, 255 };
	bool began = true;
	auto frame = [&](){
		metal::beginScene(lightCamera);
		WorldBegin(sky);
		SetRenderState(VERTEXALPHA, 0);
		SetRenderState(CULLMODE, CULLNONE);
		SetRenderState(SRCBLEND, BLENDSRCALPHA);
		SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
		metal::setLights(&light, 1);
		bool b = metal::beginPrepass() != 0;
		if(b){
			metal::addShadowCaster(wall, metal::SHADOWCASTER_STATIC);
			metal::addShadowCaster(box, metal::SHADOWCASTER_STATIC);
			wall->render();
			SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
			SetRenderState(ALPHATESTREF, 128);
			cut->render();
			metal::endPrepass();
		}
		wall->render();
		SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
		SetRenderState(ALPHATESTREF, 128);
		cut->render();
		metal::endScene(nil);
		WorldEnd();
		began = b && began;
	};
	metal::StateStats s0 = metal::getStateStats();
	for(int f = 0; f < 16; f++)
		frame();
	uint32 cutouts = metal::getStateStats().prepassCutoutDraws - s0.prepassCutoutDraws;
	metal::rtWaitForTest();
	metal::RayStats night = metal::getRayStats();
	float atNight = -1.0f, withSun = -1.0f;
	bool read = metal::readLightShadowForTest(0, CX, CY, &atNight) != 0;
	V3d axis = { 0.0f, 1.0f, 0.0f };
	rig.sunFrame->rotate(&axis, 180.0f, COMBINEREPLACE);
	rig.sunFrame->updateObjects();
	for(int f = 0; f < 16; f++)
		frame();
	metal::rtWaitForTest();
	metal::RayStats day = metal::getRayStats();
	read = metal::readLightShadowForTest(0, CX, CY, &withSun) != 0 && read;
	DestroyAtomic(cut);
	cutGeo->destroy();
	cutMat->destroy();
	tex->destroy();
	DestroyAtomic(wall);
	wallGeo->destroy();
	wallMat->destroy();
	DestroyAtomic(box);
	boxGeo->destroy();
	NightEnd(&rig);
	bool ok = began && read && night.raysPerFrame == 0 && day.raysPerFrame > 0 && night.lightSlots == 1 && day.lightSlots == 1 && atNight > 0.8f && withSun > 0.8f && cutouts > 0;
	if(!ok)
		Detail("  prepass %d read %d; sun pass rays at night %u (want 0), with the sun %u (want some); light factor under the fringe at night %.2f, with the sun %.2f (want both near 1, the cutout's own); cutout draws %u (want some)\n",
			began, read, night.raysPerFrame, day.raysPerFrame, atNight, withSun, cutouts);
	return Report(ok, name);
}

static bool
CheckLightMovingCaster(void)
{
	const char *name = "a caster moving under a light leaves a trail of at most two half-resolution cells and no lit pixel inside its umbra";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 1;
	metal::setRenderSettings(&high);
	RGBA white = { 255, 255, 255, 255 }, col[4] = { white, white, white, white };
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 6.0f, 6.0f, -6.0f, -6.0f, 89.0f, col, nil, backMat);
	Atomic *box = MakeAtomic(geo);
	RGBA grey = { 90, 90, 90, 255 }, greys[4] = { grey, grey, grey, grey };
	Material *wallMat = LitMaterial();
	Geometry *wallGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, 60.0f, 45.0f, -60.0f, -45.0f, 90.0f, greys, nil, wallMat);
	Atomic *wall = MakeAtomic(wallGeo);
	metal::LightDesc light = PointLight(161, 0.0f, 0.0f, 86.0f, 60.0f);
	metal::FrameInfo info = metal::getFrameInfo();
	const int32 W = info.internalWidth, CX = W/2, CY = info.internalHeight/2;
	const float perPixel = 180.0f/(float)W, step = 0.3f, scale = 4.0f/3.0f;
	RGBA sky = { 64, 64, 64, 255 };
	bool began = true;
	float centre = 0.0f;
	for(int f = 0; f < 24; f++){
		centre = (f - 8)*step;
		V3d at = { centre, 0.0f, 0.0f };
		box->getFrame()->translate(&at, COMBINEREPLACE);
		box->getFrame()->updateObjects();
		metal::beginScene(lightCamera);
		WorldBegin(sky);
		SetRenderState(VERTEXALPHA, 0);
		SetRenderState(CULLMODE, CULLNONE);
		metal::setLights(&light, 1);
		bool b = metal::beginPrepass() != 0;
		if(b){
			metal::addShadowCaster(wall, metal::SHADOWCASTER_STATIC);
			metal::addShadowCaster(box, metal::SHADOWCASTER_MOVING);
			wall->render();
			metal::endPrepass();
		}
		wall->render();
		metal::endScene(nil);
		WorldEnd();
		began = b && began;
	}
	metal::rtWaitForTest();
	const float lo = (centre - 6.0f)*scale, hi = (centre + 6.0f)*scale;
	int32 trail = 0, leaks = 0, dark = 0;
	bool read = true;
	for(int32 px = CX - (int32)(40.0f/perPixel); px <= CX + (int32)(40.0f/perPixel); px++){
		float v = -1.0f, x = (float)(CX - px)*perPixel;
		read = metal::readLightShadowForTest(0, px, CY, &v) != 0 && read;
		bool dim = v < 0.5f;
		dark += dim ? 1 : 0;
		float outside = x < lo ? lo - x : x > hi ? x - hi : 0.0f;
		float inside = std::min(x - lo, hi - x);
		if(dim && outside > 0.0f)
			trail = std::max(trail, (int32)ceilf(outside/perPixel - 0.5f));
		if(!dim && inside > 2.0f*perPixel)
			leaks++;
	}
	DestroyAtomic(wall);
	wallGeo->destroy();
	wallMat->destroy();
	DestroyAtomic(box);
	geo->destroy();
	NightEnd(&rig);
	bool ok = began && read && trail <= 4 && leaks == 0 && dark > 0;
	if(!ok)
		Detail("  prepass %d read %d; trail %d pixels (want at most 4, two half cells); lit pixels inside the umbra %d (want 0); dark pixels %d\n", began, read, trail, leaks, dark);
	return Report(ok, name);
}

enum { RB_WAIT, RB_CAMERA, RB_DEPTH, RB_GPUWAIT, RB_HISTORY, RB_COUNT };

static const char *ReadbackNames[RB_COUNT] = { "wait", "camera pixels", "depth pixel", "gpu wait", "history" };

static void
Readback(int kind)
{
	if(kind == RB_WAIT)
		metal::rtWaitForTest();
	else if(kind == RB_CAMERA){
		Image *img = ReadCamera();
		if(img)
			img->destroy();
	}else if(kind == RB_DEPTH){
		float d;
		metal::readDepthPixel(lightCamera->zBuffer, 100, 100, &d);
	}else if(kind == RB_GPUWAIT)
		metal::waitForGPUWrites(metal::getFrameId());
	else{
		float v, l;
		metal::readLightHistoryForTest(0, 100, 100, &v, &l);
	}
}

static void
HostFrame(const metal::LightDesc *lights, int32 n, bool submitInPrepass, int midKind = -1)
{
	RGBA sky = { 64, 64, 64, 255 };
	metal::beginScene(lightCamera);
	WorldBegin(sky);
	SetRenderState(VERTEXALPHA, 0);
	if(!submitInPrepass)
		metal::setLights(lights, n);
	bool began = metal::beginPrepass() != 0;
	if(began){
		backdrop->render();
		if(submitInPrepass){
			metal::lightShadowsPlanned();
			metal::setLights(lights, n);
		}
		metal::endPrepass();
	}
	backdrop->render();
	metal::endScene(nil);
	if(midKind >= 0)
		Readback(midKind);
	WorldEnd();
}

static bool
CheckLightReadbackKeepsPass(void)
{
	const char *name = "a readback between two frames leaves the light pass running and every slot's history growing";
	NightRig rig;
	NightBegin(&rig);
	metal::RenderSettings high = metal::getRenderSettings();
	high.shadowQuality = metal::SHADOWQUALITY_HIGH;
	high.shadowedLights = 2;
	metal::setRenderSettings(&high);
	metal::LightDesc two[2] = { PointLight(141, -12.0f, 0.0f, 70.0f, 60.0f), PointLight(142, 12.0f, 0.0f, 70.0f, 60.0f) };
	const int frames = 10;
	bool ok = true;
	for(int where = 0; where < 3; where++)
		for(int kind = 0; kind < RB_COUNT; kind++){
			bool inPrepass = where == 1, beforeShow = where == 2, ran = true;
			uint32 last = 0, missed = 0;
			float len[2] = { 0.0f, 0.0f };
			for(int f = 0; f < frames; f++){
				bool at = f == 3 || f == 5 || f == 6;
				HostFrame(two, 2, inPrepass, beforeShow && at ? kind : -1);
				metal::RayStats st = metal::getRayStats();
				if(f >= 2 && (st.lightSlots != 2 || st.lightFrame != last + 1)){
					ran = false;
					missed++;
				}
				last = st.lightFrame;
				if(!beforeShow && at)
					Readback(kind);
			}
			metal::rtWaitForTest();
			float v;
			bool read = true;
			for(int i = 0; i < 2; i++)
				read = metal::readLightHistoryForTest(i, 100, 100, &v, &len[i]) != 0 && read;
			bool good = ran && read && len[0] >= 8.0f && len[1] >= 8.0f;
			if(!good)
				Detail("  %s, %s readback: the pass missed %u frames, history lengths %.1f %.1f (want 8 or more)\n",
					beforeShow ? "before the show" : inPrepass ? "lights submitted in the prepass" : "lights submitted first", ReadbackNames[kind], missed, len[0], len[1]);
			ok = ok && good;
		}
	NightEnd(&rig);
	return Report(ok, name);
}

static bool
CheckLightForwardTiming(void)
{
	const char *name = "the forward sum timing hook reads two times, equal with no light in the frame and the lit draw costs more than the bare one";
	LightFrame(nil, 0);
	double plain = -1.0, bare = -1.0;
	bool none = metal::rtLightForwardTimingForTest(&plain, &bare) != 0;
	metal::LightDesc l[3] = { PointLight(1, -2.0f, 0.0f, 80.0f, 40.0f), PointLight(2, 2.0f, 1.0f, 80.0f, 40.0f), PointLight(3, 0.0f, -2.0f, 70.0f, 40.0f) };
	LightFrame(l, 3);
	double lit = -1.0, base = -1.0;
	bool some = metal::rtLightForwardTimingForTest(&lit, &base) != 0;
	bool ok = none && some && plain > 0.0 && bare > 0.0 && plain == bare && lit > base && base > 0.0;
	if(!ok)
		Detail("  no lights: ran %d, times %f and %f (want equal and positive); lights: ran %d, times %f and %f (want the first above the second)\n",
			none, plain, bare, some, lit, base);
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
	double sumLit = 0.0, sumBare = 0.0;
	bool summed = metal::rtLightForwardTimingForTest(&sumLit, &sumBare) != 0;
	lightCamera->frameBuffer = savedColor;
	lightCamera->zBuffer = savedDepth;
	depth->destroy();
	color->destroy();
	if(measured == 0){
		printf("  cluster pass timing: not measured (no stage-boundary counter samples)\n");
		return;
	}
	if(summed)
		printf("  forward sum timing, same frame: %.3f ms with the lights, %.3f ms without\n", sumLit, sumBare);
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
	CheckLightShadowTermOnly();
	CheckLightGroupedPair();
	CheckLightHeldSlots();
	CheckLightReplacedSlot();
	CheckLightShadowTime();
	CheckLightStatsNoWait();
	CheckLightStatsRestart();
	CheckLightGroupedPairFlip();
	CheckLightSparseSlots();
	CheckLightHoleWork();
	CheckLightHoleMarkerId();
	CheckLightGroupMembership();
	CheckLightGroupReplaced();
	CheckLightPinnedZeroRadius();
	CheckLightStatsTwoScenes();
	CheckLightOwnerOverflow();
	CheckLightTargetsRelease();
	CheckLightCutoutAtNight();
	CheckLightMovingCaster();
	CheckLightReadbackKeepsPass();
	CheckLightForwardTiming();
	PrintClusterTiming();
	LightsOff();
	WorldClose();
	return failures;
}
