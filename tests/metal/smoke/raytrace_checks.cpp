#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalstate.h>
#include <src/metal/metalraytrace.h>
#include <src/metal/metalrelit.h>
#include <src/metal/metalshadow.h>
#include "world_checks.h"
#include "raytrace_checks.h"
#include "late_pipelines.h"

using namespace rw;

static char details[8192];
static int detailsLen = 0;
static int failures = 0;
static Camera *rtCamera;

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

static Geometry*
StripGeometry(int quads, float z, Texture *tex = nil)
{
	int nv = 2*(quads + 1);
	Geometry *geo = Geometry::create(nv, 2*quads, Geometry::POSITIONS | Geometry::PRELIT | Geometry::TRISTRIP |
		(tex ? Geometry::TEXTURED : 0));
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i <= quads; i++){
		v[2*i].set((float)i, 0.0f, z);
		v[2*i+1].set((float)i, 1.0f, z);
		if(tex){
			geo->texCoords[0][2*i] = { (float)i, 0.0f };
			geo->texCoords[0][2*i+1] = { (float)i, 1.0f };
		}
	}
	for(int i = 0; i < nv; i++)
		geo->colors[i] = { 200, 200, 200, 255 };
	Material *mat = Material::create();
	mat->setTexture(tex);
	geo->matList.appendMaterial(mat);
	mat->destroy();
	for(int q = 0; q < quads; q++){
		Triangle *t = &geo->triangles[2*q];
		t[0].v[0] = 2*q; t[0].v[1] = 2*q+1; t[0].v[2] = 2*q+2; t[0].matId = 0;
		t[1].v[0] = 2*q+2; t[1].v[1] = 2*q+1; t[1].v[2] = 2*q+3; t[1].matId = 0;
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static metal::InstanceDataHeader*
Instanced(Atomic *a)
{
	a->getPipeline()->instance(a);
	return (metal::InstanceDataHeader*)a->geometry->instData;
}

struct Caster
{
	Geometry *geo;
	Atomic *atomic;
	metal::InstanceDataHeader *header;
};

static Caster
MakeCaster(int quads, float z)
{
	Caster c;
	c.geo = StripGeometry(quads, z);
	c.atomic = MakeAtomic(c.geo);
	c.header = Instanced(c.atomic);
	return c;
}

static void
DestroyCaster(Caster *c)
{
	if(c->atomic == nil)
		return;
	DestroyAtomic(c->atomic);
	c->geo->destroy();
	c->atomic = nil;
	c->geo = nil;
	c->header = nil;
}

static bool
Built(metal::InstanceDataHeader *h)
{
	uint32 t, n;
	return metal::rtBottomForTest(h, &t, &n) != 0;
}

static bool
CheckBottomBuilds(void)
{
	const char *name = "a bottom level builds from the positions of each casting mesh, strips as lists, and a host position layout builds none";
	metal::setRtFrameForTest(100);
	Caster s = MakeCaster(5, 10.0f);
	metal::RayStats before = metal::getRayStats();
	bool queued = metal::rtEnsureBottom(s.header);
	metal::rtFlushForTest();
	uint32 tris = 0, geos = 0;
	bool built = metal::rtBottomForTest(s.header, &tris, &geos) != 0;
	bool found = metal::rtEnsureBottom(s.header);
	metal::RayStats after = metal::getRayStats();

	RGBA white[4] = { { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };
	Material *opaque = Material::create(), *faded = Material::create();
	faded->color.alpha = 128;
	Geometry *split = SplitQuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 0.0f, 0.0f, 1.0f, 1.0f, 10.0f, white, nil, opaque, faded);
	opaque->destroy();
	faded->destroy();
	Atomic *sa = MakeAtomic(split);
	metal::InstanceDataHeader *sh = Instanced(sa);
	metal::rtEnsureBottom(sh);
	metal::rtFlushForTest();
	uint32 splitTris = 0, splitGeos = 0;
	bool splitBuilt = metal::rtBottomForTest(sh, &splitTris, &splitGeos) != 0;

	RGBA faint[4] = { { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 128 } };
	RGBA texels[4] = { { 255, 255, 255, 255 }, { 255, 255, 255, 128 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };
	Material *first = Material::create(), *second = Material::create(), *textured = Material::create();
	Texture *tex = MakeTexture(2, 2, texels, Texture::NEAREST, Texture::WRAP, Texture::WRAP);
	textured->setTexture(tex);
	if(tex)
		tex->destroy();
	Geometry *vsplit = SplitQuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 0.0f, 0.0f, 1.0f, 1.0f, 11.0f, faint, nil, first, second);
	Geometry *tsplit = SplitQuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 0.0f, 0.0f, 1.0f, 1.0f, 12.0f, white, nil, first, textured);
	Material *allFaded = Material::create();
	allFaded->color.alpha = 128;
	Geometry *empty = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 0.0f, 0.0f, 1.0f, 1.0f, 13.0f, white, nil, allFaded);
	first->destroy();
	second->destroy();
	textured->destroy();
	allFaded->destroy();
	Atomic *va = MakeAtomic(vsplit), *ta = MakeAtomic(tsplit), *ea = MakeAtomic(empty);
	metal::InstanceDataHeader *vh = Instanced(va), *th = Instanced(ta), *eh = Instanced(ea);
	metal::RayStats beforeEmpty = metal::getRayStats();
	metal::rtEnsureBottom(vh);
	metal::rtEnsureBottom(th);
	metal::rtEnsureBottom(eh);
	metal::rtFlushForTest();
	uint32 vTris = 0, vGeos = 0, tTris = 0, tGeos = 0;
	bool vBuilt = metal::rtBottomForTest(vh, &vTris, &vGeos) != 0;
	bool tBuilt = metal::rtBottomForTest(th, &tTris, &tGeos) != 0;
	bool emptyBuilt = Built(eh);
	metal::rtEnsureBottom(eh);
	uint32 emptyQueued = metal::rtQueuedForTest();
	metal::rtFlushForTest();
	metal::RayStats afterEmpty = metal::getRayStats();

	Caster rolled = MakeCaster(4, 14.0f);
	metal::rtEnsureBottom(rolled.header);
	metal::RayStats beforeFail = metal::getRayStats();
	metal::setRtScratchFailForTest(1);
	metal::rtFlushForTest();
	bool failBuilt = Built(rolled.header);
	uint32 failQueued = metal::rtQueuedForTest();
	metal::RayStats afterFail = metal::getRayStats();
	metal::rtFlushForTest();
	bool retryBuilt = Built(rolled.header);

	Caster host = MakeCaster(2, 10.0f);
	metal::AttribDesc *pos = nil;
	for(int32 i = 0; i < host.header->numAttribs; i++)
		if(host.header->attribDesc[i].index == metal::ATTRIB_POS)
			pos = &host.header->attribDesc[i];
	metal::RayStats beforeHost = metal::getRayStats();
	pos->offset = 4;
	metal::rtEnsureBottom(host.header);
	metal::rtEnsureBottom(host.header);
	metal::rtFlushForTest();
	bool hostBuilt = Built(host.header);
	pos->offset = 0;
	metal::RayStats afterHost = metal::getRayStats();

	DestroyAtomic(sa);
	split->destroy();
	DestroyAtomic(va);
	vsplit->destroy();
	DestroyAtomic(ta);
	tsplit->destroy();
	DestroyAtomic(ea);
	empty->destroy();
	DestroyCaster(&s);
	DestroyCaster(&rolled);
	DestroyCaster(&host);
	bool ok = !queued && built && found && tris == 10 && geos == 1 && after.bottomLevels == before.bottomLevels + 1 &&
		after.bottomMegabytes > before.bottomMegabytes && after.builds == before.builds + 1 && after.scratchMegabytes > 0.0f &&
		splitBuilt && splitTris == 1 && splitGeos == 1 &&
		vBuilt && vTris == 1 && vGeos == 1 && tBuilt && tTris == 1 && tGeos == 1 &&
		!emptyBuilt && emptyQueued == 0 && afterEmpty.levelsEmpty == beforeEmpty.levelsEmpty + 1 &&
		afterEmpty.builds == beforeEmpty.builds + 2 &&
		!failBuilt && failQueued == 1 && afterFail.bottomLevels == beforeFail.bottomLevels &&
		afterFail.bottomMegabytes == beforeFail.bottomMegabytes && afterFail.builds == beforeFail.builds && retryBuilt &&
		!hostBuilt && afterHost.levelsSkipped == beforeHost.levelsSkipped + 1 && afterHost.builds == beforeHost.builds;
	if(!ok)
		Detail("  queued-built %d built %d found %d triangles %u geometries %u levels %u -> %u builds %u -> %u MB %.4f -> %.4f scratch MB %.4f\n"
			"  split built %d triangles %u geometries %u; vertex alpha built %d %u %u; texture alpha built %d %u %u\n"
			"  no casting triangles built %d queued again %u empty %u -> %u builds %u -> %u\n"
			"  failed scratch built %d queued %u levels %u -> %u MB %.4f -> %.4f builds %u -> %u; retried built %d\n"
			"  host layout built %d skipped %u -> %u builds %u -> %u\n",
			queued, built, found, tris, geos, before.bottomLevels, after.bottomLevels, before.builds, after.builds,
			before.bottomMegabytes, after.bottomMegabytes, after.scratchMegabytes, splitBuilt, splitTris, splitGeos,
			vBuilt, vTris, vGeos, tBuilt, tTris, tGeos, emptyBuilt, emptyQueued, beforeEmpty.levelsEmpty,
			afterEmpty.levelsEmpty, beforeEmpty.builds, afterEmpty.builds, failBuilt, failQueued, beforeFail.bottomLevels,
			afterFail.bottomLevels, beforeFail.bottomMegabytes, afterFail.bottomMegabytes, beforeFail.builds, afterFail.builds,
			retryBuilt, hostBuilt, beforeHost.levelsSkipped, afterHost.levelsSkipped, beforeHost.builds, afterHost.builds);
	return Report(ok, name);
}

static bool
CheckBottomLifetime(void)
{
	const char *name = "a bottom level is forgotten when its instance is freed or re-uploaded, and a full budget evicts only levels unused for 120 frames that the GPU finished";
	metal::setRtFrameForTest(1000);
	Caster freed = MakeCaster(3, 10.0f), uploaded = MakeCaster(3, 11.0f), waiting = MakeCaster(3, 12.0f);
	metal::rtEnsureBottom(freed.header);
	metal::rtEnsureBottom(uploaded.header);
	metal::rtFlushForTest();
	metal::setRtFrameForTest(1001);
	metal::rtEnsureBottom(waiting.header);
	metal::RayStats mid = metal::getRayStats();
	uint32 retiredBefore = metal::rtRetiredForTest();
	DestroyCaster(&freed);
	DestroyCaster(&waiting);
	metal::RayStats afterFree = metal::getRayStats();
	uploaded.geo->lock(Geometry::LOCKVERTICES);
	uploaded.geo->unlock();
	Instanced(uploaded.atomic);
	bool uploadedKept = Built(uploaded.header);
	metal::RayStats afterUpload = metal::getRayStats();
	uint32 retiredForgotten = metal::rtRetiredForTest();
	metal::rtFlushForTest();
	uint32 queuedAfter = metal::rtQueuedForTest();
	uint32 retiredInFlight = metal::rtRetiredForTest();
	metal::setRtFrameForTest(1002);
	metal::rtFlushForTest();
	uint32 retiredFinished = metal::rtRetiredForTest();
	metal::RayStats afterRelease = metal::getRayStats();
	DestroyCaster(&uploaded);
	bool lifeOk = afterFree.bottomLevels == mid.bottomLevels - 1 && !uploadedKept &&
		afterUpload.bottomLevels == mid.bottomLevels - 2 && queuedAfter == 0 && retiredBefore == 0 && retiredForgotten == 2 &&
		retiredInFlight == 2 && retiredFinished == 0 && afterUpload.bottomMegabytes == mid.bottomMegabytes &&
		afterRelease.bottomMegabytes < mid.bottomMegabytes;
	if(!lifeOk)
		Detail("  levels %u -> freed %u -> re-uploaded %u (kept %d); queued after the frees %u; retired %u -> %u, in flight %u, "
			"finished %u; MB %.4f -> %.4f -> %.4f\n", mid.bottomLevels, afterFree.bottomLevels, afterUpload.bottomLevels,
			uploadedKept, queuedAfter, retiredBefore, retiredForgotten, retiredInFlight, retiredFinished, mid.bottomMegabytes,
			afterUpload.bottomMegabytes, afterRelease.bottomMegabytes);

	Caster c1 = MakeCaster(40, 10.0f), c2 = MakeCaster(40, 12.0f);
	metal::setRtFrameForTest(2000);
	metal::rtEnsureBottom(c1.header);
	metal::rtFlushForTest();
	metal::setRtBudgetForTest(1);
	metal::RayStats before = metal::getRayStats();
	metal::setRtFrameForTest(2001);
	metal::rtEnsureBottom(c2.header);
	metal::rtFlushForTest();
	bool fresh = Built(c2.header);
	metal::setRtFrameForTest(2002);
	metal::rtEnsureBottom(c1.header);
	metal::setRtFrameForTest(2003);
	metal::rtFlushForTest();
	metal::setRtFrameForTest(2122);
	metal::rtFlushForTest();
	bool at120 = Built(c2.header);
	metal::setRtFrameForTest(2130);
	metal::rtEnsureBottom(c1.header);
	metal::setRtFrameForTest(2251);
	metal::rtFlushForTest();
	bool unfinished = Built(c2.header);
	metal::setRtFrameForTest(2252);
	metal::rtFlushForTest();
	bool evicted = Built(c2.header);
	bool c1Kept = Built(c1.header);
	metal::RayStats after = metal::getRayStats();
	uint32 logs = metal::rtBudgetLogsForTest();
	char budgetMB[64];
	snprintf(budgetMB, sizeof(budgetMB), "budget of %.2f MB is full", before.bottomMegabytes);
	const char *refusal = metal::rtRefusalLogForTest();
	bool logNamed = refusal && strstr(refusal, budgetMB) != nil;
	metal::setRtBudgetForTest(0);
	DestroyCaster(&c1);
	DestroyCaster(&c2);
	bool budgetOk = !fresh && !at120 && !unfinished && evicted && !c1Kept && after.budgetFull == before.budgetFull + 4 &&
		logs == 1 && logNamed && after.bottomLevels == before.bottomLevels;
	if(!budgetOk)
		Detail("  built while the old level was fresh %d, at 120 frames %d, before the GPU finished %d, after %d; old kept %d; budget full %u -> %u logs %u levels %u -> %u\n"
			"  refusal log (want \"%s\"): %s",
			fresh, at120, unfinished, evicted, c1Kept, before.budgetFull, after.budgetFull, logs, before.bottomLevels,
			after.bottomLevels, budgetMB, refusal ? refusal : "none\n");
	return Report(lifeOk && budgetOk, name);
}

static bool
PrepassFrame(metal::InstanceDataHeader *h, uint64 *stamp)
{
	metal::TierCaps all = { true, true, true };
	metal::setTierCapsForTest(&all);
	metal::RenderSettings saved = metal::getRenderSettings(), s = saved;
	s.tier = metal::TIER_MODERN;
	s.upscale = metal::UPSCALE_NATIVE;
	s.antiAliasing = metal::ANTIALIAS_OFF;
	metal::setRenderSettings(&s);
	metal::beginScene(rtCamera);
	RGBA sky = { 64, 64, 64, 255 };
	WorldBegin(sky);
	bool began = metal::beginPrepass() != 0;
	if(began){
		metal::rtEnsureBottom(h);
		*stamp = metal::rtLastUsedForTest(h);
		metal::endPrepass();
	}
	metal::endScene(nil);
	WorldEnd();
	metal::setRenderSettings(&saved);
	metal::setTierCapsForTest(nil);
	return began;
}

static bool
CheckBuildBudget(void)
{
	const char *name = "bottom levels build under a per-frame triangle budget, eight times that after a camera cut until the cut's backlog has drained";
	metal::setRtBuildBudgetForTest(100);
	std::vector<Caster> casters;
	metal::setRtFrameForTest(3000);
	for(int i = 0; i < 25; i++){
		casters.push_back(MakeCaster(40, 10.0f + i));
		metal::rtEnsureBottom(casters.back().header);
	}
	metal::RayStats s0 = metal::getRayStats();
	uint32 built[5];
	metal::rtFlushForTest();
	metal::RayStats s1 = metal::getRayStats();
	built[0] = s1.builds - s0.builds;
	metal::cameraCut();
	metal::rtBeginFrame();
	metal::rtFlushForTest();
	metal::RayStats s2 = metal::getRayStats();
	built[1] = s2.builds - s1.builds;
	metal::rtBeginFrame();
	for(int i = 0; i < 3; i++){
		casters.push_back(MakeCaster(40, 40.0f + i));
		metal::rtEnsureBottom(casters.back().header);
	}
	uint64 stamp = metal::rtLastUsedForTest(casters.back().header);
	metal::rtFlushForTest();
	metal::RayStats s3 = metal::getRayStats();
	built[2] = s3.builds - s2.builds;
	metal::rtBeginFrame();
	metal::rtFlushForTest();
	metal::RayStats s4 = metal::getRayStats();
	built[3] = s4.builds - s3.builds;
	metal::rtBeginFrame();
	metal::rtFlushForTest();
	metal::RayStats s5 = metal::getRayStats();
	built[4] = s5.builds - s4.builds;
	uint64 clock = metal::rtFrame();
	uint32 pending = metal::rtQueuedForTest();
	for(Caster &c : casters)
		DestroyCaster(&c);
	metal::setRtFrameForTest(3005);
	metal::rtFlushForTest();
	uint32 drained = metal::rtQueuedForTest();
	casters.clear();

	Caster p = MakeCaster(2, 10.0f);
	uint64 beforePrepass = metal::rtFrame(), prepassStamp = 0;
	bool began = PrepassFrame(p.header, &prepassStamp);
	uint64 afterPrepass = metal::rtFrame();
	DestroyCaster(&p);

	for(int i = 0; i < 3; i++){
		casters.push_back(MakeCaster(40, 60.0f + i));
		metal::rtEnsureBottom(casters.back().header);
	}
	metal::cameraCut();
	metal::rtBeginFrame();
	metal::rtBeginFrame();
	metal::cameraCut();
	metal::rtTerm();
	metal::rtInit();
	uint32 termQueued = metal::rtQueuedForTest(), termRetired = metal::rtRetiredForTest();
	for(Caster &c : casters)
		metal::rtEnsureBottom(c.header);
	metal::RayStats t0 = metal::getRayStats();
	metal::rtBeginFrame();
	metal::rtFlushForTest();
	uint32 termBuilt = metal::getRayStats().builds - t0.builds;
	metal::setRtBuildBudgetForTest(0);
	for(Caster &c : casters)
		DestroyCaster(&c);
	metal::rtBeginFrame();
	metal::rtFlushForTest();
	bool ok = built[0] == 1 && built[1] == 10 && built[2] == 10 && built[3] == 4 && built[4] == 1 && pending == 2 &&
		drained == 0 && clock == 3004 && stamp == 3002 && began && afterPrepass == beforePrepass + 1 &&
		prepassStamp == afterPrepass && termQueued == 0 && termRetired == 0 && termBuilt == 1 && metal::rtQueuedForTest() == 0;
	if(!ok)
		Detail("  built per frame %u %u %u %u %u (want 1 10 10 4 1), pending %u then %u; clock %llu (want 3004), stamp %llu (want 3002)\n"
			"  prepass %d clock %llu -> %llu stamp %llu; after rtTerm queued %u retired %u built %u (want 1)\n",
			built[0], built[1], built[2], built[3], built[4], pending, drained, (unsigned long long)clock,
			(unsigned long long)stamp, began, (unsigned long long)beforePrepass, (unsigned long long)afterPrepass,
			(unsigned long long)prepassStamp, termQueued, termRetired, termBuilt);
	return Report(ok, name);
}

static metal::RenderSettings casterSaved;
static metal::RelitParams casterRelitSaved;
static Light *casterSun;
static Frame *casterSunFrame;

static void
CasterTiersOn(void)
{
	metal::TierCaps all = { true, true, true };
	metal::setTierCapsForTest(&all);
	casterSaved = metal::getRenderSettings();
	metal::RenderSettings s = casterSaved;
	s.tier = metal::TIER_MODERN;
	s.upscale = metal::UPSCALE_NATIVE;
	s.antiAliasing = metal::ANTIALIAS_OFF;
	s.shadowQuality = metal::SHADOWQUALITY_MEDIUM;
	metal::setRenderSettings(&s);
	casterRelitSaved = metal::getRelitParams();
	metal::RelitParams p = casterRelitSaved;
	p.sunScale = 1.0f;
	metal::setRelitParams(&p);
	casterSun = Light::create(Light::DIRECTIONAL);
	casterSunFrame = Frame::create();
	V3d xAxis = { 1.0f, 0.0f, 0.0f };
	casterSunFrame->rotate(&xAxis, 180.0f, COMBINEREPLACE);
	casterSunFrame->updateObjects();
	casterSun->setFrame(casterSunFrame);
	metal::setSunLight(casterSun);
}

static void
CasterTiersOff(void)
{
	metal::setRelitParams(&casterRelitSaved);
	metal::setSunLight(nil);
	casterSun->destroy();
	casterSunFrame->destroy();
	casterSun = nil;
	casterSunFrame = nil;
	metal::setRenderSettings(&casterSaved);
	metal::setTierCapsForTest(nil);
}

template<typename F> static bool
CasterFrame(F draw, bool present = true)
{
	RGBA sky = { 64, 64, 64, 255 };
	metal::beginScene(rtCamera);
	WorldBegin(sky);
	SetRenderState(VERTEXALPHA, 0);
	bool began = metal::beginPrepass() != 0;
	if(began){
		draw(true);
		metal::endPrepass();
	}
	draw(false);
	metal::endScene(nil);
	if(present)
		WorldEnd();
	else
		rtCamera->endUpdate();
	return began;
}

static void
Place(Atomic *a, float x, float y, float z, float degreesZ)
{
	V3d axis = { 0.0f, 0.0f, 1.0f }, at = { x, y, z };
	a->getFrame()->rotate(&axis, degreesZ, COMBINEREPLACE);
	a->getFrame()->translate(&at, COMBINEPOSTCONCAT);
	a->getFrame()->updateObjects();
}

static Atomic*
Roof(Geometry **geo, float halfX, float halfY, uint8 alpha = 255)
{
	RGBA c = { 120, 120, 120, 255 };
	RGBA col[4] = { c, c, c, c };
	Material *mat = Material::create();
	mat->color.alpha = alpha;
	*geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, halfX, -halfY, -halfX, halfY, 0.0f, col, nil, mat);
	mat->destroy();
	return MakeAtomic(*geo);
}

static Atomic*
SplitRoof(Geometry **geo, float halfX, float halfY)
{
	RGBA c = { 120, 120, 120, 255 };
	RGBA col[4] = { c, c, c, c };
	Material *m0 = Material::create(), *m1 = Material::create();
	*geo = SplitQuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, halfX, -halfY, -halfX, halfY, 0.0f, col, nil, m0, m1);
	m0->destroy();
	m1->destroy();
	return MakeAtomic(*geo);
}

static Geometry*
SkinnedQuadGeometry(float half)
{
	RGBA c = { 100, 100, 100, 255 };
	RGBA col[4] = { c, c, c, c };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::NORMALS | Geometry::PRELIT, half, half,
		-half, -half, 0.0f, col, nil, mat);
	mat->destroy();
	Matrix ident;
	ident.setIdentity();
	uint8 idx[4][4] = {};
	float wts[4][4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };
	AttachSkin(geo, 1, &ident, idx, wts);
	return geo;
}

static Atomic*
SkinnedAt(Geometry *geo, float x, float y, float z)
{
	Atomic *a = SkinAtomic(geo);
	Place(a, x, y, z, 0.0f);
	PoseSkin(a, 1);
	return a;
}

static bool
Hit(float x, float y, float z, uint32 mask = 0xFF)
{
	float o[3] = { x, y, z }, d[3] = { 0.0f, 0.0f, -1.0f };
	return metal::rtTraceForTest(o, d, 100.0f, mask) != 0;
}

static void
DrawNilObject(Atomic *a)
{
	a->getPipeline()->instance(a);
	metal::InstanceDataHeader *h = (metal::InstanceDataHeader*)a->geometry->instData;
	metal::setWorldMatrix(a->getFrame()->getLTM());
	int32 vsBits = metal::lightingCB(a);
	metal::setupVertexInput(h);
	metal::InstanceData *inst = h->inst;
	for(uint32 i = 0; i < h->numMeshes; i++, inst++){
		metal::setMaterial(a->geometry->flags, inst->material->color, inst->material->surfaceProps);
		metal::setTexture(0, inst->material->texture);
		metal::defaultShader->use(metal::drawVariant(vsBits));
		metal::drawInst(h, inst);
	}
	metal::teardownVertexInput(h);
}

static void
MoveVertices(Geometry *g, float dx)
{
	g->lock(Geometry::LOCKVERTICES);
	for(int32 i = 0; i < g->numVertices; i++)
		g->morphTargets[0].vertices[i].x += dx;
	g->unlock();
}

static bool
CheckRoofOccludes(void)
{
	const char *name = "a host caster that is never drawn occludes a ray in the frame it is added, rotated and translated";
	Geometry *g;
	Atomic *roof = Roof(&g, 2.0f, 1.0f);
	Place(roof, 10.0f, 0.0f, 30.0f, 90.0f);
	CasterTiersOn();
	bool began = CasterFrame([&](bool prepass){
		if(prepass)
			metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
	});
	metal::RayStats st = metal::getRayStats();
	bool under = Hit(10.0f, 1.5f, 40.0f), beside = Hit(11.5f, 0.0f, 40.0f), origin = Hit(0.0f, 1.5f, 40.0f);
	CasterTiersOff();
	DestroyAtomic(roof);
	g->destroy();
	bool ok = began && under && !beside && !origin && st.instances == 1 && st.casters == 1 && st.hostCasters == 1 &&
		st.castersWaiting == 0;
	if(!ok)
		Detail("  under %d beside %d origin %d instances %u casters %u host %u waiting %u\n", under, beside, origin,
			st.instances, st.casters, st.hostCasters, st.castersWaiting);
	return Report(ok, name);
}

static bool
CheckSkinnedFollows(void)
{
	const char *name = "a skinned caster's level follows its bones, rebuilt or refit by the mode, and is retired when freed or idle";
	bool ok = true;
	CasterTiersOn();
	for(int32 mode = metal::RT_SKIN_REBUILD; mode <= metal::RT_SKIN_REFIT; mode++){
		metal::setRtSkinModeForTest(mode);
		Geometry *geo = SkinnedQuadGeometry(1.0f), *geoB = SkinnedQuadGeometry(1.0f);
		Atomic *a = SkinnedAt(geo, 0.0f, 0.0f, 20.0f), *b = SkinnedAt(geoB, 0.0f, 10.0f, 20.0f);
		metal::RayStats s0 = metal::getRayStats();
		CasterFrame([&](bool){ a->render(); b->render(); });
		bool before0 = Hit(0.0f, 0.0f, 25.0f), before4 = Hit(4.0f, 0.0f, 25.0f);
		MoveSkinBone(a, 0, 4.0f);
		CasterFrame([&](bool){ a->render(); });
		bool after0 = Hit(0.0f, 0.0f, 25.0f), after4 = Hit(4.0f, 0.0f, 25.0f);
		metal::RayStats s2 = metal::getRayStats();
		uint32 builds2 = s2.skinnedBuilds - s0.skinnedBuilds, refits2 = s2.refits - s0.refits;
		bool modeOk = mode == metal::RT_SKIN_REBUILD ? builds2 == 3 && refits2 == 0 : builds2 == 2 && refits2 == 1;
		bool periodic = true, aged = true;
		if(mode == metal::RT_SKIN_REFIT){
			for(int f = 3; f <= 31; f++)
				CasterFrame([&](bool){ a->render(); });
			metal::RayStats s31 = metal::getRayStats();
			bool bKept = s31.skinnedLevels == s0.skinnedLevels + 2;
			CasterFrame([&](bool){ a->render(); });
			metal::RayStats s32 = metal::getRayStats();
			periodic = s31.skinnedBuilds - s0.skinnedBuilds == 3 && s31.refits - s0.refits == 29 &&
				s32.refits - s0.refits == 30;
			aged = bKept && s32.skinnedLevels == s0.skinnedLevels + 1 && !metal::rtSkinnedForTest(b);
		}
		uint32 pool = metal::getRayStats().skinnedLevels;
		DestroySkinAtomic(a);
		geo->destroy();
		uint32 freed = metal::getRayStats().skinnedLevels;
		DestroySkinAtomic(b);
		geoB->destroy();
		bool released = freed + 1 == pool;
		bool part = before0 && !before4 && !after0 && after4 && modeOk && periodic && aged && released;
		if(!part)
			Detail("  mode %d: before %d/%d after %d/%d builds %u refits %u periodic %d aged %d pool %u -> %u\n", mode,
				before0, before4, after0, after4, builds2, refits2, periodic, aged, pool, freed);
		ok = ok && part;
	}
	metal::setRtSkinModeForTest(metal::RT_SKIN_DEFAULT);
	CasterTiersOff();
	return Report(ok, name);
}

static bool
CheckInstanceCap(void)
{
	const char *name = "over the instance cap the nearest casters are kept";
	Geometry *g[3];
	Atomic *r[3];
	float xs[3] = { 80.0f, 0.0f, 40.0f };
	for(int i = 0; i < 3; i++){
		r[i] = Roof(&g[i], 2.0f, 2.0f);
		Place(r[i], xs[i], 0.0f, 30.0f, 0.0f);
	}
	CasterTiersOn();
	metal::setRtInstanceCapForTest(2);
	CasterFrame([&](bool prepass){
		if(prepass)
			for(int i = 0; i < 3; i++)
				metal::addShadowCaster(r[i], metal::SHADOWCASTER_STATIC);
	});
	metal::RayStats st = metal::getRayStats();
	bool near0 = Hit(0.0f, 0.0f, 40.0f), near1 = Hit(40.0f, 0.0f, 40.0f), far2 = Hit(80.0f, 0.0f, 40.0f);
	metal::setRtInstanceCapForTest(0);
	CasterTiersOff();
	for(int i = 0; i < 3; i++){
		DestroyAtomic(r[i]);
		g[i]->destroy();
	}
	bool ok = near0 && near1 && !far2 && st.instances == 2 && st.castersCapped == 1;
	if(!ok)
		Detail("  hits %d %d %d instances %u capped %u\n", near0, near1, far2, st.instances, st.castersCapped);
	return Report(ok, name);
}

static bool
CheckRayWorkPlacement(void)
{
	const char *name = "the ray work adds no render pass and runs once after the whole prepass, also when the prepass encoder is split";
	Geometry *ga, *gb;
	Atomic *a = Roof(&ga, 2.0f, 2.0f), *b = Roof(&gb, 2.0f, 2.0f);
	Place(a, 0.0f, 0.0f, 30.0f, 0.0f);
	Place(b, 10.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	CasterFrame([&](bool){ a->render(); });
	uint32 passes[2], rays[2];
	for(int w = 0; w < 2; w++){
		metal::setRtFrameWorkForTest(w == 0);
		uint32 p0 = metal::getFrameStats().renderPasses, r0 = metal::getRayStats().rayFrames;
		CasterFrame([&](bool){ a->render(); });
		passes[w] = metal::getFrameStats().renderPasses - p0;
		rays[w] = metal::getRayStats().rayFrames - r0;
	}
	metal::setRtFrameWorkForTest(1);
	uint32 r0 = metal::getRayStats().rayFrames;
	float n[3], m[2], d;
	CasterFrame([&](bool prepass){
		a->render();
		if(prepass)
			metal::readPrepassForTest(5, 5, n, m, &d);
		b->render();
	});
	uint32 splitRays = metal::getRayStats().rayFrames - r0;
	bool aHit = Hit(0.0f, 0.0f, 40.0f), bHit = Hit(10.0f, 0.0f, 40.0f);
	CasterTiersOff();
	DestroyAtomic(a);
	DestroyAtomic(b);
	ga->destroy();
	gb->destroy();
	bool ok = passes[0] == passes[1] && passes[0] > 0 && rays[0] == 1 && rays[1] == 0 && splitRays == 1 && aHit && bHit;
	if(!ok)
		Detail("  passes with %u without %u; ray frames %u %u; split frame ray frames %u hits %d %d\n", passes[0], passes[1],
			rays[0], rays[1], splitRays, aHit, bHit);
	return Report(ok, name);
}

static bool
CheckCasterSources(void)
{
	const char *name = "prepass casters: one per atomic and header with masks ORed, none from nil-object, blended or unflagged draws, flag reset per scene";
	Geometry *ga, *gb, *gc, *ge;
	Atomic *a = SplitRoof(&ga, 2.0f, 2.0f), *b = Roof(&gb, 2.0f, 2.0f), *c = Roof(&gc, 2.0f, 2.0f), *e = Roof(&ge, 2.0f, 2.0f, 128);
	Place(a, 0.0f, 0.0f, 30.0f, 0.0f);
	Place(b, 20.0f, 0.0f, 30.0f, 0.0f);
	Place(c, -20.0f, 0.0f, 30.0f, 0.0f);
	Place(e, 0.0f, 20.0f, 30.0f, 0.0f);
	CasterTiersOn();
	CasterFrame([&](bool prepass){
		if(prepass)
			metal::addShadowCaster(a, metal::SHADOWCASTER_STATIC);
		a->render();
		DrawNilObject(b);
		metal::setShadowCaster(0);
		c->render();
		metal::setShadowCaster(1);
		e->render();
	});
	metal::RayStats s1 = metal::getRayStats();
	bool aStatic = Hit(0.0f, 0.0f, 40.0f, metal::SHADOWCASTER_STATIC), aMoving = Hit(0.0f, 0.0f, 40.0f, metal::SHADOWCASTER_MOVING);
	bool aSkinned = Hit(0.0f, 0.0f, 40.0f, metal::SHADOWCASTER_SKINNED);
	bool bHit = Hit(20.0f, 0.0f, 40.0f), cHit1 = Hit(-20.0f, 0.0f, 40.0f), eHit = Hit(0.0f, 20.0f, 40.0f);
	metal::setShadowCaster(0);
	CasterFrame([&](bool){ c->render(); });
	metal::RayStats s2 = metal::getRayStats();
	bool cHit2 = Hit(-20.0f, 0.0f, 40.0f);
	CasterTiersOff();
	Atomic *all[4] = { a, b, c, e };
	Geometry *geos[4] = { ga, gb, gc, ge };
	for(int i = 0; i < 4; i++){
		DestroyAtomic(all[i]);
		geos[i]->destroy();
	}
	bool ok = s1.casters == 1 && s1.instances == 1 && aStatic && aMoving && !aSkinned && !bHit && !cHit1 && !eHit &&
		s2.casters == 1 && cHit2;
	if(!ok)
		Detail("  frame 1 casters %u instances %u; a static %d moving %d skinned %d; b %d c %d e %d; frame 2 casters %u c %d\n",
			s1.casters, s1.instances, aStatic, aMoving, aSkinned, bHit, cHit1, eHit, s2.casters, cHit2);
	return Report(ok, name);
}

static bool
CheckUploadedVertices(void)
{
	const char *name = "after a vertex re-upload the next frame's instance level casts from the new positions, a skinned one rebuilt";
	CasterTiersOn();
	metal::setRtSkinModeForTest(metal::RT_SKIN_REFIT);
	Geometry *gr, *gs = SkinnedQuadGeometry(1.0f);
	Atomic *r = Roof(&gr, 2.0f, 2.0f), *s = SkinnedAt(gs, 0.0f, 10.0f, 20.0f);
	Place(r, 0.0f, 0.0f, 30.0f, 0.0f);
	CasterFrame([&](bool){ r->render(); s->render(); });
	CasterFrame([&](bool){ r->render(); s->render(); });
	bool r0 = Hit(0.0f, 0.0f, 40.0f), s0 = Hit(0.0f, 10.0f, 25.0f);
	MoveVertices(gr, 10.0f);
	MoveVertices(gs, 10.0f);
	metal::RayStats before = metal::getRayStats();
	CasterFrame([&](bool){ r->render(); s->render(); });
	metal::RayStats after = metal::getRayStats();
	bool rOld = Hit(0.0f, 0.0f, 40.0f), rNew = Hit(10.0f, 0.0f, 40.0f), sOld = Hit(0.0f, 10.0f, 25.0f), sNew = Hit(10.0f, 10.0f, 25.0f);
	metal::setRtSkinModeForTest(metal::RT_SKIN_DEFAULT);
	CasterTiersOff();
	DestroyAtomic(r);
	gr->destroy();
	DestroySkinAtomic(s);
	gs->destroy();
	bool ok = r0 && s0 && !rOld && rNew && !sOld && sNew && after.skinnedBuilds - before.skinnedBuilds == 1 &&
		after.refits == before.refits && after.builds - before.builds == 1 && after.instances == 2;
	if(!ok)
		Detail("  before %d %d; rigid old %d new %d; skinned old %d new %d; skinned builds %u refits %u static builds %u instances %u\n",
			r0, s0, rOld, rNew, sOld, sNew, after.skinnedBuilds - before.skinnedBuilds, after.refits - before.refits,
			after.builds - before.builds, after.instances);
	return Report(ok, name);
}

static bool
CheckSkinnedCap(void)
{
	const char *name = "at most 24 skinned casters are kept, the nearest, and none beyond 60 m";
	Geometry *geo = SkinnedQuadGeometry(0.5f);
	std::vector<Atomic*> as;
	CasterTiersOn();
	float farA[2] = { 70.0f, 65.0f };
	for(int i = 0; i < 2; i++)
		as.push_back(SkinnedAt(geo, 0.0f, 0.0f, farA[i]));
	for(int i = 19; i >= 0; i--)
		as.push_back(SkinnedAt(geo, 0.0f, 0.0f, 6.0f + 2.0f*i));
	CasterFrame([&](bool){ for(Atomic *a : as) a->render(); });
	metal::RayStats sa = metal::getRayStats();
	bool beyond = metal::rtSkinnedForTest(as[0]) || metal::rtSkinnedForTest(as[1]);
	for(Atomic *a : as)
		DestroySkinAtomic(a);
	as.clear();
	for(int i = 25; i >= 0; i--)
		as.push_back(SkinnedAt(geo, 0.0f, 0.0f, 6.0f + 2.0f*i));
	CasterFrame([&](bool){ for(Atomic *a : as) a->render(); });
	metal::RayStats sb = metal::getRayStats();
	bool farthest = metal::rtSkinnedForTest(as[0]) || metal::rtSkinnedForTest(as[1]);
	bool nearest = metal::rtSkinnedForTest(as[25]) && metal::rtSkinnedForTest(as[2]);
	CasterTiersOff();
	for(Atomic *a : as)
		DestroySkinAtomic(a);
	geo->destroy();
	bool ok = sa.skinnedCasters == 20 && !beyond && sb.skinnedCasters == 24 && !farthest && nearest;
	if(!ok)
		Detail("  case A kept %u beyond 60 m %d; case B kept %u farthest %d nearest %d\n", sa.skinnedCasters, beyond,
			sb.skinnedCasters, farthest, nearest);
	return Report(ok, name);
}

static float32
LevelMegabytes(const metal::RayStats &s)
{
	return s.bottomMegabytes + s.skinnedMegabytes + s.topMegabytes;
}

static bool
CheckForgottenInFlight(void)
{
	const char *name = "a level named by an in-flight instance level and forgotten in the next frame keeps its heap space until that frame completes";
	CasterTiersOn();
	metal::setRtSkinModeForTest(metal::RT_SKIN_REFIT);
	Geometry *gr, *gs = SkinnedQuadGeometry(1.0f);
	Atomic *r = Roof(&gr, 2.0f, 2.0f), *s = SkinnedAt(gs, 0.0f, 10.0f, 20.0f);
	Place(r, 0.0f, 0.0f, 30.0f, 0.0f);
	CasterFrame([&](bool){ r->render(); s->render(); });
	for(int i = 0; i < 2; i++){
		metal::rtWaitForTest();
		CasterFrame([](bool){});
	}
	uint32 drained = metal::rtRetiredForTest();
	CasterFrame([&](bool){ r->render(); s->render(); });
	metal::RayStats named = metal::getRayStats();
	uint32 forgotten = 0;
	metal::RayStats atForget = named;
	CasterFrame([&](bool prepass){
		if(!prepass)
			return;
		MoveVertices(gr, 0.0f);
		MoveVertices(gs, 0.0f);
		r->getPipeline()->instance(r);
		s->getPipeline()->instance(s);
		forgotten = metal::rtRetiredForTest();
		atForget = metal::getRayStats();
	});
	uint32 afterFrame = metal::rtRetiredForTest();
	metal::rtWaitForTest();
	CasterFrame([](bool){});
	uint32 released = metal::rtRetiredForTest();
	metal::RayStats after = metal::getRayStats();
	metal::setRtSkinModeForTest(metal::RT_SKIN_DEFAULT);
	CasterTiersOff();
	DestroyAtomic(r);
	gr->destroy();
	DestroySkinAtomic(s);
	gs->destroy();
	float32 kept = LevelMegabytes(atForget) - LevelMegabytes(named), freed = LevelMegabytes(named) - LevelMegabytes(after);
	bool ok = drained == 0 && named.instances == 2 && forgotten == 2 && afterFrame == 2 && released == 0 &&
		kept > -0.0001f && kept < 0.0001f && freed > 0.0001f && atForget.skinnedLevels + 1 == named.skinnedLevels;
	if(!ok)
		Detail("  retired drained %u, named instances %u, retired at the forget %u, after its frame %u, after completion %u;"
			" MB change at the forget %.4f, released %.4f; skinned levels %u -> %u\n", drained, named.instances, forgotten,
			afterFrame, released, kept, freed, named.skinnedLevels, atForget.skinnedLevels);
	return Report(ok, name);
}

static bool
CheckSlotsBeforePresent(void)
{
	const char *name = "four prepass scenes in one frame: the first scene's instance level holds its own casters, and the three slots rotate";
	Geometry *g0, *g1;
	Atomic *first = Roof(&g0, 2.0f, 2.0f), *later = Roof(&g1, 2.0f, 2.0f);
	Place(first, 0.0f, 0.0f, 30.0f, 0.0f);
	Place(later, 40.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	const void *level[4];
	uint32 deferred = ~0u;
	for(int s = 0; s < 4; s++){
		Atomic *roof = s == 0 ? first : later;
		CasterFrame([&](bool prepass){
			if(prepass)
				metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
		}, s == 3);
		level[s] = metal::rtInstanceLevelForTest();
		if(s == 0){
			float o[3] = { 0.0f, 0.0f, 40.0f }, d[3] = { 0.0f, 0.0f, -1.0f };
			deferred = metal::rtTraceDeferredForTest(o, d, 100.0f, 0xFF);
		}
	}
	metal::rtWaitForTest();
	bool firstHit = metal::rtDeferredHitForTest(deferred) != 0;
	bool laterHit = Hit(40.0f, 0.0f, 40.0f), firstNow = Hit(0.0f, 0.0f, 40.0f);
	CasterTiersOff();
	DestroyAtomic(first);
	DestroyAtomic(later);
	g0->destroy();
	g1->destroy();
	bool rotate = level[0] && level[1] && level[2] && level[0] != level[1] && level[1] != level[2] && level[0] != level[2] &&
		level[3] == level[0];
	bool ok = deferred != ~0u && firstHit && laterHit && !firstNow && rotate;
	if(!ok)
		Detail("  deferred %u hit %d; scene 4 at 40 %d at 0 %d; slots rotate %d\n", deferred, firstHit, laterHit, firstNow, rotate);
	return Report(ok, name);
}

static bool
CheckLevelPerScene(void)
{
	const char *name = "a scene with no ray work has no instance level, also after a scene that built one";
	Geometry *g;
	Atomic *roof = Roof(&g, 2.0f, 2.0f);
	Place(roof, 0.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	CasterFrame([&](bool prepass){
		if(prepass)
			metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
	});
	bool built = metal::rtInstanceLevelForTest() != nil;
	RGBA sky = { 64, 64, 64, 255 };
	metal::beginScene(rtCamera);
	WorldBegin(sky);
	roof->render();
	metal::endScene(nil);
	WorldEnd();
	bool after = metal::rtInstanceLevelForTest() != nil;
	CasterTiersOff();
	DestroyAtomic(roof);
	g->destroy();
	bool ok = built && !after;
	if(!ok)
		Detail("  level after the prepass scene %d, after the plain scene %d\n", built, after);
	return Report(ok, name);
}

static bool
CheckSkinnedScratchFail(void)
{
	const char *name = "a skinned caster whose scratch cannot be allocated is left out of that frame, not cast from its last pose";
	CasterTiersOn();
	metal::setRtSkinModeForTest(metal::RT_SKIN_REFIT);
	Geometry *geo = SkinnedQuadGeometry(1.0f);
	Atomic *a = SkinnedAt(geo, 0.0f, 0.0f, 20.0f);
	CasterFrame([&](bool){ a->render(); });
	CasterFrame([&](bool){ a->render(); });
	bool before = Hit(0.0f, 0.0f, 25.0f);
	MoveSkinBone(a, 0, 4.0f);
	metal::setRtScratchFailForTest(1);
	CasterFrame([&](bool){ a->render(); });
	metal::RayStats st = metal::getRayStats();
	bool kept = metal::rtSkinnedForTest(a) != 0, old = Hit(0.0f, 0.0f, 25.0f), moved = Hit(4.0f, 0.0f, 25.0f);
	metal::setRtScratchFailForTest(0);
	CasterFrame([&](bool){ a->render(); });
	bool next = Hit(4.0f, 0.0f, 25.0f);
	metal::setRtSkinModeForTest(metal::RT_SKIN_DEFAULT);
	CasterTiersOff();
	DestroySkinAtomic(a);
	geo->destroy();
	bool ok = before && !kept && !old && !moved && st.instances == 0 && st.castersWaiting == 1 && next;
	if(!ok)
		Detail("  before %d; failed frame kept %d old pose %d new pose %d instances %u waiting %u; next frame %d\n", before, kept,
			old, moved, st.instances, st.castersWaiting, next);
	return Report(ok, name);
}

static bool
CheckBoneClamp(void)
{
	const char *name = "a skinned vertex whose bone index is past the palette is moved by the last bone";
	RGBA c = { 100, 100, 100, 255 };
	RGBA col[4] = { c, c, c, c };
	Material *mat = Material::create();
	Geometry *geo = QuadGeometry(Geometry::POSITIONS | Geometry::TEXTURED | Geometry::NORMALS | Geometry::PRELIT, 1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, col, nil, mat);
	mat->destroy();
	uint8 idx[4][4] = { { 200, 0, 0, 0 }, { 200, 0, 0, 0 }, { 200, 0, 0, 0 }, { 200, 0, 0, 0 } };
	float wts[4][4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };
	AttachSkin(geo, 64, nil, idx, wts);
	Atomic *a = SkinAtomic(geo);
	Place(a, 0.0f, 0.0f, 20.0f, 0.0f);
	PoseSkin(a, 64);
	MoveSkinBone(a, 63, 4.0f);
	CasterTiersOn();
	CasterFrame([&](bool){ a->render(); });
	bool moved = Hit(4.0f, 0.0f, 25.0f), origin = Hit(0.0f, 0.0f, 25.0f);
	CasterTiersOff();
	DestroySkinAtomic(a);
	geo->destroy();
	bool ok = moved && !origin;
	if(!ok)
		Detail("  at bone 63's place %d, at the bind place %d\n", moved, origin);
	return Report(ok, name);
}

static bool
CheckDrawnCasterMoves(void)
{
	const char *name = "a drawn caster casts from this frame's world matrix, not the previous frame's";
	Geometry *g;
	Atomic *roof = Roof(&g, 2.0f, 2.0f);
	Place(roof, 0.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	CasterFrame([&](bool){ roof->render(); });
	bool first = Hit(0.0f, 0.0f, 40.0f);
	Place(roof, 20.0f, 0.0f, 30.0f, 0.0f);
	CasterFrame([&](bool){ roof->render(); });
	bool old = Hit(0.0f, 0.0f, 40.0f), now = Hit(20.0f, 0.0f, 40.0f);
	CasterTiersOff();
	DestroyAtomic(roof);
	g->destroy();
	bool ok = first && !old && now;
	if(!ok)
		Detail("  frame 1 %d; frame 2 old place %d new place %d\n", first, old, now);
	return Report(ok, name);
}

static bool
CheckHostCasterOutside(void)
{
	const char *name = "addShadowCaster outside the prepass bracket adds nothing and instances nothing";
	Geometry *g;
	Atomic *roof = Roof(&g, 2.0f, 2.0f);
	Place(roof, 0.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	bool fresh = g->instData == nil;
	metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
	bool instanced = g->instData != nil;
	CasterFrame([](bool){});
	metal::RayStats st = metal::getRayStats();
	CasterTiersOff();
	DestroyAtomic(roof);
	g->destroy();
	bool ok = fresh && !instanced && st.casters == 0 && st.hostCasters == 0;
	if(!ok)
		Detail("  fresh %d instanced %d casters %u host %u\n", fresh, instanced, st.casters, st.hostCasters);
	return Report(ok, name);
}

static bool
CheckDestroyedInBracket(void)
{
	const char *name = "an atomic destroyed inside the prepass after it was added is dropped from the walk and counted";
	Geometry *g;
	Atomic *roof = Roof(&g, 2.0f, 2.0f);
	Place(roof, 0.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	CasterFrame([&](bool prepass){
		if(!prepass || roof == nil)
			return;
		metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
		DestroyAtomic(roof);
		g->destroy();
		roof = nil;
	});
	metal::RayStats st = metal::getRayStats();
	CasterTiersOff();
	bool ok = roof == nil && st.casters == 1 && st.instances == 0 && st.castersDropped == 1 && st.castersWaiting == 0;
	if(!ok)
		Detail("  casters %u instances %u dropped %u waiting %u\n", st.casters, st.instances, st.castersDropped,
			st.castersWaiting);
	return Report(ok, name);
}

static bool
CheckRealFramesDrain(void)
{
	const char *name = "real ray frames drain: what earlier checks and a forgotten level leave retired is released after a wait and one frame, with at most 24 skinned levels kept";
	Geometry *g;
	Atomic *roof = Roof(&g, 2.0f, 2.0f);
	Place(roof, 0.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	CasterFrame([&](bool){ roof->render(); });
	DestroyAtomic(roof);
	g->destroy();
	uint32 left = metal::rtRetiredForTest();
	metal::rtWaitForTest();
	CasterFrame([](bool){});
	uint32 retired = metal::rtRetiredForTest(), skinned = metal::getRayStats().skinnedLevels;
	CasterTiersOff();
	bool ok = left > 0 && retired == 0 && skinned <= 24;
	if(!ok)
		Detail("  retired %u, after a wait and a frame %u; skinned levels %u\n", left, retired, skinned);
	return Report(ok, name);
}

static void
PrintBuildTiming(void)
{
	metal::RtBuildTiming t;
	if(!metal::rtMeasureBuildsForTest(&t)){
		printf("  bottom-level build timing: not measured (no stage-boundary counter samples)\n");
		return;
	}
	printf("  bottom-level build timing (median of %u rounds, %u levels, %u triangles per round, recorded not gated):\n",
		t.rounds, t.levels, t.triangles);
	for(uint32 u = 0; u < t.usages; u++)
		printf("    usage %-26s %.3f ms per round, %.2f us per build, %.2f ns per triangle, %.1f B per triangle\n",
			t.usageName[u], t.batchMs[u], t.batchMs[u]*1000.0f/t.levels, t.batchMs[u]*1e6f/t.triangles,
			(float)t.bytes[u]/t.triangles);
	printf("    %u two-triangle levels: %.3f ms (%.2f us per build); one encoder per level: %.3f ms (%.2f us per build)\n", t.levels,
		t.tinyMs, t.tinyMs*1000.0f/t.levels, t.splitMs, t.splitMs*1000.0f/t.levels);
	printf("    fixed cost per build %.2f us, per triangle %.2f ns; triangles in 1 ms at real sizes %.0f\n", t.fixedUs,
		t.perTriangleNs, t.levels*1.0f/t.batchMs[0]*t.triangles/t.levels);
	printf("    compacted: %u of %u bytes (%.1f%%) for the first %u levels\n", t.compactedBytes, t.sampleBytes,
		100.0f*t.compactedBytes/t.sampleBytes, t.compactedLevels);
	for(uint32 i = 0; i < 3; i++)
		printf("    instance level, %u indirect instances: %.3f ms, %u bytes\n", t.instanceCounts[i], t.instanceMs[i],
			t.instanceBytes[i]);
	printf("    counter ticks to ns %.4f; whole-buffer GPU time of the last real-size round %.3f ms\n", t.nsPerTick, t.bufferMs);
}

static Geometry*
SkinnedGridGeometry(void)
{
	enum { COLS = 25, ROWS = 30, NV = (COLS + 1)*(ROWS + 1), NT = 2*COLS*ROWS };
	Geometry *geo = Geometry::create(NV, NT, Geometry::POSITIONS | Geometry::TEXTURED | Geometry::NORMALS | Geometry::PRELIT);
	V3d *v = geo->morphTargets[0].vertices, *n = geo->morphTargets[0].normals;
	uint8 (*idx)[4] = new uint8[NV][4];
	float (*wts)[4] = new float[NV][4];
	for(int y = 0; y <= ROWS; y++)
		for(int x = 0; x <= COLS; x++){
			int i = y*(COLS + 1) + x;
			v[i].set(-0.5f + x/(float)COLS, -0.9f + 1.8f*y/(float)ROWS, 0.0f);
			n[i].set(0.0f, 0.0f, -1.0f);
			geo->texCoords[0][i].u = x/(float)COLS;
			geo->texCoords[0][i].v = y/(float)ROWS;
			geo->colors[i] = { 100, 100, 100, 255 };
			idx[i][0] = x % 4;
			idx[i][1] = (x + 1) % 4;
			idx[i][2] = idx[i][3] = 0;
			wts[i][0] = wts[i][1] = 0.5f;
			wts[i][2] = wts[i][3] = 0.0f;
		}
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	for(int y = 0; y < ROWS; y++)
		for(int x = 0; x < COLS; x++){
			Triangle *t = &geo->triangles[2*(y*COLS + x)];
			int a = y*(COLS + 1) + x, b = a + 1, c = a + COLS + 1, d = c + 1;
			t[0].v[0] = a; t[0].v[1] = b; t[0].v[2] = c; t[0].matId = 0;
			t[1].v[0] = c; t[1].v[1] = b; t[1].v[2] = d; t[1].matId = 0;
		}
	geo->calculateBoundingSphere();
	geo->unlock();
	Matrix ident[4];
	for(int i = 0; i < 4; i++)
		ident[i].setIdentity();
	AttachSkin(geo, 4, ident, idx, wts);
	delete[] idx;
	delete[] wts;
	return geo;
}

static float32
Median(std::vector<float32> v)
{
	std::sort(v.begin(), v.end());
	return v.empty() ? 0.0f : v[v.size()/2];
}

static void
SkinnedTiming(const std::vector<Atomic*> &as, int32 mode, float32 *skinMs, float32 *buildMs)
{
	metal::setRtSkinModeForTest(mode);
	for(int f = 0; f < 2; f++)
		CasterFrame([&](bool){ for(Atomic *a : as) a->render(); });
	std::vector<float32> skin, build;
	for(int f = 0; f < 5; f++){
		for(Atomic *a : as)
			MoveSkinBone(a, 1, 0.05f);
		CasterFrame([&](bool){ for(Atomic *a : as) a->render(); });
		metal::RtFrameTiming t;
		if(metal::rtFrameTimingForTest(&t)){
			skin.push_back(t.skinMs);
			build.push_back(t.skinnedMs);
		}
	}
	*skinMs = Median(skin);
	*buildMs = Median(build);
}

static void
PrintCasterTiming(void)
{
	CasterTiersOn();
	metal::setRtTimingForTest(1);
	metal::RtFrameTiming probe;
	CasterFrame([](bool){});
	if(!metal::rtFrameTimingForTest(&probe)){
		metal::setRtTimingForTest(0);
		CasterTiersOff();
		printf("  caster timing: not measured (no stage-boundary counter samples)\n");
		return;
	}
	printf("  caster timing (median of 5 frames, recorded not gated):\n");
	Geometry *geo = SkinnedGridGeometry();
	std::vector<Atomic*> peds;
	for(int i = 0; i < 24; i++){
		Atomic *a = SkinAtomic(geo);
		Place(a, 0.0f, 0.0f, 10.0f + 2.0f*i, 0.0f);
		PoseSkin(a, 4);
		peds.push_back(a);
	}
	std::vector<Atomic*> one(peds.begin(), peds.begin() + 1);
	float32 ms[2][2][2];
	const char *modeName[2] = { "rebuild", "refit" };
	for(int m = 0; m < 2; m++){
		SkinnedTiming(peds, metal::RT_SKIN_REBUILD + m, &ms[m][0][0], &ms[m][0][1]);
		SkinnedTiming(one, metal::RT_SKIN_REBUILD + m, &ms[m][1][0], &ms[m][1][1]);
		printf("    skinned %-7s 24 casters of 1500 triangles: skinning %.3f ms, levels %.3f ms; 1 caster: skinning %.3f ms, level %.3f ms\n",
			modeName[m], ms[m][0][0], ms[m][0][1], ms[m][1][0], ms[m][1][1]);
	}
	metal::setRtSkinModeForTest(metal::RT_SKIN_DEFAULT);
	float32 ratio = ms[1][0][1] > 0.0f ? ms[0][0][1]/ms[1][0][1] : 0.0f;
	printf("    rebuild / refit = %.2f: %s\n", ratio, ratio <= 1.5f ? "rebuild per frame" :
		"refit with a full rebuild every 30 frames");
	for(Atomic *a : peds)
		DestroySkinAtomic(a);
	geo->destroy();

	std::vector<Geometry*> strips;
	for(int q = 1; q <= 200; q++)
		strips.push_back(StripGeometry(q, 0.0f));
	std::vector<Atomic*> grid;
	for(int i = 0; i < 8192; i++){
		Atomic *a = MakeAtomic(strips[i % 200]);
		Place(a, (float)(i % 91)*20.0f, (float)(i / 91)*20.0f, 50.0f, 0.0f);
		grid.push_back(a);
	}
	metal::setRtBuildBudgetForTest(400000);
	for(int f = 0; f < 8; f++){
		CasterFrame([&](bool prepass){
			if(prepass)
				for(Atomic *a : grid)
					metal::addShadowCaster(a, metal::SHADOWCASTER_STATIC);
		});
		if(metal::rtQueuedForTest() == 0)
			break;
	}
	metal::setRtBuildBudgetForTest(0);
	const uint32 counts[4] = { 1, 2000, 5000, 8192 };
	for(uint32 c = 0; c < 4; c++){
		std::vector<float32> inst, walk;
		uint32 instances = 0;
		for(int f = 0; f < 5; f++){
			CasterFrame([&](bool prepass){
				if(prepass)
					for(uint32 i = 0; i < counts[c]; i++)
						metal::addShadowCaster(grid[i], metal::SHADOWCASTER_STATIC);
			});
			metal::RtFrameTiming t;
			if(metal::rtFrameTimingForTest(&t)){
				inst.push_back(t.instanceMs);
				walk.push_back(t.walkMs);
				instances = t.instances;
			}
		}
		printf("    instance level, %u casters: build %.3f ms in the frame command buffer, CPU %.3f ms, %u instances\n",
			counts[c], Median(inst), Median(walk), instances);
	}
	metal::setRtTimingForTest(0);
	CasterTiersOff();
	for(Atomic *a : grid)
		DestroyAtomic(a);
	for(Geometry *g : strips)
		g->destroy();
}

static int
SX(float x, float depth)
{
	return (int)floorf(320.0f*(1.0f - x/depth));
}

struct ShadowScene
{
	Geometry *floorGeo, *roofGeo;
	Atomic *floor, *roof;
	Material *mat;
	Light *sun, *amb;
	Frame *sunFrame;
	Matrix camSaved;
	metal::RenderSettings saved;
	metal::RelitParams relitSaved;
};

static void
TurnSun(ShadowScene *s, float degrees)
{
	V3d yAxis = { 0.0f, 1.0f, 0.0f };
	s->sunFrame->rotate(&yAxis, degrees, COMBINEREPLACE);
	s->sunFrame->updateObjects();
}

static Geometry*
ShadowQuad(Material *mat, float x0, float y0, float x1, float y1, float z)
{
	RGBA c = { 120, 120, 120, 255 };
	RGBA col[4] = { c, c, c, c };
	return QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT, x0, y0, x1, y1, z, col, nil, mat);
}

static void
MoveBy(Frame *f, float x, float y, float z)
{
	V3d d = { x, y, z };
	f->translate(&d, COMBINEPOSTCONCAT);
	f->updateObjects();
}

static void
SetShadowQuality(int32 quality)
{
	metal::RenderSettings s = metal::getRenderSettings();
	if(s.shadowQuality == quality)
		return;
	s.shadowQuality = quality;
	metal::setRenderSettings(&s);
}

static void
MakeShadowScene(ShadowScene *s, float weight)
{
	s->camSaved = rtCamera->getFrame()->matrix;
	V3d xAxis = { 1.0f, 0.0f, 0.0f };
	rtCamera->getFrame()->rotate(&xAxis, 180.0f, COMBINEPOSTCONCAT);
	rtCamera->getFrame()->updateObjects();
	s->mat = Material::create();
	SurfaceProperties sp = { 1.0f, 0.0f, 1.0f };
	s->mat->surfaceProps = sp;
	s->floorGeo = ShadowQuad(s->mat, 8.0f, -8.0f, -8.0f, 8.0f, -20.0f);
	s->floor = MakeAtomic(s->floorGeo);
	s->roofGeo = ShadowQuad(s->mat, 1.0f, -1.0f, -1.0f, 1.0f, -15.0f);
	s->roof = MakeAtomic(s->roofGeo);
	s->sun = Light::create(Light::DIRECTIONAL);
	s->sun->setColor(0.8f, 0.6f, 0.5f);
	s->sunFrame = Frame::create();
	TurnSun(s, -135.0f);
	s->sun->setFrame(s->sunFrame);
	TestWorld()->addLight(s->sun);
	s->amb = Light::create(Light::AMBIENT);
	s->amb->setColor(0.1f, 0.1f, 0.1f);
	TestWorld()->addLight(s->amb);
	metal::setSunLight(s->sun);
	s->relitSaved = metal::getRelitParams();
	metal::RelitParams p = s->relitSaved;
	p.weight = weight;
	for(int i = 0; i < 3; i++){
		p.skyTop[i] = 0.3f;
		p.skyBottom[i] = 0.1f;
	}
	p.hemiScale = 1.0f;
	p.hemiSaturation = 1.0f;
	p.sunScale = 1.0f;
	p.sunRadius = 0.005f;
	metal::setRelitParams(&p);
	s->saved = metal::getRenderSettings();
	metal::TierCaps all = { true, true, true };
	metal::setTierCapsForTest(&all);
	metal::RenderSettings r = s->saved;
	r.tier = metal::TIER_MODERN;
	r.upscale = metal::UPSCALE_NATIVE;
	r.antiAliasing = metal::ANTIALIAS_OFF;
	metal::setRenderSettings(&r);
}

static void
FreeShadowScene(ShadowScene *s)
{
	metal::setRenderSettings(&s->saved);
	metal::setRelitParams(&s->relitSaved);
	metal::setSunLight(nil);
	TestWorld()->removeLight(s->sun);
	TestWorld()->removeLight(s->amb);
	s->sun->destroy();
	s->sunFrame->destroy();
	s->amb->destroy();
	DestroyAtomic(s->floor);
	DestroyAtomic(s->roof);
	s->floorGeo->destroy();
	s->roofGeo->destroy();
	s->mat->destroy();
	rtCamera->getFrame()->transform(&s->camSaved, COMBINEREPLACE);
	rtCamera->getFrame()->updateObjects();
	metal::setTierCapsForTest(nil);
}

static void
ShadowFrame(ShadowScene *s, int32 quality, int n)
{
	SetShadowQuality(quality);
	for(int i = 0; i < n; i++)
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLNONE);
			s->floor->render();
			s->roof->render();
		});
	metal::rtWaitForTest();
}

static float
ShadowExpect(int ch, float prelit, float ambient, float w, V3d n, const ShadowScene *s, float shadow)
{
	metal::RelitParams p = metal::getRelitParams();
	float wc = relitClampWeight(w);
	float classic = fminf(fmaxf(relitWeighted(prelit, wc) + ambient, 0.0f), 1.0f);
	RLColor top = { p.skyTop[0], p.skyTop[1], p.skyTop[2] }, bottom = { p.skyBottom[0], p.skyBottom[1], p.skyBottom[2] };
	RLColor h = relitDesaturate(relitHemisphere(n.z, top, bottom), p.hemiSaturation);
	float hc = ch == 0 ? h.r : ch == 1 ? h.g : h.b;
	float sc = ch == 0 ? s->sun->color.red : ch == 1 ? s->sun->color.green : s->sun->color.blue;
	V3d at = s->sunFrame->getLTM()->at;
	float live = 1.0f - wc;
	float c = relitSunCos(-(n.x*at.x + n.y*at.y + n.z*at.z), false);
	RLSplit sp = relitSplitAdd(classic, hc*live*p.hemiScale, sc*c*live*p.sunScale, 1.0f);
	return relitShadowed(sp.full, sp.sun, shadow);
}

static float
ShadowLength(int x, int y)
{
	float v = -1.0f, len = -1.0f;
	if(!metal::readShadowForTest(x, y, &v, &len))
		return -1.0f;
	return len;
}

static bool
CheckShadowOnScreen(void)
{
	const char *name = "the shadow factor is 0 on the floor behind the roof and 1 in the open and on the roof";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
	float in = -1, out = -1, roof = -1, len = 0;
	bool read = metal::readShadowForTest(SX(-5.0f, 20.0f), 240, &in, &len) && metal::readShadowForTest(SX(3.0f, 20.0f), 240, &out, &len) &&
		metal::readShadowForTest(SX(0.0f, 15.0f), 240, &roof, &len);
	metal::RayStats st = metal::getRayStats();
	FreeShadowScene(&s);
	bool ok = read && in < 0.05f && out > 0.95f && roof > 0.95f && st.instances == 2 && st.raysPerFrame > 0;
	if(!ok)
		Detail("  read %d behind the roof %.3f open %.3f roof %.3f instances %u rays %u\n", read, in, out, roof, st.instances,
			st.raysPerFrame);
	return Report(ok, name);
}

static bool
CheckShadowAccumulates(void)
{
	const char *name = "the shadow history grows to the quality's length, a moved caster's old shadow clears and its new one appears in one frame, and the history restarts where the surface moves";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 20);
	float lenSettled = ShadowLength(SX(3.0f, 20.0f), 240);
	MoveBy(s.roof->getFrame(), 4.0f, 0.0f, 0.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	float oldArea = -1, newArea = -1, len = 0;
	bool read = metal::readShadowForTest(SX(-5.0f, 20.0f), 240, &oldArea, &len) &&
		metal::readShadowForTest(SX(-1.0f, 20.0f), 240, &newArea, &len);
	MoveBy(s.floor->getFrame(), 0.0f, 0.0f, -3.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	float lenMoved = ShadowLength(SX(3.0f, 23.0f), 240);
	FreeShadowScene(&s);
	float want = shQuality(metal::SHADOWQUALITY_MEDIUM).maxHistory;
	bool ok = read && lenSettled == want && oldArea > 0.95f && newArea < 0.05f && lenMoved == 1.0f;
	if(!ok)
		Detail("  settled length %.1f (want %.1f); after the move old area %.3f new area %.3f; moved floor length %.1f\n",
			lenSettled, want, oldArea, newArea, lenMoved);
	return Report(ok, name);
}

static bool
CheckShadowHistoryCameraMotion(void)
{
	const char *name = "a static floor keeps its shadow history while the camera moves toward it, and a quad moving with the camera keeps its own";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *fg = ShadowQuad(s.mat, 40.0f, -40.0f, -40.0f, 40.0f, -30.0f);
	Geometry *qg = ShadowQuad(s.mat, -2.0f, -1.0f, -4.0f, 1.0f, -6.0f);
	Atomic *fl = MakeAtomic(fg), *q = MakeAtomic(qg);
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	for(int f = 0; f < 14; f++){
		MoveBy(rtCamera->getFrame(), 0.0f, 0.0f, -1.5f);
		MoveBy(q->getFrame(), 0.0f, 0.0f, -1.5f);
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLNONE);
			fl->render();
			q->render();
		});
	}
	metal::rtWaitForTest();
	float floorLen = ShadowLength(320, 240), quadLen = ShadowLength(SX(-3.0f, 6.0f), 240);
	DestroyAtomic(fl);
	DestroyAtomic(q);
	fg->destroy();
	qg->destroy();
	FreeShadowScene(&s);
	float want = shQuality(metal::SHADOWQUALITY_MEDIUM).maxHistory;
	bool ok = floorLen == want && quadLen == want;
	if(!ok)
		Detail("  floor length %.1f quad length %.1f (want %.1f)\n", floorLen, quadLen, want);
	return Report(ok, name);
}

static int
SY(float y, float depth)
{
	return (int)floorf(240.0f*(1.0f + y/(0.75f*depth)));
}

static bool
CheckShadowHistoryPan(void)
{
	const char *name = "a static floor keeps its shadow history beside the roof's shadow while the camera pans across it";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
	for(int f = 0; f < 16; f++){
		MoveBy(rtCamera->getFrame(), 0.25f, 0.25f, 0.0f);
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLNONE);
			s.floor->render();
			s.roof->render();
		});
	}
	metal::rtWaitForTest();
	int px[4] = { SX(-7.75f, 20.0f), SX(-10.25f, 20.0f), SX(-9.0f, 20.0f), SX(-9.0f, 20.0f) };
	int py[4] = { SY(-4.0f, 20.0f), SY(-4.0f, 20.0f), SY(-2.75f, 20.0f), SY(-5.25f, 20.0f) };
	float len[4], v[4], in = -1.0f, inLen = 0.0f;
	bool read = metal::readShadowForTest(SX(-9.0f, 20.0f), SY(-4.0f, 20.0f), &in, &inLen) != 0;
	for(int i = 0; i < 4; i++){
		v[i] = -1.0f;
		len[i] = -1.0f;
		read = metal::readShadowForTest(px[i], py[i], &v[i], &len[i]) && read;
	}
	FreeShadowScene(&s);
	float want = shQuality(metal::SHADOWQUALITY_MEDIUM).maxHistory;
	bool ok = read && in < 0.05f;
	for(int i = 0; i < 4; i++)
		ok = ok && len[i] == want && v[i] > 0.95f;
	if(!ok)
		Detail("  read %d inside %.3f; left %.3f/%.1f right %.3f/%.1f below %.3f/%.1f above %.3f/%.1f (factor/length, want length %.1f)\n",
			read, in, v[0], len[0], v[1], len[1], v[2], len[2], v[3], len[3], want);
	return Report(ok, name);
}

static bool
CheckShadowHistoryPlaneNormal(void)
{
	const char *name = "a floor sliding in its own plane keeps its shadow history under a pitched camera moving toward it";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	V3d xAxis = { 1.0f, 0.0f, 0.0f };
	rtCamera->getFrame()->rotate(&xAxis, 35.0f, COMBINEPRECONCAT);
	rtCamera->getFrame()->updateObjects();
	Geometry *fg = ShadowQuad(s.mat, 40.0f, -40.0f, -40.0f, 40.0f, -30.0f);
	Atomic *fl = MakeAtomic(fg);
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	for(int f = 0; f < 14; f++){
		V3d at = rtCamera->getFrame()->getLTM()->at;
		MoveBy(rtCamera->getFrame(), 1.5f*at.x, 1.5f*at.y, 1.5f*at.z);
		MoveBy(fl->getFrame(), 0.0f, 1.0f, 0.0f);
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLNONE);
			fl->render();
		});
	}
	metal::rtWaitForTest();
	float v = -1.0f, len = -1.0f;
	bool read = metal::readShadowForTest(320, 240, &v, &len) != 0;
	DestroyAtomic(fl);
	fg->destroy();
	FreeShadowScene(&s);
	float want = shQuality(metal::SHADOWQUALITY_MEDIUM).maxHistory;
	bool ok = read && len == want;
	if(!ok)
		Detail("  read %d centre factor %.3f length %.1f (want %.1f)\n", read, v, len, want);
	return Report(ok, name);
}

static bool
CheckShadowHistoryRestarts(void)
{
	const char *name = "the shadow history restarts after a camera cut, a sun turn over 2 degrees, a quality change and a scene without the pass, and not after a 1 degree turn";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	int x = SX(3.0f, 20.0f);
	float len[5];
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
	metal::cameraCut();
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	len[0] = ShadowLength(x, 240);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 13);
	TurnSun(&s, -138.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	len[1] = ShadowLength(x, 240);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 13);
	TurnSun(&s, -139.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	len[2] = ShadowLength(x, 240);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	ShadowFrame(&s, metal::SHADOWQUALITY_HIGH, 1);
	len[3] = ShadowLength(x, 240);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 13);
	TurnSun(&s, -87.134f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	TurnSun(&s, -139.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	len[4] = ShadowLength(x, 240);
	FreeShadowScene(&s);
	float want = shQuality(metal::SHADOWQUALITY_MEDIUM).maxHistory;
	bool ok = len[0] == 1.0f && len[1] == 1.0f && len[2] == want && len[3] == 1.0f && len[4] == 1.0f;
	if(!ok)
		Detail("  lengths after a cut %.1f, a 3 degree turn %.1f, a 1 degree turn %.1f (want %.1f), a quality change %.1f, a scene without the pass %.1f\n",
			len[0], len[1], len[2], want, len[3], len[4]);
	return Report(ok, name);
}

struct SkipState
{
	uint32 passes, relit, shadow, rays;
	float value, length;
	bool read;
	char pass[16];
};

static void
SkipMeasure(ShadowScene *s, int32 quality, SkipState *r)
{
	ShadowFrame(s, quality, 1);
	uint32 p0 = metal::getFrameStats().renderPasses;
	metal::StateStats a0 = metal::getStateStats();
	ShadowFrame(s, quality, 1);
	metal::StateStats a1 = metal::getStateStats();
	r->passes = metal::getFrameStats().renderPasses - p0;
	r->relit = a1.relitDraws - a0.relitDraws;
	r->shadow = a1.relitShadowDraws - a0.relitShadowDraws;
	r->rays = metal::getRayStats().raysPerFrame;
	r->value = -1.0f;
	r->length = -1.0f;
	r->read = metal::readShadowForTest(SX(-5.0f, 20.0f), 240, &r->value, &r->length) != 0;
	metal::logStats();
	const char *t = strstr(metal::getStatsLine(), " shadow ");
	strcpy(r->pass, "?");
	if(t)
		sscanf(t, " shadow %15s", r->pass);
}

static bool
CheckShadowSkipped(void)
{
	const char *name = "with the sun below the horizon, a sun scale of 0 or ShadowQuality off no ray is traced and the factor is 1; the relit draws keep reading the factor while ShadowQuality is on; the pass adds no render pass";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SkipState st[4];
	SkipMeasure(&s, metal::SHADOWQUALITY_MEDIUM, &st[0]);
	TurnSun(&s, -87.134f);
	SkipMeasure(&s, metal::SHADOWQUALITY_MEDIUM, &st[1]);
	TurnSun(&s, -135.0f);
	metal::RelitParams p = metal::getRelitParams();
	p.sunScale = 0.0f;
	metal::setRelitParams(&p);
	SkipMeasure(&s, metal::SHADOWQUALITY_MEDIUM, &st[2]);
	p.sunScale = 1.0f;
	metal::setRelitParams(&p);
	SkipMeasure(&s, metal::SHADOWQUALITY_OFF, &st[3]);
	FreeShadowScene(&s);
	bool ok = st[0].rays > 0 && st[0].passes > 0;
	for(int i = 1; i < 3; i++)
		ok = ok && st[i].rays == 0 && st[i].read && st[i].value == 1.0f && st[i].length == 0.0f && st[i].shadow == st[i].relit &&
			st[i].relit > 0;
	ok = ok && st[3].rays == 0 && st[3].read && st[3].value == 1.0f && st[3].shadow == 0 && st[3].relit > 0;
	for(int i = 1; i < 4; i++)
		ok = ok && st[i].passes == st[0].passes && strcmp(st[i].pass, "-") == 0;
	if(!ok)
		for(int i = 0; i < 4; i++)
			Detail("  state %c: passes %u relit %u shadow %u rays %u factor %.3f length %.1f read %d pass time %s\n", 'A' + i,
				st[i].passes, st[i].relit, st[i].shadow, st[i].rays, st[i].value, st[i].length, st[i].read, st[i].pass);
	return Report(ok, name);
}

static bool
CheckShadowSunOnly(void)
{
	const char *name = "in a relit frame the shadow removes only the sun's part of a prelit floor";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
	float shade[4] = { -1, -1, -1, -1 }, open[4] = { -1, -1, -1, -1 };
	bool read = false;
	CasterFrame([&](bool prepass){
		SetRenderState(CULLMODE, CULLNONE);
		s.floor->render();
		s.roof->render();
		if(!prepass)
			read = metal::readScenePixel(SX(-5.0f, 20.0f), 240, shade) && metal::readScenePixel(SX(6.5f, 20.0f), 240, open);
	});
	metal::rtWaitForTest();
	V3d n = { 0.0f, 0.0f, 1.0f };
	float shade0 = ShadowExpect(0, 120/255.0f, 0.1f, 0.5f, n, &s, 0.0f);
	float open1 = ShadowExpect(0, 120/255.0f, 0.1f, 0.5f, n, &s, 1.0f);
	FreeShadowScene(&s);
	bool ok = read && fabsf(shade[0] - shade0) < 2.0f/255.0f && fabsf(open[0] - open1) < 2.0f/255.0f && open1 - shade0 > 0.05f;
	if(!ok)
		Detail("  read %d shaded red %.4f (want %.4f) open red %.4f (want %.4f)\n", read, shade[0], shade0, open[0], open1);
	return Report(ok, name);
}

static bool
CheckShadowStats(void)
{
	const char *name = "the stats line reports shadows, rays, levels, derived streams, instances, builds, refits, the walk, the pass time and the shadow memory";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_HIGH, 3);
	metal::RayStats st = metal::getRayStats();
	metal::FrameInfo fi = metal::getFrameInfo();
	metal::logStats();
	const char *line = metal::getStatsLine();
	SHQuality q = shQuality(metal::SHADOWQUALITY_HIGH);
	uint32 rw = q.halfRes ? (fi.internalWidth + 1)/2 : fi.internalWidth, rh = q.halfRes ? (fi.internalHeight + 1)/2 : fi.internalHeight;
	uint32 want = rw*rh*q.raysPerPixel;
	float wantMem = (float)(((double)rw*rh*2 + 2.0*fi.internalWidth*fi.internalHeight*8)/1048576.0);
	const char *t = strstr(line, " shadows ");
	char qn[16] = "", pass[16] = "";
	unsigned rays = 0, blas = 0, tlas = 0, builds = 0, refits = 0;
	float blasMB = -1, normalsMB = -1, walk = -1, mem = -1;
	int n = t ? sscanf(t, " shadows %15s rays %u blas %u %fMB normals %fMB tlas %u build %u refit %u walk %fms shadow %15s shadowmem %fMB",
		qn, &rays, &blas, &blasMB, &normalsMB, &tlas, &builds, &refits, &walk, pass, &mem) : 0;
	FreeShadowScene(&s);
	bool timed = st.passMs < 0.0f ? strcmp(pass, "-") == 0 : strstr(pass, "ms") != nullptr;
	bool ok = n == 11 && strcmp(qn, "high") == 0 && rays == want && st.raysPerFrame == want && tlas == 2 && blas >= 2 &&
		fabsf(mem - wantMem) <= 0.5f && timed && walk >= 0.0f;
	if(!ok)
		Detail("  parsed %d, rays %u (expected %u), pass %.3f ms; line: %s", n, rays, want, st.passMs, line);
	return Report(ok, name);
}

static const uint8 checkerAlpha[4] = { 0, 255, 255, 0 };
static const uint8 clearAlpha[4] = { 0, 0, 0, 0 };
static const uint8 solidAlpha[4] = { 255, 255, 255, 254 };

static const uint8 patternAlpha[16] = {
	255, 0, 0, 0,
	255, 255, 0, 0,
	0, 255, 0, 255,
	0, 0, 255, 0 };
static const uint8 cornerAlpha[4] = { 255, 0, 0, 0 };

static Texture*
AlphaTexture(int w, int h, const uint8 *alpha, int32 addressU = Texture::WRAP, int32 addressV = Texture::WRAP)
{
	std::vector<RGBA> t(w*h);
	for(int i = 0; i < w*h; i++)
		t[i] = { 255, 255, 255, alpha[i] };
	return MakeTexture(w, h, t.data(), Texture::LINEAR, addressU, addressV);
}

static Material*
AlphaMaterial(const uint8 *alpha, int w = 2, int h = 2, uint8 materialAlpha = 255)
{
	Material *m = Material::create();
	SurfaceProperties sp = { 1.0f, 0.0f, 1.0f };
	m->surfaceProps = sp;
	m->color.alpha = materialAlpha;
	if(alpha){
		Texture *tex = AlphaTexture(w, h, alpha);
		m->setTexture(tex);
		if(tex)
			tex->destroy();
	}
	return m;
}

static Atomic*
AlphaRoof(Geometry **geo, Material *m0, Material *m1, uint8 vertexAlpha = 255, float uvScale = 1.0f)
{
	RGBA c = { 120, 120, 120, 255 };
	RGBA col[4] = { c, c, c, c };
	col[3].alpha = vertexAlpha;
	TexCoords uv[4] = { { 0.0f, 0.0f }, { uvScale, 0.0f }, { uvScale, uvScale }, { 0.0f, uvScale } };
	*geo = SplitQuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT | Geometry::TEXTURED, 1.0f, -1.0f, -1.0f, 1.0f,
		-15.0f, col, uv, m0, m1);
	m0->destroy();
	if(m1 != m0)
		m1->destroy();
	return MakeAtomic(*geo);
}

static void
AlphaRoofPass(Atomic *a, bool prepass, int32 ref, bool zwrite = true)
{
	if(prepass){
		metal::addShadowCaster(a, metal::SHADOWCASTER_STATIC);
		return;
	}
	SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
	SetRenderState(ALPHATESTREF, ref);
	SetRenderState(ZWRITEENABLE, zwrite);
	a->render();
	SetRenderState(ZWRITEENABLE, 1);
	SetRenderState(ALPHATESTREF, 3);
}

template<typename F> static void
AlphaFrames(ShadowScene *s, int n, F roofs)
{
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	for(int i = 0; i < n; i++)
		CasterFrame([&](bool prepass){
			SetRenderState(CULLMODE, CULLNONE);
			s->floor->render();
			roofs(prepass);
		});
	metal::rtWaitForTest();
}

static float
Floor(float x, float y)
{
	float v = -1.0f, len = 0.0f;
	return metal::readShadowForTest(SX(x, 20.0f), SY(y, 20.0f), &v, &len) ? v : -1.0f;
}

static void
FloorQuad(float y, float v[4])
{
	v[0] = Floor(-5.5f, y + 0.5f);
	v[1] = Floor(-4.5f, y + 0.5f);
	v[2] = Floor(-5.5f, y - 0.5f);
	v[3] = Floor(-4.5f, y - 0.5f);
}

static bool
Lit(float v)
{
	return v > 0.95f;
}

static bool
Dark(float v)
{
	return v >= 0.0f && v < 0.05f;
}

static bool
CheckAlphaCheckerboard(void)
{
	const char *name = "an alpha-tested caster casts its texture's alpha: an asymmetric 4x4 alpha pattern on a roof shows texel for texel in its shadow";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Material *m = AlphaMaterial(patternAlpha, 4, 4);
	Geometry *g;
	Atomic *roof = AlphaRoof(&g, m, m);
	AlphaFrames(&s, 16, [&](bool prepass){ AlphaRoofPass(roof, prepass, 128); });
	float v[16];
	for(int r = 0; r < 4; r++)
		for(int c = 0; c < 4; c++)
			v[r*4 + c] = Floor(1.0f - (c + 0.5f)/2.0f - 5.0f, 1.0f - (r + 0.5f)/2.0f);
	float open = Floor(3.0f, 0.0f);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(roof);
	g->destroy();
	FreeShadowScene(&s);
	bool ok = Lit(open) && st.alphaKernel == 1 && st.alphaInstances == 1 && st.alphaEntries == 1 && st.alphaTextures == 1;
	for(int i = 0; i < 16; i++)
		ok = ok && (patternAlpha[i] ? Dark(v[i]) : Lit(v[i]));
	Detail("  floor under texel rows: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f, open %.3f\n",
		v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], open);
	if(!ok)
		Detail("  alpha kernel %u instances %u entries %u textures %u\n", st.alphaKernel, st.alphaInstances, st.alphaEntries,
			st.alphaTextures);
	return Report(ok, name);
}

static bool
CheckAlphaThreshold(void)
{
	const char *name = "the alpha threshold follows the reference of the mesh's depth-writing forward draw";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	static const uint8 ramp[2] = { 100, 200 };
	Material *m = AlphaMaterial(ramp, 2, 1);
	Material *shared = m;
	shared->addRef();
	Geometry *g, *fg;
	Atomic *roof = AlphaRoof(&g, m, m);
	int32 gsRef = GetRenderState(GSALPHATESTREF);
	float lo[6], hi[6];
	auto read = [&](int i){
		lo[i] = Floor(-4.5f, 0.0f);
		hi[i] = Floor(-5.5f, 0.0f);
	};
	AlphaFrames(&s, 16, [&](bool prepass){ AlphaRoofPass(roof, prepass, 150); });
	read(0);
	AlphaFrames(&s, 16, [&](bool prepass){ AlphaRoofPass(roof, prepass, 50); });
	read(1);
	AlphaFrames(&s, 16, [&](bool prepass){ AlphaRoofPass(roof, prepass, 250); });
	read(2);
	AlphaFrames(&s, 16, [&](bool prepass){
		AlphaRoofPass(roof, prepass, 50, true);
		if(!prepass)
			AlphaRoofPass(roof, prepass, 250, false);
	});
	read(3);
	AlphaFrames(&s, 16, [&](bool prepass){
		if(prepass){
			metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
			return;
		}
		SetRenderState(GSALPHATEST, 1);
		SetRenderState(GSALPHATESTREF, 220);
		SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
		SetRenderState(ALPHATESTREF, 3);
		roof->render();
		SetRenderState(GSALPHATEST, 0);
		SetRenderState(GSALPHATESTREF, gsRef);
	});
	read(4);
	Atomic *fresh = AlphaRoof(&fg, shared, shared);
	AlphaFrames(&s, 16, [&](bool prepass){
		if(prepass)
			metal::addShadowCaster(fresh, metal::SHADOWCASTER_STATIC);
	});
	read(5);
	DestroyAtomic(roof);
	DestroyAtomic(fresh);
	g->destroy();
	fg->destroy();
	FreeShadowScene(&s);
	bool want[6] = { false, false, true, false, true, false };
	bool ok = true;
	for(int i = 0; i < 6; i++)
		ok = ok && Lit(lo[i]) && (want[i] ? Lit(hi[i]) : Dark(hi[i]));
	Detail("  alpha 100 / 200 behind the roof: A ref 150 %.3f %.3f, B ref 50 %.3f %.3f, C ref 250 %.3f %.3f, D 50 written then 250 unwritten %.3f %.3f, E PS2 ref 220 %.3f %.3f, F never drawn %.3f %.3f\n",
		lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], lo[4], hi[4], lo[5], hi[5]);
	return Report(ok, name);
}

static bool
CheckAlphaBlendedCastNothing(void)
{
	const char *name = "meshes with vertex alpha or material alpha below 255 cast nothing, alpha texture or not";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	bool ok = true;
	for(int k = 0; k < 2; k++){
		Material *m = AlphaMaterial(checkerAlpha, 2, 2, k == 0 ? 255 : 128);
		Geometry *g;
		Atomic *roof = AlphaRoof(&g, m, m, k == 0 ? 128 : 255);
		metal::RayStats before = metal::getRayStats();
		AlphaFrames(&s, 16, [&](bool prepass){ AlphaRoofPass(roof, prepass, 128); });
		float v[4];
		FloorQuad(0.0f, v);
		metal::RayStats st = metal::getRayStats();
		DestroyAtomic(roof);
		g->destroy();
		bool one = Lit(v[0]) && Lit(v[1]) && Lit(v[2]) && Lit(v[3]) && st.alphaInstances == 0 && st.alphaKernel == 0 &&
			st.levelsEmpty == before.levelsEmpty + 1;
		if(!one)
			Detail("  %s: floor %.3f %.3f %.3f %.3f alpha instances %u kernel %u empty levels %u -> %u\n",
				k == 0 ? "vertex alpha" : "material alpha", v[0], v[1], v[2], v[3], st.alphaInstances, st.alphaKernel,
				before.levelsEmpty, st.levelsEmpty);
		ok = ok && one;
	}
	FreeShadowScene(&s);
	return Report(ok, name);
}

static Atomic*
MixedRoof(Geometry **geo)
{
	Atomic *a = AlphaRoof(geo, AlphaMaterial(nil), AlphaMaterial(solidAlpha));
	Place(a, 0.0f, 4.0f, 0.0f, 0.0f);
	return a;
}

static bool
CheckAlphaGeometries(void)
{
	const char *name = "a level's alpha geometries each use their own texture, and an opaque mesh beside an alpha mesh still casts";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *ga, *gb;
	Atomic *a = AlphaRoof(&ga, AlphaMaterial(clearAlpha), AlphaMaterial(solidAlpha));
	Atomic *b = MixedRoof(&gb);
	AlphaFrames(&s, 16, [&](bool prepass){
		AlphaRoofPass(a, prepass, 128);
		AlphaRoofPass(b, prepass, 128);
	});
	float a0 = Floor(-5.5f, 0.5f), a1 = Floor(-4.5f, -0.5f), b0 = Floor(-5.5f, 4.5f), b1 = Floor(-4.5f, 3.5f);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(a);
	DestroyAtomic(b);
	ga->destroy();
	gb->destroy();
	FreeShadowScene(&s);
	bool ok = Lit(a0) && Dark(a1) && Dark(b0) && Dark(b1) && st.alphaEntries == 3 && st.alphaInstances == 2;
	if(!ok)
		Detail("  A triangle 0 %.3f triangle 1 %.3f, B triangle 0 %.3f triangle 1 %.3f, alpha instances %u entries %u\n", a0, a1, b0, b1,
			st.alphaInstances, st.alphaEntries);
	return Report(ok, name);
}

static bool
CheckAlphaKernelChoice(void)
{
	const char *name = "a frame without alpha casters traces with the opaque kernel and no table, and the alpha kernel gives the same visibility on opaque geometry";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	float in[2], out[2], roof[2];
	metal::RayStats st[2];
	for(int k = 0; k < 2; k++){
		metal::setShadowAlphaKernelForTest(k == 0 ? -1 : 1);
		ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
		float len = 0;
		in[k] = out[k] = roof[k] = -1.0f;
		metal::readShadowForTest(SX(-5.0f, 20.0f), 240, &in[k], &len);
		metal::readShadowForTest(SX(3.0f, 20.0f), 240, &out[k], &len);
		metal::readShadowForTest(SX(0.0f, 15.0f), 240, &roof[k], &len);
		st[k] = metal::getRayStats();
	}
	metal::setShadowAlphaKernelForTest(-1);
	FreeShadowScene(&s);
	bool ok = st[0].alphaKernel == 0 && st[0].alphaInstances == 0 && st[0].alphaEntries == 0 && st[0].alphaTextures == 0 &&
		st[1].alphaKernel == 1 && st[1].alphaEntries == 0;
	for(int k = 0; k < 2; k++)
		ok = ok && Dark(in[k]) && Lit(out[k]) && Lit(roof[k]);
	if(!ok)
		Detail("  automatic: kernel %u instances %u entries %u textures %u behind %.3f open %.3f roof %.3f; forced: kernel %u entries %u behind %.3f open %.3f roof %.3f\n",
			st[0].alphaKernel, st[0].alphaInstances, st[0].alphaEntries, st[0].alphaTextures, in[0], out[0], roof[0],
			st[1].alphaKernel, st[1].alphaEntries, in[1], out[1], roof[1]);
	return Report(ok, name);
}

static bool
CheckAlphaTextureLifetime(void)
{
	const char *name = "an alpha caster's texture is alive while the frame that sampled it is in flight and released after it, and a swapped texture is sampled the next frame";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Material *m = AlphaMaterial(checkerAlpha);
	m->addRef();
	Geometry *g;
	Atomic *roof = AlphaRoof(&g, m, m);
	auto drawn = [&](bool prepass){ AlphaRoofPass(roof, prepass, 128); };
	AlphaFrames(&s, 15, drawn);
	CasterFrame([&](bool prepass){
		SetRenderState(CULLMODE, CULLNONE);
		s.floor->render();
		if(prepass)
			metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
	}, false);
	Raster *ras = m->texture ? m->texture->raster : nil;
	bool stamped = ras && PLUGINOFFSET(metal::MetalRaster, ras, metal::nativeRasterOffset)->lastUseFrame == metal::getFrameId();
	metal::rtWatchTextureForTest(ras);
	Texture *next = AlphaTexture(2, 2, solidAlpha);
	m->setTexture(next);
	if(next)
		next->destroy();
	bool aliveInFlight = metal::rtWatchedTextureAliveForTest() != 0;
	metal::rtWaitForTest();
	AlphaFrames(&s, 2, drawn);
	bool aliveAfter = metal::rtWatchedTextureAliveForTest() != 0;
	AlphaFrames(&s, 16, drawn);
	float v[4];
	FloorQuad(0.0f, v);
	DestroyAtomic(roof);
	g->destroy();
	m->destroy();
	FreeShadowScene(&s);
	bool ok = stamped && aliveInFlight && !aliveAfter && Dark(v[0]) && Dark(v[1]) && Dark(v[2]) && Dark(v[3]);
	if(!ok)
		Detail("  stamped %d alive in flight %d alive after %d swapped floor %.3f %.3f %.3f %.3f\n", stamped, aliveInFlight, aliveAfter,
			v[0], v[1], v[2], v[3]);
	return Report(ok, name);
}

static bool
CheckAlphaCap(void)
{
	const char *name = "alpha casters past the table cap let light through their alpha meshes and keep their opaque ones";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *ga, *gb;
	Material *m = AlphaMaterial(checkerAlpha);
	Atomic *a = AlphaRoof(&ga, m, m);
	Atomic *b = MixedRoof(&gb);
	metal::setRtAlphaCapForTest(1);
	AlphaFrames(&s, 16, [&](bool prepass){
		AlphaRoofPass(a, prepass, 128);
		AlphaRoofPass(b, prepass, 128);
	});
	metal::setRtAlphaCapForTest(0);
	float v[4];
	FloorQuad(0.0f, v);
	float b0 = Floor(-5.5f, 4.5f), b1 = Floor(-4.5f, 3.5f);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(a);
	DestroyAtomic(b);
	ga->destroy();
	gb->destroy();
	FreeShadowScene(&s);
	bool ok = Dark(v[0]) && Lit(v[1]) && Lit(v[2]) && Dark(v[3]) && Dark(b0) && Lit(b1) && st.alphaEntries == 1 && st.alphaCapped == 1;
	if(!ok)
		Detail("  A %.3f %.3f %.3f %.3f, B triangle 0 %.3f triangle 1 %.3f, entries %u capped %u\n", v[0], v[1], v[2], v[3], b0, b1,
			st.alphaEntries, st.alphaCapped);
	return Report(ok, name);
}

static bool
RoofHit(float x, float y, float z, uint32 mask = 0xFF)
{
	float o[3] = { x, y, z }, d[3] = { 0.0f, 0.0f, -1.0f };
	return metal::rtTraceForTest(o, d, 7.0f, mask) != 0;
}

static bool
CheckAlphaUnderOpaque(void)
{
	const char *name = "an opaque roof above an alpha-tested roof blocks rays through its solid and its clear texels, and a clear texel with nothing above lets light through";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Material *m = AlphaMaterial(checkerAlpha);
	Geometry *g, *og;
	Atomic *roof = AlphaRoof(&g, m, m);
	Atomic *over = Roof(&og, 1.0f, 0.5f);
	Place(over, 3.0f, 0.5f, -12.0f, 0.0f);
	AlphaFrames(&s, 16, [&](bool prepass){
		AlphaRoofPass(roof, prepass, 128);
		if(prepass)
			metal::addShadowCaster(over, metal::SHADOWCASTER_STATIC);
	});
	float v[4];
	FloorQuad(0.0f, v);
	float open = Floor(3.0f, 0.0f);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(roof);
	DestroyAtomic(over);
	g->destroy();
	og->destroy();
	FreeShadowScene(&s);
	bool ok = Dark(v[0]) && Dark(v[1]) && Lit(v[2]) && Dark(v[3]) && Lit(open) && st.alphaKernel == 1 && st.alphaInstances == 1;
	if(!ok)
		Detail("  solid under roof %.3f, clear under roof %.3f, clear alone %.3f, solid alone %.3f, open %.3f, kernel %u alpha instances %u\n",
			v[0], v[1], v[2], v[3], open, st.alphaKernel, st.alphaInstances);
	return Report(ok, name);
}

static bool
CheckAlphaMaskBit(void)
{
	const char *name = "an alpha-level instance given the opaque-only bit blocks as a solid quad, and one given neither bit lets all light through";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *ga, *gb;
	Atomic *a = AlphaRoof(&ga, AlphaMaterial(checkerAlpha), AlphaMaterial(checkerAlpha));
	Atomic *b = MixedRoof(&gb);
	float v[2][4], b0[2], b1[2];
	metal::RayStats st[2];
	for(int k = 0; k < 2; k++){
		metal::setRtAlphaMaskForTest(k + 1);
		AlphaFrames(&s, 16, [&](bool prepass){
			AlphaRoofPass(a, prepass, 128);
			AlphaRoofPass(b, prepass, 128);
		});
		metal::setRtAlphaMaskForTest(0);
		FloorQuad(0.0f, v[k]);
		b0[k] = Floor(-5.5f, 4.5f);
		b1[k] = Floor(-4.5f, 3.5f);
		st[k] = metal::getRayStats();
	}
	DestroyAtomic(a);
	DestroyAtomic(b);
	ga->destroy();
	gb->destroy();
	FreeShadowScene(&s);
	bool ok = true;
	for(int i = 0; i < 4; i++)
		ok = ok && Dark(v[0][i]) && Lit(v[1][i]);
	ok = ok && Dark(b0[0]) && Dark(b1[0]) && Lit(b0[1]) && Lit(b1[1]);
	for(int k = 0; k < 2; k++)
		ok = ok && st[k].alphaKernel == 1 && st[k].alphaInstances == 2;
	if(!ok)
		for(int k = 0; k < 2; k++)
			Detail("  %s: A %.3f %.3f %.3f %.3f, B triangle 0 %.3f triangle 1 %.3f, kernel %u alpha instances %u\n",
				k == 0 ? "opaque-only bit" : "neither bit", v[k][0], v[k][1], v[k][2], v[k][3], b0[k], b1[k], st[k].alphaKernel,
				st[k].alphaInstances);
	return Report(ok, name);
}

static bool
CheckAlphaMaskCarriers(void)
{
	const char *name = "instances of levels with alpha geometry carry the alpha-level bit and static and moving opaque casters the opaque-only bit";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *ga, *gs, *gm;
	Atomic *a = AlphaRoof(&ga, AlphaMaterial(checkerAlpha), AlphaMaterial(checkerAlpha));
	Atomic *still = Roof(&gs, 1.0f, 1.0f);
	Atomic *moving = Roof(&gm, 1.0f, 1.0f);
	Place(still, 0.0f, 8.0f, -15.0f, 0.0f);
	Place(moving, 0.0f, -4.0f, -15.0f, 0.0f);
	AlphaFrames(&s, 4, [&](bool prepass){
		AlphaRoofPass(a, prepass, 128);
		if(prepass){
			metal::addShadowCaster(still, metal::SHADOWCASTER_STATIC);
			metal::addShadowCaster(moving, metal::SHADOWCASTER_MOVING);
		}
	});
	bool alphaBit[3] = { RoofHit(0.0f, 0.0f, -10.0f, SH_INSTANCE_ALPHA), RoofHit(0.0f, 8.0f, -10.0f, SH_INSTANCE_ALPHA),
		RoofHit(0.0f, -4.0f, -10.0f, SH_INSTANCE_ALPHA) };
	bool opaqueBit[3] = { RoofHit(0.0f, 0.0f, -10.0f, SH_INSTANCE_OPAQUE), RoofHit(0.0f, 8.0f, -10.0f, SH_INSTANCE_OPAQUE),
		RoofHit(0.0f, -4.0f, -10.0f, SH_INSTANCE_OPAQUE) };
	bool any[3] = { RoofHit(0.0f, 0.0f, -10.0f), RoofHit(0.0f, 8.0f, -10.0f), RoofHit(0.0f, -4.0f, -10.0f) };
	DestroyAtomic(a);
	DestroyAtomic(still);
	DestroyAtomic(moving);
	ga->destroy();
	gs->destroy();
	gm->destroy();
	FreeShadowScene(&s);
	bool ok = alphaBit[0] && !alphaBit[1] && !alphaBit[2] && !opaqueBit[0] && opaqueBit[1] && opaqueBit[2] && any[0] && any[1] && any[2];
	if(!ok)
		Detail("  alpha-level bit: alpha roof %d static roof %d moving roof %d; opaque-only bit: %d %d %d; all bits: %d %d %d\n",
			alphaBit[0], alphaBit[1], alphaBit[2], opaqueBit[0], opaqueBit[1], opaqueBit[2], any[0], any[1], any[2]);
	return Report(ok, name);
}

static bool
CheckAlphaCappedLevel(void)
{
	const char *name = "a capped alpha-level instance carries the alpha-level bit: its opaque mesh casts and its alpha mesh lets light through";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *ga, *gb;
	Material *m = AlphaMaterial(checkerAlpha);
	Atomic *a = AlphaRoof(&ga, m, m);
	Atomic *b = MixedRoof(&gb);
	metal::setRtAlphaCapForTest(1);
	AlphaFrames(&s, 16, [&](bool prepass){
		AlphaRoofPass(a, prepass, 128);
		AlphaRoofPass(b, prepass, 128);
	});
	float b0 = Floor(-5.5f, 4.5f), b1 = Floor(-4.5f, 3.5f);
	metal::RayStats st = metal::getRayStats();
	bool alphaBit = RoofHit(0.5f, 4.5f, -10.0f, SH_INSTANCE_ALPHA), opaqueBit = RoofHit(0.5f, 4.5f, -10.0f, SH_INSTANCE_OPAQUE);
	metal::setRtAlphaCapForTest(0);
	DestroyAtomic(a);
	DestroyAtomic(b);
	ga->destroy();
	gb->destroy();
	FreeShadowScene(&s);
	bool ok = Dark(b0) && Lit(b1) && alphaBit && !opaqueBit && st.alphaCapped == 1 && st.alphaEntries == 1 && st.alphaKernel == 1;
	if(!ok)
		Detail("  triangle 0 %.3f triangle 1 %.3f, alpha-level bit %d opaque-only bit %d, capped %u entries %u kernel %u\n", b0, b1,
			alphaBit, opaqueBit, st.alphaCapped, st.alphaEntries, st.alphaKernel);
	return Report(ok, name);
}

static bool
CheckAlphaIndexGuard(void)
{
	const char *name = "an alpha caster whose table index lies past the frame's entries lets light through its alpha mesh and keeps its opaque one";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *gb;
	Atomic *b = MixedRoof(&gb);
	metal::setRtAlphaIndexPastForTest(1);
	AlphaFrames(&s, 16, [&](bool prepass){ AlphaRoofPass(b, prepass, 128); });
	metal::setRtAlphaIndexPastForTest(0);
	float b0 = Floor(-5.5f, 4.5f), b1 = Floor(-4.5f, 3.5f);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(b);
	gb->destroy();
	FreeShadowScene(&s);
	bool ok = Dark(b0) && Lit(b1) && st.alphaKernel == 1 && st.alphaEntries == 1;
	if(!ok)
		Detail("  triangle 0 %.3f triangle 1 %.3f, kernel %u entries %u\n", b0, b1, st.alphaKernel, st.alphaEntries);
	return Report(ok, name);
}

static bool
CheckAlphaTextureGone(void)
{
	const char *name = "a frame whose alpha caster texture is gone skips the shadow pass and shows a factor of 1 instead of tracing alpha meshes as solid";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Material *m = AlphaMaterial(checkerAlpha);
	Geometry *g;
	Atomic *roof = AlphaRoof(&g, m, m);
	metal::setRtAlphaTextureLostForTest(1);
	AlphaFrames(&s, 4, [&](bool prepass){ AlphaRoofPass(roof, prepass, 128); });
	metal::setRtAlphaTextureLostForTest(0);
	float v[4];
	FloorQuad(0.0f, v);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(roof);
	g->destroy();
	FreeShadowScene(&s);
	bool ok = v[0] == 1.0f && v[1] == 1.0f && v[2] == 1.0f && v[3] == 1.0f && st.raysPerFrame == 0 && st.alphaKernel == 0 &&
		st.alphaInstances == 1;
	if(!ok)
		Detail("  floor %.3f %.3f %.3f %.3f rays %u kernel %u alpha instances %u\n", v[0], v[1], v[2], v[3], st.raysPerFrame,
			st.alphaKernel, st.alphaInstances);
	return Report(ok, name);
}

static bool
CheckAlphaRefSceneOnly(void)
{
	const char *name = "an off-screen draw of an alpha-tested mesh does not overwrite the reference its scene draw recorded";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	static const uint8 ramp[2] = { 100, 200 };
	Material *m = AlphaMaterial(ramp, 2, 1);
	Geometry *g;
	Atomic *roof = AlphaRoof(&g, m, m);
	Camera *other = Camera::create();
	other->setFrame(Frame::create());
	other->frameBuffer = Raster::create(64, 64, 0, Raster::CAMERATEXTURE);
	other->zBuffer = Raster::create(64, 64, 0, Raster::ZBUFFER);
	AlphaFrames(&s, 16, [&](bool prepass){
		AlphaRoofPass(roof, prepass, 50);
		if(prepass)
			return;
		rtCamera->endUpdate();
		other->beginUpdate();
		AlphaRoofPass(roof, false, 250);
		other->endUpdate();
		rtCamera->beginUpdate();
	});
	float lo = Floor(-4.5f, 0.0f), hi = Floor(-5.5f, 0.0f);
	Frame *of = other->getFrame();
	Raster *fb = other->frameBuffer, *z = other->zBuffer;
	other->destroy();
	of->destroy();
	fb->destroy();
	z->destroy();
	DestroyAtomic(roof);
	g->destroy();
	FreeShadowScene(&s);
	bool ok = Lit(lo) && Dark(hi);
	if(!ok)
		Detail("  alpha 100 %.3f alpha 200 %.3f (scene ref 50, off-screen ref 250)\n", lo, hi);
	return Report(ok, name);
}

static bool
CheckAlphaAddressAxes(void)
{
	const char *name = "an alpha caster with clamp on u and wrap on v samples coordinates beyond 1 per axis";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Material *m = AlphaMaterial(nil);
	Texture *tex = AlphaTexture(2, 2, cornerAlpha, Texture::CLAMP, Texture::WRAP);
	m->setTexture(tex);
	if(tex)
		tex->destroy();
	Geometry *g;
	Atomic *roof = AlphaRoof(&g, m, m, 255, 2.0f);
	AlphaFrames(&s, 16, [&](bool prepass){ AlphaRoofPass(roof, prepass, 128); });
	float uClamped = Floor(-5.25f, 0.75f), vWrapped = Floor(-4.25f, -0.25f);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(roof);
	g->destroy();
	FreeShadowScene(&s);
	bool ok = Lit(uClamped) && Dark(vWrapped) && st.alphaKernel == 1;
	if(!ok)
		Detail("  u 1.25 v 0.25 %.3f (want lit), u 0.25 v 1.25 %.3f (want dark), kernel %u\n", uClamped, vWrapped, st.alphaKernel);
	return Report(ok, name);
}

static Geometry*
ShadowCylinder(float radius, float halfLength, float cz, int sides)
{
	int nv = 2*(sides + 1);
	Geometry *geo = Geometry::create(nv, 2*sides, Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices, *n = geo->morphTargets[0].normals;
	for(int k = 0; k <= sides; k++){
		float a = 6.28318531f*k/sides, c = cosf(a), sn = sinf(a);
		v[2*k].set(radius*c, -halfLength, cz + radius*sn);
		v[2*k+1].set(radius*c, halfLength, cz + radius*sn);
		n[2*k].set(c, 0.0f, sn);
		n[2*k+1].set(c, 0.0f, sn);
	}
	Material *mat = Material::create();
	SurfaceProperties sp = { 1.0f, 0.0f, 1.0f };
	mat->surfaceProps = sp;
	geo->matList.appendMaterial(mat);
	mat->destroy();
	for(int k = 0; k < sides; k++){
		Triangle *t = &geo->triangles[2*k];
		t[0].v[0] = 2*k; t[0].v[1] = 2*k+1; t[0].v[2] = 2*k+2; t[0].matId = 0;
		t[1].v[0] = 2*k+2; t[1].v[1] = 2*k+1; t[1].v[2] = 2*k+3; t[1].matId = 0;
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static void
PrintShadowTerminator(void)
{
	enum { SIDES = 12 };
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *cg = ShadowCylinder(2.0f, 3.0f, -17.0f, SIDES);
	Atomic *cyl = MakeAtomic(cg);
	TurnSun(&s, -100.0f);
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	for(int f = 0; f < 16; f++)
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLNONE);
			s.floor->render();
			cyl->render();
		});
	metal::rtWaitForTest();
	const float deg = 3.14159265f/180.0f, half = 3.14159265f/SIDES;
	float lx = cosf(10.0f*deg), lz = sinf(10.0f*deg);
	printf("  shadow terminator, %d-sided cylinder, sun 10 degrees above the horizon (recorded, not gated):\n", SIDES);
	int findings = 0;
	float lastLit = 2.0f, firstDark = -2.0f;
	for(int k = 0; k < SIDES; k++){
		float am = (2.0f*k + 1.0f)*half;
		if(sinf(am) <= 0.0f)
			continue;
		float a0 = 2.0f*k*half, a1 = 2.0f*(k + 1)*half;
		float nl = cosf(am)*lx + sinf(am)*lz;
		float sx = cosf(a0) + cosf(a1), sz = sinf(a0) + sinf(a1), sl = sqrtf(sx*sx + sz*sz);
		float smooth = (sx*lx + sz*lz)/sl;
		float rc = 2.0f*cosf(half);
		float xc = rc*cosf(am), depth = 17.0f - rc*sinf(am);
		float v = -1.0f, len = 0.0f;
		bool read = metal::readShadowForTest(SX(xc, depth), 240, &v, &len) != 0;
		printf("    facet at %5.1f degrees: n.L %+.3f, smooth normal cosine %+.3f, factor %.3f%s\n", am/deg, nl, smooth, v,
			read ? "" : " (not read)");
		if((nl > 0.05f && v < 0.5f) || (nl < -0.3f && v > 0.5f))
			findings++;
		if(v >= 0.5f && nl < lastLit)
			lastLit = nl;
		if(v < 0.5f && nl > firstDark)
			firstDark = nl;
	}
	printf("    boundary: lit down to n.L %+.3f, dark from n.L %+.3f; %s\n", lastLit, firstDark,
		findings ? "FINDING: a facet reads against its sign" : "every facet with n.L above 0.05 reads lit and every facet below -0.3 reads dark");
	auto across = [&](float a0, float a1, float t, float *smooth, float *v){
		float nx = (1.0f - t)*cosf(a0) + t*cosf(a1), nz = (1.0f - t)*sinf(a0) + t*sinf(a1);
		*smooth = (nx*lx + nz*lz)/sqrtf(nx*nx + nz*nz);
		float len = 0.0f;
		*v = -1.0f;
		return metal::readShadowForTest(SX(2.0f*nx, 17.0f - 2.0f*nz), 240, v, &len) != 0;
	};
	const float e60 = 60.0f*deg, e90 = 90.0f*deg, e120 = 120.0f*deg;
	float litCos, litV, edgeCos, edgeV;
	bool litRead = across(e60, e90, 0.95f, &litCos, &litV), edgeRead = across(e90, e120, 0.05f, &edgeCos, &edgeV);
	printf("    edge between the 75 and 105 degree facets: lit side smooth cosine %+.3f factor %.3f%s, unlit side smooth cosine %+.3f factor %.3f%s\n",
		litCos, litV, litRead ? "" : " (not read)", edgeCos, edgeV, edgeRead ? "" : " (not read)");
	for(int q = 1; q <= 3; q++){
		float c, v;
		bool read = across(e90, e120, q*0.25f, &c, &v);
		printf("    105 degree facet at %d/4: smooth cosine %+.3f, factor %.3f%s\n", q, c, v, read ? "" : " (not read)");
	}
	float stepCos = edgeCos, stepT = edgeV < 0.5f ? 0.0f : -1.0f, step = fmaxf(edgeCos, 0.0f)*(litV - edgeV), prevCos = edgeCos, prevV = edgeV;
	for(int i = 1; i <= 32 && stepT < 0.0f; i++){
		float c, v;
		if(!across(e90, e120, i/32.0f, &c, &v))
			continue;
		if(prevV >= 0.5f && v < 0.5f){
			stepT = i/32.0f;
			stepCos = prevCos;
			step = fmaxf(prevCos, 0.0f)*(prevV - v);
		}
		prevCos = c;
		prevV = v;
	}
	if(stepT == 0.0f)
		printf("    terminator step at the edge: %.3f of the full sun term (smooth cosine %+.3f, clamped at 0, times the factor's drop)\n",
			step, stepCos);
	else if(stepT >= 0.0f)
		printf("    the factor drops %.0f/32 into the 105 degree facet, not at the edge: a step of %.3f of the full sun term (smooth cosine %+.3f, clamped at 0, times the factor's drop)\n",
			stepT*32.0f, step, stepCos);
	else
		printf("    the factor does not drop on the 105 degree facet\n");
	DestroyAtomic(cyl);
	cg->destroy();
	FreeShadowScene(&s);
}

struct TimingGrid
{
	float32 ms[2][3], shadowed[2][3], ground, megabytes, walkMs;
	bool measured;
};

static void
SettleRetired(void)
{
	CasterTiersOn();
	for(int f = 0; f < 2; f++){
		CasterFrame([](bool){});
		metal::rtWaitForTest();
	}
	CasterTiersOff();
}

static void
ShadowTimingGrid(Texture *tex, int32 alphaKernel, TimingGrid *t, bool mixed = false)
{
	SettleRetired();
	metal::RayStats before = metal::getRayStats();
	std::vector<Geometry*> strips;
	for(int q = 1; q <= 200; q++)
		strips.push_back(StripGeometry(q, 0.0f, !mixed || q % 10 == 0 ? tex : nil));
	std::vector<Atomic*> grid;
	for(int i = 0; i < (mixed ? 8191 : 8192); i++){
		Atomic *a = MakeAtomic(strips[i % 200]);
		Place(a, (float)(i % 91)*20.0f, (float)(i / 91)*20.0f, 10.0f, 0.0f);
		grid.push_back(a);
	}
	Geometry *coverGeo = nil;
	if(mixed){
		RGBA c = { 200, 200, 200, 255 };
		RGBA col[4] = { c, c, c, c };
		Material *mat = Material::create();
		coverGeo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT, 0.0f, -200.0f, -1000.0f, 2000.0f, 0.0f, col, nil, mat);
		mat->destroy();
		Atomic *cover = MakeAtomic(coverGeo);
		Place(cover, 910.0f, 0.0f, 30.0f, 0.0f);
		grid.push_back(cover);
	}
	CasterTiersOn();
	auto all = [&](bool prepass){
		if(prepass)
			for(Atomic *a : grid)
				metal::addShadowCaster(a, metal::SHADOWCASTER_STATIC);
	};
	metal::setRtBuildBudgetForTest(400000);
	for(int f = 0; f < 8; f++){
		CasterFrame(all);
		if(metal::rtQueuedForTest() == 0)
			break;
	}
	metal::setRtBuildBudgetForTest(0);
	CasterFrame(all);
	metal::rtWaitForTest();
	metal::RayStats after = metal::getRayStats();
	t->megabytes = after.bottomMegabytes - before.bottomMegabytes;
	t->walkMs = after.walkMs;
	const float32 eye[3] = { 910.0f, -150.0f, 120.0f }, target[3] = { 910.0f, 500.0f, 0.0f };
	float32 sun[3] = { 0.5f, 0.3f, 0.81f };
	float32 l = sqrtf(sun[0]*sun[0] + sun[1]*sun[1] + sun[2]*sun[2]);
	for(int i = 0; i < 3; i++)
		sun[i] /= l;
	const uint32 sizes[2][2] = { { 2560, 1440 }, { 3840, 2160 } };
	t->measured = true;
	metal::setShadowAlphaKernelForTest(alphaKernel);
	for(int r = 0; r < 2; r++)
		for(int q = 0; q < 3; q++)
			t->measured = t->measured && metal::rtShadowTimingForTest(sizes[r][0], sizes[r][1], metal::SHADOWQUALITY_LOW + q, eye,
				target, sun, &t->ms[r][q], &t->ground, &t->shadowed[r][q]);
	metal::setShadowAlphaKernelForTest(-1);
	CasterTiersOff();
	for(Atomic *a : grid)
		DestroyAtomic(a);
	for(Geometry *g : strips)
		g->destroy();
	if(coverGeo)
		coverGeo->destroy();
	SettleRetired();
}

static void
PrintShadowTiming(void)
{
	static const uint8 stripes[64] = {
		0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 255, 0, 255, 0, 255, 0,
		0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 255, 0, 255, 0, 255, 0,
		0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 255, 0, 255, 0, 255, 0,
		0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 255, 0, 255, 0, 255, 0 };
	TimingGrid g[4];
	ShadowTimingGrid(nil, -1, &g[0]);
	ShadowTimingGrid(nil, 1, &g[1]);
	Texture *tex = AlphaTexture(8, 8, stripes);
	ShadowTimingGrid(tex, -1, &g[2]);
	ShadowTimingGrid(tex, -1, &g[3], true);
	if(tex)
		tex->destroy();
	if(!g[0].measured || !g[1].measured || !g[2].measured || !g[3].measured){
		printf("  shadow ray timing: not measured (no stage-boundary counter samples)\n");
		return;
	}
	const uint32 sizes[2][2] = { { 2560, 1440 }, { 3840, 2160 } };
	const char *rows[4] = { "opaque grid", "opaque grid, alpha kernel", "alpha grid", "mixed grid" };
	printf("  shadow ray timing (median of 5, recorded not gated), ground %.0f%%:\n", g[0].ground*100.0f);
	for(int k = 0; k < 4; k++)
		for(int r = 0; r < 2; r++){
			printf("    %s %ux%u low %.3f ms medium %.3f ms high %.3f ms", rows[k], sizes[r][0], sizes[r][1], g[k].ms[r][0],
				g[k].ms[r][1], g[k].ms[r][2]);
			if(k > 0)
				printf(" (%+.0f%% %+.0f%% %+.0f%%)", (g[k].ms[r][0]/g[0].ms[r][0] - 1.0f)*100.0f,
					(g[k].ms[r][1]/g[0].ms[r][1] - 1.0f)*100.0f, (g[k].ms[r][2]/g[0].ms[r][2] - 1.0f)*100.0f);
			printf(" shadowed %.0f%%\n", g[k].shadowed[r][1]*100.0f);
		}
	printf("    bottom levels: opaque grid %.2f MB, alpha grid %.2f MB; alpha grid walk %.2f ms\n", g[0].megabytes, g[2].megabytes,
		g[2].walkMs);
	printf("    D1: medium at 2560x1440 is %.3f ms: %s\n", g[0].ms[0][1], g[0].ms[0][1] > 1.5f ? "half-resolution medium" : "keep the table");
}

int
RunRaytraceChecks(Camera *camera)
{
	failures = 0;
	rtCamera = camera;
	WorldOpen(camera);
	if(!metal::rtAvailable()){
		Report(false, "the device supports ray tracing");
		WorldClose();
		return failures;
	}
	CheckRealFramesDrain();
	metal::rtResetForTest();
	CheckBottomBuilds();
	CheckBottomLifetime();
	CheckBuildBudget();
	CheckRoofOccludes();
	CheckSkinnedFollows();
	CheckInstanceCap();
	CheckRayWorkPlacement();
	CheckCasterSources();
	CheckUploadedVertices();
	CheckSkinnedCap();
	CheckForgottenInFlight();
	CheckSlotsBeforePresent();
	CheckLevelPerScene();
	CheckSkinnedScratchFail();
	CheckBoneClamp();
	CheckDrawnCasterMoves();
	CheckHostCasterOutside();
	CheckDestroyedInBracket();
	CheckShadowOnScreen();
	CheckShadowAccumulates();
	CheckShadowHistoryCameraMotion();
	CheckShadowHistoryPan();
	CheckShadowHistoryPlaneNormal();
	CheckShadowHistoryRestarts();
	CheckShadowSkipped();
	CheckShadowSunOnly();
	CheckShadowStats();
	CheckAlphaCheckerboard();
	CheckAlphaThreshold();
	CheckAlphaBlendedCastNothing();
	CheckAlphaGeometries();
	CheckAlphaKernelChoice();
	CheckAlphaTextureLifetime();
	CheckAlphaCap();
	CheckAlphaUnderOpaque();
	CheckAlphaMaskBit();
	CheckAlphaMaskCarriers();
	CheckAlphaCappedLevel();
	CheckAlphaIndexGuard();
	CheckAlphaTextureGone();
	CheckAlphaRefSceneOnly();
	CheckAlphaAddressAxes();
	PrintShadowTerminator();
	PrintBuildTiming();
	PrintCasterTiming();
	PrintShadowTiming();
	WorldClose();
	return failures;
}
