#ifdef RW_METAL
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>
#include "metalobjc.h"
#include "metalstate.h"
#include "metalnormals.h"
#include "metalprepass.h"
#include "metalframe.h"
#include "metalshadow.h"
#include "metallights.h"
#include "metalraytrace.h"
#include "shaders/raytrace_metal.inc"
#include "shaders/shadowh_metal.inc"
#include "shaders/shadow_metal.inc"

extern const char *lightsh_metal_src;

namespace rw {
namespace metal {

extern const char *prepassh_metal_src;

enum { RT_HEAP_SIZE = 64<<20, RT_EVICT_FRAMES = 120, RT_CUT_FACTOR = 8, RT_BUILD_TRIANGLES = 50000 };
static const uint64 RT_BUDGET = 256ull<<20;

enum { LEVEL_QUEUED, LEVEL_BUILT, LEVEL_NONE };

struct BottomLevel
{
	id<MTLAccelerationStructure> accel;
	MTLPrimitiveAccelerationStructureDescriptor *desc;
	uint64 serial;
	uint64 lastUsed;
	uint64 bytes;
	uint32 triangles;
	uint32 geometries;
	int32 state;
	std::vector<uint32> alphaMeshes;
};

struct QueueEntry
{
	const InstanceDataHeader *header;
	uint64 serial;
};

struct Retired
{
	id held;
	uint64 frame;
	uint64 bytes;
	uint64 heapBytes;
};

struct Taken
{
	const InstanceDataHeader *header;
	BottomLevel *level;
	bool backlog;
};

enum { REFUSE_BUDGET, REFUSE_HEAP, REFUSE_FRAGMENTED };

static struct
{
	bool available;
	std::unordered_map<const InstanceDataHeader*, BottomLevel> bottoms;
	std::deque<QueueEntry> queue;
	std::vector<Retired> retired;
	std::vector<id<MTLHeap>> heaps;
	uint64 heapBytes;
	uint64 usedBytes;
	uint64 budget;
	uint32 buildBudgetTest;
	uint64 serial;
	uint64 frame;
	std::atomic<uint64> completed;
	uint32 spent;
	uint32 backlog;
	bool cutFrame, cutPending;
	uint32 logs;
	char refusal[256];
	bool scratchFail;
	id<MTLBuffer> scratch;
	std::unordered_map<const InstanceDataHeader*, float32> radii;
	RayStats stats;
} rt;

enum { RT_MAX_INSTANCES = 8192, RT_MAX_SKINNED = 48, RT_SKIN_REBUILD_FRAMES = 30, RT_SKIN_KEEP_FRAMES = 30 };
enum { RT_MAX_ALPHA_ENTRIES = 4096, RT_MAX_ALPHA_TEXTURES = 1024, RT_ALPHA_TRIANGLE = 24 };
enum { RT_SAMPLE_SKIN = 0, RT_SAMPLE_SKINNED = 2, RT_SAMPLE_TOP = 4, RT_SAMPLE_BOTTOM = 6, RT_BUILD_SAMPLES = 8 };
static const float32 RT_SKIN_DISTANCE = 100.0f;
static const float32 RT_CASTER_RADIUS = 500.0f;
static const int32 RT_SKIN_MODE = RT_SKIN_REFIT;
static const char *raytraceHead =
"#include <metal_stdlib>\n#include <metal_raytracing>\nusing namespace metal;\nusing namespace raytracing;\n";

struct CasterKey
{
	const InstanceDataHeader *header;
	const void *object;
	bool operator==(const CasterKey &o) const { return header == o.header && object == o.object; }
};

struct CasterKeyHash
{
	size_t operator()(const CasterKey &k) const
	{
		return std::hash<const void*>()(k.header) ^ (std::hash<const void*>()(k.object) * 0x9E3779B97F4A7C15ull);
	}
};

struct Caster
{
	InstanceDataHeader *header;
	const void *object;
	RawMatrix world;
	float32 dist2;
	uint32 mask;
	int32 bones;
	uint32 owner;
};

struct SkinnedLevel
{
	const InstanceDataHeader *header;
	uint32 vertices;
	MTLAccelerationStructureUsage usage;
	id<MTLBuffer> positions;
	MTLPrimitiveAccelerationStructureDescriptor *desc;
	id<MTLAccelerationStructure> accel;
	uint64 bytes;
	NSUInteger buildScratch, refitScratch;
	uint32 triangles, geometries;
	uint64 lastUsed, builtFrame;
	bool built, empty;
};

struct TopSlot
{
	id<MTLAccelerationStructure> accel;
	id<MTLBuffer> instances;
	id<MTLBuffer> bones;
	id<MTLBuffer> alpha;
	id<MTLBuffer> alphaParams;
	id<MTLIntersectionFunctionTable> table;
	uint64 bytes;
	uint64 frame;
};

struct SkinJobHost
{
	uint32 stride, pos, weights, indices, count;
};

static struct
{
	std::vector<Caster> casters;
	std::unordered_map<CasterKey, uint32, CasterKeyHash> byKey;
	std::vector<RawMatrix> bones;
	std::unordered_map<const void*, SkinnedLevel> skinned;
	TopSlot tops[MAXFRAMESINFLIGHT];
	uint32 topIndex;
	uint64 topBytes, skinnedBytes;
	id<MTLBuffer> topScratch, skinScratch;
	id<MTLComputePipelineState> skinPipe, tracePipe;
	float32 eye[3];
	bool open, workOff;
	uint32 cap, hostCount, owner;
	uint64 adds, palettes;
	int32 skinModeTest;
	id<MTLAccelerationStructure> current, last;
	id<MTLBuffer> currentAlpha, lastAlpha, currentParams, lastParams;
	id<MTLIntersectionFunctionTable> currentTable, lastTable, passTable;
	std::vector<__weak id<MTLTexture>> alphaTextures;
	uint32 alphaCap, skinCapTest;
	float32 skinDistanceTest;
	int32 alphaFunctionTest;
	bool alphaIndexPast, alphaLost;
	__weak id<MTLTexture> watched;
	__weak id<MTLIntersectionFunctionTable> watchedTable;
	id<MTLCounterSampleBuffer> timing, frameTiming, buildTiming[MAXFRAMESINFLIGHT];
	uint32 timed, buildSlot;
	std::atomic<uint64> topTicks, buildTicks;
	float32 walkMs;
	MTLTimestamp cpu0, gpu0;
	std::chrono::steady_clock::time_point wall0;
	std::vector<id<MTLBuffer>> deferred;
} rf;

static float32 casterMaxRadius = RT_CASTER_RADIUS;

static const float32 SH_RAY_DISTANCE = 300.0f;

struct ShadowRayParamsHost
{
	simd_float4x4 invViewProj, viewToWorld;
	simd_float4 sun, eye;
	simd_uint2 size, depthSize;
	uint32 frame, raysPerPixel, halfRes, alphaEntries, terminatorOff, ownerOff, listCap;
};

struct ShadowAccumParamsHost
{
	simd_float4x4 invViewProj, prevInvViewProj, viewToWorld;
	simd_float4 eye, prevEye;
	simd_uint2 size, rawSize;
	uint32 halfRes, valid;
	float32 maxHistory, pad;
};

struct ShadowAlphaEntryHost
{
	MTLResourceID tex;
	float32 threshold;
	uint32 address;
};

static_assert(sizeof(ShadowAlphaEntryHost) == 16, "shadow alpha entry");
static_assert(sizeof(ShadowRayParamsHost) == 208, "shadow ray params");
static_assert(sizeof(ShadowAccumParamsHost) == 256, "shadow accumulation params");
static_assert(sizeof(LightRaySlotHost) == 64, "light ray slot");
static_assert(LT_MAX_SHADOWED == 8, "RayStats carries eight light slots");

static struct
{
	id<MTLComputePipelineState> rays, raysAlpha, accumulate[4], clear, ownerReset, ownerFix;
	id<MTLBuffer> ownerList, ownerTaken;
	std::atomic<uint32> listed, overflow;
	id<MTLComputePipelineState> lightRays, lightRaysAlpha, lightFix;
	id<MTLTexture> lightRaw;
	id<MTLBuffer> lightList, lightCounts[MAXFRAMESINFLIGHT];
	std::atomic<uint32> lightRayCount[LT_MAX_SHADOWED];
	uint32 lightListCap, lightFrame, lightCountSlot;
	uint64 lightBytes;
	bool lightReported;
	id<MTLFunction> alphaHit;
	bool forceAlpha, terminatorOff, ownerOff;
	uint32 listCapacity, listCapTest = ~0u;
	id<MTLTexture> raw, history[2];
	uint32 index, frame, width, height, samples;
	int32 quality;
	bool valid, fresh, ran, reported;
	uint64 bytes;
	simd_float4x4 prevInvViewProj;
	simd_float3 prevEye, prevSun;
	id<MTLCounterSampleBuffer> timing[MAXFRAMESINFLIGHT];
	uint32 timingSlot;
	std::atomic<uint64> ticks;
	MTLTimestamp cpu0, gpu0;
	std::chrono::steady_clock::time_point wall0;
} sh;

static uint64 tickSpan(id<MTLCounterSampleBuffer> sb, uint32 first, uint32 last);
static id<MTLAccelerationStructureCommandEncoder> timedBuild(id<MTLCommandBuffer> cb, uint32 first);

static id<MTLCounterSet>
timestampCounters(id<MTLDevice> dev)
{
	if(![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
		return nil;
	for(id<MTLCounterSet> s in dev.counterSets)
		if([s.name isEqualToString:MTLCommonCounterSetTimestamp])
			return s;
	return nil;
}

static id<MTLCounterSampleBuffer>
sampleBuffer(id<MTLDevice> dev, id<MTLCounterSet> timestamps, uint32 count)
{
	if(timestamps == nil)
		return nil;
	MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
	sd.counterSet = timestamps;
	sd.storageMode = MTLStorageModeShared;
	sd.sampleCount = count;
	return [dev newCounterSampleBufferWithDescriptor:sd error:nil];
}

static id<MTLCounterSampleBuffer>
twoSampleBuffer(id<MTLDevice> dev, id<MTLCounterSet> timestamps)
{
	return sampleBuffer(dev, timestamps, 2);
}

static void
initShadows(id<MTLDevice> dev)
{
	NSMutableString *src = [NSMutableString string];
	const char *parts[] = { raytraceHead, prepassh_metal_src, shadowh_metal_src, lightsh_metal_src, shadow_metal_src };
	for(const char *p : parts)
		[src appendString:[NSString stringWithUTF8String:p]];
	MTLCompileOptions *o = [MTLCompileOptions new];
	if(@available(macOS 15.0, *))
		o.mathMode = MTLMathModeRelaxed;
	else
		o.fastMathEnabled = NO;
	NSError *err = nil;
	id<MTLLibrary> lib = [dev newLibraryWithSource:src options:o error:&err];
	id<MTLFunction> rays = lib ? [lib newFunctionWithName:@"shadowRays"] : nil;
	id<MTLFunction> raysAlpha = lib ? [lib newFunctionWithName:@"shadowRaysAlpha"] : nil;
	sh.alphaHit = lib ? [lib newFunctionWithName:@"shadowAlphaHit"] : nil;
	sh.rays = rays ? [dev newComputePipelineStateWithFunction:rays error:&err] : nil;
	bool accumulate = lib != nil;
	for(uint32 k = 0; k < 4; k++){
		MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
		[cv setConstantValue:&k type:MTLDataTypeUInt atIndex:0];
		id<MTLFunction> f = lib ? [lib newFunctionWithName:@"shadowAccumulate" constantValues:cv error:&err] : nil;
		sh.accumulate[k] = f ? [dev newComputePipelineStateWithFunction:f error:&err] : nil;
		accumulate = accumulate && sh.accumulate[k];
	}
	sh.clear = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"shadowClear"] error:&err] : nil;
	sh.ownerReset = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"shadowOwnerReset"] error:&err] : nil;
	sh.ownerFix = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"shadowOwnerFix"] error:&err] : nil;
	sh.ownerTaken = [dev newBufferWithLength:sizeof(uint32) options:MTLResourceStorageModeShared];
	if(sh.rays == nil || !accumulate || sh.clear == nil || sh.ownerTaken == nil){
		sh.rays = sh.clear = sh.ownerReset = sh.ownerFix = nil;
		for(uint32 k = 0; k < 4; k++)
			sh.accumulate[k] = nil;
		fprintf(stderr, "rw::metal: the shadow kernels failed to build; ray-traced shadows are off: %s\n",
			err ? err.localizedDescription.UTF8String : "");
	}
	if(sh.rays && (sh.ownerReset == nil || sh.ownerFix == nil))
		fprintf(stderr, "rw::metal: the shadow owner kernel failed to build; class rays keep hits on their own body\n");
	NSError *alphaErr = nil;
	if(sh.rays && raysAlpha && sh.alphaHit){
		MTLComputePipelineDescriptor *pd = [MTLComputePipelineDescriptor new];
		pd.computeFunction = raysAlpha;
		MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
		linked.functions = @[ sh.alphaHit ];
		pd.linkedFunctions = linked;
		sh.raysAlpha = [dev newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&alphaErr];
	}else
		sh.raysAlpha = nil;
	if(sh.rays && (sh.raysAlpha == nil || dev.argumentBuffersSupport != MTLArgumentBuffersTier2))
		fprintf(stderr, "rw::metal: the alpha-tested shadow kernel is unavailable; meshes with texture alpha cast no shadow: %s\n",
			alphaErr ? alphaErr.localizedDescription.UTF8String : "argument buffers tier 2 missing");
	sh.lightRays = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lightShadowRays"] error:&err] : nil;
	sh.lightFix = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lightShadowOwnerFix"] error:&err] : nil;
	id<MTLFunction> lightAlpha = lib ? [lib newFunctionWithName:@"lightShadowRaysAlpha"] : nil;
	sh.lightRaysAlpha = nil;
	if(sh.rays && lightAlpha && sh.alphaHit){
		MTLComputePipelineDescriptor *pd = [MTLComputePipelineDescriptor new];
		pd.computeFunction = lightAlpha;
		MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
		linked.functions = @[ sh.alphaHit ];
		pd.linkedFunctions = linked;
		sh.lightRaysAlpha = [dev newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&alphaErr];
	}
	if(sh.rays && (sh.lightRays == nil || sh.lightFix == nil))
		fprintf(stderr, "rw::metal: the light ray kernels failed to build; shadowed lights are unshadowed: %s\n",
			err ? err.localizedDescription.UTF8String : "");
	for(uint32 i = 0; i < MAXFRAMESINFLIGHT; i++)
		sh.lightCounts[i] = [dev newBufferWithLength:LT_MAX_SHADOWED*sizeof(uint32) options:MTLResourceStorageModeShared];
	id<MTLCounterSet> timestamps = timestampCounters(dev);
	for(uint32 i = 0; i < MAXFRAMESINFLIGHT; i++){
		sh.timing[i] = twoSampleBuffer(dev, timestamps);
		rf.buildTiming[i] = sampleBuffer(dev, timestamps, RT_BUILD_SAMPLES);
	}
	sh.timingSlot = 0;
	rf.buildSlot = 0;
	[dev sampleTimestamps:&sh.cpu0 gpuTimestamp:&sh.gpu0];
	sh.wall0 = std::chrono::steady_clock::now();
}

static void
retireAccel(id<MTLAccelerationStructure> a, uint64 bytes)
{
	rt.retired.push_back({ a, rt.frame, bytes, 0 });
}

static void
retireHeld(id held, uint64 heapBytes)
{
	if(held)
		rt.retired.push_back({ held, rt.frame, 0, heapBytes });
}

static std::unordered_map<const void*, SkinnedLevel>::iterator
retireSkinned(std::unordered_map<const void*, SkinnedLevel>::iterator it)
{
	if(it->second.accel){
		retireAccel(it->second.accel, it->second.bytes);
		rf.skinnedBytes -= it->second.bytes;
	}
	return rf.skinned.erase(it);
}

bool
rtInit(void)
{
	MetalContext *ctx = getContext();
	rt.available = ctx && ctx->device.supportsRaytracing;
	rt.budget = RT_BUDGET;
	if(rt.available && rf.skinPipe == nil){
		NSString *src = [[NSString stringWithUTF8String:raytraceHead] stringByAppendingString:
			[NSString stringWithUTF8String:raytrace_metal_src]];
		NSError *err = nil;
		id<MTLLibrary> lib = [ctx->device newLibraryWithSource:src options:nil error:&err];
		rf.skinPipe = lib ? [ctx->device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"skinPositions"] error:&err] : nil;
		rf.tracePipe = lib ? [ctx->device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"traceOne"] error:&err] : nil;
		if(rf.skinPipe == nil || rf.tracePipe == nil)
			fprintf(stderr, "rw::metal: the shadow caster kernels failed to build; skinned casters are left out: %s\n",
				err ? err.localizedDescription.UTF8String : "");
	}
	if(rt.available && sh.rays == nil)
		initShadows(ctx->device);
	return rt.available;
}

void
rtTerm(void)
{
	rt.bottoms.clear();
	rt.queue.clear();
	rt.retired.clear();
	rt.spent = 0;
	rt.backlog = 0;
	rt.cutFrame = false;
	rt.cutPending = false;
	rt.heaps.clear();
	rt.heapBytes = 0;
	rt.usedBytes = 0;
	rt.scratch = nil;
	rt.radii.clear();
	rt.stats.bottomLevels = 0;
	rf.casters.clear();
	rf.byKey.clear();
	rf.bones.clear();
	rf.skinned.clear();
	for(TopSlot &t : rf.tops)
		t = TopSlot();
	rf.topIndex = 0;
	rf.topBytes = rf.skinnedBytes = 0;
	rf.topScratch = rf.skinScratch = nil;
	rf.current = rf.last = nil;
	rf.currentAlpha = rf.lastAlpha = rf.currentParams = rf.lastParams = nil;
	rf.currentTable = rf.lastTable = rf.passTable = nil;
	rf.alphaTextures.clear();
	rf.open = false;
	rf.skinPipe = rf.tracePipe = nil;
	rf.timing = rf.frameTiming = nil;
	for(uint32 i = 0; i < MAXFRAMESINFLIGHT; i++)
		rf.buildTiming[i] = nil;
	rf.topTicks.store(0);
	rf.buildTicks.store(0);
	rf.deferred.clear();
	rt.stats.instances = rt.stats.skinnedLevels = 0;
	rtDropShadowTargets();
	rtDropLightTargets();
	sh.rays = sh.raysAlpha = sh.clear = sh.ownerReset = sh.ownerFix = nil;
	sh.lightRays = sh.lightRaysAlpha = sh.lightFix = nil;
	for(uint32 i = 0; i < MAXFRAMESINFLIGHT; i++)
		sh.lightCounts[i] = nil;
	sh.ownerTaken = nil;
	sh.listed.store(0);
	sh.overflow.store(0);
	for(uint32 k = 0; k < 4; k++)
		sh.accumulate[k] = nil;
	sh.alphaHit = nil;
	for(uint32 i = 0; i < MAXFRAMESINFLIGHT; i++)
		sh.timing[i] = nil;
	sh.ticks.store(0);
	sh.valid = sh.ran = false;
	sh.frame = 0;
	sh.lightFrame = 0;
}

bool
rtAvailable(void)
{
	return rt.available;
}

uint64
rtFrame(void)
{
	return rt.frame;
}

static void
beginRayFrame(uint64 frame)
{
	rt.frame = frame;
	rt.spent = 0;
	if(rt.cutFrame)
		rt.backlog = (uint32)rt.queue.size();
	rt.cutFrame = rt.cutPending;
	rt.cutPending = false;
}

void
rtBeginFrame(void)
{
	beginRayFrame(rt.frame + 1);
}

void
rtCameraCut(void)
{
	rt.cutPending = true;
}

static uint32
buildBudget(void)
{
	uint32 b = rt.buildBudgetTest ? rt.buildBudgetTest : (uint32)RT_BUILD_TRIANGLES;
	return rt.cutFrame || rt.backlog > 0 ? b*RT_CUT_FACTOR : b;
}

static const AttribDesc*
castTexCoords(const InstanceDataHeader *h)
{
	if(h->vertexBuffer == nil)
		return nil;
	for(int32 i = 0; i < h->numAttribs; i++)
		if(h->attribDesc[i].index == ATTRIB_TEXCOORDS0)
			return h->attribDesc[i].format == ATTRIBFMT_FLOAT2 ? &h->attribDesc[i] : nil;
	return nil;
}

static int
meshCasts(const InstanceData *inst, bool texCoords, bool alphaCasters)
{
	Material *m = inst->material;
	if(m == nil)
		return SH_CAST_NONE;
	Raster *r = m->texture ? m->texture->raster : nil;
	bool textureAlpha = r && r->platform == PLATFORM_METAL && GETMETALRASTEREXT(r)->hasAlpha;
	return shMeshCast(inst->vertexAlpha != 0, m->color.alpha, textureAlpha, texCoords, alphaCasters);
}

static const AttribDesc*
castPosition(const InstanceDataHeader *h)
{
	for(int32 i = 0; i < h->numAttribs; i++)
		if(h->attribDesc[i].index == ATTRIB_POS)
			return h->attribDesc[i].format == ATTRIBFMT_FLOAT3 && h->attribDesc[i].offset == 0 ? &h->attribDesc[i] : nil;
	return nil;
}

static MTLPrimitiveAccelerationStructureDescriptor*
levelDescriptor(const InstanceDataHeader *h, id<MTLBuffer> vertices, uint32 stride, bool alpha, uint32 *triangles, uint32 *geometries,
	std::vector<uint32> *alphaMeshes)
{
	if(vertices == nil || h->indexBuffer == nil)
		return nil;
	const AttribDesc *uv = castTexCoords(h);
	bool alphaCasters = alpha && rtAlphaCastersSupported();
	std::vector<uint32> all, starts, counts, meshes;
	uint32 alphaCount = 0;
	for(int32 pass = 0; pass < 2; pass++)
		for(uint32 m = 0; m < h->numMeshes; m++){
			const InstanceData *inst = &h->inst[m];
			int cast = meshCasts(inst, uv != nil, alphaCasters);
			if(pass == 0 && alpha && cast == SH_CAST_NONE && meshCasts(inst, true, true) == SH_CAST_ALPHA)
				rt.stats.alphaSkipped++;
			if(cast != (pass == 0 ? SH_CAST_ALPHA : SH_CAST_OPAQUE))
				continue;
			uint32 at = (uint32)all.size();
			uint32 n = appendMeshList((const uint16*)((const uint8*)h->indexBuffer + inst->offset), inst->numIndex,
				h->primType == MTLPrimitiveTypeTriangleStrip, &all);
			if(n == 0)
				continue;
			starts.push_back(at);
			counts.push_back(n);
			meshes.push_back(m);
			alphaCount += pass == 0;
		}
	if(all.empty())
		return nil;
	id<MTLDevice> dev = getContext()->device;
	id<MTLBuffer> ib = [dev newBufferWithBytes:all.data() length:all.size()*4 options:MTLResourceStorageModeShared];
	if(ib == nil)
		return nil;
	std::vector<NSUInteger> dataOffsets;
	std::vector<float32> data;
	for(uint32 i = 0; i < alphaCount; i++){
		size_t at = (data.size()*4 + 255) & ~(size_t)255;
		data.resize(at/4 + counts[i]*2);
		dataOffsets.push_back(at);
		float32 *out = &data[at/4];
		for(uint32 k = 0; k < counts[i]; k++){
			const float32 *t = (const float32*)(h->vertexBuffer + (size_t)all[starts[i] + k]*uv->stride + uv->offset);
			out[2*k] = t[0];
			out[2*k + 1] = t[1];
		}
	}
	id<MTLBuffer> pb = nil;
	if(alphaCount){
		pb = [dev newBufferWithBytes:data.data() length:data.size()*4 options:MTLResourceStorageModeShared];
		if(pb == nil)
			return nil;
	}
	NSMutableArray *geos = [NSMutableArray array];
	*triangles = 0;
	for(size_t i = 0; i < starts.size(); i++){
		MTLAccelerationStructureTriangleGeometryDescriptor *g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		g.vertexBuffer = vertices;
		g.vertexBufferOffset = 0;
		g.vertexStride = stride;
		g.vertexFormat = MTLAttributeFormatFloat3;
		g.indexBuffer = ib;
		g.indexBufferOffset = (NSUInteger)starts[i]*4;
		g.indexType = MTLIndexTypeUInt32;
		g.triangleCount = counts[i]/3;
		g.opaque = i >= alphaCount;
		if(i < alphaCount){
			g.primitiveDataBuffer = pb;
			g.primitiveDataBufferOffset = dataOffsets[i];
			g.primitiveDataStride = RT_ALPHA_TRIANGLE;
			g.primitiveDataElementSize = RT_ALPHA_TRIANGLE;
		}
		[geos addObject:g];
		*triangles += counts[i]/3;
	}
	MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	d.geometryDescriptors = geos;
	*geometries = (uint32)starts.size();
	if(alphaMeshes)
		alphaMeshes->assign(meshes.begin(), meshes.begin() + alphaCount);
	return d;
}

static int32
ensureBottomState(InstanceDataHeader *h)
{
	if(!rt.available || h == nil)
		return LEVEL_NONE;
	auto it = rt.bottoms.find(h);
	if(it != rt.bottoms.end()){
		it->second.lastUsed = rt.frame;
		return it->second.state;
	}
	BottomLevel b = {};
	b.serial = ++rt.serial;
	b.lastUsed = rt.frame;
	b.state = LEVEL_QUEUED;
	if(castPosition(h) == nil){
		b.state = LEVEL_NONE;
		rt.stats.levelsSkipped++;
	}
	rt.bottoms[h] = b;
	if(b.state == LEVEL_QUEUED)
		rt.queue.push_back({ h, b.serial });
	return b.state;
}

bool
rtEnsureBottom(InstanceDataHeader *h)
{
	return ensureBottomState(h) == LEVEL_BUILT;
}

static void
dropLevel(std::unordered_map<const InstanceDataHeader*, BottomLevel>::iterator it, bool retire)
{
	if(it->second.state == LEVEL_BUILT){
		rt.stats.bottomLevels--;
		if(retire)
			retireAccel(it->second.accel, it->second.bytes);
		else
			rt.usedBytes -= it->second.bytes;
	}
	rt.bottoms.erase(it);
}

static void
releaseRetired(void)
{
	uint64 done = rt.completed.load();
	size_t n = 0;
	for(Retired &r : rt.retired){
		if(r.frame <= done){
			rt.usedBytes -= r.bytes;
			rt.heapBytes -= r.heapBytes;
		}else
			rt.retired[n++] = r;
	}
	rt.retired.resize(n);
}

void
rtForgetHeader(InstanceDataHeader *h)
{
	auto it = rt.bottoms.find(h);
	if(it != rt.bottoms.end())
		dropLevel(it, true);
	rt.radii.erase(h);
	for(auto s = rf.skinned.begin(); s != rf.skinned.end();)
		s = s->second.header == h ? retireSkinned(s) : std::next(s);
	for(Caster &c : rf.casters)
		if(c.header == h){
			rf.byKey.erase({ h, c.object });
			c.header = nil;
		}
}

void
rtReleaseLevels(void)
{
	for(auto it = rt.bottoms.begin(); it != rt.bottoms.end();){
		auto next = std::next(it);
		if(it->second.state != LEVEL_NONE)
			dropLevel(it, true);
		it = next;
	}
	rt.queue.clear();
	rt.backlog = 0;
	for(auto it = rf.skinned.begin(); it != rf.skinned.end();)
		it = retireSkinned(it);
	for(TopSlot &t : rf.tops){
		if(t.accel){
			retireAccel(t.accel, t.bytes);
			rf.topBytes -= t.bytes;
		}
		retireHeld(t.instances, 0);
		retireHeld(t.bones, 0);
		retireHeld(t.alpha, 0);
		retireHeld(t.alphaParams, 0);
		retireHeld(t.table, 0);
		t = TopSlot();
	}
	retireHeld(rt.scratch, 0);
	retireHeld(rf.topScratch, 0);
	retireHeld(rf.skinScratch, 0);
	rt.scratch = nil;
	rf.topScratch = rf.skinScratch = nil;
	for(id<MTLHeap> h : rt.heaps)
		retireHeld(h, RT_HEAP_SIZE);
	rt.heaps.clear();
	rf.current = rf.last = nil;
	rf.currentAlpha = rf.lastAlpha = rf.currentParams = rf.lastParams = nil;
	rf.currentTable = rf.lastTable = rf.passTable = nil;
}

static bool
evictOne(void)
{
	auto victim = rt.bottoms.end();
	uint64 done = rt.completed.load();
	for(auto it = rt.bottoms.begin(); it != rt.bottoms.end(); ++it){
		const BottomLevel &b = it->second;
		if(b.state == LEVEL_BUILT && b.lastUsed + RT_EVICT_FRAMES < rt.frame && b.lastUsed <= done &&
		   (victim == rt.bottoms.end() || b.lastUsed < victim->second.lastUsed))
			victim = it;
	}
	if(victim == rt.bottoms.end())
		return false;
	dropLevel(victim, false);
	return true;
}

static id<MTLHeap>
newHeap(void)
{
	MTLHeapDescriptor *d = [MTLHeapDescriptor new];
	d.size = RT_HEAP_SIZE;
	d.storageMode = MTLStorageModePrivate;
	d.hazardTrackingMode = MTLHazardTrackingModeTracked;
	d.type = MTLHeapTypeAutomatic;
	return [getContext()->device newHeapWithDescriptor:d];
}

static id<MTLAccelerationStructure>
allocAccel(MTLSizeAndAlign sa, int32 *cause)
{
	for(;;){
		if(rt.usedBytes + sa.size <= rt.budget){
			for(id<MTLHeap> h : rt.heaps)
				if([h maxAvailableSizeWithAlignment:sa.align] >= sa.size){
					id<MTLAccelerationStructure> a = [h newAccelerationStructureWithSize:sa.size];
					if(a)
						return a;
				}
			if(rt.heapBytes + RT_HEAP_SIZE <= RT_BUDGET){
				id<MTLHeap> h = newHeap();
				if(h){
					rt.heaps.push_back(h);
					rt.heapBytes += RT_HEAP_SIZE;
					continue;
				}
				*cause = REFUSE_HEAP;
			}else
				*cause = REFUSE_FRAGMENTED;
		}else
			*cause = REFUSE_BUDGET;
		if(!evictOne())
			return nil;
	}
}

static void
logRefusal(int32 cause)
{
	double mb = rt.budget/1048576.0;
	if(cause == REFUSE_BUDGET)
		snprintf(rt.refusal, sizeof(rt.refusal), "rw::metal: the acceleration structure budget of %.2f MB is full; some shadow casters wait\n", mb);
	else if(cause == REFUSE_HEAP)
		snprintf(rt.refusal, sizeof(rt.refusal), "rw::metal: an acceleration structure heap could not be allocated (budget %.2f MB); some shadow casters wait\n", mb);
	else
		snprintf(rt.refusal, sizeof(rt.refusal), "rw::metal: the acceleration structure heaps are too fragmented for a level (budget %.2f MB); some shadow casters wait\n", mb);
	fputs(rt.refusal, stderr);
}

static bool
popQueue(void)
{
	rt.queue.pop_front();
	if(rt.backlog == 0)
		return false;
	rt.backlog--;
	return true;
}

static void
rollBack(const std::vector<Taken> &work, NSArray *descs)
{
	for(size_t i = work.size(); i-- > 0;){
		BottomLevel &b = *work[i].level;
		rt.usedBytes -= b.bytes;
		rt.spent -= b.triangles;
		rt.stats.builds--;
		rt.stats.bottomLevels--;
		b.accel = nil;
		b.bytes = 0;
		b.desc = descs[i];
		b.state = LEVEL_QUEUED;
		rt.queue.push_front({ work[i].header, b.serial });
		if(work[i].backlog)
			rt.backlog++;
	}
}

void
rtEncodeBuilds(id<MTLCommandBuffer> cb)
{
	uint64 frame = rt.frame;
	id<MTLDevice> dev = getContext()->device;
	releaseRetired();
	std::vector<Taken> work;
	std::vector<NSUInteger> scratchSizes;
	NSMutableArray *descs = [NSMutableArray array];
	NSUInteger scratch = 0;
	while(!rt.queue.empty()){
		QueueEntry e = rt.queue.front();
		auto it = rt.bottoms.find(e.header);
		if(it == rt.bottoms.end() || it->second.serial != e.serial || it->second.state != LEVEL_QUEUED){
			popQueue();
			continue;
		}
		BottomLevel &b = it->second;
		if(b.desc == nil){
			b.desc = levelDescriptor(e.header, (__bridge id<MTLBuffer>)e.header->mtlVertexBuffer, castPosition(e.header)->stride, true,
				&b.triangles, &b.geometries, &b.alphaMeshes);
			if(b.desc == nil){
				b.state = LEVEL_NONE;
				rt.stats.levelsEmpty++;
				popQueue();
				continue;
			}
		}
		if(rt.spent > 0 && rt.spent + b.triangles > buildBudget())
			break;
		MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:b.desc];
		MTLSizeAndAlign sa = [dev heapAccelerationStructureSizeAndAlignWithSize:sz.accelerationStructureSize];
		if(sa.size > RT_HEAP_SIZE){
			b.state = LEVEL_NONE;
			b.desc = nil;
			rt.stats.levelsSkipped++;
			popQueue();
			continue;
		}
		b.lastUsed = std::max(b.lastUsed, frame);
		int32 cause = REFUSE_BUDGET;
		id<MTLAccelerationStructure> a = allocAccel(sa, &cause);
		if(a == nil){
			rt.stats.budgetFull++;
			if(rt.logs == 0){
				rt.logs++;
				logRefusal(cause);
			}
			break;
		}
		bool backlog = popQueue();
		b.accel = a;
		b.bytes = sa.size;
		b.state = LEVEL_BUILT;
		rt.usedBytes += sa.size;
		rt.spent += b.triangles;
		rt.stats.builds++;
		rt.stats.bottomLevels++;
		[descs addObject:b.desc];
		b.desc = nil;
		work.push_back({ e.header, &b, backlog });
		NSUInteger s = (sz.buildScratchBufferSize + 255) & ~(NSUInteger)255;
		scratchSizes.push_back(s);
		scratch += s;
	}
	if(!work.empty()){
		if(rt.scratch == nil || rt.scratch.length < scratch || rt.scratchFail){
			rt.scratch = rt.scratchFail ? nil : [dev newBufferWithLength:scratch options:MTLResourceStorageModePrivate];
			rt.scratchFail = false;
		}
		if(rt.scratch == nil)
			rollBack(work, descs);
		else{
			id<MTLAccelerationStructureCommandEncoder> enc = timedBuild(cb, RT_SAMPLE_BOTTOM);
			NSUInteger off = 0;
			for(size_t i = 0; i < work.size(); i++){
				[enc buildAccelerationStructure:work[i].level->accel descriptor:descs[i] scratchBuffer:rt.scratch scratchBufferOffset:off];
				off += scratchSizes[i];
			}
			[enc endEncoding];
		}
	}
	id<MTLBuffer> scratchKeep = rt.scratch;
	[cb addCompletedHandler:^(id<MTLCommandBuffer>){
		(void)descs;
		(void)scratchKeep;
		rt.completed.store(frame);
	}];
}

id<MTLAccelerationStructure>
rtBottomLevel(const InstanceDataHeader *h)
{
	auto it = rt.bottoms.find(h);
	return it != rt.bottoms.end() && it->second.state == LEVEL_BUILT ? it->second.accel : nil;
}

void
rtBeginCasters(const float32 eye[3], bool passRuns)
{
	rf.casters.clear();
	rf.byKey.clear();
	rf.bones.clear();
	rf.hostCount = 0;
	rf.owner = 0;
	memcpy(rf.eye, eye, sizeof(rf.eye));
	rf.open = rt.available && passRuns;
	rf.current = nil;
	rf.currentAlpha = rf.currentParams = nil;
	rf.currentTable = nil;
}

void
rtEndCasters(void)
{
	rf.open = false;
}

bool
rtCastersOpen(void)
{
	return rf.open;
}

void
rtSceneBegin(void)
{
	rf.current = nil;
	rf.currentAlpha = rf.currentParams = nil;
	rf.currentTable = rf.passTable = nil;
	releaseRetired();
	if(!sh.ran)
		sh.valid = false;
	sh.ran = false;
	rt.stats.raysPerFrame = 0;
	rt.stats.alphaKernel = 0;
}

bool
rtFrameWorkEnabled(void)
{
	return !rf.workOff;
}

static Caster*
addCaster(InstanceDataHeader *h, const RawMatrix &world, const void *object, float32 dist2, uint32 mask)
{
	CasterKey k = { h, object };
	auto it = rf.byKey.find(k);
	if(it != rf.byKey.end()){
		Caster &c = rf.casters[it->second];
		c.mask |= mask;
		c.dist2 = std::min(c.dist2, dist2);
		if(c.owner == 0)
			c.owner = rf.owner;
		return nil;
	}
	rf.byKey[k] = (uint32)rf.casters.size();
	rf.casters.push_back({ h, object, world, dist2, mask, -1, rf.owner });
	return &rf.casters.back();
}

void
rtAddDrawCaster(InstanceDataHeader *h, const RawMatrix &world, const void *object, const RawMatrix *bones)
{
	if(!rf.open || h == nil || object == nil)
		return;
	float32 dx = world.pos.x - rf.eye[0], dy = world.pos.y - rf.eye[1], dz = world.pos.z - rf.eye[2];
	Caster *c = addCaster(h, world, object, dx*dx + dy*dy + dz*dz, bones ? SHADOWCASTER_SKINNED : SHADOWCASTER_MOVING);
	rf.adds++;
	if(c && bones){
		c->bones = (int32)(rf.bones.size()/MAXSKINBONES);
		rf.bones.insert(rf.bones.end(), bones, bones + MAXSKINBONES);
		rf.palettes++;
	}
}

void
setShadowOwner(uint32 owner)
{
	rf.owner = owner <= 255 ? owner : 0;
}

bool
rtAddHostCaster(InstanceDataHeader *h, const RawMatrix &world, const void *object, const float32 sphere[4], uint32 mask)
{
	if(!rf.open || h == nil || object == nil)
		return false;
	float32 dx = sphere[0] - rf.eye[0], dy = sphere[1] - rf.eye[1], dz = sphere[2] - rf.eye[2];
	float32 d = std::max(sqrtf(dx*dx + dy*dy + dz*dz) - sphere[3], 0.0f);
	addCaster(h, world, object, d*d, mask);
	rf.hostCount++;
	rf.adds++;
	return true;
}

static int32
skinMode(void)
{
	return rf.skinModeTest ? rf.skinModeTest : RT_SKIN_MODE;
}

static bool
skinJob(const InstanceDataHeader *h, SkinJobHost *j)
{
	const AttribDesc *pos = castPosition(h);
	if(pos == nil || rf.skinPipe == nil || h->mtlVertexBuffer == nil)
		return false;
	int32 found = 0;
	j->stride = pos->stride;
	j->pos = 0;
	j->count = h->totalNumVertex;
	for(int32 i = 0; i < h->numAttribs; i++){
		const AttribDesc &a = h->attribDesc[i];
		if(a.index == ATTRIB_WEIGHTS && a.format == ATTRIBFMT_FLOAT4){
			j->weights = a.offset;
			found |= 1;
		}else if(a.index == ATTRIB_INDICES && a.format == ATTRIBFMT_UCHAR4){
			j->indices = a.offset;
			found |= 2;
		}
	}
	return found == 3;
}

static float32
headerRadius(const InstanceDataHeader *h)
{
	auto it = rt.radii.find(h);
	if(it != rt.radii.end())
		return it->second;
	const AttribDesc *pos = castPosition(h);
	float32 r = 0.0f;
	if(pos && h->vertexBuffer && h->totalNumVertex > 0){
		float32 lo[3], hi[3];
		for(uint32 v = 0; v < (uint32)h->totalNumVertex; v++){
			const float32 *p = (const float32*)(h->vertexBuffer + (size_t)v*pos->stride);
			for(int32 k = 0; k < 3; k++){
				lo[k] = v == 0 ? p[k] : std::min(lo[k], p[k]);
				hi[k] = v == 0 ? p[k] : std::max(hi[k], p[k]);
			}
		}
		float32 dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
		r = 0.5f*sqrtf(dx*dx + dy*dy + dz*dz);
	}
	rt.radii[h] = r;
	return r;
}

static float32
casterRadius(const Caster &c)
{
	const V3d *axes[3] = { &c.world.right, &c.world.up, &c.world.at };
	float32 scale = 0.0f;
	for(const V3d *a : axes)
		scale = std::max(scale, sqrtf(a->x*a->x + a->y*a->y + a->z*a->z));
	return headerRadius(c.header)*scale;
}

static uint32
skinCap(void)
{
	return rf.skinCapTest ? std::min<uint32>(rf.skinCapTest, RT_MAX_SKINNED) : (uint32)RT_MAX_SKINNED;
}

static float32
skinDistance(void)
{
	return rf.skinCapTest ? rf.skinDistanceTest : RT_SKIN_DISTANCE;
}

static void
chooseCasters(std::vector<uint32> *chosen)
{
	uint32 n = (uint32)rf.casters.size();
	std::vector<float32> d2(n);
	std::vector<uint32> order(n);
	uint32 dropped = 0;
	for(uint32 i = 0; i < n; i++){
		d2[i] = rf.casters[i].dist2;
		dropped += rf.casters[i].header == nil;
	}
	shCapByDistance(d2.data(), n, n, order.data());
	uint32 cap = rf.cap ? rf.cap : (uint32)RT_MAX_INSTANCES, skinned = 0, k = 0, large = 0;
	uint32 skinMax = skinCap();
	float32 skinFar = skinDistance();
	SkinJobHost j;
	for(; k < n && chosen->size() < cap; k++){
		Caster &c = rf.casters[order[k]];
		if(c.header == nil)
			continue;
		if(casterMaxRadius > 0.0f && casterRadius(c) > casterMaxRadius){
			large++;
			continue;
		}
		if(c.bones >= 0){
			if(skinned == skinMax || c.dist2 > skinFar*skinFar || !skinJob(c.header, &j))
				continue;
			skinned++;
		}else if(ensureBottomState(c.header) == LEVEL_NONE)
			continue;
		chosen->push_back(order[k]);
	}
	rt.stats.casters = n;
	rt.stats.hostCasters = rf.hostCount;
	rt.stats.castersCapped = n - k;
	rt.stats.castersDropped = dropped;
	rt.stats.skinnedCasters = skinned;
	rt.stats.castersLarge = large;
}

static MTLInstanceAccelerationStructureDescriptor*
topDescriptor(const TopSlot &t, uint32 count)
{
	MTLInstanceAccelerationStructureDescriptor *d = [MTLInstanceAccelerationStructureDescriptor descriptor];
	d.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
	d.instanceDescriptorBuffer = t.instances;
	d.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
	d.instanceCount = count;
	d.usage = MTLAccelerationStructureUsagePreferFastBuild;
	return d;
}

static id<MTLIntersectionFunctionTable>
alphaFunctionTable(id<MTLBuffer> alpha, id<MTLBuffer> params)
{
	if(sh.raysAlpha == nil || sh.alphaHit == nil)
		return nil;
	id<MTLFunctionHandle> fn = [sh.raysAlpha functionHandleWithFunction:sh.alphaHit];
	MTLIntersectionFunctionTableDescriptor *d = [MTLIntersectionFunctionTableDescriptor intersectionFunctionTableDescriptor];
	d.functionCount = 1;
	id<MTLIntersectionFunctionTable> table = fn ? [sh.raysAlpha newIntersectionFunctionTableWithDescriptor:d] : nil;
	if(table == nil)
		return nil;
	[table setFunction:fn atIndex:0];
	[table setBuffer:alpha offset:0 atIndex:0];
	[table setBuffer:params offset:0 atIndex:1];
	return table;
}

static bool
ensureTopSlot(id<MTLCommandBuffer> cb, TopSlot &t)
{
	id<MTLDevice> dev = getContext()->device;
	if(t.accel){
		if(t.frame + MAXFRAMESINFLIGHT <= getFrameId())
			return true;
		id<MTLBuffer> instances = [dev newBufferWithLength:t.instances.length options:MTLResourceStorageModeShared];
		id<MTLBuffer> bones = [dev newBufferWithLength:t.bones.length options:MTLResourceStorageModeShared];
		id<MTLBuffer> alpha = [dev newBufferWithLength:t.alpha.length options:MTLResourceStorageModeShared];
		id<MTLBuffer> params = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
		if(instances == nil || bones == nil || alpha == nil || params == nil)
			return false;
		id<MTLBuffer> oldInstances = t.instances, oldBones = t.bones, oldAlpha = t.alpha, oldParams = t.alphaParams;
		id<MTLIntersectionFunctionTable> oldTable = t.table;
		[cb addCompletedHandler:^(id<MTLCommandBuffer>){
			(void)oldInstances;
			(void)oldBones;
			(void)oldAlpha;
			(void)oldParams;
			(void)oldTable;
		}];
		t.instances = instances;
		t.bones = bones;
		t.alpha = alpha;
		t.alphaParams = params;
		t.table = alphaFunctionTable(alpha, params);
		return true;
	}
	t.instances = [dev newBufferWithLength:RT_MAX_INSTANCES*sizeof(MTLIndirectAccelerationStructureInstanceDescriptor)
		options:MTLResourceStorageModeShared];
	t.bones = [dev newBufferWithLength:RT_MAX_SKINNED*MAXSKINBONES*sizeof(RawMatrix) options:MTLResourceStorageModeShared];
	t.alpha = [dev newBufferWithLength:RT_MAX_ALPHA_ENTRIES*sizeof(ShadowAlphaEntryHost) options:MTLResourceStorageModeShared];
	t.alphaParams = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
	if(t.instances == nil || t.bones == nil || t.alpha == nil || t.alphaParams == nil){
		t.instances = t.bones = t.alpha = t.alphaParams = nil;
		return false;
	}
	t.table = alphaFunctionTable(t.alpha, t.alphaParams);
	MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:topDescriptor(t, RT_MAX_INSTANCES)];
	if(rf.topScratch == nil || rf.topScratch.length < sz.buildScratchBufferSize)
		rf.topScratch = [dev newBufferWithLength:std::max<NSUInteger>(sz.buildScratchBufferSize, 256)
			options:MTLResourceStorageModePrivate];
	MTLSizeAndAlign sa = [dev heapAccelerationStructureSizeAndAlignWithSize:sz.accelerationStructureSize];
	int32 cause = REFUSE_BUDGET;
	id<MTLAccelerationStructure> a = rf.topScratch ? allocAccel(sa, &cause) : nil;
	if(a == nil){
		t.instances = t.bones = t.alpha = t.alphaParams = nil;
		t.table = nil;
		rt.stats.budgetFull++;
		return false;
	}
	t.accel = a;
	t.bytes = sa.size;
	rt.usedBytes += sa.size;
	rf.topBytes += sa.size;
	return true;
}

static SkinnedLevel*
skinnedLevel(const Caster &c)
{
	MTLAccelerationStructureUsage usage = skinMode() == RT_SKIN_REBUILD ? MTLAccelerationStructureUsagePreferFastBuild :
		MTLAccelerationStructureUsageRefit;
	auto it = rf.skinned.find(c.object);
	if(it != rf.skinned.end()){
		SkinnedLevel &s = it->second;
		if(s.header == c.header && s.vertices == c.header->totalNumVertex && (s.empty || s.usage == usage))
			return s.empty ? nil : &s;
		retireSkinned(it);
	}
	id<MTLDevice> dev = getContext()->device;
	SkinnedLevel s = {};
	s.header = c.header;
	s.vertices = c.header->totalNumVertex;
	s.usage = usage;
	s.lastUsed = rt.frame;
	s.positions = [dev newBufferWithLength:(NSUInteger)s.vertices*12 options:MTLResourceStorageModePrivate];
	s.desc = s.positions ? levelDescriptor(c.header, s.positions, 12, false, &s.triangles, &s.geometries, nil) : nil;
	if(s.desc == nil){
		s.empty = true;
		s.positions = nil;
		rf.skinned[c.object] = s;
		return nil;
	}
	s.desc.usage = usage;
	MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:s.desc];
	MTLSizeAndAlign sa = [dev heapAccelerationStructureSizeAndAlignWithSize:sz.accelerationStructureSize];
	int32 cause = REFUSE_BUDGET;
	s.accel = sa.size <= RT_HEAP_SIZE ? allocAccel(sa, &cause) : nil;
	if(s.accel == nil){
		rt.stats.budgetFull++;
		return nil;
	}
	s.bytes = sa.size;
	s.buildScratch = sz.buildScratchBufferSize;
	s.refitScratch = sz.refitScratchBufferSize;
	rt.usedBytes += sa.size;
	rf.skinnedBytes += sa.size;
	return &(rf.skinned[c.object] = s);
}

static id<MTLCounterSampleBuffer>
encoderTiming(void)
{
	return rf.timing ? rf.timing : rf.frameTiming;
}

static id<MTLComputeCommandEncoder>
timedCompute(id<MTLCommandBuffer> cb, uint32 first)
{
	id<MTLCounterSampleBuffer> sb = encoderTiming();
	if(sb == nil)
		return [cb computeCommandEncoder];
	MTLComputePassDescriptor *d = [MTLComputePassDescriptor computePassDescriptor];
	d.sampleBufferAttachments[0].sampleBuffer = sb;
	d.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
	d.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
	rf.timed |= 1u << first;
	return [cb computeCommandEncoderWithDescriptor:d];
}

static id<MTLAccelerationStructureCommandEncoder>
timedBuild(id<MTLCommandBuffer> cb, uint32 first)
{
	id<MTLCounterSampleBuffer> sb = encoderTiming();
	if(sb == nil)
		return [cb accelerationStructureCommandEncoder];
	MTLAccelerationStructurePassDescriptor *d = [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
	d.sampleBufferAttachments[0].sampleBuffer = sb;
	d.sampleBufferAttachments[0].startOfEncoderSampleIndex = first;
	d.sampleBufferAttachments[0].endOfEncoderSampleIndex = first + 1;
	rf.timed |= 1u << first;
	return [cb accelerationStructureCommandEncoderWithDescriptor:d];
}

struct SkinWork
{
	SkinnedLevel *s;
	uint32 slot;
	bool build;
	NSUInteger offset;
	SkinJobHost job;
};

static void
encodeSkinned(id<MTLCommandBuffer> cb, const std::vector<uint32> &chosen, TopSlot &t)
{
	std::vector<SkinWork> work;
	NSUInteger scratch = 0;
	for(uint32 i : chosen){
		if(work.size() == skinCap())
			break;
		Caster &c = rf.casters[i];
		SkinWork w = {};
		if(c.bones < 0 || !skinJob(c.header, &w.job))
			continue;
		w.s = skinnedLevel(c);
		if(w.s == nil)
			continue;
		w.slot = (uint32)work.size();
		memcpy((uint8*)t.bones.contents + (size_t)w.slot*MAXSKINBONES*sizeof(RawMatrix),
			&rf.bones[(size_t)c.bones*MAXSKINBONES], MAXSKINBONES*sizeof(RawMatrix));
		w.build = !w.s->built || skinMode() == RT_SKIN_REBUILD || rt.cutFrame ||
			rt.frame >= w.s->builtFrame + RT_SKIN_REBUILD_FRAMES;
		w.offset = scratch;
		scratch += std::max<NSUInteger>(((w.build ? w.s->buildScratch : w.s->refitScratch) + 255) & ~(NSUInteger)255, 256);
		work.push_back(w);
	}
	if(work.empty())
		return;
	if(rf.skinScratch == nil || rf.skinScratch.length < scratch || rt.scratchFail){
		rf.skinScratch = rt.scratchFail ? nil : [getContext()->device newBufferWithLength:scratch options:MTLResourceStorageModePrivate];
		rt.scratchFail = false;
	}
	if(rf.skinScratch == nil)
		return;
	id<MTLComputeCommandEncoder> ce = timedCompute(cb, RT_SAMPLE_SKIN);
	[ce setComputePipelineState:rf.skinPipe];
	for(SkinWork &w : work){
		[ce setBuffer:(__bridge id<MTLBuffer>)w.s->header->mtlVertexBuffer offset:0 atIndex:0];
		[ce setBytes:&w.job length:sizeof(w.job) atIndex:1];
		[ce setBuffer:t.bones offset:(NSUInteger)w.slot*MAXSKINBONES*sizeof(RawMatrix) atIndex:2];
		[ce setBuffer:w.s->positions offset:0 atIndex:3];
		[ce dispatchThreads:MTLSizeMake(w.job.count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	}
	[ce endEncoding];
	NSMutableArray *descs = [NSMutableArray array];
	id<MTLAccelerationStructureCommandEncoder> ae = timedBuild(cb, RT_SAMPLE_SKINNED);
	for(SkinWork &w : work){
		SkinnedLevel *s = w.s;
		[descs addObject:s->desc];
		if(w.build){
			[ae buildAccelerationStructure:s->accel descriptor:s->desc scratchBuffer:rf.skinScratch scratchBufferOffset:w.offset];
			s->built = true;
			s->builtFrame = rt.frame;
			rt.stats.skinnedBuilds++;
		}else{
			[ae refitAccelerationStructure:s->accel descriptor:s->desc destination:nil scratchBuffer:rf.skinScratch
				scratchBufferOffset:w.offset];
			rt.stats.refits++;
		}
		s->lastUsed = rt.frame;
	}
	[ae endEncoding];
	[cb addCompletedHandler:^(id<MTLCommandBuffer>){
		(void)descs;
	}];
}

static MTLPackedFloat4x3
packed(const RawMatrix &m)
{
	MTLPackedFloat4x3 p;
	p.columns[0] = MTLPackedFloat3Make(m.right.x, m.right.y, m.right.z);
	p.columns[1] = MTLPackedFloat3Make(m.up.x, m.up.y, m.up.z);
	p.columns[2] = MTLPackedFloat3Make(m.at.x, m.at.y, m.at.z);
	p.columns[3] = MTLPackedFloat3Make(m.pos.x, m.pos.y, m.pos.z);
	return p;
}

struct AlphaFill
{
	ShadowAlphaEntryHost *table;
	uint32 cap, entries, instances, capped;
	std::unordered_map<const InstanceDataHeader*, uint32> bases;
	std::unordered_map<const void*, uint32> textures;
	NSMutableArray *keep;
};

static uint32
alphaBase(AlphaFill &f, const InstanceDataHeader *h, const std::vector<uint32> &meshes)
{
	auto found = f.bases.find(h);
	if(found != f.bases.end())
		return found->second;
	id<MTLTexture> white = (__bridge id<MTLTexture>)getWhiteTexture();
	std::vector<id<MTLTexture>> tex(meshes.size());
	uint32 fresh = 0;
	for(size_t g = 0; g < meshes.size(); g++){
		Material *m = h->inst[meshes[g]].material;
		Raster *r = m->texture ? m->texture->raster : nil;
		tex[g] = r && r->platform == PLATFORM_METAL ? (__bridge id<MTLTexture>)getRasterSampleTexture(r) : nil;
		id<MTLTexture> use = tex[g] ? tex[g] : white;
		bool seen = f.textures.count((__bridge const void*)use) != 0;
		for(size_t k = 0; k < g && !seen; k++)
			seen = (tex[k] ? tex[k] : white) == use;
		fresh += !seen;
	}
	if(f.entries + meshes.size() > f.cap || f.textures.size() + fresh > RT_MAX_ALPHA_TEXTURES){
		f.capped++;
		return SH_ALPHA_NONE;
	}
	uint32 base = f.entries;
	for(size_t g = 0; g < meshes.size(); g++){
		const InstanceData *inst = &h->inst[meshes[g]];
		Texture *rwTex = inst->material->texture;
		ShadowAlphaEntryHost &e = f.table[base + g];
		id<MTLTexture> use = tex[g] ? tex[g] : white;
		e.tex = use.gpuResourceID;
		e.threshold = tex[g] ? shAlphaThreshold(inst->castAlphaRef) : 0.0f;
		e.address = shAlphaAddressBits(tex[g] ? shAddressMode(rwTex->getAddressU()) : 0u, tex[g] ? shAddressMode(rwTex->getAddressV()) : 0u,
			(uint32)meshes.size());
		if(tex[g])
			GETMETALRASTEREXT(rwTex->raster->parent)->lastUseFrame = getFrameId();
		if(f.textures.emplace((__bridge const void*)use, (uint32)f.textures.size()).second){
			[f.keep addObject:use];
			rf.alphaTextures.push_back(use);
		}
	}
	f.entries += (uint32)meshes.size();
	f.bases[h] = base;
	return base;
}

static void
encodeInstances(id<MTLCommandBuffer> cb, const std::vector<uint32> &chosen, TopSlot &t)
{
	MTLIndirectAccelerationStructureInstanceDescriptor *di =
		(MTLIndirectAccelerationStructureInstanceDescriptor*)t.instances.contents;
	AlphaFill f = {};
	f.table = (ShadowAlphaEntryHost*)t.alpha.contents;
	f.cap = rf.alphaCap ? std::min<uint32>(rf.alphaCap, RT_MAX_ALPHA_ENTRIES) : (uint32)RT_MAX_ALPHA_ENTRIES;
	f.keep = [NSMutableArray new];
	rf.alphaTextures.clear();
	uint32 count = 0, waiting = 0, past = ~0u, owned = 0;
	for(uint32 i : chosen){
		Caster &c = rf.casters[i];
		id<MTLAccelerationStructure> as = nil;
		if(c.bones >= 0){
			auto it = rf.skinned.find(c.object);
			if(it != rf.skinned.end() && it->second.built && it->second.lastUsed == rt.frame)
				as = it->second.accel;
		}else
			as = rtBottomLevel(c.header);
		if(as == nil){
			waiting++;
			continue;
		}
		uint32 user = SH_ALPHA_NONE;
		if(c.bones < 0){
			const std::vector<uint32> &meshes = rt.bottoms.find(c.header)->second.alphaMeshes;
			if(!meshes.empty()){
				f.instances++;
				user = alphaBase(f, c.header, meshes);
			}
		}
		di[count].transformationMatrix = packed(c.world);
		SHVec3 right = { c.world.right.x, c.world.right.y, c.world.right.z }, up = { c.world.up.x, c.world.up.y, c.world.up.z };
		SHVec3 at = { c.world.at.x, c.world.at.y, c.world.at.z };
		di[count].options = shFrontCounterClockwise(right, up, at) ? MTLAccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise :
			MTLAccelerationStructureInstanceOptionNone;
		di[count].mask = c.mask;
		di[count].intersectionFunctionTableOffset = 0;
		di[count].userID = shUserId(user, c.owner);
		di[count].accelerationStructureID = as.gpuResourceID;
		if(rf.alphaIndexPast && past == ~0u && user != SH_ALPHA_NONE)
			past = count;
		owned += c.owner != 0;
		count++;
	}
	if(past != ~0u)
		di[past].userID = shUserId(f.entries, shOwnerOf(di[past].userID));
	rt.stats.instances = count;
	rt.stats.castersWaiting = waiting;
	rt.stats.alphaInstances = f.instances;
	rt.stats.alphaEntries = f.entries;
	rt.stats.alphaTextures = (uint32)f.textures.size();
	rt.stats.alphaCapped = f.capped;
	rt.stats.ownedInstances = owned;
	uint32 *tp = (uint32*)t.alphaParams.contents;
	tp[0] = f.entries;
	tp[1] = (uint32)rf.alphaFunctionTest;
	tp[2] = tp[3] = 0;
	if(f.keep.count){
		NSMutableArray *keep = f.keep;
		[cb addCompletedHandler:^(id<MTLCommandBuffer>){
			[keep removeAllObjects];
		}];
	}
	if(count == 0)
		return;
	id<MTLAccelerationStructureCommandEncoder> enc = timedBuild(cb, RT_SAMPLE_TOP);
	rtUseHeapsForBuild(enc);
	[enc buildAccelerationStructure:t.accel descriptor:topDescriptor(t, count) scratchBuffer:rf.topScratch scratchBufferOffset:0];
	[enc endEncoding];
	rf.current = rf.last = t.accel;
	rf.currentAlpha = rf.lastAlpha = t.alpha;
	rf.currentParams = rf.lastParams = t.alphaParams;
	rf.currentTable = rf.lastTable = t.table;
}

static void
ageSkinned(void)
{
	for(auto it = rf.skinned.begin(); it != rf.skinned.end();)
		it = it->second.lastUsed + RT_SKIN_KEEP_FRAMES < rt.frame ? retireSkinned(it) : std::next(it);
}

void
rtEncodeFrame(id<MTLCommandBuffer> cb, bool casters)
{
	rf.open = false;
	rf.current = rf.last = nil;
	rf.currentAlpha = rf.lastAlpha = rf.currentParams = rf.lastParams = nil;
	rf.currentTable = rf.lastTable = nil;
	rf.alphaTextures.clear();
	if(!rt.available)
		return;
	uint32 b0 = rt.stats.builds + rt.stats.skinnedBuilds, r0 = rt.stats.refits;
	rt.stats.instances = 0;
	rt.stats.alphaInstances = rt.stats.alphaEntries = rt.stats.alphaTextures = rt.stats.alphaCapped = 0;
	rt.stats.rayFrames++;
	rf.timed = 0;
	rf.frameTiming = rf.buildTiming[rf.buildSlot];
	rf.buildSlot = (rf.buildSlot + 1) % MAXFRAMESINFLIGHT;
	auto t0 = std::chrono::steady_clock::now();
	std::vector<uint32> chosen;
	if(casters)
		chooseCasters(&chosen);
	rtEncodeBuilds(cb);
	if(casters){
		TopSlot &t = rf.tops[rf.topIndex];
		rf.topIndex = (rf.topIndex + 1) % MAXFRAMESINFLIGHT;
		if(ensureTopSlot(cb, t)){
			t.frame = getFrameId();
			encodeSkinned(cb, chosen, t);
			encodeInstances(cb, chosen, t);
		}
		ageSkinned();
	}
	rf.casters.clear();
	rf.byKey.clear();
	rf.bones.clear();
	rf.walkMs = (float32)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	rt.stats.frameBuilds = rt.stats.builds + rt.stats.skinnedBuilds - b0;
	rt.stats.frameRefits = rt.stats.refits - r0;
	id<MTLCounterSampleBuffer> sb = encoderTiming();
	rf.frameTiming = nil;
	if(sb == nil)
		return;
	uint32 timed = rf.timed;
	[cb addCompletedHandler:^(id<MTLCommandBuffer>){
		uint64 top = 0, build = 0;
		for(uint32 first = 0; first < RT_BUILD_SAMPLES; first += 2){
			if(!(timed & (1u << first)))
				continue;
			uint64 t = tickSpan(sb, first, first + 1);
			if(first == RT_SAMPLE_TOP)
				top = t;
			else
				build += t;
		}
		rf.topTicks.store(top);
		rf.buildTicks.store(build);
	}];
}

id<MTLAccelerationStructure>
rtInstanceLevel(void)
{
	return rf.current;
}

void
rtUseHeaps(id<MTLComputeCommandEncoder> e)
{
	for(id<MTLHeap> h : rt.heaps)
		[e useHeap:h];
}

void
rtUseHeapsForBuild(id<MTLAccelerationStructureCommandEncoder> e)
{
	for(id<MTLHeap> h : rt.heaps)
		[e useHeap:h];
}

bool
rtShadowsSupported(void)
{
	return rt.available && sh.rays && sh.accumulate[0] && sh.clear;
}

bool
rtAlphaCastersSupported(void)
{
	return rtShadowsSupported() && sh.raysAlpha != nil && getContext()->device.argumentBuffersSupport == MTLArgumentBuffersTier2;
}

static bool
useAlphaKernel(id<MTLBuffer> alpha, id<MTLIntersectionFunctionTable> table)
{
	return alpha != nil && table != nil && rtAlphaCastersSupported() && (rt.stats.alphaInstances > 0 || sh.forceAlpha);
}

static bool
alphaTextureList(std::vector<__unsafe_unretained id<MTLResource>> *list)
{
	for(id<MTLTexture> t : rf.alphaTextures){
		if(t == nil || rf.alphaLost)
			return false;
		list->push_back(t);
	}
	return true;
}

static void
encodeAlphaTable(id<MTLComputeCommandEncoder> e, id<MTLIntersectionFunctionTable> table, id<MTLBuffer> alpha, id<MTLBuffer> params,
	const std::vector<__unsafe_unretained id<MTLResource>> &list)
{
	[e setIntersectionFunctionTable:table atBufferIndex:2];
	[e useResource:alpha usage:MTLResourceUsageRead];
	[e useResource:params usage:MTLResourceUsageRead];
	if(!list.empty())
		[e useResources:list.data() count:list.size() usage:MTLResourceUsageRead];
}

void
rtDropShadowTargets(void)
{
	sh.raw = nil;
	sh.history[0] = sh.history[1] = nil;
	sh.ownerList = nil;
	sh.listCapacity = 0;
	sh.width = sh.height = 0;
	sh.bytes = 0;
	sh.valid = false;
}

static bool
ensureShadowTargets(const ShadowInputs &in, const SHQuality &q)
{
	uint32 rw = q.halfRes ? (in.width + 1)/2 : in.width, rh = q.halfRes ? (in.height + 1)/2 : in.height;
	if(sh.raw && sh.width == in.width && sh.height == in.height && sh.raw.width == rw && sh.raw.height == rh){
		if(sh.quality != in.quality || sh.samples != in.samples)
			sh.valid = false;
		sh.quality = in.quality;
		sh.samples = in.samples;
		return true;
	}
	rtDropShadowTargets();
	id<MTLDevice> dev = getContext()->device;
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float width:rw height:rh
		mipmapped:NO];
	d.storageMode = MTLStorageModePrivate;
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	sh.raw = [dev newTextureWithDescriptor:d];
	d.pixelFormat = MTLPixelFormatRGBA16Float;
	d.width = in.width;
	d.height = in.height;
	sh.history[0] = [dev newTextureWithDescriptor:d];
	sh.history[1] = [dev newTextureWithDescriptor:d];
	uint32 capacity = shOwnerListCapacity(rw, rh);
	sh.ownerList = [dev newBufferWithLength:16 + (NSUInteger)capacity*8 options:MTLResourceStorageModePrivate];
	if(sh.raw == nil || sh.history[0] == nil || sh.history[1] == nil || sh.ownerList == nil){
		rtDropShadowTargets();
		if(!sh.reported){
			sh.reported = true;
			fprintf(stderr, "rw::metal: shadow targets for %ux%u could not be created; the shadow factor is 1\n", in.width, in.height);
		}
		return false;
	}
	sh.listCapacity = capacity;
	sh.width = in.width;
	sh.height = in.height;
	sh.quality = in.quality;
	sh.samples = in.samples;
	sh.index = 0;
	sh.fresh = true;
	sh.valid = false;
	sh.bytes = (uint64)rw*rh*2 + 2ull*in.width*in.height*8 + 16 + (uint64)capacity*8;
	return true;
}

static id<MTLComputeCommandEncoder>
shadowEncoder(id<MTLCommandBuffer> cb)
{
	id<MTLCounterSampleBuffer> sb = sh.timing[sh.timingSlot];
	sh.timingSlot = (sh.timingSlot + 1) % MAXFRAMESINFLIGHT;
	if(sb == nil)
		return [cb computeCommandEncoder];
	MTLComputePassDescriptor *pd = [MTLComputePassDescriptor computePassDescriptor];
	pd.sampleBufferAttachments[0].sampleBuffer = sb;
	pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
	pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1;
	[cb addCompletedHandler:^(id<MTLCommandBuffer>){
		uint64 t = tickSpan(sb, 0, 1);
		if(t)
			sh.ticks.store(t);
	}];
	return [cb computeCommandEncoderWithDescriptor:pd];
}

id<MTLTexture>
rtEncodeShadows(id<MTLCommandBuffer> cb, const ShadowInputs &in)
{
	SHQuality q = shQuality(in.quality);
	id<MTLAccelerationStructure> tlas = rf.current;
	rt.stats.raysPerFrame = 0;
	rt.stats.alphaKernel = 0;
	if(tlas == nil || !rtShadowsSupported() || q.raysPerPixel == 0 || in.depth == nil || in.normals == nil || in.motion == nil ||
	   !ensureShadowTargets(in, q)){
		sh.valid = false;
		sh.listed.store(0);
		sh.overflow.store(0);
		return nil;
	}
	bool alpha = useAlphaKernel(rf.currentAlpha, rf.currentTable);
	std::vector<__unsafe_unretained id<MTLResource>> textures;
	if(alpha && !alphaTextureList(&textures)){
		sh.valid = false;
		return nil;
	}
	simd_float4x4 inv = simd_inverse(in.viewProj), toWorld = simd_inverse(in.view);
	simd_float3 eye = toWorld.columns[3].xyz;
	simd_float3 sun = simd_normalize(simd_make_float3(in.toSun[0], in.toSun[1], in.toSun[2]));
	SHVec3 s0 = { sh.prevSun.x, sh.prevSun.y, sh.prevSun.z }, s1 = { sun.x, sun.y, sun.z };
	bool valid = sh.valid && in.history && !rt.cutFrame && !shSunMoved(s0, s1);
	uint32 rw = (uint32)sh.raw.width, rh = (uint32)sh.raw.height, frameNo = sh.frame++;
	bool owners = sh.ownerFix && sh.ownerReset && !sh.ownerOff && !sh.terminatorOff && rt.stats.ownedInstances > 0;
	uint32 listCap = std::min(sh.listCapacity, sh.listCapTest);
	ShadowRayParamsHost rp = { inv, toWorld, simd_make_float4(sun, in.radius), simd_make_float4(eye, SH_RAY_DISTANCE),
		simd_make_uint2(rw, rh), simd_make_uint2(in.width, in.height), frameNo, q.raysPerPixel, q.halfRes, rt.stats.alphaEntries,
		sh.terminatorOff ? 1u : 0u, owners ? 0u : 1u, listCap };
	ShadowAccumParamsHost ap = { inv, sh.prevInvViewProj, toWorld, simd_make_float4(eye, 1.0f), simd_make_float4(sh.prevEye, 1.0f),
		simd_make_uint2(in.width, in.height), simd_make_uint2(rw, rh), q.halfRes, valid ? 1u : 0u, q.maxHistory, 0.0f };
	id<MTLTexture> prev = sh.history[sh.index], next = sh.history[sh.index ^ 1];
	id<MTLComputeCommandEncoder> e = shadowEncoder(cb);
	if(sh.fresh){
		[e setComputePipelineState:sh.clear];
		[e setTexture:prev atIndex:0];
		[e dispatchThreads:MTLSizeMake(in.width, in.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
		[e setTexture:next atIndex:0];
		[e dispatchThreads:MTLSizeMake(in.width, in.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
		[e memoryBarrierWithScope:MTLBarrierScopeTextures];
		sh.fresh = false;
	}
	if(alpha)
		encodeAlphaTable(e, rf.currentTable, rf.currentAlpha, rf.currentParams, textures);
	[e setBuffer:sh.ownerList offset:0 atIndex:3];
	[e setBuffer:sh.ownerList offset:16 atIndex:4];
	if(owners){
		[e setComputePipelineState:sh.ownerReset];
		[e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
		[e memoryBarrierWithScope:MTLBarrierScopeBuffers];
	}
	[e setComputePipelineState:alpha ? sh.raysAlpha : sh.rays];
	rt.stats.alphaKernel = alpha ? 1 : 0;
	rf.passTable = alpha ? rf.currentTable : nil;
	rtUseHeaps(e);
	[e setAccelerationStructure:tlas atBufferIndex:0];
	[e setBytes:&rp length:sizeof(rp) atIndex:1];
	[e setTexture:in.depth atIndex:0];
	[e setTexture:sh.raw atIndex:1];
	[e setTexture:in.normals atIndex:2];
	[e dispatchThreads:MTLSizeMake(rw, rh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	if(owners){
		ShadowRayParamsHost fp = rp;
		fp.alphaEntries = alpha ? rp.alphaEntries : 0u;
		[e memoryBarrierWithScope:MTLBarrierScopeTextures | MTLBarrierScopeBuffers];
		[e setComputePipelineState:sh.ownerFix];
		[e setBytes:&fp length:sizeof(fp) atIndex:1];
		[e setBuffer:alpha ? rf.currentAlpha : sh.ownerList offset:0 atIndex:5];
		[e setBuffer:sh.ownerTaken offset:0 atIndex:6];
		[e dispatchThreads:MTLSizeMake(SH_OWNER_THREADS, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	}
	[e memoryBarrierWithScope:MTLBarrierScopeTextures];
	[e setComputePipelineState:sh.accumulate[frameNo & 3]];
	[e setTexture:sh.raw atIndex:0];
	[e setTexture:prev atIndex:1];
	[e setTexture:in.motion atIndex:2];
	[e setTexture:in.normals atIndex:3];
	[e setTexture:in.depth atIndex:4];
	[e setTexture:next atIndex:5];
	[e setBytes:&ap length:sizeof(ap) atIndex:0];
	[e dispatchThreads:MTLSizeMake(in.width, in.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	[e endEncoding];
	{
		id<MTLBuffer> taken = sh.ownerTaken;
		uint32 cap = listCap;
		[cb addCompletedHandler:^(id<MTLCommandBuffer>){
			uint32 n = owners ? *(const uint32*)taken.contents : 0u;
			sh.listed.store(n);
			sh.overflow.store(n > cap ? n - cap : 0u);
		}];
	}
	sh.index ^= 1;
	sh.valid = true;
	sh.ran = true;
	sh.prevInvViewProj = inv;
	sh.prevEye = eye;
	sh.prevSun = sun;
	rt.stats.raysPerFrame = rw*rh*q.raysPerPixel;
	return next;
}

void
rtDropLightTargets(void)
{
	sh.lightRaw = nil;
	sh.lightList = nil;
	sh.lightListCap = 0;
	sh.lightBytes = 0;
}

static bool
ensureLightTargets(const ShadowInputs &in, const SHQuality &q, uint32 slices)
{
	uint32 rw = q.halfRes ? (in.width + 1)/2 : in.width, rh = q.halfRes ? (in.height + 1)/2 : in.height;
	if(sh.lightRaw && sh.lightRaw.width == rw && sh.lightRaw.height == rh && sh.lightRaw.arrayLength == slices)
		return true;
	rtDropLightTargets();
	id<MTLDevice> dev = getContext()->device;
	MTLTextureDescriptor *d = [MTLTextureDescriptor new];
	d.textureType = MTLTextureType2DArray;
	d.pixelFormat = MTLPixelFormatR16Float;
	d.width = rw;
	d.height = rh;
	d.arrayLength = slices;
	d.storageMode = MTLStorageModePrivate;
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	sh.lightRaw = [dev newTextureWithDescriptor:d];
	uint32 capacity = shOwnerListCapacity(rw, rh);
	sh.lightList = [dev newBufferWithLength:16 + (NSUInteger)capacity*8 options:MTLResourceStorageModePrivate];
	if(sh.lightRaw == nil || sh.lightList == nil){
		rtDropLightTargets();
		if(!sh.lightReported){
			sh.lightReported = true;
			fprintf(stderr, "rw::metal: light shadow targets for %ux%u could not be created; shadowed lights are unshadowed\n", in.width, in.height);
		}
		return false;
	}
	sh.lightListCap = capacity;
	sh.lightBytes = (uint64)rw*rh*2*slices + 16 + (uint64)capacity*8;
	return true;
}

id<MTLTexture>
rtLightRawTexture(void)
{
	return sh.lightRaw;
}

void
rtEncodeLightRays(id<MTLCommandBuffer> cb, const ShadowInputs &in, const LightRaySlotHost *slots, uint32 count, uint32 budget)
{
	SHQuality q = shQuality(in.quality);
	q.halfRes = 1u;
	id<MTLAccelerationStructure> tlas = rf.current;
	rt.stats.lightSlots = 0;
	for(uint32 i = 0; i < LT_MAX_SHADOWED; i++){
		rt.stats.lightSlotId[i] = 0;
		sh.lightRayCount[i].store(0);
	}
	count = std::min(count, std::min(budget, (uint32)LT_MAX_SHADOWED));
	if(count == 0 || tlas == nil || !rtShadowsSupported() || sh.lightRays == nil || sh.lightFix == nil || q.raysPerPixel == 0 || in.depth == nil ||
	   in.normals == nil || !ensureLightTargets(in, q, std::min(budget, (uint32)LT_MAX_SHADOWED)))
		return;
	bool alpha = sh.lightRaysAlpha != nil && useAlphaKernel(rf.currentAlpha, rf.currentTable);
	std::vector<__unsafe_unretained id<MTLResource>> textures;
	if(alpha && !alphaTextureList(&textures))
		return;
	simd_float4x4 inv = simd_inverse(in.viewProj), toWorld = simd_inverse(in.view);
	simd_float3 eye = toWorld.columns[3].xyz;
	uint32 rw = (uint32)sh.lightRaw.width, rh = (uint32)sh.lightRaw.height, frameNo = sh.lightFrame++;
	bool owners = sh.ownerReset && !sh.ownerOff && rt.stats.ownedInstances > 0;
	ShadowRayParamsHost rp = { inv, toWorld, simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f), simd_make_float4(eye, SH_RAY_DISTANCE),
		simd_make_uint2(rw, rh), simd_make_uint2(in.width, in.height), frameNo, 1u, q.halfRes, alpha ? rt.stats.alphaEntries : 0u, 1u,
		owners ? 0u : 1u, sh.lightListCap };
	id<MTLBuffer> counts = sh.lightCounts[sh.lightCountSlot];
	sh.lightCountSlot = (sh.lightCountSlot + 1) % MAXFRAMESINFLIGHT;
	memset(counts.contents, 0, LT_MAX_SHADOWED*sizeof(uint32));
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	if(alpha)
		encodeAlphaTable(e, rf.currentTable, rf.currentAlpha, rf.currentParams, textures);
	[e setBuffer:sh.lightList offset:0 atIndex:3];
	[e setBuffer:sh.lightList offset:16 atIndex:4];
	if(owners){
		[e setComputePipelineState:sh.ownerReset];
		[e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
		[e memoryBarrierWithScope:MTLBarrierScopeBuffers];
	}
	[e setComputePipelineState:alpha ? sh.lightRaysAlpha : sh.lightRays];
	rf.passTable = alpha ? rf.currentTable : nil;
	rtUseHeaps(e);
	[e setAccelerationStructure:tlas atBufferIndex:0];
	[e setBytes:&rp length:sizeof(rp) atIndex:1];
	[e setBytes:slots length:count*sizeof(LightRaySlotHost) atIndex:7];
	[e setBuffer:counts offset:0 atIndex:8];
	[e setTexture:in.depth atIndex:0];
	[e setTexture:sh.lightRaw atIndex:1];
	[e setTexture:in.normals atIndex:2];
	[e dispatchThreads:MTLSizeMake(rw, rh, count) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	if(owners){
		[e memoryBarrierWithScope:MTLBarrierScopeTextures | MTLBarrierScopeBuffers];
		[e setComputePipelineState:sh.lightFix];
		[e setBuffer:alpha ? rf.currentAlpha : sh.lightList offset:0 atIndex:5];
		[e dispatchThreads:MTLSizeMake(SH_OWNER_THREADS, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	}
	[e endEncoding];
	[cb addCompletedHandler:^(id<MTLCommandBuffer>){
		const uint32 *c = (const uint32*)counts.contents;
		for(uint32 i = 0; i < LT_MAX_SHADOWED; i++)
			sh.lightRayCount[i].store(c[i]);
	}];
	rt.stats.lightSlots = count;
	rt.stats.lightFrame = frameNo;
	for(uint32 i = 0; i < count; i++)
		rt.stats.lightSlotId[i] = slots[i].ids.x;
}

RayStats
getRayStats(void)
{
	RayStats s = rt.stats;
	s.bottomMegabytes = (float32)((rt.usedBytes - rf.topBytes - rf.skinnedBytes)/1048576.0);
	s.skinnedMegabytes = (float32)(rf.skinnedBytes/1048576.0);
	s.topMegabytes = (float32)(rf.topBytes/1048576.0);
	s.skinnedLevels = (uint32)rf.skinned.size();
	s.scratchMegabytes = rt.scratch ? (float32)(rt.scratch.length/1048576.0) : 0.0f;
	s.walkMs = rf.walkMs;
	s.ownerListed = sh.listed.load();
	s.ownerOverflow = sh.overflow.load();
	s.shadowMegabytes = (float32)((sh.bytes + sh.lightBytes)/1048576.0);
	for(uint32 i = 0; i < LT_MAX_SHADOWED; i++)
		s.lightRays[i] = sh.lightRayCount[i].load();
	s.passMs = -1.0f;
	s.topMs = s.buildMs = -1.0f;
	MetalContext *ctx = getContext();
	if(ctx == nil || (sh.timing[0] == nil && rf.buildTiming[0] == nil))
		return s;
	MTLTimestamp cpu1, gpu1;
	[ctx->device sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
	double wall = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - sh.wall0).count();
	double nsPerTick = gpu1 > sh.gpu0 ? wall/(double)(gpu1 - sh.gpu0) : 1.0;
	if(sh.timing[0] != nil && s.raysPerFrame != 0)
		s.passMs = (float32)(sh.ticks.load()*nsPerTick/1e6);
	if(rf.buildTiming[0] != nil){
		s.topMs = (float32)(rf.topTicks.load()*nsPerTick/1e6);
		s.buildMs = (float32)(rf.buildTicks.load()*nsPerTick/1e6);
	}
	return s;
}

void
setRtBudgetForTest(uint64 bytes)
{
	rt.budget = bytes == 0 ? RT_BUDGET : bytes == 1 ? rt.usedBytes : bytes;
}

void
setRtBuildBudgetForTest(uint32 triangles)
{
	rt.buildBudgetTest = triangles;
}

void
setRtFrameForTest(uint64 frame)
{
	beginRayFrame(frame);
}

void
rtFlushForTest(void)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || !rt.available)
		return;
	@autoreleasepool {
		id<MTLCommandBuffer> cb = [ctx->queue commandBuffer];
		rtEncodeBuilds(cb);
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		[cb addCompletedHandler:^(id<MTLCommandBuffer>){
			dispatch_semaphore_signal(done);
		}];
		[cb commit];
		dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
		if(cb.error)
			fprintf(stderr, "rw::metal: command buffer error: %s\n", cb.error.localizedDescription.UTF8String);
	}
}

bool32
rtBottomForTest(InstanceDataHeader *h, uint32 *triangles, uint32 *geometries)
{
	auto it = rt.bottoms.find(h);
	if(it == rt.bottoms.end() || rtBottomLevel(h) == nil)
		return 0;
	*triangles = it->second.triangles;
	*geometries = it->second.geometries;
	return 1;
}

uint32
rtQueuedForTest(void)
{
	return (uint32)rt.queue.size();
}

uint32
rtBudgetLogsForTest(void)
{
	return rt.logs;
}

const char*
rtRefusalLogForTest(void)
{
	return rt.refusal[0] ? rt.refusal : nil;
}

uint32
rtRetiredForTest(void)
{
	return (uint32)rt.retired.size();
}

uint64
rtCasterAddsForTest(void)
{
	return rf.adds;
}

uint64
rtPaletteCopiesForTest(void)
{
	return rf.palettes;
}

uint64
rtHeapBytesForTest(void)
{
	return rt.heapBytes;
}

uint64
rtLastUsedForTest(InstanceDataHeader *h)
{
	auto it = rt.bottoms.find(h);
	return it != rt.bottoms.end() ? it->second.lastUsed : 0;
}

void
setRtScratchFailForTest(bool32 fail)
{
	rt.scratchFail = fail != 0;
}

static const float timingQuantiles[13][2] = {
	{ 0.0f, 1 }, { 0.1f, 20 }, { 0.2f, 51 }, { 0.3f, 91 }, { 0.4f, 148 }, { 0.5f, 219 }, { 0.6f, 320 },
	{ 0.7f, 472 }, { 0.8f, 709 }, { 0.9f, 1269 }, { 0.95f, 1826 }, { 0.99f, 3288 }, { 1.0f, 7402 }
};

struct TimingLevels
{
	NSMutableArray *descs;
	std::vector<uint32> triangles;
	uint32 total;
};

static uint32
timingSize(uint32 i, uint32 n)
{
	float p = (i + 0.5f)/n;
	for(int k = 1; k < 13; k++)
		if(p <= timingQuantiles[k][0]){
			float t = (p - timingQuantiles[k-1][0])/(timingQuantiles[k][0] - timingQuantiles[k-1][0]);
			return std::max(1u, (uint32)lroundf(timingQuantiles[k-1][1] + t*(timingQuantiles[k][1] - timingQuantiles[k-1][1])));
		}
	return 7402;
}

static TimingLevels
timingLevels(id<MTLDevice> dev, uint32 n, bool tiny, MTLAccelerationStructureUsage usage)
{
	TimingLevels l;
	l.descs = [NSMutableArray array];
	l.total = 0;
	for(uint32 i = 0; i < n; i++){
		uint32 tris = tiny ? 2 : timingSize(i, n);
		uint32 quads = (tris + 1)/2, cols = std::min(quads, 16u), rows = (quads + cols - 1)/cols;
		uint32 stride = 24, nv = (cols + 1)*(rows + 1);
		std::vector<uint8> verts((size_t)nv*stride);
		for(uint32 y = 0; y <= rows; y++)
			for(uint32 x = 0; x <= cols; x++){
				float p[3] = { (float)x + 0.1f*i, (float)y, 0.25f*sinf(0.7f*x + 1.3f*y + i) };
				memcpy(&verts[(size_t)(y*(cols + 1) + x)*stride], p, 12);
			}
		std::vector<uint32> idx;
		for(uint32 q = 0; q < quads; q++){
			uint32 x = q % cols, y = q / cols, a = y*(cols + 1) + x, b = a + 1, c = a + cols + 1, d = c + 1;
			uint32 t[6] = { a, b, c, c, b, d };
			idx.insert(idx.end(), t, t + (2*q + 1 < tris ? 6 : 3));
		}
		MTLAccelerationStructureTriangleGeometryDescriptor *g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		g.vertexBuffer = [dev newBufferWithBytes:verts.data() length:verts.size() options:MTLResourceStorageModeShared];
		g.vertexStride = stride;
		g.vertexFormat = MTLAttributeFormatFloat3;
		g.indexBuffer = [dev newBufferWithBytes:idx.data() length:idx.size()*4 options:MTLResourceStorageModeShared];
		g.indexType = MTLIndexTypeUInt32;
		g.triangleCount = idx.size()/3;
		g.opaque = YES;
		MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		d.geometryDescriptors = @[ g ];
		d.usage = usage;
		[l.descs addObject:d];
		l.triangles.push_back((uint32)idx.size()/3);
		l.total += (uint32)idx.size()/3;
	}
	return l;
}

struct TimingRun
{
	float32 ms;
	float32 bufferMs;
	uint64 bytes;
	NSMutableArray *accels;
};

static uint64
tickSpan(id<MTLCounterSampleBuffer> sb, uint32 first, uint32 last)
{
	NSData *data = [sb resolveCounterRange:NSMakeRange(first, last - first + 1)];
	if(data == nil || data.length < (last - first + 1)*sizeof(MTLCounterResultTimestamp))
		return 0;
	const MTLCounterResultTimestamp *t = (const MTLCounterResultTimestamp*)data.bytes;
	uint64 a = t[0].timestamp, b = t[last - first].timestamp;
	if(a == MTLCounterErrorValue || b == MTLCounterErrorValue || b < a)
		return 0;
	return b - a;
}

static TimingRun
timeBuilds(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLCounterSampleBuffer> sb, const TimingLevels &l, bool split,
	float nsPerTick)
{
	TimingRun r = { 0.0f, 0.0f, 0, [NSMutableArray array] };
	NSUInteger scratch = 0;
	std::vector<NSUInteger> offsets;
	for(MTLPrimitiveAccelerationStructureDescriptor *d in l.descs){
		MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:d];
		id<MTLAccelerationStructure> a = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
		[r.accels addObject:a];
		r.bytes += sz.accelerationStructureSize;
		offsets.push_back(scratch);
		scratch += (sz.buildScratchBufferSize + 255) & ~(NSUInteger)255;
	}
	id<MTLBuffer> s = [dev newBufferWithLength:std::max<NSUInteger>(scratch, 256) options:MTLResourceStorageModePrivate];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	uint32 n = (uint32)l.descs.count, encoders = split ? n : 1;
	for(uint32 e = 0; e < encoders; e++){
		MTLAccelerationStructurePassDescriptor *pd = [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
		pd.sampleBufferAttachments[0].sampleBuffer = sb;
		pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 2*e;
		pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = 2*e + 1;
		id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoderWithDescriptor:pd];
		for(uint32 i = split ? e : 0; i < (split ? e + 1 : n); i++)
			[enc buildAccelerationStructure:r.accels[i] descriptor:l.descs[i] scratchBuffer:s scratchBufferOffset:offsets[i]];
		[enc endEncoding];
	}
	[cb commit];
	[cb waitUntilCompleted];
	r.ms = tickSpan(sb, 0, 2*encoders - 1)*nsPerTick/1e6f;
	r.bufferMs = (float32)((cb.GPUEndTime - cb.GPUStartTime)*1000.0);
	return r;
}

static float32
median(std::vector<float32> v)
{
	std::sort(v.begin(), v.end());
	return v.empty() ? 0.0f : v[v.size()/2];
}

bool32
rtMeasureBuildsForTest(RtBuildTiming *t)
{
	MetalContext *ctx = getContext();
	memset(t, 0, sizeof(*t));
	if(ctx == nil || !rt.available || ![ctx->device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
		return 0;
	id<MTLDevice> dev = ctx->device;
	id<MTLCounterSet> timestamps = nil;
	for(id<MTLCounterSet> s in dev.counterSets)
		if([s.name isEqualToString:MTLCommonCounterSetTimestamp])
			timestamps = s;
	if(timestamps == nil)
		return 0;
	@autoreleasepool {
		enum { LEVELS = 200, ROUNDS = 5 };
		MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
		sd.counterSet = timestamps;
		sd.storageMode = MTLStorageModeShared;
		sd.sampleCount = 2*LEVELS;
		id<MTLCounterSampleBuffer> sb = [dev newCounterSampleBufferWithDescriptor:sd error:nil];
		if(sb == nil)
			return 0;
		id<MTLCommandQueue> q = [dev newCommandQueue];
		MTLTimestamp cpu0, gpu0, cpu1, gpu1;
		auto wall0 = std::chrono::steady_clock::now();
		[dev sampleTimestamps:&cpu0 gpuTimestamp:&gpu0];
		TimingLevels warm = timingLevels(dev, LEVELS, false, MTLAccelerationStructureUsageNone);
		timeBuilds(dev, q, sb, warm, false, 1.0f);
		[dev sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
		auto wall1 = std::chrono::steady_clock::now();
		if(gpu1 <= gpu0)
			return 0;
		float nsPerTick = (float)(std::chrono::duration<double, std::nano>(wall1 - wall0).count()/(double)(gpu1 - gpu0));
		t->nsPerTick = nsPerTick;
		t->rounds = ROUNDS;
		t->levels = LEVELS;
		t->triangles = warm.total;

		MTLAccelerationStructureUsage usages[RT_TIMING_USAGES];
		uint32 nu = 0;
		usages[nu] = MTLAccelerationStructureUsageNone;
		t->usageName[nu++] = "none";
		usages[nu] = MTLAccelerationStructureUsagePreferFastBuild;
		t->usageName[nu++] = "prefer fast build";
		if(@available(macOS 26.0, *)){
			usages[nu] = MTLAccelerationStructureUsageMinimizeMemory;
			t->usageName[nu++] = "minimise memory";
			usages[nu] = MTLAccelerationStructureUsagePreferFastIntersection;
			t->usageName[nu++] = "prefer fast intersection";
		}
		t->usages = nu;
		TimingRun last = {};
		for(uint32 u = 0; u < nu; u++){
			TimingLevels l = timingLevels(dev, LEVELS, false, usages[u]);
			std::vector<float32> ms;
			for(uint32 k = 0; k < ROUNDS; k++){
				TimingRun r = timeBuilds(dev, q, sb, l, false, nsPerTick);
				ms.push_back(r.ms);
				t->bytes[u] = r.bytes;
				if(u == 0)
					last = r;
			}
			t->batchMs[u] = median(ms);
		}
		t->bufferMs = last.bufferMs;
		TimingLevels tiny = timingLevels(dev, LEVELS, true, MTLAccelerationStructureUsageNone);
		std::vector<float32> tinyMs, splitMs;
		for(uint32 k = 0; k < ROUNDS; k++){
			tinyMs.push_back(timeBuilds(dev, q, sb, tiny, false, nsPerTick).ms);
			splitMs.push_back(timeBuilds(dev, q, sb, warm, true, nsPerTick).ms);
		}
		t->tinyMs = median(tinyMs);
		t->splitMs = median(splitMs);
		t->fixedUs = t->tinyMs*1000.0f/LEVELS;
		t->perTriangleNs = (t->batchMs[0] - t->tinyMs)*1e6f/(float)(t->triangles - tiny.total);

		enum { COMPACTED = 50 };
		id<MTLBuffer> sizes = [dev newBufferWithLength:4*COMPACTED options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
		for(uint32 i = 0; i < COMPACTED; i++){
			[enc writeCompactedAccelerationStructureSize:last.accels[i] toBuffer:sizes offset:4*i];
			t->sampleBytes += (uint32)[last.accels[i] size];
		}
		[enc endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		for(uint32 i = 0; i < COMPACTED; i++)
			t->compactedBytes += ((const uint32*)sizes.contents)[i];
		t->compactedLevels = COMPACTED;

		const uint32 counts[3] = { 2000, 5000, 8192 };
		for(uint32 c = 0; c < 3; c++){
			uint32 n = counts[c];
			id<MTLBuffer> inst = [dev newBufferWithLength:n*sizeof(MTLIndirectAccelerationStructureInstanceDescriptor)
				options:MTLResourceStorageModeShared];
			MTLIndirectAccelerationStructureInstanceDescriptor *d = (MTLIndirectAccelerationStructureInstanceDescriptor*)inst.contents;
			for(uint32 i = 0; i < n; i++){
				memset(&d[i], 0, sizeof(d[i]));
				d[i].transformationMatrix.columns[0].x = 1.0f;
				d[i].transformationMatrix.columns[1].y = 1.0f;
				d[i].transformationMatrix.columns[2].z = 1.0f;
				d[i].transformationMatrix.columns[3].x = (float)(i % 91)*20.0f;
				d[i].transformationMatrix.columns[3].y = (float)(i / 91)*20.0f;
				d[i].mask = 0xFF;
				d[i].userID = i;
				d[i].accelerationStructureID = [(id<MTLAccelerationStructure>)last.accels[i % LEVELS] gpuResourceID];
			}
			MTLInstanceAccelerationStructureDescriptor *id_ = [MTLInstanceAccelerationStructureDescriptor descriptor];
			id_.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
			id_.instanceDescriptorBuffer = inst;
			id_.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
			id_.instanceCount = n;
			MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:id_];
			id<MTLAccelerationStructure> top = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
			id<MTLBuffer> s = [dev newBufferWithLength:std::max<NSUInteger>(sz.buildScratchBufferSize, 256)
				options:MTLResourceStorageModePrivate];
			std::vector<float32> ms;
			for(uint32 k = 0; k < ROUNDS; k++){
				id<MTLCommandBuffer> icb = [q commandBuffer];
				MTLAccelerationStructurePassDescriptor *pd = [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
				pd.sampleBufferAttachments[0].sampleBuffer = sb;
				pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
				pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1;
				id<MTLAccelerationStructureCommandEncoder> ienc = [icb accelerationStructureCommandEncoderWithDescriptor:pd];
				for(id<MTLAccelerationStructure> a in last.accels)
					[ienc useResource:a usage:MTLResourceUsageRead];
				[ienc buildAccelerationStructure:top descriptor:id_ scratchBuffer:s scratchBufferOffset:0];
				[ienc endEncoding];
				[icb commit];
				[icb waitUntilCompleted];
				ms.push_back(tickSpan(sb, 0, 1)*nsPerTick/1e6f);
			}
			t->instanceCounts[c] = n;
			t->instanceMs[c] = median(ms);
			t->instanceBytes[c] = (uint32)sz.accelerationStructureSize;
		}
	}
	return 1;
}

static id<MTLBuffer>
encodeTrace(id<MTLAccelerationStructure> level, const float32 origin[3], const float32 dir[3], float32 maxDistance, uint32 mask)
{
	flushPasses();
	id<MTLBuffer> out = [getContext()->device newBufferWithLength:4 options:MTLResourceStorageModeShared];
	float32 r[8] = { origin[0], origin[1], origin[2], maxDistance, dir[0], dir[1], dir[2], 0.0f };
	id<MTLComputeCommandEncoder> e = [frameCommandBuffer() computeCommandEncoder];
	[e setComputePipelineState:rf.tracePipe];
	[e setAccelerationStructure:level atBufferIndex:0];
	[e setBytes:r length:sizeof(r) atIndex:1];
	[e setBytes:&mask length:4 atIndex:2];
	[e setBuffer:out offset:0 atIndex:3];
	rtUseHeaps(e);
	[e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
	[e endEncoding];
	return out;
}

bool32
rtTraceForTest(const float32 origin[3], const float32 dir[3], float32 maxDistance, uint32 mask)
{
	if(getContext() == nil || rf.last == nil || rf.tracePipe == nil)
		return 0;
	@autoreleasepool {
		id<MTLBuffer> out = encodeTrace(rf.last, origin, dir, maxDistance, mask);
		if(!commitAndWait(frameCommandBuffer()))
			return 0;
		return *(const uint32*)out.contents != 0;
	}
}

uint32
rtTraceDeferredForTest(const float32 origin[3], const float32 dir[3], float32 maxDistance, uint32 mask)
{
	if(getContext() == nil || rf.current == nil || rf.tracePipe == nil)
		return ~0u;
	@autoreleasepool {
		rf.deferred.push_back(encodeTrace(rf.current, origin, dir, maxDistance, mask));
		return (uint32)rf.deferred.size() - 1;
	}
}

bool32
rtDeferredHitForTest(uint32 index)
{
	return index < rf.deferred.size() && *(const uint32*)rf.deferred[index].contents != 0;
}

const void*
rtInstanceLevelForTest(void)
{
	return (__bridge const void*)rf.current;
}

void
rtWaitForTest(void)
{
	MetalContext *ctx = getContext();
	if(ctx == nil)
		return;
	@autoreleasepool {
		flushPasses();
		if(ctx->commandBuffer)
			commitAndWait(ctx->commandBuffer);
		id<MTLCommandBuffer> cb = [ctx->queue commandBuffer];
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		[cb addCompletedHandler:^(id<MTLCommandBuffer>){
			dispatch_semaphore_signal(done);
		}];
		[cb commit];
		dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	}
}

void
rtResetForTest(void)
{
	rtWaitForTest();
	rtTerm();
	rt.frame = 0;
	rt.completed.store(0);
	rt.stats = RayStats();
	rf.skinCapTest = 0;
	rf.skinDistanceTest = 0.0f;
	rtInit();
}

void
setRtInstanceCapForTest(uint32 cap)
{
	rf.cap = cap;
}

void
setRtAlphaCapForTest(uint32 entries)
{
	rf.alphaCap = entries;
}

void
setShadowAlphaKernelForTest(int32 mode)
{
	sh.forceAlpha = mode == 1;
}

void
setShadowTerminatorRuleForTest(bool32 on)
{
	sh.terminatorOff = !on;
}

void
setShadowOwnerRuleForTest(bool32 on)
{
	sh.ownerOff = !on;
}

void
setShadowOwnerListCapForTest(uint32 cap)
{
	sh.listCapTest = cap;
}

void
setRtAlphaFunctionForTest(int32 mode)
{
	rf.alphaFunctionTest = mode;
}

const void*
rtAlphaTableForTest(void)
{
	return (__bridge const void*)rf.passTable;
}

void
setRtSkinnedLimitForTest(uint32 cap, float32 distance)
{
	rf.skinCapTest = cap;
	rf.skinDistanceTest = distance;
}

void
setShadowCasterMaxRadius(float32 metres)
{
	casterMaxRadius = metres > 0.0f ? metres : 0.0f;
}

void
setRtAlphaIndexPastForTest(bool32 on)
{
	rf.alphaIndexPast = on != 0;
}

void
setRtAlphaTextureLostForTest(bool32 on)
{
	rf.alphaLost = on != 0;
}

void
rtWatchTextureForTest(Raster *raster)
{
	rf.watched = raster ? (__bridge id<MTLTexture>)getRasterSampleTexture(raster) : nil;
}

bool32
rtWatchedTextureAliveForTest(void)
{
	return rf.watched != nil;
}

void
rtWatchAlphaTableForTest(void)
{
	rf.watchedTable = rf.passTable;
}

bool32
rtWatchedTableAliveForTest(void)
{
	return rf.watchedTable != nil;
}

void
setRtFrameWorkForTest(bool32 on)
{
	rf.workOff = !on;
}

void
setRtSkinModeForTest(int32 mode)
{
	rf.skinModeTest = mode;
}

bool32
rtSkinnedForTest(const void *object)
{
	auto it = rf.skinned.find(object);
	return it != rf.skinned.end() && it->second.built && it->second.lastUsed == rt.frame;
}

void
setRtTimingForTest(bool32 on)
{
	MetalContext *ctx = getContext();
	rf.timing = nil;
	if(!on || ctx == nil || ![ctx->device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
		return;
	id<MTLCounterSet> timestamps = nil;
	for(id<MTLCounterSet> s in ctx->device.counterSets)
		if([s.name isEqualToString:MTLCommonCounterSetTimestamp])
			timestamps = s;
	if(timestamps == nil)
		return;
	MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
	sd.counterSet = timestamps;
	sd.storageMode = MTLStorageModeShared;
	sd.sampleCount = RT_BUILD_SAMPLES;
	rf.timing = [ctx->device newCounterSampleBufferWithDescriptor:sd error:nil];
	[ctx->device sampleTimestamps:&rf.cpu0 gpuTimestamp:&rf.gpu0];
	rf.wall0 = std::chrono::steady_clock::now();
}

bool32
rtFrameTimingForTest(RtFrameTiming *t)
{
	memset(t, 0, sizeof(*t));
	MetalContext *ctx = getContext();
	if(ctx == nil || rf.timing == nil)
		return 0;
	rtWaitForTest();
	MTLTimestamp cpu1, gpu1;
	[ctx->device sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
	double wall = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - rf.wall0).count();
	double nsPerTick = gpu1 > rf.gpu0 ? wall/(double)(gpu1 - rf.gpu0) : 1.0;
	float32 *ms[3] = { &t->skinMs, &t->skinnedMs, &t->instanceMs };
	for(uint32 i = 0; i < 3; i++)
		if(rf.timed & (1u << 2*i))
			*ms[i] = (float32)(tickSpan(rf.timing, 2*i, 2*i + 1)*nsPerTick/1e6);
	t->walkMs = rf.walkMs;
	t->instances = rt.stats.instances;
	t->skinned = rt.stats.skinnedCasters;
	return 1;
}

bool32
rtShadowTimingForTest(uint32 width, uint32 height, int32 quality, const float32 eye[3], const float32 target[3],
	const float32 toSun[3], float32 *ms, float32 *ground, float32 *shadowed)
{
	MetalContext *ctx = getContext();
	SHQuality q = shQuality(quality);
	if(ctx == nil || rf.last == nil || !rtShadowsSupported() || q.raysPerPixel == 0 || width == 0 || height == 0)
		return 0;
	id<MTLDevice> dev = ctx->device;
	id<MTLCounterSampleBuffer> sb = twoSampleBuffer(dev, timestampCounters(dev));
	if(sb == nil)
		return 0;
	@autoreleasepool {
		simd_float3 e = simd_make_float3(eye[0], eye[1], eye[2]), t = simd_make_float3(target[0], target[1], target[2]);
		simd_float3 f = simd_normalize(t - e);
		simd_float3 r = simd_normalize(simd_cross(f, simd_make_float3(0.0f, 0.0f, 1.0f)));
		simd_float3 u = simd_cross(r, f);
		simd_float4x4 view;
		view.columns[0] = simd_make_float4(r.x, u.x, -f.x, 0.0f);
		view.columns[1] = simd_make_float4(r.y, u.y, -f.y, 0.0f);
		view.columns[2] = simd_make_float4(r.z, u.z, -f.z, 0.0f);
		view.columns[3] = simd_make_float4(-simd_dot(r, e), -simd_dot(u, e), simd_dot(f, e), 1.0f);
		float32 n = 1.0f, fa = 3000.0f, fy = 1.0f/tanf(30.0f*3.14159265f/180.0f), aspect = (float32)width/height;
		simd_float4x4 proj = simd_float4x4{};
		proj.columns[0].x = fy/aspect;
		proj.columns[1].y = fy;
		proj.columns[2].z = -(fa + n)/(fa - n);
		proj.columns[2].w = -1.0f;
		proj.columns[3].z = -2.0f*fa*n/(fa - n);
		simd_float4x4 vp = simd_mul(proj, view), inv = simd_inverse(vp);
		id<MTLBuffer> depthBuf = [dev newBufferWithLength:(NSUInteger)width*height*4 options:MTLResourceStorageModeShared];
		float32 *dp = (float32*)depthBuf.contents;
		uint64 hits = 0;
		for(uint32 y = 0; y < height; y++)
			for(uint32 x = 0; x < width; x++){
				float32 nx = (x + 0.5f)/width*2.0f - 1.0f, ny = 1.0f - (y + 0.5f)/height*2.0f;
				simd_float4 a4 = simd_mul(inv, simd_make_float4(nx, ny, -1.0f, 1.0f)), b4 = simd_mul(inv, simd_make_float4(nx, ny, 1.0f, 1.0f));
				simd_float3 a = a4.xyz/a4.w, b = b4.xyz/b4.w, d = b - a;
				float32 depth = 1.0f;
				if(d.z < 0.0f){
					float32 s = -a.z/d.z;
					if(s >= 0.0f && s <= 1.0f){
						simd_float4 c = simd_mul(vp, simd_make_float4(a + d*s, 1.0f));
						depth = 0.5f*(c.z/c.w + 1.0f);
						hits++;
					}
				}
				dp[(size_t)y*width + x] = depth;
			}
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:width
			height:height mipmapped:NO];
		td.storageMode = MTLStorageModePrivate;
		td.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> depthTex = [dev newTextureWithDescriptor:td];
		uint32 rw = q.halfRes ? (width + 1)/2 : width, rh = q.halfRes ? (height + 1)/2 : height;
		MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float width:rw height:rh
			mipmapped:NO];
		rd.storageMode = MTLStorageModePrivate;
		rd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		id<MTLTexture> raw = [dev newTextureWithDescriptor:rd];
		MTLTextureDescriptor *nd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Snorm width:width
			height:height mipmapped:NO];
		nd.storageMode = MTLStorageModePrivate;
		nd.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> normalTex = [dev newTextureWithDescriptor:nd];
		id<MTLBuffer> normalBuf = [dev newBufferWithLength:(NSUInteger)width*height*4 options:MTLResourceStorageModeShared];
		if(depthTex == nil || raw == nil || normalTex == nil || normalBuf == nil)
			return 0;
		simd_float4 gn = simd_mul(view, simd_make_float4(0.0f, 0.0f, 1.0f, 0.0f));
		PPVec2 oe = ppOctEncode(gn.x, gn.y, gn.z);
		int16_t packedNormal[2] = { (int16_t)lrintf(std::min(std::max(oe.x, -1.0f), 1.0f)*32767.0f),
			(int16_t)lrintf(std::min(std::max(oe.y, -1.0f), 1.0f)*32767.0f) };
		int16_t *np = (int16_t*)normalBuf.contents;
		for(size_t i = 0; i < (size_t)width*height; i++){
			np[2*i] = packedNormal[0];
			np[2*i + 1] = packedNormal[1];
		}
		id<MTLCommandBuffer> cb = [ctx->queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromBuffer:depthBuf sourceOffset:0 sourceBytesPerRow:width*4 sourceBytesPerImage:(NSUInteger)width*height*4
			sourceSize:MTLSizeMake(width, height, 1) toTexture:depthTex destinationSlice:0 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit copyFromBuffer:normalBuf sourceOffset:0 sourceBytesPerRow:width*4 sourceBytesPerImage:(NSUInteger)width*height*4
			sourceSize:MTLSizeMake(width, height, 1) toTexture:normalTex destinationSlice:0 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		simd_float3 sun = simd_normalize(simd_make_float3(toSun[0], toSun[1], toSun[2]));
		bool alpha = useAlphaKernel(rf.lastAlpha, rf.lastTable);
		std::vector<__unsafe_unretained id<MTLResource>> textures;
		if(alpha && !alphaTextureList(&textures))
			return 0;
		std::vector<float32> times;
		id<MTLBuffer> noList = [dev newBufferWithLength:32 options:MTLResourceStorageModePrivate];
		for(uint32 k = 0; k < 5; k++){
			ShadowRayParamsHost rp = { inv, simd_inverse(view), simd_make_float4(sun, 0.02f), simd_make_float4(e, SH_RAY_DISTANCE),
				simd_make_uint2(rw, rh), simd_make_uint2(width, height), k, q.raysPerPixel, q.halfRes, rt.stats.alphaEntries, 0u, 1u, 0u };
			id<MTLCommandBuffer> tcb = [ctx->queue commandBuffer];
			MTLComputePassDescriptor *pd = [MTLComputePassDescriptor computePassDescriptor];
			pd.sampleBufferAttachments[0].sampleBuffer = sb;
			pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
			pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1;
			id<MTLComputeCommandEncoder> enc = [tcb computeCommandEncoderWithDescriptor:pd];
			[enc setComputePipelineState:alpha ? sh.raysAlpha : sh.rays];
			if(alpha)
				encodeAlphaTable(enc, rf.lastTable, rf.lastAlpha, rf.lastParams, textures);
			rtUseHeaps(enc);
			[enc setBuffer:noList offset:0 atIndex:3];
			[enc setBuffer:noList offset:16 atIndex:4];
			[enc setAccelerationStructure:rf.last atBufferIndex:0];
			[enc setBytes:&rp length:sizeof(rp) atIndex:1];
			[enc setTexture:depthTex atIndex:0];
			[enc setTexture:raw atIndex:1];
			[enc setTexture:normalTex atIndex:2];
			[enc dispatchThreads:MTLSizeMake(rw, rh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
			[enc endEncoding];
			[tcb commit];
			[tcb waitUntilCompleted];
			MTLTimestamp cpu1, gpu1;
			[dev sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
			double wall = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - sh.wall0).count();
			double nsPerTick = gpu1 > sh.gpu0 ? wall/(double)(gpu1 - sh.gpu0) : 1.0;
			times.push_back((float32)(tickSpan(sb, 0, 1)*nsPerTick/1e6));
		}
		*ms = median(times);
		*ground = (float32)((double)hits/((double)width*height));
		id<MTLBuffer> rawBuf = [dev newBufferWithLength:(NSUInteger)rw*rh*2 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> rcb = [ctx->queue commandBuffer];
		id<MTLBlitCommandEncoder> rb = [rcb blitCommandEncoder];
		[rb copyFromTexture:raw sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rw, rh, 1)
			toBuffer:rawBuf destinationOffset:0 destinationBytesPerRow:rw*2 destinationBytesPerImage:(NSUInteger)rw*rh*2];
		[rb endEncoding];
		[rcb commit];
		[rcb waitUntilCompleted];
		const __fp16 *rv = (const __fp16*)rawBuf.contents;
		uint64 groundRays = 0, dark = 0;
		for(uint32 y = 0; y < rh; y++)
			for(uint32 x = 0; x < rw; x++){
				uint32 sx = q.halfRes ? std::min(2*x, width - 1) : x, sy = q.halfRes ? std::min(2*y, height - 1) : y;
				if(dp[(size_t)sy*width + sx] >= 1.0f)
					continue;
				groundRays++;
				dark += (float)rv[(size_t)y*rw + x] < 0.5f;
			}
		*shadowed = groundRays ? (float32)((double)dark/groundRays) : 0.0f;
	}
	return 1;
}

}
}
#endif
