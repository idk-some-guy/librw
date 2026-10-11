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
	const char *name = "at most 48 skinned casters are kept, the nearest, and none beyond 100 m";
	Geometry *geo = SkinnedQuadGeometry(0.5f);
	std::vector<Atomic*> as;
	CasterTiersOn();
	float farA[2] = { 105.0f, 102.0f };
	for(int i = 0; i < 2; i++)
		as.push_back(SkinnedAt(geo, 0.0f, 0.0f, farA[i]));
	for(int i = 45; i >= 0; i--)
		as.push_back(SkinnedAt(geo, 0.0f, 0.0f, 6.0f + 2.0f*i));
	CasterFrame([&](bool){ for(Atomic *a : as) a->render(); });
	metal::RayStats sa = metal::getRayStats();
	bool beyond = metal::rtSkinnedForTest(as[0]) || metal::rtSkinnedForTest(as[1]);
	for(Atomic *a : as)
		DestroySkinAtomic(a);
	as.clear();
	for(int i = 49; i >= 0; i--)
		as.push_back(SkinnedAt(geo, 0.0f, 0.0f, 6.0f + 2.0f*i));
	CasterFrame([&](bool){ for(Atomic *a : as) a->render(); });
	metal::RayStats sb = metal::getRayStats();
	bool farthest = metal::rtSkinnedForTest(as[0]) || metal::rtSkinnedForTest(as[1]);
	bool nearest = metal::rtSkinnedForTest(as[49]) && metal::rtSkinnedForTest(as[2]);
	CasterTiersOff();
	for(Atomic *a : as)
		DestroySkinAtomic(a);
	geo->destroy();
	bool ok = sa.skinnedCasters == 46 && !beyond && sb.skinnedCasters == 48 && !farthest && nearest;
	if(!ok)
		Detail("  case A kept %u beyond 100 m %d; case B kept %u farthest %d nearest %d\n", sa.skinnedCasters, beyond,
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
CheckLevelRequested(void)
{
	const char *name = "requestShadowLevel builds an atomic's level and keeps it in use without making it a caster, and does nothing outside the prepass bracket";
	Geometry *g, *go;
	Atomic *roof = Roof(&g, 2.0f, 2.0f), *outside = Roof(&go, 2.0f, 2.0f);
	Place(roof, 0.0f, 0.0f, 30.0f, 0.0f);
	Place(outside, 20.0f, 0.0f, 30.0f, 0.0f);
	CasterTiersOn();
	CasterFrame([&](bool prepass){
		if(prepass)
			metal::requestShadowLevel(roof);
	});
	metal::RayStats st = metal::getRayStats();
	metal::InstanceDataHeader *h = (metal::InstanceDataHeader*)g->instData;
	bool built = h && Built(h), hit = Hit(0.0f, 0.0f, 40.0f);
	uint64 used1 = h ? metal::rtLastUsedForTest(h) : 0, frame1 = metal::rtFrame();
	CasterFrame([](bool){});
	uint64 used2 = h ? metal::rtLastUsedForTest(h) : 0;
	CasterFrame([&](bool prepass){
		if(prepass)
			metal::requestShadowLevel(roof);
	});
	uint64 used3 = h ? metal::rtLastUsedForTest(h) : 0, frame3 = metal::rtFrame();
	metal::requestShadowLevel(outside);
	bool outsideInstanced = go->instData != nil;
	CasterFrame([](bool){});
	CasterTiersOff();
	DestroyAtomic(roof);
	DestroyAtomic(outside);
	g->destroy();
	go->destroy();
	bool ok = built && !hit && st.casters == 0 && st.instances == 0 && used1 == frame1 && used2 == frame1 &&
		used3 == frame3 && frame3 > frame1 && !outsideInstanced;
	if(!ok)
		Detail("  built %d hit %d casters %u instances %u; last used %llu %llu %llu at frames %llu %llu; outside instanced %d\n",
			built, hit, st.casters, st.instances, (unsigned long long)used1, (unsigned long long)used2,
			(unsigned long long)used3, (unsigned long long)frame1, (unsigned long long)frame3, outsideInstanced);
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
	std::vector<Atomic*> crowd(peds);
	for(int i = 24; i < 48; i++){
		Atomic *a = SkinAtomic(geo);
		Place(a, 1.0f, 0.0f, 10.0f + 1.25f*(i - 24), 0.0f);
		PoseSkin(a, 4);
		peds.push_back(a);
		crowd.push_back(a);
	}
	for(int i = 0; i < 24; i++)
		Place(crowd[i], -1.0f, 0.0f, 10.0f + 1.25f*i, 0.0f);
	metal::setRtSkinnedLimitForTest(48, 100.0f);
	float32 skin48 = 0.0f, refit48 = 0.0f;
	SkinnedTiming(crowd, metal::RT_SKIN_REFIT, &skin48, &refit48);
	uint32 kept48 = metal::getRayStats().skinnedCasters;
	metal::setRtSkinnedLimitForTest(0, 0.0f);
	metal::setRtSkinModeForTest(metal::RT_SKIN_DEFAULT);
	printf("    48 skinned casters of 1500 triangles, refit: skinning %.3f ms, refits %.3f ms, total %.3f ms (R5 limit 0.3 ms, %u kept)\n",
		skin48, refit48, skin48 + refit48, kept48);
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
ShadowExpect(int ch, float prelit, float ambient, float w, V3d n, const ShadowScene *s, float shadow, float strength)
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
	return relitShadowedPrelit(sp.full, sp.sun, classic, shadow, relitFacedShadow(shadow, c, 1.0f), strength);
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

static Geometry*
SkinnedArmGeometry(void)
{
	Geometry *geo = Geometry::create(8, 4, Geometry::POSITIONS | Geometry::TEXTURED | Geometry::NORMALS | Geometry::PRELIT);
	V3d *v = geo->morphTargets[0].vertices;
	float y0[2] = { -1.0f, 2.0f }, y1[2] = { 1.0f, 3.0f };
	uint8 idx[8][4] = {};
	float wts[8][4] = {};
	Material *mat = Material::create();
	geo->matList.appendMaterial(mat);
	mat->destroy();
	for(int q = 0; q < 2; q++){
		v[4*q].set(-1.0f, y1[q], 0.0f);
		v[4*q+1].set(1.0f, y1[q], 0.0f);
		v[4*q+2].set(1.0f, y0[q], 0.0f);
		v[4*q+3].set(-1.0f, y0[q], 0.0f);
		for(int i = 4*q; i < 4*q+4; i++){
			geo->morphTargets[0].normals[i].set(0.0f, 0.0f, -1.0f);
			geo->colors[i] = { 100, 100, 100, 255 };
			geo->texCoords[0][i] = { 0.0f, 0.0f };
			idx[i][0] = (uint8)q;
			wts[i][0] = 1.0f;
		}
		Triangle *t = &geo->triangles[2*q];
		t[0].v[0] = 4*q; t[0].v[1] = 4*q+1; t[0].v[2] = 4*q+2; t[0].matId = 0;
		t[1].v[0] = 4*q; t[1].v[1] = 4*q+2; t[1].v[2] = 4*q+3; t[1].matId = 0;
	}
	geo->calculateBoundingSphere();
	geo->unlock();
	Matrix ident[2];
	ident[0].setIdentity();
	ident[1].setIdentity();
	AttachSkin(geo, 2, ident, idx, wts);
	return geo;
}

static void
SkinnedCasterFrames(ShadowScene *s, Atomic *a, int n)
{
	for(int i = 0; i < n; i++)
		CasterFrame([&](bool prepass){
			SetRenderState(CULLMODE, CULLNONE);
			s->floor->render();
			if(prepass)
				metal::addSkinnedShadowCaster(a);
		});
	metal::rtWaitForTest();
}

static bool
CheckSkinnedCasterUndrawn(void)
{
	const char *name = "a skinned atomic added without a draw casts its posed shadow, and does nothing outside the casters bracket";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	Geometry *geo = SkinnedArmGeometry();
	Atomic *a = SkinAtomic(geo);
	Place(a, 14.0f, 0.0f, -4.0f, 0.0f);
	PoseSkin(a, 2);
	V3d up = { 0.0f, 0.0f, 3.0f };
	Skin::getHierarchy(a)->matrices[1].translate(&up, COMBINEPOSTCONCAT);
	SkinnedCasterFrames(&s, a, 24);
	metal::RayStats posedStats = metal::getRayStats();
	float raised = -1, bodyPosed = -1, armRest = -1, len = 0;
	bool read = metal::readShadowForTest(SX(-5.0f, 20.0f), SY(2.5f, 20.0f), &raised, &len) &&
		metal::readShadowForTest(SX(-2.0f, 20.0f), SY(0.0f, 20.0f), &bodyPosed, &len);
	PoseSkin(a, 2);
	SkinnedCasterFrames(&s, a, 24);
	float raisedBind = -1, bodyBind = -1;
	read = read && metal::readShadowForTest(SX(-5.0f, 20.0f), SY(2.5f, 20.0f), &raisedBind, &len) &&
		metal::readShadowForTest(SX(-2.0f, 20.0f), SY(0.0f, 20.0f), &bodyBind, &len) &&
		metal::readShadowForTest(SX(-2.0f, 20.0f), SY(2.5f, 20.0f), &armRest, &len);

	Geometry *geoOut = SkinnedArmGeometry();
	Atomic *out = SkinAtomic(geoOut);
	Place(out, 14.0f, 0.0f, -4.0f, 0.0f);
	PoseSkin(out, 2);
	metal::addSkinnedShadowCaster(out);
	bool outInstanced = geoOut->instData != nil;
	CasterFrame([&](bool){
		SetRenderState(CULLMODE, CULLNONE);
		s.floor->render();
	});
	metal::rtWaitForTest();
	metal::RayStats outStats = metal::getRayStats();
	DestroySkinAtomic(out);
	geoOut->destroy();
	DestroySkinAtomic(a);
	geo->destroy();
	FreeShadowScene(&s);
	bool ok = read && raised < 0.3f && bodyPosed < 0.3f && raisedBind > 0.9f && bodyBind < 0.3f && armRest < 0.3f &&
		posedStats.skinnedCasters == 1 && !outInstanced && outStats.skinnedCasters == 0;
	if(!ok)
		Detail("  read %d; posed: under the raised arm %.3f body %.3f skinned %u; bind pose: there %.3f body %.3f arm %.3f; "
			"outside the bracket instanced %d skinned %u\n", read, raised, bodyPosed, posedStats.skinnedCasters, raisedBind,
			bodyBind, armRest, outInstanced, outStats.skinnedCasters);
	return Report(ok, name);
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

static bool SkipStrengthIdentity(ShadowScene *s);

static bool
CheckShadowSkipped(void)
{
	const char *name = "with the sun below the horizon, a sun scale of 0 or ShadowQuality off no ray is traced and the factor is 1; the relit draws keep reading the factor while ShadowQuality is on; the pass adds no render pass; at a shadow strength of 0.6 a skipped pass (sun down, sun scale 0, an interior at weight 1) draws exactly the frame without the strength";
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
	bool strength = SkipStrengthIdentity(&s);
	FreeShadowScene(&s);
	bool ok = st[0].rays > 0 && st[0].passes > 0 && strength;
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

struct OffState
{
	uint64 adds, palettes, heap;
	uint32 levels, skinned, retired;
	float megabytes;
	bool hit, skinHit;
};

static void
OffMeasure(OffState *o)
{
	metal::RayStats st = metal::getRayStats();
	o->adds = metal::rtCasterAddsForTest();
	o->palettes = metal::rtPaletteCopiesForTest();
	o->heap = metal::rtHeapBytesForTest();
	o->levels = st.bottomLevels;
	o->skinned = st.skinnedLevels;
	o->retired = metal::rtRetiredForTest();
	o->megabytes = st.bottomMegabytes + st.skinnedMegabytes + st.topMegabytes;
	o->hit = Hit(0.0f, 0.0f, 40.0f);
	o->skinHit = Hit(0.0f, 10.0f, 25.0f, metal::SHADOWCASTER_SKINNED);
}

static bool
CheckShadowOffReleases(void)
{
	const char *name = "a frame whose shadow pass will not run collects no caster and copies no bone palette; ShadowQuality off or leaving the modern tier releases the levels and heaps after one completed frame, and switching back rebuilds and traces";
	Geometry *g, *gs = SkinnedQuadGeometry(1.0f);
	Atomic *roof = Roof(&g, 2.0f, 2.0f), *skin = SkinnedAt(gs, 0.0f, 10.0f, 20.0f);
	Place(roof, 0.0f, 0.0f, 30.0f, 0.0f);
	auto draw = [&](bool prepass){
		roof->render();
		skin->render();
		if(prepass)
			metal::addShadowCaster(roof, metal::SHADOWCASTER_STATIC);
	};
	auto settle = [&](OffState *before, OffState *after){
		CasterFrame(draw);
		metal::rtWaitForTest();
		OffMeasure(before);
		CasterFrame(draw);
		metal::rtWaitForTest();
		OffMeasure(after);
	};
	CasterTiersOn();
	OffState on0, on, night0, night, off0, off, back0, back, enhanced0, enhanced, modern0, modern;
	settle(&on0, &on);

	metal::RelitParams p = metal::getRelitParams();
	p.sunScale = 0.0f;
	metal::setRelitParams(&p);
	settle(&night0, &night);
	p.sunScale = 1.0f;
	metal::setRelitParams(&p);

	SetShadowQuality(metal::SHADOWQUALITY_OFF);
	settle(&off0, &off);

	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	settle(&back0, &back);

	metal::RenderSettings s = metal::getRenderSettings();
	s.tier = metal::TIER_ENHANCED;
	metal::setRenderSettings(&s);
	settle(&enhanced0, &enhanced);

	s.tier = metal::TIER_MODERN;
	metal::setRenderSettings(&s);
	settle(&modern0, &modern);
	CasterTiersOff();
	DestroySkinAtomic(skin);
	gs->destroy();
	DestroyAtomic(roof);
	g->destroy();

	bool collected = on.adds > on0.adds && on.palettes > on0.palettes && on.hit && on.skinHit && on.levels > 0 && on.skinned > 0 &&
		on.megabytes > 0.0f && on.heap > 0;
	bool nightOk = night.adds == night0.adds && night.palettes == night0.palettes && night.levels == on.levels &&
		night.skinned == on.skinned && night.heap == on.heap && !night.hit;
	auto released = [&](const OffState &o, const OffState &before){
		return o.adds == before.adds && o.palettes == before.palettes && o.levels == 0 && o.skinned == 0 &&
			o.megabytes == 0.0f && o.heap == 0 && o.retired == 0 && !o.hit && !o.skinHit;
	};
	auto rebuilt = [](const OffState &o){
		return o.hit && o.skinHit && o.levels > 0 && o.skinned > 0 && o.heap > 0;
	};
	bool ok = collected && nightOk && released(off, off0) && rebuilt(back) && released(enhanced, enhanced0) && rebuilt(modern);
	if(!ok){
		const OffState *all[12] = { &on0, &on, &night0, &night, &off0, &off, &back0, &back, &enhanced0, &enhanced, &modern0, &modern };
		const char *names[12] = { "on0", "on", "night0", "night", "off0", "off", "back0", "back", "enhanced0", "enhanced", "modern0",
			"modern" };
		for(int i = 0; i < 12; i++)
			Detail("  %s: adds %llu palettes %llu levels %u skinned %u retired %u %.2f MB heaps %llu hit %d skinned hit %d\n",
				names[i], (unsigned long long)all[i]->adds, (unsigned long long)all[i]->palettes, all[i]->levels,
				all[i]->skinned, all[i]->retired, all[i]->megabytes, (unsigned long long)all[i]->heap, all[i]->hit,
				all[i]->skinHit);
	}
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
	float shade0 = ShadowExpect(0, 120/255.0f, 0.1f, 0.5f, n, &s, 0.0f, 0.0f);
	float open1 = ShadowExpect(0, 120/255.0f, 0.1f, 0.5f, n, &s, 1.0f, 0.0f);
	FreeShadowScene(&s);
	bool ok = read && fabsf(shade[0] - shade0) < 2.0f/255.0f && fabsf(open[0] - open1) < 2.0f/255.0f && open1 - shade0 > 0.05f;
	if(!ok)
		Detail("  read %d shaded red %.4f (want %.4f) open red %.4f (want %.4f)\n", read, shade[0], shade0, open[0], open1);
	return Report(ok, name);
}

static void
SetShadowStrength(float strength)
{
	metal::RelitParams p = metal::getRelitParams();
	p.shadowStrength = strength;
	metal::setRelitParams(&p);
}

static bool
ShadowScenePixels(ShadowScene *s, bool keptBaked, float shade[4], float open[4])
{
	bool read = false;
	CasterFrame([&](bool prepass){
		SetRenderState(CULLMODE, CULLNONE);
		if(keptBaked && !prepass)
			metal::setRelitDraw(0);
		s->floor->render();
		metal::setRelitDraw(1);
		s->roof->render();
		if(!prepass)
			read = metal::readScenePixel(SX(-5.0f, 20.0f), 240, shade) && metal::readScenePixel(SX(6.5f, 20.0f), 240, open);
	});
	metal::rtWaitForTest();
	return read;
}

static bool
CheckPlainShadowTwin(void)
{
	const char *name = "at weight 1 a prelit floor in shadow is darkened by the shadow strength through the plain variant's shadow twin, and the open floor equals the frame without the strength";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetShadowStrength(0.6f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
	float shadeA[4], openA[4], shadeB[4], openB[4];
	metal::StateStats a0 = metal::getStateStats();
	bool read = ShadowScenePixels(&s, false, shadeA, openA);
	metal::StateStats a1 = metal::getStateStats();
	SetShadowStrength(0.0f);
	read = ShadowScenePixels(&s, false, shadeB, openB) && read;
	FreeShadowScene(&s);
	uint32 plainDraws = a1.plainShadowDraws - a0.plainShadowDraws, relitDraws = a1.relitShadowDraws - a0.relitShadowDraws;
	float plain = fminf(120/255.0f + 0.1f, 1.0f), dark = relitPlainShadowed(plain, 0.0f, 0.6f);
	bool same = true;
	for(int i = 0; i < 4; i++)
		same = same && openA[i] == openB[i];
	bool ok = read && fabsf(shadeA[0] - dark) < 2.0f/255.0f && same && fabsf(shadeB[0] - plain) < 2.0f/255.0f && plainDraws >= 1 &&
		relitDraws == 0;
	if(!ok)
		Detail("  read %d; strength 0.6: shaded red %.4f (want %.4f) open %.4f; strength 0: shaded red %.4f (want %.4f) open %.4f; open equal %d; plain shadow draws %u relit shadow draws %u\n",
			read, shadeA[0], dark, openA[0], shadeB[0], plain, openB[0], same, plainDraws, relitDraws);
	return Report(ok, name);
}

static Geometry*
CornerBoxGeometry(Material *mat, float x0, float y0, float z0, float x1, float y1, float z1)
{
	Geometry *geo = Geometry::create(8, 12, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i < 8; i++){
		v[i].set(i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0);
		geo->colors[i] = { 120, 120, 120, 255 };
	}
	static const int f[12][3] = { { 0, 1, 3 }, { 0, 3, 2 }, { 4, 6, 7 }, { 4, 7, 5 }, { 0, 4, 5 }, { 0, 5, 1 }, { 2, 3, 7 }, { 2, 7, 6 },
		{ 0, 2, 6 }, { 0, 6, 4 }, { 1, 5, 7 }, { 1, 7, 3 } };
	for(int i = 0; i < 12; i++){
		geo->triangles[i].v[0] = f[i][0];
		geo->triangles[i].v[1] = f[i][1];
		geo->triangles[i].v[2] = f[i][2];
		geo->triangles[i].matId = 0;
	}
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
CheckPrelitClassicShadow(void)
{
	const char *name = "below weight 1 a prelit floor in shadow loses its sun share and the shadow strength's share of its classic part";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetShadowStrength(0.6f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
	float shade[4] = { -1, -1, -1, -1 }, open[4] = { -1, -1, -1, -1 };
	bool read = ShadowScenePixels(&s, false, shade, open);
	V3d n = { 0.0f, 0.0f, 1.0f };
	float shade0 = ShadowExpect(0, 120/255.0f, 0.1f, 0.5f, n, &s, 0.0f, 0.6f);
	float open1 = ShadowExpect(0, 120/255.0f, 0.1f, 0.5f, n, &s, 1.0f, 0.6f);
	float sunOnly = ShadowExpect(0, 120/255.0f, 0.1f, 0.5f, n, &s, 0.0f, 0.0f);
	Geometry *slabGeo = CornerBoxGeometry(s.mat, -6.0f, -1.0f, -20.0f, -4.0f, 1.0f, -19.9f);
	Atomic *slab = MakeAtomic(slabGeo);
	metal::setFaceFetchForTest(0);
	float flat[4] = { -1, -1, -1, -1 };
	bool readFlat = false;
	for(int i = 0; i < 17; i++)
		CasterFrame([&](bool prepass){
			SetRenderState(CULLMODE, CULLNONE);
			s.floor->render();
			s.roof->render();
			slab->render();
			if(!prepass && i == 16)
				readFlat = metal::readScenePixel(SX(-5.0f, 19.9f), 240, flat);
		});
	metal::rtWaitForTest();
	metal::setFaceFetchForTest(1);
	DestroyAtomic(slab);
	slabGeo->destroy();
	FreeShadowScene(&s);
	float flatWant = relitPlainShadowed(fminf(120/255.0f + 0.1f, 1.0f), 0.0f, 0.6f);
	bool ok = read && fabsf(shade[0] - shade0) < 2.0f/255.0f && fabsf(open[0] - open1) < 2.0f/255.0f && sunOnly - shade[0] > 0.05f &&
		readFlat && fabsf(flat[0] - flatWant) < 2.0f/255.0f;
	if(!ok)
		Detail("  read %d shaded red %.4f (want %.4f, %.4f without the strength) open red %.4f (want %.4f); without a face normal %d red %.4f (want %.4f)\n",
			read, shade[0], shade0, sunOnly, open[0], open1, readFlat, flat[0], flatWant);
	return Report(ok, name);
}

static bool
CheckKeptBakedShadow(void)
{
	const char *name = "a prelit draw the host keeps baked is darkened by the shadow strength in shadow, and with the strength 0 or the shadows off it draws the enhanced frame's colour";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetShadowStrength(0.6f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 16);
	float shade[4][4], open[4][4];
	metal::StateStats a0 = metal::getStateStats();
	bool read = ShadowScenePixels(&s, true, shade[0], open[0]);
	metal::StateStats a1 = metal::getStateStats();
	SetShadowStrength(0.0f);
	read = ShadowScenePixels(&s, true, shade[1], open[1]) && read;
	SetShadowStrength(0.6f);
	ShadowFrame(&s, metal::SHADOWQUALITY_OFF, 2);
	read = ShadowScenePixels(&s, true, shade[2], open[2]) && read;
	metal::RenderSettings r = metal::getRenderSettings();
	r.tier = metal::TIER_ENHANCED;
	metal::setRenderSettings(&r);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 2);
	read = ShadowScenePixels(&s, true, shade[3], open[3]) && read;
	FreeShadowScene(&s);
	uint32 plainDraws = a1.plainShadowDraws - a0.plainShadowDraws;
	float plain = fminf(120/255.0f + 0.1f, 1.0f), dark = relitPlainShadowed(plain, 0.0f, 0.6f);
	bool same = true;
	for(int k = 1; k < 3; k++)
		for(int i = 0; i < 4; i++)
			same = same && shade[k][i] == shade[3][i] && open[k][i] == open[3][i];
	bool ok = read && fabsf(shade[0][0] - dark) < 2.0f/255.0f && fabsf(open[0][0] - plain) < 2.0f/255.0f && same && plainDraws >= 1;
	if(!ok)
		Detail("  read %d; strength 0.6: shaded red %.4f (want %.4f) open %.4f (want %.4f); strength 0: %.4f %.4f; shadows off: %.4f %.4f; enhanced: %.4f %.4f; equal %d; plain shadow draws %u\n",
			read, shade[0][0], dark, open[0][0], plain, shade[1][0], open[1][0], shade[2][0], open[2][0], shade[3][0], open[3][0], same,
			plainDraws);
	return Report(ok, name);
}

static Geometry*
BoxGeometry(Material *mat, float x0, float y0, float z0, float x1, float y1, float z1)
{
	Geometry *geo = Geometry::create(24, 12, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices;
	const float c[8][3] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 },
		{ x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } };
	const int faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
	for(int f = 0; f < 6; f++){
		for(int k = 0; k < 4; k++){
			const float *p = c[faces[f][k]];
			v[f*4 + k].set(p[0], p[1], p[2]);
			geo->colors[f*4 + k] = { 120, 120, 120, 255 };
		}
		Triangle *t = &geo->triangles[2*f];
		t[0].v[0] = f*4; t[0].v[1] = f*4 + 1; t[0].v[2] = f*4 + 2; t[0].matId = 0;
		t[1].v[0] = f*4; t[1].v[1] = f*4 + 2; t[1].v[2] = f*4 + 3; t[1].matId = 0;
	}
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static void
SetSunRadius(float radius)
{
	metal::RelitParams p = metal::getRelitParams();
	p.sunRadius = radius;
	metal::setRelitParams(&p);
}

static bool
CheckShadowContact(void)
{
	const char *name = "a kerb 0.15 m tall seen from 60 m casts a strip that starts at its base: the origin moves along the surface normal, not toward the camera";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	V2d savedWindow = rtCamera->viewWindow;
	float half = tanf(2.0f*3.14159265f/180.0f);
	V2d window = { half, half*0.75f };
	rtCamera->setViewWindow(&window);
	Geometry *floorGeo = ShadowQuad(s.mat, 8.0f, -8.0f, -8.0f, 8.0f, -60.0f);
	Geometry *kerbGeo = BoxGeometry(s.mat, -0.25f, -2.0f, -60.0f, 0.25f, 2.0f, -59.85f);
	Atomic *floor = MakeAtomic(floorGeo), *kerb = MakeAtomic(kerbGeo);
	SetSunRadius(0.0f);
	TurnSun(&s, -110.0f);
	V3d at = s.sunFrame->getLTM()->at;
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	for(int i = 0; i < 16; i++)
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLNONE);
			floor->render();
			kerb->render();
		});
	metal::rtWaitForTest();
	float W = 60.0f*half, pixel = 2.0f*W/640.0f;
	int c0 = (int)ceilf(320.0f*(1.0f + 0.25f/W) - 0.5f), dark = 0;
	float first = -1.0f;
	for(int c = c0; c < 640; c++){
		float v = -1.0f, len = 0.0f;
		if(!metal::readShadowForTest(c, 240, &v, &len) || v >= 0.5f){
			first = v;
			break;
		}
		dark++;
	}
	float strip = dark*pixel;
	DestroyAtomic(floor);
	DestroyAtomic(kerb);
	floorGeo->destroy();
	kerbGeo->destroy();
	rtCamera->setViewWindow(&savedWindow);
	FreeShadowScene(&s);
	bool ok = strip >= 0.33f;
	Detail("  toward the sun (%.3f, %.3f, %.3f); strip %.3f m (%d pixels of %.5f m from column %d, then %.3f)\n", -at.x, -at.y, -at.z,
		strip, dark, pixel, c0, first);
	return Report(ok, name);
}

static bool
CheckShadowUpsampleEdge(void)
{
	const char *name = "at half resolution the upsample keeps a lit roof's edge lit over the shadowed floor beside it in every ray phase, and in four frames each pixel beside the edge takes its own ray";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetSunRadius(0.0f);
	TurnSun(&s, 160.0f);
	int roofCol = (int)ceilf(320.0f*(1.0f - 1.0f/15.0f));
	ShadowFrame(&s, metal::SHADOWQUALITY_LOW, 4);
	float v[4][2][4];
	int own[2][4] = {};
	bool read = true, edge = true;
	for(int f = 0; f < 4; f++){
		metal::cameraCut();
		ShadowFrame(&s, metal::SHADOWQUALITY_LOW, 1);
		for(int r = 0; r < 2; r++)
			for(int c = 0; c < 4; c++){
				float len = 0.0f;
				v[f][r][c] = -1.0f;
				read = metal::readShadowForTest(roofCol - 2 + c, 240 + r, &v[f][r][c], &len) && read;
				edge = edge && (c >= 2 ? v[f][r][c] >= 0.95f : v[f][r][c] >= 0.0f && v[f][r][c] <= 0.05f);
				own[r][c] += v[f][r][c] == (c >= 2 ? 1.0f : 0.0f);
			}
	}
	FreeShadowScene(&s);
	bool once = true;
	for(int r = 0; r < 2; r++)
		for(int c = 0; c < 4; c++)
			once = once && own[r][c] >= 1;
	bool ok = read && edge && once;
	if(!ok)
		for(int f = 0; f < 4; f++)
			Detail("  frame %d, columns %d to %d: row 240 %.4f %.4f %.4f %.4f, row 241 %.4f %.4f %.4f %.4f\n", f, roofCol - 2, roofCol + 1,
				v[f][0][0], v[f][0][1], v[f][0][2], v[f][0][3], v[f][1][0], v[f][1][1], v[f][1][2], v[f][1][3]);
	return Report(ok, name);
}

static bool
CheckShadowPhaseStable(void)
{
	const char *name = "a static scene at medium is stable once converged: across the roof's edge and the far edge of its shadow the factor changes by at most 0.05 from one frame to the next as the ray phase turns";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetSunRadius(0.0f);
	TurnSun(&s, 160.0f);
	V3d at = s.sunFrame->getLTM()->at;
	int cols[2] = { (int)ceilf(320.0f*(1.0f - 1.0f/15.0f)), (int)floorf(320.0f*(1.0f - (1.0f - 5.0f*at.x/at.z)/20.0f)) };
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 32);
	enum { N = 2*2*9 };
	float last[N], worst = 0.0f;
	int worstPx = -1;
	bool read = true;
	for(int f = 0; f < 9; f++){
		ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
		for(int k = 0; k < N; k++){
			float v = -1.0f, len = 0.0f;
			read = metal::readShadowForTest(cols[k/18] - 4 + k % 9, 240 + (k/9) % 2, &v, &len) && read;
			if(f > 0 && fabsf(v - last[k]) > worst){
				worst = fabsf(v - last[k]);
				worstPx = k;
			}
			last[k] = v;
		}
	}
	FreeShadowScene(&s);
	bool ok = read && worst <= 0.05f;
	Detail("  largest change between frames %.4f at column %d row %d (edges at columns %d and %d)\n", worst,
		worstPx < 0 ? -1 : cols[worstPx/18] - 4 + worstPx % 9, 240 + (worstPx < 0 ? 0 : (worstPx/9) % 2), cols[0], cols[1]);
	return Report(ok, name);
}

static void
PanBand(int32 quality, bool pan, float *v, int c0, int n, int row)
{
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetSunRadius(0.0f);
	metal::cameraCut();
	if(pan){
		ShadowFrame(&s, quality, 16);
		for(int f = 0; f < 16; f++){
			MoveBy(rtCamera->getFrame(), 0.23f, 0.23f, 0.0f);
			CasterFrame([&](bool){
				SetRenderState(CULLMODE, CULLNONE);
				s.floor->render();
				s.roof->render();
			});
		}
		metal::rtWaitForTest();
	}else{
		MoveBy(rtCamera->getFrame(), 3.68f, 3.68f, 0.0f);
		ShadowFrame(&s, quality, 24);
	}
	for(int i = 0; i < n; i++){
		float len = 0.0f;
		v[i] = -1.0f;
		metal::readShadowForTest(c0 + i, row, &v[i], &len);
	}
	FreeShadowScene(&s);
}

static void
MovingRoofBand(int32 quality, bool moving, float *v, int c0, int n, int row)
{
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetSunRadius(0.0f);
	metal::cameraCut();
	if(moving){
		ShadowFrame(&s, quality, 16);
		for(int f = 0; f < 16; f++){
			MoveBy(s.roof->getFrame(), 0.23f, 0.0f, 0.0f);
			CasterFrame([&](bool){
				SetRenderState(CULLMODE, CULLNONE);
				s.floor->render();
				s.roof->render();
			});
		}
		metal::rtWaitForTest();
	}else{
		MoveBy(s.roof->getFrame(), 3.68f, 0.0f, 0.0f);
		ShadowFrame(&s, quality, 24);
	}
	for(int i = 0; i < n; i++){
		float len = 0.0f;
		v[i] = -1.0f;
		metal::readShadowForTest(c0 + i, row, &v[i], &len);
	}
	FreeShadowScene(&s);
}

static bool
CheckShadowPanSmear(void)
{
	const char *name = "a camera pan smears a shadow at medium by at most two pixels more than at high, and a moving caster's shadow by at most two pixels more than at high and at most two at high, against each quality's own still frame";
	enum { N = 81, M = 128 };
	int c0 = SX(-6.0f, 20.0f), row = SY(-4.0f, 20.0f), m0 = SX(1.0f, 20.0f), mrow = SY(0.0f, 20.0f);
	const int32 q[2] = { metal::SHADOWQUALITY_MEDIUM, metal::SHADOWQUALITY_HIGH };
	float pan[M], still[M];
	int smear[2] = { 0, 0 }, dark[2] = { 0, 0 }, trail[2] = { 0, 0 }, shade[2] = { 0, 0 };
	for(int k = 0; k < 2; k++){
		PanBand(q[k], true, pan, c0, N, row);
		PanBand(q[k], false, still, c0, N, row);
		for(int i = 0; i < N; i++){
			smear[k] += pan[i] < 0.0f || still[i] < 0.0f || fabsf(pan[i] - still[i]) > 0.25f;
			dark[k] += still[i] >= 0.0f && still[i] < 0.5f;
		}
		MovingRoofBand(q[k], true, pan, m0, M, mrow);
		MovingRoofBand(q[k], false, still, m0, M, mrow);
		for(int i = 0; i < M; i++){
			trail[k] += pan[i] < 0.0f || still[i] < 0.0f || fabsf(pan[i] - still[i]) > 0.25f;
			shade[k] += still[i] >= 0.0f && still[i] < 0.5f;
		}
	}
	bool ok = smear[0] <= smear[1] + 2 && dark[0] > 0 && dark[1] > 0 && trail[0] <= trail[1] + 2 && trail[1] <= 2 && shade[0] > 0 && shade[1] > 0;
	Detail("  pixels off their still value by more than 0.25 after the pan: medium %d, high %d (dark in the still frame: %d, %d of %d)\n",
		smear[0], smear[1], dark[0], dark[1], (int)N);
	Detail("  after the caster moved: medium %d, high %d (dark in the still frame: %d, %d of %d)\n",
		trail[0], trail[1], shade[0], shade[1], (int)M);
	return Report(ok, name);
}

static Geometry*
NormalQuad(Material *mat, float nx, float ny, float nz)
{
	Geometry *geo = Geometry::create(4, 2, Geometry::POSITIONS | Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED);
	V3d *v = geo->morphTargets[0].vertices;
	v[0].set(-4.0f, 4.0f, -20.0f);
	v[1].set(4.0f, 4.0f, -20.0f);
	v[2].set(4.0f, -4.0f, -20.0f);
	v[3].set(-4.0f, -4.0f, -20.0f);
	float l = sqrtf(nx*nx + ny*ny + nz*nz);
	for(int i = 0; i < 4; i++){
		geo->morphTargets[0].normals[i].set(nx/l, ny/l, nz/l);
		geo->colors[i] = { 120, 120, 120, 255 };
		geo->texCoords[0][i] = { 0.0f, 0.0f };
	}
	geo->matList.appendMaterial(mat);
	Triangle *t = geo->triangles;
	t[0].v[0] = 0; t[0].v[1] = 1; t[0].v[2] = 2; t[0].matId = 0;
	t[1].v[0] = 0; t[1].v[1] = 2; t[1].v[2] = 3; t[1].matId = 0;
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
CheckShadowFacingAway(void)
{
	const char *name = "a surface whose normal faces away from the sun by more than the sun's disc reads factor 0 without a caster, at medium and high; facing the sun it reads 1";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	V3d at = s.sunFrame->getLTM()->at;
	Geometry *awayGeo = NormalQuad(s.mat, at.x*2.0f, at.y*2.0f, 0.3f), *upGeo = NormalQuad(s.mat, 0.0f, 0.0f, 1.0f);
	Atomic *away = MakeAtomic(awayGeo), *up = MakeAtomic(upGeo);
	float v[2][2] = { { -1, -1 }, { -1, -1 } };
	const int32 q[2] = { metal::SHADOWQUALITY_MEDIUM, metal::SHADOWQUALITY_HIGH };
	bool read = true;
	for(int k = 0; k < 2; k++)
		for(int a = 0; a < 2; a++){
			Atomic *quad = a == 0 ? away : up;
			SetShadowQuality(q[k]);
			for(int i = 0; i < 8; i++)
				CasterFrame([&](bool){
					SetRenderState(CULLMODE, CULLNONE);
					quad->render();
				});
			metal::rtWaitForTest();
			float len = 0.0f;
			read = metal::readShadowForTest(320, 240, &v[k][a], &len) && read;
		}
	DestroyAtomic(away);
	DestroyAtomic(up);
	awayGeo->destroy();
	upGeo->destroy();
	FreeShadowScene(&s);
	float nl = (2.0f*at.x*-at.x + 2.0f*at.y*-at.y + 0.3f*-at.z)/sqrtf(4.0f*at.x*at.x + 4.0f*at.y*at.y + 0.09f);
	bool ok = read && v[0][0] == 0.0f && v[1][0] == 0.0f && v[0][1] == 1.0f && v[1][1] == 1.0f && nl < -0.1f;
	if(!ok)
		Detail("  read %d; N.L of the turned normal %.3f; facing away: medium %.4f high %.4f; facing the sun: medium %.4f high %.4f\n", read, nl,
			v[0][0], v[1][0], v[0][1], v[1][1]);
	return Report(ok, name);
}

static bool
CheckShadowEmptyNormal(void)
{
	const char *name = "a pixel whose stored normal is unwritten (the empty code) is traced, not skipped, though that code turns to face away from the sun, at medium and high";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	V3d yAxis = { 0.0f, 1.0f, 0.0f };
	rtCamera->getFrame()->rotate(&yAxis, 45.0f, COMBINEREPLACE);
	rtCamera->getFrame()->updateObjects();
	V3d at = rtCamera->getFrame()->getLTM()->at, sunAt = s.sunFrame->getLTM()->at;
	Geometry *geo = Geometry::create(4, 2, Geometry::POSITIONS | Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED);
	V3d *v = geo->morphTargets[0].vertices;
	const float corner[4][2] = { { -4.0f, 4.0f }, { 4.0f, 4.0f }, { 4.0f, -4.0f }, { -4.0f, -4.0f } };
	for(int i = 0; i < 4; i++){
		v[i].set(20.0f*at.x + corner[i][0]*at.z, 20.0f*at.y + corner[i][1], 20.0f*at.z - corner[i][0]*at.x);
		geo->morphTargets[0].normals[i].set(-at.x, -at.y, -at.z);
		geo->colors[i] = { 120, 120, 120, 255 };
		geo->texCoords[0][i] = { 0.0f, 0.0f };
	}
	geo->matList.appendMaterial(s.mat);
	Triangle *t = geo->triangles;
	t[0].v[0] = 0; t[0].v[1] = 1; t[0].v[2] = 2; t[0].matId = 0;
	t[1].v[0] = 0; t[1].v[1] = 2; t[1].v[2] = 3; t[1].matId = 0;
	geo->calculateBoundingSphere();
	geo->unlock();
	Atomic *quad = MakeAtomic(geo);
	float val[2] = { -1.0f, -1.0f }, n[2][3], motion[2], depth[2] = { 1.0f, 1.0f };
	const int32 q[2] = { metal::SHADOWQUALITY_MEDIUM, metal::SHADOWQUALITY_HIGH };
	bool read = true, empty = true;
	metal::setPrepassNormalsUnwrittenForTest(1);
	for(int k = 0; k < 2; k++){
		SetShadowQuality(q[k]);
		for(int i = 0; i < 8; i++)
			CasterFrame([&](bool){
				SetRenderState(CULLMODE, CULLNONE);
				s.floor->render();
				metal::setShadowCaster(0);
				quad->render();
				metal::setShadowCaster(1);
			});
		metal::rtWaitForTest();
		float len = 0.0f;
		read = metal::readShadowForTest(320, 240, &val[k], &len) && metal::readPrepassForTest(320, 240, n[k], motion, &depth[k]) && read;
		empty = empty && n[k][0] == 0.0f && n[k][1] == 0.0f && n[k][2] == 0.0f && depth[k] < 1.0f;
	}
	metal::setPrepassNormalsUnwrittenForTest(0);
	DestroyAtomic(quad);
	geo->destroy();
	FreeShadowScene(&s);
	float nl = at.x*sunAt.x + at.y*sunAt.y + at.z*sunAt.z;
	bool ok = read && empty && val[0] == 1.0f && val[1] == 1.0f && nl < -0.9f;
	Detail("  read %d, stored normal empty with depth %d (%.4f %.4f); N.L of the code turned toward the eye %.3f; medium %.4f high %.4f\n",
		read, empty, depth[0], depth[1], nl, val[0], val[1]);
	return Report(ok, name);
}

static Geometry*
SmoothCylinder(Material *mat, float x, float radius, float z)
{
	enum { SEG = 8 };
	Geometry *geo = Geometry::create(2*SEG, 2*SEG, Geometry::POSITIONS | Geometry::NORMALS | Geometry::PRELIT | Geometry::TEXTURED);
	V3d *v = geo->morphTargets[0].vertices, *n = geo->morphTargets[0].normals;
	for(int k = 0; k < SEG; k++){
		float a = 6.28318531f*k/SEG, c = cosf(a), s = sinf(a);
		for(int e = 0; e < 2; e++){
			v[2*k + e].set(x + radius*c, e ? 3.0f : -3.0f, z + radius*s);
			n[2*k + e].set(c, 0.0f, s);
			geo->colors[2*k + e] = { 120, 120, 120, 255 };
			geo->texCoords[0][2*k + e] = { 0.0f, 0.0f };
		}
	}
	for(int k = 0; k < SEG; k++){
		int a0 = 2*k, a1 = 2*k + 1, b0 = 2*((k + 1) % SEG), b1 = b0 + 1;
		Triangle *t = &geo->triangles[2*k];
		t[0].v[0] = a0; t[0].v[1] = b0; t[0].v[2] = b1; t[0].matId = 0;
		t[1].v[0] = a0; t[1].v[1] = b1; t[1].v[2] = a1; t[1].matId = 0;
	}
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
CheckShadowSmoothRim(void)
{
	const char *name = "the sun-facing silhouette of a smooth cylinder is not shadowed by the cylinder itself: a normal that faces a little away from the eye is reflected, not negated, before the ray leaves";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetSunRadius(0.0f);
	Geometry *cylGeo = SmoothCylinder(s.mat, 1.5f, 2.0f, -15.0f);
	Atomic *cyl = MakeAtomic(cylGeo);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	for(int i = 0; i < 8; i++)
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLNONE);
			s.floor->render();
			cyl->render();
		});
	metal::rtWaitForTest();
	int first = -1, rim = 0;
	float v[3] = { -1, -1, -1 }, nx[3] = { 0, 0, 0 };
	bool read = true;
	for(int c = SX(3.8f, 15.0f); c < 320 && rim < 3; c++){
		float n[3], m[2], d = 0.0f;
		if(!metal::readPrepassForTest(c, 240, n, m, &d) || fabsf(n[0]) < 0.3f)
			continue;
		if(first < 0)
			first = c;
		float len = 0.0f;
		nx[rim] = n[0];
		read = metal::readShadowForTest(c, 240, &v[rim], &len) && read;
		rim++;
	}
	DestroyAtomic(cyl);
	cylGeo->destroy();
	FreeShadowScene(&s);
	bool ok = read && rim == 3 && v[0] >= 0.95f && v[1] >= 0.95f && v[2] >= 0.95f;
	if(!ok)
		Detail("  read %d; rim pixels from column %d: factor %.3f %.3f %.3f, stored normal x %.3f %.3f %.3f\n", read, first, v[0], v[1], v[2],
			nx[0], nx[1], nx[2]);
	return Report(ok, name);
}

static Geometry*
TerminatorCylinder(Material *mat, float x, float radius, float z, bool inward)
{
	enum { SEG = 8 };
	Geometry *geo = Geometry::create(2*SEG, 2*SEG, Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices, *n = geo->morphTargets[0].normals;
	for(int k = 0; k < SEG; k++){
		float a = 6.28318531f*k/SEG, c = cosf(a), s = sinf(a);
		for(int e = 0; e < 2; e++){
			v[2*k + e].set(x + radius*c, e ? 3.0f : -3.0f, z + radius*s);
			n[2*k + e].set(c, 0.0f, s);
		}
	}
	for(int k = 0; k < SEG; k++){
		int a0 = 2*k, a1 = 2*k + 1, b0 = 2*((k + 1) % SEG), b1 = b0 + 1;
		Triangle *t = &geo->triangles[2*k];
		t[0].v[0] = a0; t[0].v[1] = inward ? b0 : a1; t[0].v[2] = b1; t[0].matId = 0;
		t[1].v[0] = a0; t[1].v[1] = b1; t[1].v[2] = inward ? a1 : b0; t[1].matId = 0;
	}
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static Atomic*
MirroredX(Geometry *geo)
{
	Atomic *a = MakeAtomic(geo);
	Matrix m;
	m.setIdentity();
	m.right.x = -1.0f;
	m.optimize();
	a->getFrame()->transform(&m, COMBINEREPLACE);
	a->getFrame()->updateObjects();
	return a;
}

static const float terminatorRadius = 4.0f;

static bool
TerminatorScanline(const char *name, bool mirrored)
{
	enum { N = 192 };
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetSunRadius(0.0f);
	TurnSun(&s, -100.0f);
	Geometry *geo = TerminatorCylinder(s.mat, mirrored ? -1.5f : 1.5f, terminatorRadius, -15.0f, mirrored);
	Atomic *cyl = mirrored ? MirroredX(geo) : MakeAtomic(geo);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	const float deg = 3.14159265f/180.0f;
	auto col = [&](float a){ return SX(1.5f + terminatorRadius*cosf(a*deg), 15.0f - terminatorRadius*sinf(a*deg)); };
	int c0 = col(45.0f), n = std::min(col(135.0f) - c0 + 1, (int)N), mid = col(90.0f);
	float row[N][4], nrm[3], mot[2], dNone = -1.0f, dBack = -2.0f;
	bool read = n > 40;
	auto draw = [&](bool prepass, int k){
		SetRenderState(CULLMODE, k == 0 ? CULLNONE : CULLBACK);
		s.floor->render();
		cyl->render();
		if(!prepass && k == 16)
			for(int i = 0; i < n; i++)
				read = metal::readScenePixel(c0 + i, 240, row[i]) && read;
	};
	for(int k = 0; k <= 16; k++){
		CasterFrame([&](bool prepass){ draw(prepass, k); });
		if(k == 0 || k == 16)
			read = metal::readPrepassForTest(mid, 240, nrm, mot, k == 0 ? &dNone : &dBack) && read;
	}
	SetRenderState(CULLMODE, CULLNONE);
	DestroyAtomic(cyl);
	geo->destroy();
	FreeShadowScene(&s);
	float step = 0.0f, rise = 0.0f;
	int stepAt = -1;
	for(int i = 0; read && i + 1 < n; i++)
		for(int ch = 0; ch < 3; ch++){
			float d = row[i][ch] - row[i + 1][ch];
			if(d > step){
				step = d;
				stepAt = c0 + i;
			}
			rise = fmaxf(rise, -d);
		}
	bool ok = read && dBack == dNone && step <= 4.0f/255.0f && rise <= 1.0f/1024.0f;
	Detail("  read %d over columns %d to %d; depth at column %d culled %.6f, unculled %.6f; largest drop between neighbours %.4f (%.1f/255) after column %d; largest rise %.5f; rgb from %.4f %.4f %.4f to %.4f %.4f %.4f\n",
		read, c0, c0 + n - 1, mid, dBack, dNone, step, step*255.0f, stepAt, rise, row[0][0], row[0][1], row[0][2], row[n - 1][0],
		row[n - 1][1], row[n - 1][2]);
	return Report(ok, name);
}

static bool
CheckShadowTerminator(void)
{
	return TerminatorScanline("a smooth low-poly cylinder lit by a low sun shades to its terminator without a step", false);
}

static bool
CheckShadowTerminatorMirrored(void)
{
	return TerminatorScanline("a smooth low-poly cylinder under a mirrored transform shades to its terminator without a step", true);
}

static bool
CheckShadowTerminatorFloor(void)
{
	const char *name = "a body's cast shadow on the floor is unchanged by the terminator rule";
	enum { W = 640 };
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetSunRadius(0.0f);
	Geometry *geo = TerminatorCylinder(s.mat, 1.5f, 2.0f, -15.0f, false);
	Atomic *cyl = MakeAtomic(geo);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	static float rows[2][W][4];
	bool read = true;
	for(int pass = 0; pass < 2; pass++){
		metal::setShadowTerminatorRuleForTest(pass == 0);
		for(int k = 0; k <= 16; k++)
			CasterFrame([&](bool prepass){
				SetRenderState(CULLMODE, CULLBACK);
				s.floor->render();
				cyl->render();
				if(!prepass && k == 16)
					for(int c = 0; c < W; c++)
						read = metal::readScenePixel(c, 240, rows[pass][c]) && read;
			});
	}
	metal::setShadowTerminatorRuleForTest(true);
	SetRenderState(CULLMODE, CULLNONE);
	DestroyAtomic(cyl);
	geo->destroy();
	FreeShadowScene(&s);
	int lo = SX(3.5f, 15.0f) - 8, hi = SX(-0.5f, 15.0f) + 8, unequal = 0, compared = 0;
	float dark = 2.0f, bright = -1.0f;
	for(int c = SX(7.5f, 20.0f); read && c <= SX(-7.5f, 20.0f); c++){
		if(c >= lo && c <= hi)
			continue;
		compared++;
		unequal += memcmp(rows[0][c], rows[1][c], 3*sizeof(float)) != 0;
		dark = fminf(dark, rows[0][c][0]);
		bright = fmaxf(bright, rows[0][c][0]);
	}
	bool ok = read && compared > 100 && unequal == 0 && bright - dark > 0.1f;
	Detail("  read %d; %d floor pixels outside columns %d to %d compared, %d not equal bit for bit; floor red from %.4f to %.4f\n", read,
		compared, lo, hi, unequal, dark, bright);
	return Report(ok, name);
}

static bool
CheckShadowTerminatorRoof(void)
{
	const char *name = "a pixel on a facet facing the sun under a single-sided roof that faces the sun is still shadowed";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetSunRadius(0.0f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	for(int k = 0; k <= 16; k++)
		CasterFrame([&](bool){
			SetRenderState(CULLMODE, CULLBACK);
			s.floor->render();
			s.roof->render();
		});
	metal::rtWaitForTest();
	float nrm[3], mot[2], dRoof = -1.0f, dFloor = -1.0f;
	bool read = metal::readPrepassForTest(SX(0.0f, 15.0f), 240, nrm, mot, &dRoof) && metal::readPrepassForTest(SX(-7.5f, 20.0f), 240, nrm, mot, &dFloor);
	int lo = SX(1.0f, 15.0f) - 8, hi = SX(-1.0f, 15.0f) + 8, shadowed = 0;
	float least = 2.0f;
	for(int c = SX(7.5f, 20.0f); read && c <= SX(-7.5f, 20.0f); c += 2){
		if(c >= lo && c <= hi)
			continue;
		float v = -1.0f, len = 0.0f;
		read = metal::readShadowForTest(c, 240, &v, &len) && read;
		shadowed += v <= 0.05f;
		least = fminf(least, v);
	}
	SetRenderState(CULLMODE, CULLNONE);
	FreeShadowScene(&s);
	bool ok = read && dRoof < dFloor && shadowed >= 4;
	Detail("  read %d; depth roof %.6f floor %.6f; %d floor pixels with a factor of at most 0.05, least %.3f\n", read, dRoof, dFloor, shadowed,
		least);
	return Report(ok, name);
}

static Geometry*
SideWall(Material *mat, float x, float y0, float y1, float zNear, float zFar)
{
	Geometry *geo = Geometry::create(4, 2, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices;
	v[0].set(x, y1, zNear);
	v[1].set(x, y1, zFar);
	v[2].set(x, y0, zFar);
	v[3].set(x, y0, zNear);
	RGBA c = { 120, 120, 120, 255 };
	for(int i = 0; i < 4; i++)
		geo->colors[i] = c;
	geo->matList.appendMaterial(mat);
	Triangle *t = geo->triangles;
	t[0].v[0] = 0; t[0].v[1] = 1; t[0].v[2] = 2; t[0].matId = 0;
	t[1].v[0] = 0; t[1].v[1] = 2; t[1].v[2] = 3; t[1].matId = 0;
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
WallFacingAway(const char *name, float weight, bool twoSided = false)
{
	ShadowScene s;
	MakeShadowScene(&s, weight);
	TurnSun(&s, -150.0f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	Geometry *geo = SideWall(s.mat, 4.0f, -3.0f, 3.0f, -12.0f, -20.0f);
	Atomic *wall = MakeAtomic(geo);
	float px[3][4] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
	uint32 plain[3], relit[3], rays[3];
	bool read = true;
	for(int pass = 0; pass < 3; pass++){
		metal::setShadowTerminatorRuleForTest(pass != 0);
		SetShadowStrength(pass == 2 ? 0.0f : 0.6f);
		metal::StateStats a0 = metal::getStateStats();
		for(int k = 0; k <= 16; k++)
			CasterFrame([&](bool prepass){
				SetRenderState(CULLMODE, twoSided ? CULLNONE : CULLBACK);
				metal::setRelitTwoSided(twoSided);
				wall->render();
				if(!prepass && k == 16)
					read = metal::readScenePixel(SX(4.0f, 16.0f), 240, px[pass]) && read;
			});
		metal::rtWaitForTest();
		metal::StateStats a1 = metal::getStateStats();
		plain[pass] = a1.plainShadowDraws - a0.plainShadowDraws;
		relit[pass] = a1.relitShadowDraws - a0.relitShadowDraws;
		rays[pass] = metal::getRayStats().raysPerFrame;
	}
	metal::setShadowTerminatorRuleForTest(true);
	SetRenderState(CULLMODE, CULLNONE);
	V3d at = s.sunFrame->getLTM()->at;
	DestroyAtomic(wall);
	geo->destroy();
	FreeShadowScene(&s);
	bool twin = weight >= 1.0f, drawn = true;
	for(int pass = 0; pass < 2; pass++)
		drawn = drawn && (twin ? plain[pass] >= 1 && relit[pass] == 0 : relit[pass] >= 1 && plain[pass] == 0);
	bool ok = read && memcmp(px[0], px[1], 3*sizeof(float)) == 0 && px[2][0] - px[0][0] > 0.1f && drawn && rays[0] > 0 && rays[1] > 0 &&
		-at.z > 0.8f && -at.x > 0.4f;
	Detail("  read %d; toward-sun x %.3f z %.3f; rays %u %u; red with the rule %.4f, without %.4f, without the strength %.4f; plain shadow draws %u %u, relit shadow draws %u %u\n",
		read, -at.x, -at.z, rays[1], rays[0], px[1][0], px[0][0], px[2][0], plain[1], plain[0], relit[1], relit[0]);
	return Report(ok, name);
}

static bool
CheckShadowWallFacingAway(void)
{
	return WallFacingAway("a prelit wall facing away from the sun at weight 1 keeps its shadow", 1.0f);
}

static bool
CheckShadowWallFacingAwayRelit(void)
{
	return WallFacingAway("a prelit wall facing away from the sun below weight 1 keeps its shadow strength's share", 0.5f);
}

static bool
CheckShadowWallFacingAwayTwoSided(void)
{
	return WallFacingAway("a two-sided prelit wall facing away from the sun below weight 1 takes no sun share, as with the terminator rule off", 0.5f,
		true);
}

enum { PLAIN_ROW0 = 70, PLAIN_ROWN = 54 };

static bool
PlainWallRow(float offset, float row[PLAIN_ROWN][4], uint32 *plainDraws)
{
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	TurnSun(&s, -179.9f);
	SetShadowStrength(0.6f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	Geometry *geo = SideWall(s.mat, 1.2f, -3.0f, 3.0f, -1.0f, -12.0f);
	Atomic *wall = MakeAtomic(geo);
	V3d t = { offset, offset, offset };
	for(Atomic *a : { wall, s.floor, s.roof }){
		a->getFrame()->translate(&t, COMBINEPOSTCONCAT);
		a->getFrame()->updateObjects();
	}
	rtCamera->getFrame()->translate(&t, COMBINEPOSTCONCAT);
	rtCamera->getFrame()->updateObjects();
	bool read = true;
	metal::StateStats a0 = metal::getStateStats();
	for(int k = 0; k <= 16; k++)
		CasterFrame([&](bool prepass){
			SetRenderState(CULLMODE, CULLBACK);
			wall->render();
			if(!prepass && k == 16)
				for(int i = 0; i < PLAIN_ROWN; i++)
					read = metal::readScenePixel(PLAIN_ROW0 + 4*i, 240, row[i]) && read;
		});
	metal::rtWaitForTest();
	*plainDraws = metal::getStateStats().plainShadowDraws - a0.plainShadowDraws;
	SetRenderState(CULLMODE, CULLNONE);
	DestroyAtomic(wall);
	geo->destroy();
	FreeShadowScene(&s);
	return read;
}

static bool
CheckPlainTwinFacetNearCamera(void)
{
	const char *name = "a prelit wall facing away from a grazing sun, a metre from the camera at coordinates of 100000, keeps the plain twin's facing rule along its length as it does at the origin";
	static float row[2][PLAIN_ROWN][4];
	uint32 draws[2];
	bool read = PlainWallRow(0.0f, row[0], &draws[0]);
	read = PlainWallRow(100000.0f, row[1], &draws[1]) && read;
	int differ = 0, first = -1;
	float worst = 0.0f, brightest = 0.0f;
	for(int i = 0; i < PLAIN_ROWN; i++)
		for(int c = 0; c < 3; c++){
			float d = fabsf(row[0][i][c] - row[1][i][c]);
			worst = fmaxf(worst, d);
			brightest = fmaxf(brightest, row[0][i][c]);
			if(d > 1.0f/255.0f){
				differ++;
				if(first < 0)
					first = PLAIN_ROW0 + 4*i;
			}
		}
	bool ok = read && differ == 0 && brightest < 0.3f && draws[0] >= 1 && draws[1] >= 1;
	Detail("  read %d; %d of %d samples move by more than 1/255 between the origin and coordinates of 100000, largest %.4f, first at column %d; brightest sample at the origin %.4f; plain shadow draws %u %u\n",
		read, differ, 3*PLAIN_ROWN, worst, first, brightest, draws[0], draws[1]);
	return Report(ok, name);
}

static Geometry*
FacedQuads(Material *mat, const V3d (*c)[4], const V3d *out, int faces)
{
	Geometry *geo = Geometry::create(4*faces, 2*faces, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices;
	RGBA col = { 120, 120, 120, 255 };
	for(int f = 0; f < faces; f++){
		int o[4] = { 0, 1, 2, 3 };
		if(dot(cross(sub(c[f][1], c[f][0]), sub(c[f][2], c[f][0])), out[f]) < 0.0f)
			std::swap(o[1], o[3]);
		for(int i = 0; i < 4; i++){
			v[4*f + i] = c[f][o[i]];
			geo->colors[4*f + i] = col;
		}
		Triangle *t = &geo->triangles[2*f];
		t[0].v[0] = 4*f; t[0].v[1] = 4*f + 1; t[0].v[2] = 4*f + 2; t[0].matId = 0;
		t[1].v[0] = 4*f; t[1].v[1] = 4*f + 2; t[1].v[2] = 4*f + 3; t[1].matId = 0;
	}
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static bool
ContactShadow(const char *name, bool body, bool owned = false)
{
	enum { H = 480, C = 8, R = 6 };
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetSunRadius(0.0f);
	V3d xAxis = { 1.0f, 0.0f, 0.0f };
	rtCamera->getFrame()->rotate(&xAxis, 90.0f, COMBINEPRECONCAT);
	rtCamera->getFrame()->updateObjects();
	Matrix *cam = rtCamera->getFrame()->getLTM();
	V3d E = cam->pos, A = cam->at, Z = { 0.0f, 0.0f, 1.0f }, Rt = normalize(cross(A, Z));
	auto P = [&](float dist, float side, float height){ return add(add(E, scale(A, dist)), add(scale(Rt, side), scale(Z, height))); };
	Matrix m;
	m.setIdentity();
	m.at = normalize(sub(scale(A, -0.5f), scale(Z, 0.8660254f)));
	m.right = Rt;
	m.up = cross(m.at, m.right);
	m.optimize();
	s.sunFrame->transform(&m, COMBINEREPLACE);
	s.sunFrame->updateObjects();
	const V3d ground[1][4] = { { P(1.0f, -15.0f, -2.0f), P(1.0f, 15.0f, -2.0f), P(28.0f, 15.0f, -2.0f), P(28.0f, -15.0f, -2.0f) } };
	const V3d box[6][4] = {
		{ P(12.0f, -4.0f, -2.0f), P(12.0f, 4.0f, -2.0f), P(12.0f, 4.0f, 4.0f), P(12.0f, -4.0f, 4.0f) },
		{ P(16.0f, -4.0f, -2.0f), P(16.0f, 4.0f, -2.0f), P(16.0f, 4.0f, 4.0f), P(16.0f, -4.0f, 4.0f) },
		{ P(12.0f, -4.0f, -2.0f), P(16.0f, -4.0f, -2.0f), P(16.0f, -4.0f, 4.0f), P(12.0f, -4.0f, 4.0f) },
		{ P(12.0f, 4.0f, -2.0f), P(16.0f, 4.0f, -2.0f), P(16.0f, 4.0f, 4.0f), P(12.0f, 4.0f, 4.0f) },
		{ P(12.0f, -4.0f, -2.0f), P(16.0f, -4.0f, -2.0f), P(16.0f, 4.0f, -2.0f), P(12.0f, 4.0f, -2.0f) },
		{ P(12.0f, -4.0f, 4.0f), P(16.0f, -4.0f, 4.0f), P(16.0f, 4.0f, 4.0f), P(12.0f, 4.0f, 4.0f) } };
	const V3d up[1] = { Z }, out[12] = { scale(A, -1.0f), A, scale(Rt, -1.0f), Rt, scale(Z, -1.0f), Z, A, scale(A, -1.0f), Rt, scale(Rt, -1.0f), Z,
		scale(Z, -1.0f) };
	V3d shell[12][4];
	for(int f = 0; f < 6; f++)
		for(int i = 0; i < 4; i++){
			shell[f][i] = box[f][i];
			V3d c = P(14.0f, 0.0f, 1.0f), d = sub(box[f][i], c);
			shell[6 + f][i] = add(c, scale(d, 0.92f));
		}
	Geometry *gg = FacedQuads(s.mat, ground, up, 1), *bg = FacedQuads(s.mat, shell, out, owned ? 12 : body ? 6 : 1);
	Atomic *ga = MakeAtomic(gg), *ba = MakeAtomic(bg);
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	static float nrm[H][3], dep[H];
	enum { F = 8 };
	static float v[2][F][R][C];
	float face[2] = { -1.0f, -1.0f };
	int edge = -1, step = 1, faceRow = -1;
	bool read = true;
	auto frames = [&](int n, bool withBody){
		for(int k = 0; k < n; k++)
			CasterFrame([&](bool prepass){
				SetRenderState(CULLMODE, CULLBACK);
				ga->render();
				if(prepass && owned)
					metal::setShadowOwner(1);
				if(withBody)
					ba->render();
				if(prepass)
					metal::setShadowOwner(0);
			});
		metal::rtWaitForTest();
	};
	frames(2, true);
	for(int r = 0; r < H; r++){
		float mo[2];
		dep[r] = 1.0f;
		read = metal::readPrepassForTest(320, r, nrm[r], mo, &dep[r]) && read;
	}
	for(int r = 0; r + 1 < H && edge < 0; r++)
		if(dep[r] < 1.0f && dep[r + 1] < 1.0f && nrm[r][0]*nrm[r + 1][0] + nrm[r][1]*nrm[r + 1][1] + nrm[r][2]*nrm[r + 1][2] < 0.5f)
			edge = r;
	bool above = edge >= 0;
	for(int r = 0; r <= edge; r++)
		above = above && dep[r] < 1.0f;
	step = above ? -1 : 1;
	edge = above ? edge : edge + 1;
	faceRow = edge - 4*step;
	for(int pass = 0; pass < 2; pass++){
		if(owned)
			metal::setShadowOwnerRuleForTest(pass == 1);
		else
			metal::setShadowTerminatorRuleForTest(pass == 1);
		metal::cameraCut();
		frames(24, false);
		for(int f = 0; f < F; f++){
			if(f == F/2)
				frames(20, true);
			frames(1, true);
			float len = 0.0f;
			for(int r = 0; r < R; r++)
				for(int c = 0; c < C; c++){
					v[pass][f][r][c] = -1.0f;
					read = edge >= 0 && metal::readShadowForTest(316 + c, edge + r*step, &v[pass][f][r][c], &len) && read;
				}
		}
		float len = 0.0f;
		read = edge >= 0 && metal::readShadowForTest(320, faceRow, &face[pass], &len) && read;
	}
	metal::setShadowTerminatorRuleForTest(true);
	metal::setShadowOwnerRuleForTest(true);
	SetRenderState(CULLMODE, CULLNONE);
	DestroyAtomic(ga);
	DestroyAtomic(ba);
	gg->destroy();
	bg->destroy();
	FreeShadowScene(&s);
	float worst[2] = { 0.0f, 0.0f }, lightest = -1.0f;
	int at[2] = { -1, -1 };
	for(int f = 0; f < F; f++)
		for(int r = 0; r < R; r++)
			for(int c = 0; c < C; c++){
				float d = fabsf(v[1][f][r][c] - v[0][f][r][c]);
				if(d > worst[f/(F/2)]){
					worst[f/(F/2)] = d;
					at[f/(F/2)] = (f*R + r)*C + c;
				}
				if(f >= F/2)
					lightest = fmaxf(lightest, v[0][f][r][c]);
			}
	bool ok = read && fabsf(A.z) < 1e-3f && worst[0] <= 0.02f && worst[1] <= 0.02f && lightest <= 0.05f && face[0] == 0.0f && face[1] >= 0.95f;
	Detail("  read %d; view direction z %.4f; ground from row %d going %d, columns 316 to 323; face at row %d: %.4f without the rule, %.4f with it\n",
		read, A.z, edge, step, faceRow, face[0], face[1]);
	for(int k = 0; k < 2; k++)
		Detail("  %s: largest change with the rule %.4f at frame %d column %d row %d\n", k == 0 ? "first four frames after the body appears on a lit ground" :
			"four frames once converged", worst[k], at[k] < 0 ? -1 : at[k]/(R*C) % (F/2), at[k] < 0 ? -1 : 316 + at[k] % C,
			at[k] < 0 ? -1 : edge + (at[k]/C % R)*step);
	Detail("  converged ground without the rule at most %.4f\n", lightest);
	return Report(ok, name);
}

static bool
CheckShadowContactWall(void)
{
	return ContactShadow("at medium the ground along a wall that faces away from the sun keeps its shadow in every ray phase and from the frame the wall appears, as with the terminator rule off", false);
}

static bool
CheckShadowContactBody(void)
{
	return ContactShadow("at medium the ground along the shaded side of a closed body resting on it keeps its shadow in every ray phase and from the frame the body appears, as with the terminator rule off", true);
}

static bool
CheckShadowContactOwned(void)
{
	return ContactShadow("at medium the ground along the shaded side of a closed body with an inner shell under one owner keeps its shadow in every ray phase and from the frame the body appears, as with the owner rule off",
		true, true);
}

static bool
CheckShadowCreaseGround(void)
{
	const char *name = "at medium a lit ground pixel beside a wall whose factor reads 0 takes its new value at once, and a shadowed one beside a lit wall does too";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetSunRadius(0.0f);
	TurnSun(&s, -135.0f);
	const V3d walls[2][4] = {
		{ { 4.0f, -3.0f, -19.0f }, { 4.0f, 3.0f, -19.0f }, { 4.0f, 3.0f, -12.0f }, { 4.0f, -3.0f, -12.0f } },
		{ { -4.0f, -3.0f, -19.0f }, { -4.0f, 3.0f, -19.0f }, { -4.0f, 3.0f, -12.0f }, { -4.0f, -3.0f, -12.0f } } };
	const V3d cardA[1][4] = { { { 4.3f, -3.0f, -19.5f }, { 6.5f, -3.0f, -19.5f }, { 6.5f, 3.0f, -19.5f }, { 4.3f, 3.0f, -19.5f } } };
	const V3d cardB[1][4] = { { { -3.7f, -3.0f, -19.5f }, { -3.1f, -3.0f, -19.5f }, { -3.1f, 3.0f, -19.5f }, { -3.7f, 3.0f, -19.5f } } };
	const V3d wallOut[2] = { { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, cardOut[1] = { { 0.0f, 0.0f, 1.0f } };
	Geometry *wg = FacedQuads(s.mat, walls, wallOut, 2), *ag = FacedQuads(s.mat, cardA, cardOut, 1), *bg = FacedQuads(s.mat, cardB, cardOut, 1);
	Atomic *wall = MakeAtomic(wg), *a = MakeAtomic(ag), *b = MakeAtomic(bg);
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	bool read = true;
	auto frames = [&](int n, bool withA, bool withB){
		for(int k = 0; k < n; k++)
			CasterFrame([&](bool){
				SetRenderState(CULLMODE, CULLBACK);
				s.floor->render();
				wall->render();
				if(withA)
					a->render();
				if(withB)
					b->render();
			});
		metal::rtWaitForTest();
	};
	frames(2, true, false);
	float nrm[3], mo[2], d = 1.0f;
	int darkCol = -1, litCol = -1;
	for(int c = 241; c < 270 && darkCol < 0; c++){
		read = metal::readPrepassForTest(c, 240, nrm, mo, &d) && read;
		if(fabsf(nrm[2]) > 0.7f)
			darkCol = c;
	}
	for(int c = 395; c > 370 && litCol < 0; c--){
		read = metal::readPrepassForTest(c, 240, nrm, mo, &d) && read;
		if(fabsf(nrm[2]) > 0.7f)
			litCol = c;
	}
	enum { N = 4 };
	float wallRaw[2] = { -1.0f, -1.0f }, v[2][2][N];
	auto sample = [&](int phase){
		float len = 0.0f;
		for(int k = 0; k < N; k++){
			v[phase][0][k] = v[phase][1][k] = -1.0f;
			read = metal::readShadowForTest(darkCol + k, 240, &v[phase][0][k], &len) && read;
			read = metal::readShadowForTest(litCol - k, 240, &v[phase][1][k], &len) && read;
		}
	};
	metal::cameraCut();
	frames(24, true, false);
	sample(0);
	float len = 0.0f;
	read = metal::readShadowForTest(darkCol - 1, 240, &wallRaw[0], &len) && metal::readShadowForTest(litCol + 1, 240, &wallRaw[1], &len) && read;
	frames(1, false, true);
	sample(1);
	SetRenderState(CULLMODE, CULLNONE);
	DestroyAtomic(wall);
	DestroyAtomic(a);
	DestroyAtomic(b);
	wg->destroy();
	ag->destroy();
	bg->destroy();
	FreeShadowScene(&s);
	bool ok = read && darkCol > 0 && litCol > 0 && wallRaw[0] == 0.0f && wallRaw[1] == 1.0f;
	for(int k = 0; k < 2; k++){
		ok = ok && v[0][0][k] <= 0.05f && v[0][1][k] >= 0.95f;
		ok = ok && v[1][0][k] >= 0.95f && v[1][1][k] <= 0.05f;
	}
	Detail("  read %d; ground beside the dark wall from column %d, beside the lit wall from column %d going down; wall factors %.3f and %.3f\n", read,
		darkCol, litCol, wallRaw[0], wallRaw[1]);
	for(int p = 0; p < 2; p++)
		Detail("  %s: beside the dark wall %.4f %.4f %.4f %.4f, beside the lit wall %.4f %.4f %.4f %.4f\n",
			p == 0 ? "card over the dark side" : "first frame after the cards swap", v[p][0][0], v[p][0][1],
			v[p][0][2], v[p][0][3], v[p][1][0], v[p][1][1], v[p][1][2], v[p][1][3]);
	return Report(ok, name);
}

static bool
CheckShadowClassDark(void)
{
	const char *name = "a wall whose facet faces away from the sun reads factor 0 below a shading cosine of -0.6 and is traced to 1 above it";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetSunRadius(0.0f);
	Geometry *geo = SideWall(s.mat, 4.0f, -3.0f, 3.0f, -12.0f, -20.0f);
	Atomic *wall = MakeAtomic(geo);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	const float turn[2] = { -135.0f, -150.0f };
	float v[2] = { -1.0f, -1.0f }, ns[2];
	bool read = true;
	for(int k = 0; k < 2; k++){
		TurnSun(&s, turn[k]);
		metal::cameraCut();
		for(int i = 0; i < 8; i++)
			CasterFrame([&](bool){
				SetRenderState(CULLMODE, CULLBACK);
				wall->render();
			});
		metal::rtWaitForTest();
		float len = 0.0f;
		read = metal::readShadowForTest(SX(4.0f, 16.0f), 240, &v[k], &len) && read;
		ns[k] = s.sunFrame->getLTM()->at.x;
	}
	SetRenderState(CULLMODE, CULLNONE);
	DestroyAtomic(wall);
	geo->destroy();
	FreeShadowScene(&s);
	bool ok = read && ns[0] < -0.6f && ns[1] > -0.6f && ns[1] < 0.0f && v[0] == 0.0f && v[1] == 1.0f;
	Detail("  read %d; shading cosine %.3f: factor %.4f; shading cosine %.3f: factor %.4f\n", read, ns[0], v[0], ns[1], v[1]);
	return Report(ok, name);
}

static bool
CheckShadowSkippedWall(void)
{
	const char *name = "with the shadow pass skipped, a prelit wall at weight 1 is drawn as with strength 0";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	TurnSun(&s, -87.134f);
	SetShadowStrength(0.6f);
	ShadowFrame(&s, metal::SHADOWQUALITY_HIGH, 2);
	float shade[2][4], open[2][4];
	metal::StateStats a0 = metal::getStateStats();
	bool read = ShadowScenePixels(&s, false, shade[0], open[0]);
	metal::StateStats a1 = metal::getStateStats();
	uint32 rays = metal::getRayStats().raysPerFrame;
	SetShadowStrength(0.0f);
	read = ShadowScenePixels(&s, false, shade[1], open[1]) && read;
	V3d at = s.sunFrame->getLTM()->at;
	FreeShadowScene(&s);
	uint32 plain = a1.plainShadowDraws - a0.plainShadowDraws, relit = a1.relitShadowDraws - a0.relitShadowDraws;
	bool same = memcmp(shade[0], shade[1], sizeof(shade[0])) == 0 && memcmp(open[0], open[1], sizeof(open[0])) == 0;
	bool ok = read && same && rays == 0 && plain >= 1 && relit == 0 && at.z > 0.0f;
	Detail("  read %d; toward-sun z %.3f; rays %u; plain shadow draws %u, relit shadow draws %u; shade red at 0.6 %.4f, at 0 %.4f; bit-equal %d\n",
		read, -at.z, rays, plain, relit, shade[0][0], shade[1][0], same);
	return Report(ok, name);
}

static bool
CheckPlainTwinFailure(void)
{
	const char *name = "a plain shadow twin that fails to build drops only the twin: at weight 1 the prelit floor keeps its plain colour, below weight 1 the shader's relit variants still draw with the shadow, and the twin returns when it builds";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetShadowStrength(0.6f);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 8);
	metal::setPlainTwinFailForTest(1);
	float shade[4] = { -1, -1, -1, -1 }, open[4] = { -1, -1, -1, -1 };
	metal::StateStats a0 = metal::getStateStats();
	bool read = ShadowScenePixels(&s, false, shade, open);
	metal::StateStats a1 = metal::getStateStats();
	metal::RelitParams p = metal::getRelitParams();
	p.weight = 0.5f;
	metal::setRelitParams(&p);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	metal::StateStats a2 = metal::getStateStats();
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	metal::StateStats a3 = metal::getStateStats();
	metal::setPlainTwinFailForTest(0);
	p.weight = 1.0f;
	metal::setRelitParams(&p);
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	metal::StateStats a4 = metal::getStateStats();
	ShadowFrame(&s, metal::SHADOWQUALITY_MEDIUM, 1);
	metal::StateStats a5 = metal::getStateStats();
	FreeShadowScene(&s);
	float plain = fminf(120/255.0f + 0.1f, 1.0f);
	uint32 failedTwin = a1.plainShadowDraws - a0.plainShadowDraws, relit = a3.relitDraws - a2.relitDraws;
	uint32 relitShadow = a3.relitShadowDraws - a2.relitShadowDraws, twinBack = a5.plainShadowDraws - a4.plainShadowDraws;
	bool ok = read && failedTwin == 0 && fabsf(shade[0] - plain) < 2.0f/255.0f && relit > 0 && relitShadow > 0 && twinBack > 0;
	if(!ok)
		Detail("  read %d; twin failing: plain shadow draws %u, shaded red %.4f (want %.4f); below weight 1: relit draws %u, relit shadow draws %u; twin back: plain shadow draws %u\n",
			read, failedTwin, shade[0], plain, relit, relitShadow, twinBack);
	return Report(ok, name);
}

static bool
SkipStrengthState(ShadowScene *s, int state, float *darkUp, bool *same)
{
	metal::RelitParams p = metal::getRelitParams();
	p.weight = state == 2 ? 1.0f : 0.5f;
	p.sunScale = 1.0f;
	p.shadowStrength = 0.6f;
	metal::setRelitParams(&p);
	TurnSun(s, -135.0f);
	ShadowFrame(s, metal::SHADOWQUALITY_MEDIUM, 4);
	float up[4], upOpen[4], shade[2][4], open[2][4];
	bool read = ShadowScenePixels(s, false, up, upOpen);
	if(state == 0)
		TurnSun(s, -87.134f);
	else{
		p.sunScale = 0.0f;
		metal::setRelitParams(&p);
	}
	read = ShadowScenePixels(s, false, shade[0], open[0]) && read;
	SetShadowStrength(0.0f);
	read = ShadowScenePixels(s, false, shade[1], open[1]) && read;
	*same = true;
	for(int i = 0; i < 4; i++)
		*same = *same && shade[0][i] == shade[1][i] && open[0][i] == open[1][i];
	*darkUp = upOpen[0] - up[0];
	TurnSun(s, -135.0f);
	p.weight = 0.5f;
	p.sunScale = 1.0f;
	p.shadowStrength = 0.0f;
	metal::setRelitParams(&p);
	return read;
}

static bool
SkipStrengthIdentity(ShadowScene *s)
{
	bool ok = true;
	for(int k = 0; k < 3; k++){
		float darkUp = 0.0f;
		bool same = false, read = SkipStrengthState(s, k, &darkUp, &same);
		ok = ok && read && same && darkUp > 0.05f;
		if(!read || !same || darkUp <= 0.05f)
			Detail("  strength 0.6, %s: read %d, the frame before darkened the shade by %.4f, skipped frame equal to strength 0 %d\n",
				k == 0 ? "sun down" : k == 1 ? "sun scale 0" : "interior at weight 1", read, darkUp, same);
	}
	return ok;
}

static bool
CheckShadowSkippedSamples(void)
{
	const char *name = "at four samples a skipped shadow pass (sun down, sun scale 0, an interior at weight 1) draws exactly the frame without the shadow strength";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	bool ok = SkipStrengthIdentity(&s);
	FreeShadowScene(&s);
	return Report(ok, name);
}

static bool
CheckCasterRadiusLimit(void)
{
	const char *name = "a caster whose world bounding radius exceeds the host's limit casts nothing and is counted, and casts again when the limit is lifted";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Atomic *big = MakeAtomic(s.roofGeo);
	V3d scale = { 300.0f, 300.0f, 1.0f };
	big->getFrame()->scale(&scale, COMBINEREPLACE);
	big->getFrame()->updateObjects();
	const float limits[3] = { 0.0f, 400.0f, 500.0f };
	float v[3] = { -1, -1, -1 };
	uint32 large[3];
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	for(int k = 0; k < 3; k++){
		metal::setShadowCasterMaxRadius(limits[k]);
		for(int i = 0; i < 8; i++)
			CasterFrame([&](bool prepass){
				SetRenderState(CULLMODE, CULLNONE);
				s.floor->render();
				if(prepass)
					metal::addShadowCaster(big, metal::SHADOWCASTER_STATIC);
			});
		metal::rtWaitForTest();
		float len = 0.0f;
		if(!metal::readShadowForTest(320, 240, &v[k], &len))
			v[k] = -1.0f;
		large[k] = metal::getRayStats().castersLarge;
	}
	metal::setShadowCasterMaxRadius(500.0f);
	DestroyAtomic(big);
	FreeShadowScene(&s);
	bool ok = v[0] >= 0.0f && v[0] < 0.05f && v[1] > 0.95f && large[1] == 1 && v[2] >= 0.0f && v[2] < 0.05f && large[2] == 0 &&
		large[0] == 0;
	if(!ok)
		Detail("  no limit %.3f (large %u), 400 m %.3f (large %u), 500 m %.3f (large %u)\n", v[0], large[0], v[1], large[1], v[2], large[2]);
	return Report(ok, name);
}

static bool
CheckShadowStats(void)
{
	const char *name = "the stats line reports shadows, rays, levels, derived streams, instances and their build time, builds and their time, refits, the walk, the pass time and the shadow memory";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	ShadowFrame(&s, metal::SHADOWQUALITY_HIGH, 1);
	metal::RayStats first = metal::getRayStats();
	ShadowFrame(&s, metal::SHADOWQUALITY_HIGH, 2);
	metal::RayStats st = metal::getRayStats();
	metal::FrameInfo fi = metal::getFrameInfo();
	metal::logStats();
	const char *line = metal::getStatsLine();
	SHQuality q = shQuality(metal::SHADOWQUALITY_HIGH);
	uint32 rw = q.halfRes ? (fi.internalWidth + 1)/2 : fi.internalWidth, rh = q.halfRes ? (fi.internalHeight + 1)/2 : fi.internalHeight;
	uint32 want = rw*rh*q.raysPerPixel;
	float wantMem = (float)(((double)rw*rh*2 + 2.0*fi.internalWidth*fi.internalHeight*8 + 16.0 + 8.0*shOwnerListCapacity(rw, rh))/1048576.0);
	const char *t = strstr(line, " shadows ");
	char qn[16] = "", pass[16] = "", topTime[16] = "", buildTime[16] = "";
	unsigned rays = 0, blas = 0, tlas = 0, builds = 0, refits = 0, alphaN = ~0u, texN = ~0u, large = ~0u;
	float blasMB = -1, normalsMB = -1, walk = -1, mem = -1;
	int n = t ? sscanf(t, " shadows %15s rays %u blas %u %fMB normals %fMB tlas %u %15s build %u %15s refit %u walk %fms shadow %15s shadowmem %fMB alpha %u tex %u large %u",
		qn, &rays, &blas, &blasMB, &normalsMB, &tlas, topTime, &builds, buildTime, &refits, &walk, pass, &mem, &alphaN, &texN, &large) : 0;
	FreeShadowScene(&s);
	bool timed = st.passMs < 0.0f ? strcmp(pass, "-") == 0 : strstr(pass, "ms") != nullptr;
	float topMs = -1, buildMs = -1;
	bool topParsed = strcmp(topTime, "-") == 0 || sscanf(topTime, "%fms", &topMs) == 1;
	bool buildParsed = strcmp(buildTime, "-") == 0 || sscanf(buildTime, "%fms", &buildMs) == 1;
	bool counted = st.passMs < 0.0f ||
		(topMs > 0.0f && fabsf(topMs - st.topMs) < 0.006f && buildMs >= 0.0f && fabsf(buildMs - st.buildMs) < 0.006f &&
		 first.frameBuilds > 0 && first.buildMs > 0.0f && first.topMs > 0.0f);
	bool ok = n == 16 && large == 0 && strcmp(qn, "high") == 0 && rays == want && st.raysPerFrame == want && tlas == 2 && blas >= 2 &&
		fabsf(mem - wantMem) <= 0.5f && timed && walk >= 0.0f && topParsed && buildParsed && counted;
	if(!ok)
		Detail("  parsed %d, rays %u (expected %u), pass %.3f ms, instance build %.3f ms, builds %.3f ms, first frame %u builds %.3f ms instance build %.3f ms; line: %s",
			n, rays, want, st.passMs, st.topMs, st.buildMs, first.frameBuilds, first.buildMs, first.topMs, line);
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
CheckAlphaFunctionDecides(void)
{
	const char *name = "with the intersection function forced to accept every candidate an alpha mesh blocks as a solid quad, forced to reject it lets all light through, and the opaque mesh of the same level blocks in both";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *ga, *gb;
	Atomic *a = AlphaRoof(&ga, AlphaMaterial(checkerAlpha), AlphaMaterial(checkerAlpha));
	Atomic *b = MixedRoof(&gb);
	float v[3][4], b0[3], b1[3];
	metal::RayStats st[3];
	const int32 modes[3] = { 1, 2, 0 };
	for(int k = 0; k < 3; k++){
		metal::setRtAlphaFunctionForTest(modes[k]);
		AlphaFrames(&s, 16, [&](bool prepass){
			AlphaRoofPass(a, prepass, 128);
			AlphaRoofPass(b, prepass, 128);
		});
		metal::setRtAlphaFunctionForTest(0);
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
	ok = ok && Dark(v[2][0]) && Lit(v[2][1]) && Lit(v[2][2]) && Dark(v[2][3]);
	ok = ok && Dark(b0[0]) && Dark(b1[0]) && Dark(b0[1]) && Lit(b1[1]) && Dark(b0[2]) && Dark(b1[2]);
	for(int k = 0; k < 3; k++)
		ok = ok && st[k].alphaKernel == 1 && st[k].alphaInstances == 2;
	if(!ok)
		for(int k = 0; k < 3; k++)
			Detail("  %s: A %.3f %.3f %.3f %.3f, B opaque mesh %.3f alpha mesh %.3f, kernel %u alpha instances %u\n",
				k == 0 ? "accept all" : k == 1 ? "reject all" : "texture", v[k][0], v[k][1], v[k][2], v[k][3], b0[k], b1[k],
				st[k].alphaKernel, st[k].alphaInstances);
	return Report(ok, name);
}

static const uint8 patternAlphaB[16] = {
	0, 0, 0, 0,
	255, 255, 0, 0,
	0, 255, 0, 255,
	0, 0, 255, 0 };

static bool
CheckAlphaTablePerScene(void)
{
	const char *name = "two prepass scenes in one frame each bind the alpha table and function table of their own instance level";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *g0, *g1;
	Atomic *roofs[2] = { AlphaRoof(&g0, AlphaMaterial(patternAlpha, 4, 4), AlphaMaterial(patternAlpha, 4, 4)),
		AlphaRoof(&g1, AlphaMaterial(patternAlphaB, 4, 4), AlphaMaterial(patternAlphaB, 4, 4)) };
	SetShadowQuality(metal::SHADOWQUALITY_MEDIUM);
	const void *tables[2][2] = { { nil, nil }, { nil, nil } };
	float v[2] = { -1, -1 };
	for(int order = 0; order < 2; order++){
		for(int f = 0; f < 8; f++)
			for(int k = 0; k < 2; k++){
				Atomic *roof = roofs[order ? 1 - k : k];
				CasterFrame([&](bool prepass){
					SetRenderState(CULLMODE, CULLNONE);
					s.floor->render();
					AlphaRoofPass(roof, prepass, 128);
				}, k == 1);
				if(order == 0)
					tables[f & 1][k] = metal::rtAlphaTableForTest();
			}
		metal::rtWaitForTest();
		v[order] = Floor(1.0f - 0.25f - 5.0f, 1.0f - 0.25f);
	}
	DestroyAtomic(roofs[0]);
	DestroyAtomic(roofs[1]);
	g0->destroy();
	g1->destroy();
	FreeShadowScene(&s);
	bool ok = tables[0][0] && tables[0][1] && tables[0][0] != tables[0][1] && tables[1][0] && tables[1][1] &&
		tables[1][0] != tables[1][1] && Lit(v[0]) && Dark(v[1]);
	if(!ok)
		Detail("  tables %p %p, %p %p; texel under the second scene's roof %.3f (pattern B, want lit), swapped %.3f (pattern A, want dark)\n",
			tables[0][0], tables[0][1], tables[1][0], tables[1][1], v[0], v[1]);
	return Report(ok, name);
}

static bool
CheckAlphaTableReleased(void)
{
	const char *name = "the shadow pass lets go of its intersection function table on a frame without the pass and when the levels are released, and the released table is freed";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Geometry *g;
	Atomic *roof = AlphaRoof(&g, AlphaMaterial(patternAlpha, 4, 4), AlphaMaterial(patternAlpha, 4, 4));
	auto draw = [&](bool prepass){
		SetRenderState(CULLMODE, CULLNONE);
		s.floor->render();
		AlphaRoofPass(roof, prepass, 128);
	};
	AlphaFrames(&s, 4, draw);
	bool bound = metal::rtAlphaTableForTest() != nil;
	TurnSun(&s, -87.134f);
	AlphaFrames(&s, 1, draw);
	bool skipped = metal::rtAlphaTableForTest() == nil;
	TurnSun(&s, -135.0f);
	AlphaFrames(&s, 4, draw);
	bool again = metal::rtAlphaTableForTest() != nil;
	metal::rtWatchAlphaTableForTest();
	SetShadowQuality(metal::SHADOWQUALITY_OFF);
	bool released = metal::rtAlphaTableForTest() == nil;
	for(int i = 0; i < 3; i++)
		CasterFrame(draw);
	metal::rtWaitForTest();
	CasterFrame(draw);
	bool freed = !metal::rtWatchedTableAliveForTest();
	DestroyAtomic(roof);
	g->destroy();
	FreeShadowScene(&s);
	bool ok = bound && skipped && again && released && freed;
	if(!ok)
		Detail("  table bound %d; dropped after a frame without the pass %d; bound again %d; dropped at release %d; freed %d\n", bound, skipped,
			again, released, freed);
	return Report(ok, name);
}

static bool
CheckAlphaCappedLevel(void)
{
	const char *name = "a capped alpha-level instance casts with its opaque mesh and lets light through its alpha mesh";
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
	metal::setRtAlphaCapForTest(0);
	DestroyAtomic(a);
	DestroyAtomic(b);
	ga->destroy();
	gb->destroy();
	FreeShadowScene(&s);
	bool ok = Dark(b0) && Lit(b1) && st.alphaCapped == 1 && st.alphaEntries == 1 && st.alphaKernel == 1;
	if(!ok)
		Detail("  triangle 0 %.3f triangle 1 %.3f, capped %u entries %u kernel %u\n", b0, b1, st.alphaCapped, st.alphaEntries,
			st.alphaKernel);
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

static Atomic*
CutoutQuad(Geometry **geo, uint8 rightAlpha)
{
	RGBA t[16];
	for(int i = 0; i < 16; i++)
		t[i] = { 255, 96, 32, (uint8)(i % 4 < 2 ? 255 : rightAlpha) };
	Material *m = Material::create();
	SurfaceProperties sp = { 1.0f, 0.0f, 1.0f };
	m->surfaceProps = sp;
	Texture *tex = MakeTexture(4, 4, t, Texture::LINEAR, Texture::WRAP, Texture::WRAP);
	m->setTexture(tex);
	if(tex)
		tex->destroy();
	RGBA c = { 120, 120, 120, 255 };
	RGBA col[4] = { c, c, c, c };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	*geo = QuadGeometry(Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT | Geometry::TEXTURED, -5.0f, -1.0f, -3.0f, 1.0f,
		-19.0f, col, uv, m);
	m->destroy();
	return MakeAtomic(*geo);
}

enum { CUTOUT_NONE, CUTOUT_BOTH, CUTOUT_FORWARD };
enum { CUTOUT_ROW0 = 360, CUTOUT_ROWN = 52 };

template<typename F> static void
CutoutFrames(ShadowScene *s, Atomic *quad, int mode, int n, F read)
{
	for(int i = 0; i < n; i++)
		CasterFrame([&](bool prepass){
			SetRenderState(CULLMODE, CULLNONE);
			s->floor->render();
			s->roof->render();
			if(quad && (mode == CUTOUT_BOTH || (mode == CUTOUT_FORWARD && !prepass))){
				SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
				SetRenderState(ALPHATESTREF, 128);
				quad->render();
				SetRenderState(ALPHATESTREF, 3);
			}
			if(!prepass && i == n - 1)
				read();
		});
	metal::rtWaitForTest();
}

static bool
CheckCutoutReceives(void)
{
	const char *name = "an alpha-tested quad under a roof's shadow takes the shadow on its opaque texels, and the floor seen through its clear texels keeps its own factor";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	Geometry *gc, *go;
	Atomic *cut = CutoutQuad(&gc, 0), *opaque = CutoutQuad(&go, 255);
	Atomic *quads[4] = { cut, opaque, cut, nil };
	const int modes[4] = { CUTOUT_BOTH, CUTOUT_BOTH, CUTOUT_FORWARD, CUTOUT_NONE };
	float px[4][2][4];
	bool read = true;
	uint32 cutouts[4], shown = 0, shownDraws = 0, shownEqual = 0, shownUnkeyed = 0;
	bool printed = false;
	for(int k = 0; k < 4; k++){
		metal::StateStats a0 = metal::getStateStats();
		bool got = false;
		CutoutFrames(&s, quads[k], modes[k], 24, [&](){
			got = metal::readScenePixel(SX(-4.5f, 19.0f), 240, px[k][0]) && metal::readScenePixel(SX(-3.4f, 19.0f), 240, px[k][1]);
		});
		read = read && got;
		cutouts[k] = metal::getStateStats().prepassCutoutDraws - a0.prepassCutoutDraws;
		if(k == 0){
			metal::logStats();
			const char *tail = strstr(metal::getStatsLine(), " prepass ");
			printed = tail && sscanf(tail, " prepass %u equal %u unkeyed %u cutout %u", &shownDraws, &shownEqual, &shownUnkeyed,
				&shown) == 4 && shown == 1 && shownDraws == 3 && shownEqual == 3;
		}
	}
	DestroyAtomic(cut);
	DestroyAtomic(opaque);
	gc->destroy();
	go->destroy();
	FreeShadowScene(&s);
	bool shaded = true, floorKept = true;
	for(int i = 0; i < 3; i++){
		shaded = shaded && fabsf(px[0][0][i] - px[1][0][i]) <= 2.0f/255.0f;
		floorKept = floorKept && fabsf(px[0][1][i] - px[3][1][i]) <= 2.0f/255.0f;
	}
	bool sunGone = px[2][0][0] - px[0][0][0] > 0.05f;
	bool ok = read && shaded && floorKept && sunGone && cutouts[0] == 24 && cutouts[1] == 0 && cutouts[2] == 0 && printed;
	Detail("  read %d; opaque texel: cutout %.4f %.4f %.4f, opaque quad %.4f %.4f %.4f, not in the prepass %.4f %.4f %.4f; clear texel: cutout %.4f %.4f %.4f, floor alone %.4f %.4f %.4f; cutout prepass draws %u %u %u; stats line prepass %u equal %u unkeyed %u cutout %u (%d)\n",
			read, px[0][0][0], px[0][0][1], px[0][0][2], px[1][0][0], px[1][0][1], px[1][0][2], px[2][0][0], px[2][0][1], px[2][0][2],
			px[0][1][0], px[0][1][1], px[0][1][2], px[3][1][0], px[3][1][1], px[3][1][2], cutouts[0], cutouts[1], cutouts[2], shownDraws,
			shownEqual, shownUnkeyed, shown, printed);
	return Report(ok, name);
}

static bool
CutoutRow(ShadowScene *s, Atomic *quad, int mode, float row[CUTOUT_ROWN][4])
{
	bool read = true;
	CutoutFrames(s, quad, mode, 2, [&](){
		for(int i = 0; i < CUTOUT_ROWN; i++)
			read = metal::readScenePixel(CUTOUT_ROW0 + i, 240, row[i]) && read;
	});
	return read;
}

static bool
CutoutIdentity(const char *name)
{
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	SetShadowQuality(metal::SHADOWQUALITY_OFF);
	Geometry *g;
	Atomic *cut = CutoutQuad(&g, 0);
	static float rows[3][CUTOUT_ROWN][4];
	metal::StateStats a0 = metal::getStateStats();
	bool read = CutoutRow(&s, cut, CUTOUT_BOTH, rows[0]);
	metal::StateStats a1 = metal::getStateStats();
	read = CutoutRow(&s, cut, CUTOUT_FORWARD, rows[1]) && read;
	metal::RenderSettings r = metal::getRenderSettings();
	r.tier = metal::TIER_ENHANCED;
	metal::setRenderSettings(&r);
	read = CutoutRow(&s, cut, CUTOUT_BOTH, rows[2]) && read;
	DestroyAtomic(cut);
	g->destroy();
	FreeShadowScene(&s);
	uint32 cutouts = a1.prepassCutoutDraws - a0.prepassCutoutDraws;
	float worst[2] = { 0.0f, 0.0f };
	int at[2] = { -1, -1 };
	for(int k = 0; k < 2; k++)
		for(int i = 0; i < CUTOUT_ROWN; i++)
			for(int c = 0; c < 3; c++){
				float d = fabsf(rows[k][i][c] - rows[2][i][c]);
				if(d > worst[k]){
					worst[k] = d;
					at[k] = CUTOUT_ROW0 + i;
				}
			}
	float span = 0.0f;
	for(int i = 0; i < CUTOUT_ROWN; i++)
		span = fmaxf(span, fabsf(rows[2][i][1] - rows[2][0][1]));
	bool ok = read && worst[0] <= 1.0f/255.0f && worst[1] <= 1.0f/255.0f && cutouts == 2 && span > 0.1f;
	Detail("  read %d; largest step from the enhanced row: prepass cutout %.4f at x %d, cutout not in the prepass %.4f at x %d; cutout prepass draws %u; quad contrast %.3f\n",
			read, worst[0], at[0], worst[1], at[1], cutouts, span);
	return Report(ok, name);
}

static Geometry*
LayerQuadGeometry(float x0, float x1, const uint8 alpha[4], bool skinned)
{
	RGBA t[16];
	for(int i = 0; i < 16; i++)
		t[i] = { 255, 96, 32, alpha[i % 4] };
	Material *m = Material::create();
	SurfaceProperties sp = { 1.0f, 0.0f, 1.0f };
	m->surfaceProps = sp;
	Texture *tex = MakeTexture(4, 4, t, Texture::LINEAR, Texture::CLAMP, Texture::CLAMP);
	m->setTexture(tex);
	if(tex)
		tex->destroy();
	RGBA c = { 120, 120, 120, 255 };
	RGBA col[4] = { c, c, c, c };
	TexCoords uv[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	uint32 flags = Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT | Geometry::TEXTURED;
	Geometry *geo = QuadGeometry(skinned ? flags | Geometry::NORMALS : flags, x0, -1.0f, x1, 1.0f, -19.0f, col, uv, m);
	m->destroy();
	if(skinned){
		for(int i = 0; i < 4; i++)
			geo->morphTargets[0].normals[i].set(0.0f, 0.0f, 1.0f);
		Matrix ident;
		ident.setIdentity();
		uint8 idx[4][4] = {};
		float wts[4][4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };
		AttachSkin(geo, 1, &ident, idx, wts);
	}
	return geo;
}

template<typename B, typename A> static void
LayerFrames(ShadowScene *s, Atomic *quad, bool pair, int n, B before, A after)
{
	for(int i = 0; i < n; i++)
		CasterFrame([&](bool prepass){
			SetRenderState(CULLMODE, CULLNONE);
			s->floor->render();
			s->roof->render();
			bool last = !prepass && i == n - 1;
			if(last)
				before();
			if(quad){
				SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
				SetRenderState(ALPHATESTREF, 128);
				if(pair){
					SetRenderState(GSALPHATEST, 1);
					SetRenderState(GSALPHATESTREF, 128);
				}
				quad->render();
				SetRenderState(GSALPHATEST, 0);
				SetRenderState(ALPHATESTREF, 3);
			}
			if(last)
				after();
		});
	metal::rtWaitForTest();
}

struct LayerRun
{
	float px[4];
	float depth;
	uint32 cutouts, shadowDraws;
	bool read;
};

static LayerRun
LayerPixel(ShadowScene *s, Atomic *quad, bool pair, int x, int y)
{
	LayerRun r = {};
	float n[3], m[2];
	bool before = false, after = false;
	metal::StateStats a0 = metal::getStateStats();
	LayerFrames(s, quad, pair, 24, [&](){
		before = metal::readPrepassForTest(x, y, n, m, &r.depth);
	}, [&](){
		after = metal::readScenePixel(x, y, r.px);
	});
	metal::StateStats a1 = metal::getStateStats();
	r.cutouts = a1.prepassCutoutDraws - a0.prepassCutoutDraws;
	r.shadowDraws = a1.relitShadowDraws - a0.relitShadowDraws;
	r.read = before && after;
	return r;
}

static float
BlendStep(const float got[4], float a, const float src[4], const float dst[4])
{
	float worst = 0.0f;
	for(int c = 0; c < 3; c++)
		worst = fmaxf(worst, fabsf(got[c] - (a*src[c] + (1.0f - a)*dst[c])));
	return worst;
}

static LayerRun
LayerQuadPixel(ShadowScene *s, float x0, float x1, const uint8 alpha[4], bool skinned, bool pair, int x)
{
	Geometry *g = LayerQuadGeometry(x0, x1, alpha, skinned);
	Atomic *a = skinned ? SkinAtomic(g) : MakeAtomic(g);
	if(skinned)
		PoseSkin(a, 1);
	LayerRun r = LayerPixel(s, a, pair, x, 240);
	if(skinned)
		DestroySkinAtomic(a);
	else
		DestroyAtomic(a);
	g->destroy();
	return r;
}

static void
CheckCutoutLayer(void)
{
	const char *names[4] = {
		"a cutout texel of alpha 0.75 inside a roof's shadow over a lit floor is darkened as the same quad drawn opaque, within the blend's share",
		"a cutout texel of alpha 0.75 in the sun over a shadowed floor is lit",
		"the fringe of a PS2 alpha test pair takes the shadow variant and the factor of its pixel",
		"the main prepass depth under a cutout texel of alpha 0.75 is the floor's, and the floor behind it is drawn",
	};
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	const uint8 edge[4] = { 255, 255, 191, 191 }, soft[4] = { 191, 191, 191, 191 }, fringe[4] = { 64, 64, 255, 255 };
	const uint8 solid[4] = { 255, 255, 255, 255 };
	int xa = SX(-3.4f, 19.0f), xb = SX(-5.5f, 19.0f), xc = SX(-4.5f, 19.0f), xs = SX(-5.0f, 20.0f), xl = SX(-2.0f, 20.0f);
	LayerRun fa = LayerPixel(&s, nil, false, xa, 240), fs = LayerPixel(&s, nil, false, xs, 240);
	LayerRun fl = LayerPixel(&s, nil, false, xl, 240), fc = LayerPixel(&s, nil, false, xc, 240);
	LayerRun ca = LayerQuadPixel(&s, -5.0f, -3.0f, edge, false, false, xa), oa = LayerQuadPixel(&s, -5.0f, -3.0f, solid, false, false, xa);
	LayerRun cb = LayerQuadPixel(&s, -6.0f, -4.0f, soft, false, false, xb), ob = LayerQuadPixel(&s, -6.0f, -4.0f, solid, false, false, xb);
	LayerRun cc = LayerQuadPixel(&s, -5.0f, -3.0f, fringe, false, true, xc), oc = LayerQuadPixel(&s, -5.0f, -3.0f, solid, false, true, xc);
	FreeShadowScene(&s);
	bool read = fa.read && fs.read && fl.read && fc.read && ca.read && oa.read && cb.read && ob.read && cc.read && oc.read;
	float stepA = BlendStep(ca.px, 191/255.0f, oa.px, fs.px), stepB = BlendStep(cb.px, 191/255.0f, ob.px, fl.px);
	float stepC = BlendStep(cc.px, 64/255.0f, oc.px, fc.px);
	float sunA = fl.px[0] - fs.px[0];
	bool okA = read && stepA <= 2.0f/255.0f && ca.cutouts == 24 && sunA > 0.05f;
	bool okB = read && stepB <= 2.0f/255.0f && cb.cutouts == 24;
	bool okC = read && stepC <= 2.0f/255.0f && cc.cutouts == 24 && cc.shadowDraws == oc.shadowDraws + 24;
	bool okD = read && fabsf(ca.depth - fa.depth) <= 1e-6f && fa.depth < 1.0f;
	Detail("  read %d; in shadow over the lit floor: cutout %.4f %.4f %.4f, opaque quad %.4f %.4f %.4f, shadowed floor %.4f %.4f %.4f, lit floor %.4f %.4f %.4f, step %.4f, cutout draws %u\n",
		read, ca.px[0], ca.px[1], ca.px[2], oa.px[0], oa.px[1], oa.px[2], fs.px[0], fs.px[1], fs.px[2], fl.px[0], fl.px[1], fl.px[2], stepA,
		ca.cutouts);
	Detail("  in the sun over the shadowed floor: cutout %.4f %.4f %.4f, opaque quad %.4f %.4f %.4f, step %.4f, cutout draws %u\n",
		cb.px[0], cb.px[1], cb.px[2], ob.px[0], ob.px[1], ob.px[2], stepB, cb.cutouts);
	Detail("  fringe: pair %.4f %.4f %.4f, opaque quad %.4f %.4f %.4f, floor %.4f %.4f %.4f, step %.4f, cutout draws %u, shadow draws %u against %u opaque\n",
		cc.px[0], cc.px[1], cc.px[2], oc.px[0], oc.px[1], oc.px[2], fc.px[0], fc.px[1], fc.px[2], stepC, cc.cutouts, cc.shadowDraws,
		oc.shadowDraws);
	Detail("  main prepass depth under the 0.75 texel %.7f, floor alone %.7f\n", ca.depth, fa.depth);
	Report(okA, names[0]);
	Report(okB, names[1]);
	Report(okC, names[2]);
	Report(okD, names[3]);
}

static float
LayerMegabytes(void)
{
	metal::logStats();
	const char *t = strstr(metal::getStatsLine(), " layer ");
	float mb = -1.0f;
	return t && sscanf(t, " layer %fMB", &mb) == 1 ? mb : -1.0f;
}

static bool
CheckCutoutLayerReleased(void)
{
	const char *name = "the cutout layer's targets are released when ShadowQuality turns off or the modern tier is left, and built again by the next cutout frame";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	Geometry *gc;
	Atomic *cut = CutoutQuad(&gc, 0);
	float mb[5];
	CutoutFrames(&s, cut, CUTOUT_BOTH, 3, [](){});
	mb[0] = LayerMegabytes();
	SetShadowQuality(metal::SHADOWQUALITY_OFF);
	CutoutFrames(&s, cut, CUTOUT_BOTH, 3, [](){});
	mb[1] = LayerMegabytes();
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	CutoutFrames(&s, cut, CUTOUT_BOTH, 3, [](){});
	mb[2] = LayerMegabytes();
	metal::RenderSettings r = metal::getRenderSettings();
	r.tier = metal::TIER_ENHANCED;
	metal::setRenderSettings(&r);
	CutoutFrames(&s, cut, CUTOUT_BOTH, 3, [](){});
	mb[3] = LayerMegabytes();
	r.tier = metal::TIER_MODERN;
	metal::setRenderSettings(&r);
	CutoutFrames(&s, cut, CUTOUT_BOTH, 3, [](){});
	mb[4] = LayerMegabytes();
	DestroyAtomic(cut);
	gc->destroy();
	FreeShadowScene(&s);
	bool built = mb[0] > 1.0f && mb[2] == mb[0] && mb[4] == mb[0];
	bool ok = built && mb[1] == 0.0f && mb[3] == 0.0f;
	Detail("  layer megabytes: shadows on %.2f, off %.2f, on again %.2f, enhanced tier %.2f, modern tier again %.2f\n",
		mb[0], mb[1], mb[2], mb[3], mb[4]);
	return Report(ok, name);
}

static bool
CheckSkinnedCutoutLayer(void)
{
	const char *name = "a skinned cutout texel of alpha 0.75 inside a roof's shadow over a lit floor is darkened as the same skinned quad drawn opaque, within the blend's share";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	const uint8 edge[4] = { 255, 255, 191, 191 }, solid[4] = { 255, 255, 255, 255 };
	int xa = SX(-3.4f, 19.0f), xs = SX(-5.0f, 20.0f);
	LayerRun fs = LayerPixel(&s, nil, false, xs, 240);
	LayerRun ca = LayerQuadPixel(&s, -5.0f, -3.0f, edge, true, false, xa), oa = LayerQuadPixel(&s, -5.0f, -3.0f, solid, true, false, xa);
	FreeShadowScene(&s);
	bool read = fs.read && ca.read && oa.read;
	float step = BlendStep(ca.px, 191/255.0f, oa.px, fs.px);
	bool ok = read && step <= 2.0f/255.0f && ca.cutouts == 24 && oa.cutouts == 0;
	Detail("  read %d; skinned cutout %.4f %.4f %.4f, skinned opaque quad %.4f %.4f %.4f, shadowed floor %.4f %.4f %.4f, step %.4f, cutout draws %u %u\n",
		read, ca.px[0], ca.px[1], ca.px[2], oa.px[0], oa.px[1], oa.px[2], fs.px[0], fs.px[1], fs.px[2], step, ca.cutouts, oa.cutouts);
	return Report(ok, name);
}

static bool
CheckSkinnedCutoutSolid(void)
{
	const char *name = "a skinned cutout in a roof's shadow over a lit floor takes the shadow on its solid texels and shows the floor through its clear ones";
	ShadowScene s;
	MakeShadowScene(&s, 0.5f);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
	const uint8 right[4] = { 0, 0, 255, 255 }, solid[4] = { 255, 255, 255, 255 };
	int xSolid = SX(-3.5f, 19.0f), xClear = SX(-4.7f, 19.0f);
	LayerRun floorClear = LayerPixel(&s, nil, false, xClear, 240);
	LayerRun cut = LayerQuadPixel(&s, -5.0f, -3.0f, right, true, false, xSolid), cutClear = LayerQuadPixel(&s, -5.0f, -3.0f, right, true, false, xClear);
	LayerRun opaque = LayerQuadPixel(&s, -5.0f, -3.0f, solid, true, false, xSolid);
	SetShadowQuality(metal::SHADOWQUALITY_OFF);
	LayerRun unshaded = LayerQuadPixel(&s, -5.0f, -3.0f, right, true, false, xSolid);
	FreeShadowScene(&s);
	bool read = floorClear.read && cut.read && cutClear.read && opaque.read && unshaded.read;
	float solidStep = 0.0f, clearStep = 0.0f;
	for(int c = 0; c < 3; c++){
		solidStep = fmaxf(solidStep, fabsf(cut.px[c] - opaque.px[c]));
		clearStep = fmaxf(clearStep, fabsf(cutClear.px[c] - floorClear.px[c]));
	}
	float shade = unshaded.px[0] - cut.px[0];
	bool ok = read && solidStep <= 2.0f/255.0f && clearStep <= 2.0f/255.0f && shade > 0.05f && cut.cutouts == 24 && cutClear.cutouts == 24;
	Detail("  read %d; solid texel %.4f %.4f %.4f, the same quad opaque %.4f %.4f %.4f, without shadows %.4f %.4f %.4f; clear texel %.4f %.4f %.4f, floor alone %.4f %.4f %.4f; cutout draws %u %u\n",
		read, cut.px[0], cut.px[1], cut.px[2], opaque.px[0], opaque.px[1], opaque.px[2], unshaded.px[0], unshaded.px[1], unshaded.px[2],
		cutClear.px[0], cutClear.px[1], cutClear.px[2], floorClear.px[0], floorClear.px[1], floorClear.px[2], cut.cutouts, cutClear.cutouts);
	return Report(ok, name);
}

static Geometry*
OwnerShell(Material *mat, float x, float z, float outer, float inner, float gap0, float gap1)
{
	enum { SEG = 8, CUTS = 4 };
	const float ys[CUTS] = { -3.0f, gap0, gap1, 3.0f };
	auto open = [](int slab, int k){ return slab == 1 && (k == 1 || k == 2); };
	int tris = 0;
	for(int sh = 0; sh < 2; sh++)
		for(int slab = 0; slab < CUTS - 1; slab++)
			for(int k = 0; k < SEG; k++)
				tris += open(slab, k) ? 0 : 2;
	Geometry *geo = Geometry::create(2*CUTS*SEG, tris, Geometry::POSITIONS | Geometry::NORMALS | Geometry::LIGHT);
	V3d *v = geo->morphTargets[0].vertices, *n = geo->morphTargets[0].normals;
	for(int sh = 0; sh < 2; sh++)
		for(int c = 0; c < CUTS; c++)
			for(int k = 0; k < SEG; k++){
				float a = 6.28318531f*k/SEG, r = sh ? inner : outer, d = sh ? -1.0f : 1.0f;
				int i = (sh*CUTS + c)*SEG + k;
				v[i].set(x + r*cosf(a), ys[c], z + r*sinf(a));
				n[i].set(d*cosf(a), 0.0f, d*sinf(a));
			}
	Triangle *t = geo->triangles;
	for(int sh = 0; sh < 2; sh++)
		for(int slab = 0; slab < CUTS - 1; slab++)
			for(int k = 0; k < SEG; k++){
				if(open(slab, k))
					continue;
				int lo = sh*CUTS*SEG + slab*SEG, hi = lo + SEG, k1 = (k + 1) % SEG;
				int a0 = lo + k, a1 = hi + k, b0 = lo + k1, b1 = hi + k1;
				t[0].v[0] = a0; t[0].v[1] = sh ? b0 : a1; t[0].v[2] = b1; t[0].matId = 0;
				t[1].v[0] = a0; t[1].v[1] = b1; t[1].v[2] = sh ? a1 : b0; t[1].matId = 0;
				t += 2;
			}
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static Geometry*
OwnerBox(Material *mat, V3d lo, V3d hi, bool skinned)
{
	const V3d c[6][4] = {
		{ { lo.x, lo.y, lo.z }, { lo.x, hi.y, lo.z }, { lo.x, hi.y, hi.z }, { lo.x, lo.y, hi.z } },
		{ { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { hi.x, lo.y, hi.z } },
		{ { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { lo.x, lo.y, hi.z } },
		{ { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } },
		{ { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z } },
		{ { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } } };
	const V3d out[6] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
	if(!skinned)
		return FacedQuads(mat, c, out, 6);
	Geometry *geo = Geometry::create(24, 12, Geometry::POSITIONS | Geometry::TEXTURED | Geometry::NORMALS | Geometry::PRELIT);
	V3d *v = geo->morphTargets[0].vertices;
	uint8 idx[24][4] = {};
	float wts[24][4] = {};
	for(int f = 0; f < 6; f++){
		int o[4] = { 0, 1, 2, 3 };
		if(dot(cross(sub(c[f][1], c[f][0]), sub(c[f][2], c[f][0])), out[f]) < 0.0f)
			std::swap(o[1], o[3]);
		for(int i = 0; i < 4; i++){
			v[4*f + i] = c[f][o[i]];
			geo->morphTargets[0].normals[4*f + i] = out[f];
			geo->colors[4*f + i] = { 100, 100, 100, 255 };
			geo->texCoords[0][4*f + i] = { 0.0f, 0.0f };
			wts[4*f + i][0] = 1.0f;
		}
		Triangle *t = &geo->triangles[2*f];
		t[0].v[0] = 4*f; t[0].v[1] = 4*f + 1; t[0].v[2] = 4*f + 2; t[0].matId = 0;
		t[1].v[0] = 4*f; t[1].v[1] = 4*f + 2; t[1].v[2] = 4*f + 3; t[1].matId = 0;
	}
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	Matrix ident;
	ident.setIdentity();
	AttachSkin(geo, 1, &ident, idx, wts);
	return geo;
}

static Geometry*
OwnerPanel(Material *mat, float x, float y0, float y1, float z0, float z1)
{
	Geometry *geo = Geometry::create(4, 2, Geometry::POSITIONS | Geometry::PRELIT | Geometry::LIGHT | Geometry::TEXTURED);
	V3d c[4] = { { x, y0, z0 }, { x, y1, z0 }, { x, y1, z1 }, { x, y0, z1 } }, out = { -1.0f, 0.0f, 0.0f };
	TexCoords uv[4] = { { 0.1f, 0.5f }, { 0.1f, 0.5f }, { 0.9f, 0.5f }, { 0.9f, 0.5f } };
	int o[4] = { 0, 1, 2, 3 };
	if(dot(cross(sub(c[1], c[0]), sub(c[2], c[0])), out) < 0.0f)
		std::swap(o[1], o[3]);
	for(int i = 0; i < 4; i++){
		geo->morphTargets[0].vertices[i] = c[o[i]];
		geo->texCoords[0][i] = uv[o[i]];
		geo->colors[i] = { 120, 120, 120, 255 };
	}
	geo->triangles[0].v[0] = 0; geo->triangles[0].v[1] = 1; geo->triangles[0].v[2] = 2; geo->triangles[0].matId = 0;
	geo->triangles[1].v[0] = 0; geo->triangles[1].v[1] = 2; geo->triangles[1].v[2] = 3; geo->triangles[1].matId = 0;
	geo->matList.appendMaterial(mat);
	geo->calculateBoundingSphere();
	geo->unlock();
	return geo;
}

static const float ownerX = 1.5f, ownerZ = -15.0f, ownerR = 4.0f;
static const V3d ownerSeatLo = { 1.1f, -1.5f, -11.6f }, ownerSeatHi = { 1.5f, 0.6f, -11.45f };
static const V3d ownerRiderLo = { 1.55f, -1.5f, -11.8f }, ownerRiderHi = { 1.85f, 0.6f, -11.5f };
static const float ownerPanelX = 1.3f, ownerPanelZ0 = -11.75f, ownerPanelZ1 = -11.32f;

struct OwnerScene
{
	ShadowScene s;
	Geometry *bodyGeo, *seatGeo;
	Atomic *body, *seat;
	V3d toSun;
};

static void
MakeOwnerScene(OwnerScene *o, bool inner = true)
{
	MakeShadowScene(&o->s, 0.5f);
	SetSunRadius(0.0f);
	TurnSun(&o->s, -100.0f);
	o->toSun = scale(o->s.sunFrame->getLTM()->at, -1.0f);
	o->bodyGeo = inner ? OwnerShell(o->s.mat, ownerX, ownerZ, ownerR, 3.8f, 0.8f, 2.6f) : TerminatorCylinder(o->s.mat, ownerX, ownerR, ownerZ, false);
	o->body = MakeAtomic(o->bodyGeo);
	o->seatGeo = OwnerBox(o->s.mat, ownerSeatLo, ownerSeatHi, false);
	o->seat = MakeAtomic(o->seatGeo);
	SetShadowQuality(metal::SHADOWQUALITY_HIGH);
}

static void
FreeOwnerScene(OwnerScene *o)
{
	metal::setShadowOwnerRuleForTest(true);
	metal::setShadowOwnerListCapForTest(~0u);
	SetRenderState(CULLMODE, CULLNONE);
	DestroyAtomic(o->body);
	DestroyAtomic(o->seat);
	o->bodyGeo->destroy();
	o->seatGeo->destroy();
	FreeShadowScene(&o->s);
}

template<typename F> static void
OwnerFrames(OwnerScene *o, uint32 seatOwner, int n, F extra)
{
	metal::cameraCut();
	for(int k = 0; k < n; k++)
		CasterFrame([&](bool prepass){
			SetRenderState(CULLMODE, CULLBACK);
			o->s.floor->render();
			if(prepass)
				metal::setShadowOwner(1);
			o->body->render();
			if(prepass)
				metal::setShadowOwner(seatOwner);
			o->seat->render();
			if(prepass)
				metal::setShadowOwner(0);
			extra(prepass, k);
		});
	metal::rtWaitForTest();
}

static int
OwnerCol(float degrees)
{
	float a = degrees*3.14159265f/180.0f;
	return SX(ownerX + ownerR*cosf(a), -ownerZ - ownerR*sinf(a));
}

static bool
OwnerRayStart(int col, float *px, float *pz)
{
	const float a90 = 3.14159265f/2.0f, a135 = 3.0f*3.14159265f/4.0f;
	float ax = ownerX + ownerR*cosf(a90), az = ownerZ + ownerR*sinf(a90);
	float bx = ownerX + ownerR*cosf(a135), bz = ownerZ + ownerR*sinf(a135);
	float k = 1.0f - (col + 0.5f)/320.0f;
	float u = (-k*az - ax)/((bx - ax) + k*(bz - az));
	*px = ax + u*(bx - ax);
	*pz = az + u*(bz - az);
	return u > 0.03f && u < 0.97f;
}

static int
OwnerRayCrosses(float px, float pz, V3d L, V3d lo, V3d hi, float m)
{
	auto hits = [&](float g){
		float t0 = 0.0f, t1 = 100.0f;
		const float o[2] = { px, pz }, d[2] = { L.x, L.z }, a[2] = { lo.x + g, lo.z + g }, b[2] = { hi.x - g, hi.z - g };
		for(int i = 0; i < 2; i++){
			if(fabsf(d[i]) < 1e-6f){
				if(o[i] < a[i] || o[i] > b[i])
					return false;
				continue;
			}
			float ta = (a[i] - o[i])/d[i], tb = (b[i] - o[i])/d[i];
			t0 = fmaxf(t0, fminf(ta, tb));
			t1 = fminf(t1, fmaxf(ta, tb));
		}
		return t0 <= t1;
	};
	return hits(m) ? 1 : !hits(-m) ? 0 : -1;
}

static bool
OwnerScanline(OwnerScene *o, uint32 seatOwner, float row[][4], int c0, int n)
{
	bool read = true;
	OwnerFrames(o, seatOwner, 17, [&](bool prepass, int k){
		if(!prepass && k == 16)
			for(int i = 0; i < n; i++)
				read = metal::readScenePixel(c0 + i, 240, row[i]) && read;
	});
	return read;
}

static void
OwnerSteps(float row[][4], int n, float *step, float *rise, int *at)
{
	*step = *rise = 0.0f;
	*at = -1;
	for(int i = 0; i + 1 < n; i++)
		for(int ch = 0; ch < 3; ch++){
			float d = row[i][ch] - row[i + 1][ch];
			if(d > *step){
				*step = d;
				*at = i;
			}
			*rise = fmaxf(*rise, -d);
		}
}

static bool
CheckOwnerTerminator(void)
{
	const char *name = "a closed body with an inner seat and an inner far panel, all under one owner, shades to its terminator under a low sun";
	enum { N = 192 };
	OwnerScene o;
	MakeOwnerScene(&o);
	int c0 = OwnerCol(45.0f), n = std::min(OwnerCol(135.0f) - c0 + 1, (int)N);
	static float row[2][N][4];
	bool read = n > 40;
	for(int pass = 0; pass < 2; pass++){
		metal::setShadowOwnerRuleForTest(pass == 0);
		read = OwnerScanline(&o, 1, row[pass], c0, n) && read;
	}
	metal::RayStats st = metal::getRayStats();
	FreeOwnerScene(&o);
	float step[2], rise[2];
	int at[2];
	for(int pass = 0; pass < 2; pass++)
		OwnerSteps(row[pass], n, &step[pass], &rise[pass], &at[pass]);
	bool ok = read && st.ownedInstances == 2 && step[0] <= 4.0f/255.0f && rise[0] <= 1.0f/1024.0f && step[1] > 4.0f/255.0f;
	Detail("  read %d over columns %d to %d; owned instances %u; with the owner rule: largest drop %.1f/255 after column %d, largest rise %.5f; without it: largest drop %.1f/255 after column %d\n",
		read, c0, c0 + n - 1, st.ownedInstances, step[0]*255.0f, c0 + at[0], rise[0], step[1]*255.0f, c0 + at[1]);
	return Report(ok, name);
}

template<typename Expect, typename F> static bool
OwnerFactors(const char *name, uint32 seatOwner, Expect expect, F extra, bool inner = true)
{
	OwnerScene o;
	MakeOwnerScene(&o, inner);
	OwnerFrames(&o, seatOwner, 17, extra);
	int c0 = OwnerCol(90.0f), c1 = OwnerCol(135.0f), dark = 0, lit = 0, wrong = 0, firstWrong = -1;
	bool read = true;
	float got = -1.0f;
	for(int c = c0; c <= c1; c++){
		float px, pz;
		if(!OwnerRayStart(c, &px, &pz))
			continue;
		int e = expect(px, pz, o.toSun);
		if(e < 0)
			continue;
		float v = -1.0f, len = 0.0f;
		read = metal::readShadowForTest(c, 240, &v, &len) && read;
		bool good = e ? Dark(v) : Lit(v);
		(e ? dark : lit)++;
		if(!good){
			wrong++;
			if(firstWrong < 0){
				firstWrong = c;
				got = v;
			}
		}
	}
	FreeOwnerScene(&o);
	bool ok = read && dark >= 5 && lit >= 5 && wrong == 0;
	Detail("  read %d; columns %d to %d: %d expected dark, %d expected lit, %d wrong (first at column %d, factor %.3f)\n", read, c0, c1, dark,
		lit, wrong, firstWrong, got);
	return Report(ok, name);
}

static bool
CheckOwnerSeatOther(void)
{
	return OwnerFactors("a closed body whose seat has another owner is shadowed on its near side where the seat lies on the ray path and lit beside it", 2,
		[](float px, float pz, V3d L){ return OwnerRayCrosses(px, pz, L, ownerSeatLo, ownerSeatHi, 0.03f); }, [](bool, int){});
}

static const V3d ownerOtherLo = { 6.0f, -1.5f, -10.6f }, ownerOtherHi = { 7.0f, 0.6f, -10.35f };

static bool
CheckOwnerOtherBody(void)
{
	Material *m = Material::create();
	Geometry *g = OwnerBox(m, ownerOtherLo, ownerOtherHi, false);
	m->destroy();
	Atomic *a = MakeAtomic(g);
	bool ok = OwnerFactors("a closed body is shadowed by a second body with another owner standing toward the sun", 1,
		[](float px, float pz, V3d L){ return OwnerRayCrosses(px, pz, L, ownerOtherLo, ownerOtherHi, 0.03f); },
		[&](bool prepass, int){
			if(prepass)
				metal::setShadowOwner(2);
			a->render();
			if(prepass)
				metal::setShadowOwner(0);
		}, false);
	DestroyAtomic(a);
	g->destroy();
	return ok;
}

static bool
CheckOwnerRider(bool skinned)
{
	Material *m = Material::create();
	Geometry *g = OwnerBox(m, ownerRiderLo, ownerRiderHi, skinned);
	m->destroy();
	Atomic *a = skinned ? SkinAtomic(g) : MakeAtomic(g);
	if(skinned)
		PoseSkin(a, 1);
	bool ok = OwnerFactors(skinned ? "a skinned caster added after the owner scope closes still shadows the body around it" :
		"a caster drawn after the owner scope closes still shadows the body around it", 1,
		[](float px, float pz, V3d L){ return OwnerRayCrosses(px, pz, L, ownerRiderLo, ownerRiderHi, 0.03f); },
		[&](bool prepass, int){
			if(skinned){
				if(prepass)
					metal::addSkinnedShadowCaster(a);
			}else
				a->render();
		});
	if(skinned)
		DestroySkinAtomic(a);
	else
		DestroyAtomic(a);
	g->destroy();
	return ok;
}

static bool
CheckOwnerOutsideClass(void)
{
	const char *name = "pixels outside the terminator class are bit-equal with the owner rule on and off: the floor, the body's sunlit top and the far inner panel seen through an opening";
	struct Area { int x0, x1, y0, y1; };
	const Area areas[4] = { { 200, 440, 120, 120 }, { 382, 445, 200, 200 }, { OwnerCol(50.0f), OwnerCol(85.0f), 240, 240 }, { 302, 330, 270, 282 } };
	enum { MAXPX = 1024 };
	static float px[2][MAXPX][4];
	OwnerScene o;
	MakeOwnerScene(&o);
	bool read = true;
	int count = 0;
	float dTop = -1.0f, dPanel = -1.0f;
	for(int pass = 0; pass < 2; pass++){
		metal::setShadowOwnerRuleForTest(pass == 0);
		OwnerFrames(&o, 1, 17, [&](bool prepass, int k){
			if(prepass || k != 16)
				return;
			int i = 0;
			for(const Area &a : areas)
				for(int y = a.y0; y <= a.y1; y++)
					for(int x = a.x0; x <= a.x1 && i < MAXPX; x++)
						read = metal::readScenePixel(x, y, px[pass][i++]) && read;
			count = i;
		});
	}
	float nrm[3], mo[2];
	read = metal::readPrepassForTest(OwnerCol(110.0f), 240, nrm, mo, &dTop) && metal::readPrepassForTest(316, 276, nrm, mo, &dPanel) && read;
	FreeOwnerScene(&o);
	int unequal = 0;
	for(int i = 0; i < count; i++)
		unequal += memcmp(px[0][i], px[1][i], 3*sizeof(float)) != 0;
	bool ok = read && count > 400 && unequal == 0 && dPanel > dTop;
	Detail("  read %d; %d pixels compared, %d not bit-equal; depth through the opening %.6f, at the near side %.6f\n", read, count, unequal,
		dPanel, dTop);
	return Report(ok, name);
}

static bool
CheckOwnerCutout(void)
{
	const char *name = "an owned body with a cutout part lets the sun through its clear texels";
	ShadowScene s;
	MakeShadowScene(&s, 1.0f);
	Material *m = AlphaMaterial(patternAlpha, 4, 4);
	Geometry *g;
	Atomic *roof = AlphaRoof(&g, m, m);
	AlphaFrames(&s, 16, [&](bool prepass){
		if(prepass)
			metal::setShadowOwner(1);
		AlphaRoofPass(roof, prepass, 128);
		if(prepass)
			metal::setShadowOwner(0);
	});
	float v[16];
	for(int r = 0; r < 4; r++)
		for(int c = 0; c < 4; c++)
			v[r*4 + c] = Floor(1.0f - (c + 0.5f)/2.0f - 5.0f, 1.0f - (r + 0.5f)/2.0f);
	metal::RayStats st = metal::getRayStats();
	DestroyAtomic(roof);
	g->destroy();
	FreeShadowScene(&s);
	bool ok = st.alphaKernel == 1 && st.ownedInstances == 1;
	for(int i = 0; i < 16; i++)
		ok = ok && (patternAlpha[i] ? Dark(v[i]) : Lit(v[i]));
	Detail("  floor under texel rows: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f; alpha kernel %u, owned instances %u\n",
		v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], st.alphaKernel, st.ownedInstances);
	return Report(ok, name);
}

static bool
CheckOwnerCutoutRetrace(void)
{
	static const uint8 halfAlpha[2] = { 255, 0 };
	Material *m = AlphaMaterial(halfAlpha, 2, 1);
	Geometry *g = OwnerPanel(m, ownerPanelX, -1.5f, 1.5f, ownerPanelZ0, ownerPanelZ1);
	m->destroy();
	Atomic *a = MakeAtomic(g);
	const float mid = 0.5f*(ownerPanelZ0 + ownerPanelZ1);
	bool ok = OwnerFactors("a listed pixel whose second trace meets another owner's cutout part is lit through its clear texels and shadowed by its solid ones", 1,
		[mid](float px, float pz, V3d L){
			if(px >= ownerPanelX)
				return -1;
			float z = pz + (ownerPanelX - px)/L.x*L.z;
			if(z > ownerPanelZ0 + 0.03f && z < mid - 0.03f)
				return 1;
			if(z > mid + 0.03f && z < ownerPanelZ1 - 0.03f)
				return 0;
			return -1;
		},
		[&](bool prepass, int){
			if(!prepass)
				return;
			metal::setShadowOwner(2);
			metal::addShadowCaster(a, metal::SHADOWCASTER_STATIC);
			metal::setShadowOwner(0);
		});
	DestroyAtomic(a);
	g->destroy();
	return ok;
}

static bool
CheckOwnerListFull(void)
{
	const char *name = "with the owner list full, a listed pixel keeps the result it has without the owner rule";
	enum { N = 192 };
	OwnerScene o;
	MakeOwnerScene(&o);
	int c0 = OwnerCol(45.0f), n = std::min(OwnerCol(135.0f) - c0 + 1, (int)N);
	static float row[3][N][4];
	bool read = n > 40;
	for(int pass = 0; pass < 3; pass++){
		metal::setShadowOwnerRuleForTest(pass != 0);
		metal::setShadowOwnerListCapForTest(pass == 1 ? 0u : ~0u);
		read = OwnerScanline(&o, 1, row[pass], c0, n) && read;
	}
	FreeOwnerScene(&o);
	int full = 0, listed = 0;
	for(int i = 0; i < n; i++){
		full += memcmp(row[0][i], row[1][i], 3*sizeof(float)) != 0;
		listed += memcmp(row[0][i], row[2][i], 3*sizeof(float)) != 0;
	}
	bool ok = read && full == 0 && listed > 5;
	Detail("  read %d over columns %d to %d; with the list full %d pixels not bit-equal to the rule off; with room %d pixels change\n", read, c0,
		c0 + n - 1, full, listed);
	return Report(ok, name);
}

static int
OwnerListLine(unsigned *owned, unsigned *listed, unsigned *overflow)
{
	metal::logStats();
	const char *t = strstr(metal::getStatsLine(), " owned ");
	*owned = *listed = *overflow = ~0u;
	return t ? sscanf(t, " owned %u listed %u overflow %u", owned, listed, overflow) : 0;
}

static bool
CheckOwnerListOverflow(void)
{
	const char *name = "a body that fills the screen lists a third of its pixels without overflowing the owner list, and the stats line reports the list's fill and its overflow";
	OwnerScene o;
	MakeOwnerScene(&o);
	V3d forward = { 0.0f, 0.0f, -9.5f };
	rtCamera->getFrame()->translate(&forward, COMBINEPOSTCONCAT);
	rtCamera->getFrame()->updateObjects();
	OwnerFrames(&o, 1, 17, [](bool, int){});
	metal::RayStats full = metal::getRayStats();
	metal::FrameInfo fi = metal::getFrameInfo();
	unsigned owned[2], listed[2], overflow[2];
	int parsed[2];
	parsed[0] = OwnerListLine(&owned[0], &listed[0], &overflow[0]);
	uint32 tenth = full.ownerListed/10;
	metal::setShadowOwnerListCapForTest(tenth);
	OwnerFrames(&o, 1, 17, [](bool, int){});
	metal::RayStats cut = metal::getRayStats();
	parsed[1] = OwnerListLine(&owned[1], &listed[1], &overflow[1]);
	FreeOwnerScene(&o);
	uint32 texels = (uint32)(fi.internalWidth*fi.internalHeight);
	bool ok = parsed[0] == 3 && parsed[1] == 3 && owned[0] == full.ownedInstances && listed[0] == full.ownerListed &&
		overflow[0] == full.ownerOverflow && listed[1] == cut.ownerListed && overflow[1] == cut.ownerOverflow &&
		full.ownerListed*4 > texels && full.ownerOverflow == 0 && cut.ownerListed == full.ownerListed &&
		cut.ownerOverflow == full.ownerListed - tenth;
	Detail("  stats line parsed %d and %d: owned %u listed %u overflow %u, then owned %u listed %u overflow %u; of %u texels the list took %u with overflow %u; with the list cut to a tenth (%u) it took %u with overflow %u\n",
		parsed[0], parsed[1], owned[0], listed[0], overflow[0], owned[1], listed[1], overflow[1], texels, full.ownerListed, full.ownerOverflow, tenth,
		cut.ownerListed, cut.ownerOverflow);
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
	CasterFrame([](bool){});
	metal::rtWaitForTest();
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
	CheckLevelRequested();
	CheckDestroyedInBracket();
	CheckShadowOnScreen();
	CheckShadowAccumulates();
	CheckShadowHistoryCameraMotion();
	CheckShadowHistoryPan();
	CheckShadowHistoryPlaneNormal();
	CheckShadowHistoryRestarts();
	CheckShadowSkipped();
	CheckShadowOffReleases();
	CheckShadowSunOnly();
	CheckPlainShadowTwin();
	CheckPrelitClassicShadow();
	CheckKeptBakedShadow();
	CheckShadowContact();
	CheckShadowUpsampleEdge();
	CheckShadowPhaseStable();
	CheckShadowPanSmear();
	CheckShadowFacingAway();
	CheckShadowEmptyNormal();
	CheckShadowSmoothRim();
	CheckShadowTerminator();
	CheckShadowTerminatorMirrored();
	CheckShadowTerminatorFloor();
	CheckShadowTerminatorRoof();
	CheckShadowWallFacingAway();
	CheckShadowWallFacingAwayRelit();
	CheckShadowWallFacingAwayTwoSided();
	CheckPlainTwinFacetNearCamera();
	CheckShadowContactWall();
	CheckShadowContactBody();
	CheckShadowContactOwned();
	CheckShadowCreaseGround();
	CheckOwnerTerminator();
	CheckOwnerSeatOther();
	CheckOwnerOtherBody();
	CheckOwnerRider(false);
	CheckOwnerRider(true);
	CheckOwnerOutsideClass();
	CheckOwnerCutout();
	CheckOwnerCutoutRetrace();
	CheckOwnerListFull();
	CheckOwnerListOverflow();
	CheckShadowClassDark();
	CheckShadowSkippedWall();
	CheckPlainTwinFailure();
	CheckCasterRadiusLimit();
	CheckShadowStats();
	CheckAlphaCheckerboard();
	CheckAlphaThreshold();
	CheckAlphaBlendedCastNothing();
	CheckAlphaGeometries();
	CheckAlphaKernelChoice();
	CheckAlphaTextureLifetime();
	CheckAlphaCap();
	CheckAlphaUnderOpaque();
	CheckAlphaFunctionDecides();
	CheckAlphaTablePerScene();
	CheckAlphaTableReleased();
	CheckAlphaCappedLevel();
	CheckAlphaIndexGuard();
	CheckAlphaTextureGone();
	CheckAlphaRefSceneOnly();
	CheckAlphaAddressAxes();
	CheckSkinnedCasterUndrawn();
	CheckCutoutReceives();
	CutoutIdentity("a cutout draw with shadows off draws the enhanced frame's colour, and one the prepass did not draw keeps its own depth test, at one sample");
	CheckCutoutLayer();
	CheckCutoutLayerReleased();
	CheckSkinnedCutoutLayer();
	CheckSkinnedCutoutSolid();
	PrintShadowTerminator();
	PrintBuildTiming();
	PrintCasterTiming();
	PrintShadowTiming();
	WorldClose();
	return failures;
}

int
RunRaytraceSampleChecks(Camera *camera)
{
	failures = 0;
	rtCamera = camera;
	if(metal::rtAvailable()){
		CheckShadowSkippedSamples();
		CutoutIdentity("a cutout draw with shadows off draws the enhanced frame's colour, and one the prepass did not draw keeps its own depth test, at four samples");
	}
	return failures;
}
