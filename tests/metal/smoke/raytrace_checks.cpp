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

static Camera *rtCamera;

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
	CheckBottomBuilds();
	CheckBottomLifetime();
	CheckBuildBudget();
	PrintBuildTiming();
	WorldClose();
	return failures;
}
