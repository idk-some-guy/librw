#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <rw.h>
#include <src/metal/rwmetalimpl.h>
#include <src/metal/metalraytrace.h>
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
StripGeometry(int quads, float z)
{
	int nv = 2*(quads + 1);
	Geometry *geo = Geometry::create(nv, 2*quads, Geometry::POSITIONS | Geometry::PRELIT | Geometry::TRISTRIP);
	V3d *v = geo->morphTargets[0].vertices;
	for(int i = 0; i <= quads; i++){
		v[2*i].set((float)i, 0.0f, z);
		v[2*i+1].set((float)i, 1.0f, z);
	}
	for(int i = 0; i < nv; i++)
		geo->colors[i] = { 200, 200, 200, 255 };
	Material *mat = Material::create();
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
	metal::setRenderSettings(&s);
}

static void
CasterTiersOff(void)
{
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
	PrintBuildTiming();
	PrintCasterTiming();
	WorldClose();
	return failures;
}
