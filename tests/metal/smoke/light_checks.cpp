#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metallights.h>
#include "world_checks.h"
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
	CheckLightSceneCamera();
	CheckLightLate();
	CheckLightDegenerate();
	PrintClusterTiming();
	LightsOff();
	WorldClose();
	return failures;
}
