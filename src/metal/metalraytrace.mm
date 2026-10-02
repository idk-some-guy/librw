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
#include "metalraytrace.h"

namespace rw {
namespace metal {

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
};

struct QueueEntry
{
	const InstanceDataHeader *header;
	uint64 serial;
};

struct Retired
{
	id<MTLAccelerationStructure> accel;
	uint64 frame;
	uint64 bytes;
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
	RayStats stats;
} rt;

bool
rtInit(void)
{
	MetalContext *ctx = getContext();
	rt.available = ctx && ctx->device.supportsRaytracing;
	rt.budget = RT_BUDGET;
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
	rt.stats.bottomLevels = 0;
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

static bool
meshCasts(const InstanceData *inst)
{
	Material *m = inst->material;
	if(inst->vertexAlpha || m == nil || m->color.alpha != 255)
		return false;
	Raster *r = m->texture ? m->texture->raster : nil;
	return !(r && r->platform == PLATFORM_METAL && GETMETALRASTEREXT(r)->hasAlpha);
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
bottomDescriptor(const InstanceDataHeader *h, uint32 *triangles, uint32 *geometries)
{
	const AttribDesc *pos = castPosition(h);
	if(pos == nil || h->mtlVertexBuffer == nil || h->indexBuffer == nil)
		return nil;
	std::vector<uint32> all, starts, counts;
	for(uint32 m = 0; m < h->numMeshes; m++){
		const InstanceData *inst = &h->inst[m];
		if(!meshCasts(inst))
			continue;
		uint32 at = (uint32)all.size();
		uint32 n = appendMeshList((const uint16*)((const uint8*)h->indexBuffer + inst->offset), inst->numIndex,
			h->primType == MTLPrimitiveTypeTriangleStrip, &all);
		if(n == 0)
			continue;
		starts.push_back(at);
		counts.push_back(n);
	}
	if(all.empty())
		return nil;
	id<MTLBuffer> ib = [getContext()->device newBufferWithBytes:all.data() length:all.size()*4
		options:MTLResourceStorageModeShared];
	if(ib == nil)
		return nil;
	NSMutableArray *geos = [NSMutableArray array];
	*triangles = 0;
	for(size_t i = 0; i < starts.size(); i++){
		MTLAccelerationStructureTriangleGeometryDescriptor *g = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		g.vertexBuffer = (__bridge id<MTLBuffer>)h->mtlVertexBuffer;
		g.vertexBufferOffset = 0;
		g.vertexStride = pos->stride;
		g.vertexFormat = MTLAttributeFormatFloat3;
		g.indexBuffer = ib;
		g.indexBufferOffset = (NSUInteger)starts[i]*4;
		g.indexType = MTLIndexTypeUInt32;
		g.triangleCount = counts[i]/3;
		g.opaque = YES;
		[geos addObject:g];
		*triangles += counts[i]/3;
	}
	MTLPrimitiveAccelerationStructureDescriptor *d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	d.geometryDescriptors = geos;
	*geometries = (uint32)starts.size();
	return d;
}

bool
rtEnsureBottom(InstanceDataHeader *h)
{
	if(!rt.available || h == nil)
		return false;
	auto it = rt.bottoms.find(h);
	if(it != rt.bottoms.end()){
		it->second.lastUsed = rt.frame;
		return it->second.state == LEVEL_BUILT;
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
	return false;
}

static void
dropLevel(std::unordered_map<const InstanceDataHeader*, BottomLevel>::iterator it, bool retire)
{
	if(it->second.state == LEVEL_BUILT){
		rt.stats.bottomLevels--;
		if(retire)
			rt.retired.push_back({ it->second.accel, rt.frame, it->second.bytes });
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
		if(r.frame <= done)
			rt.usedBytes -= r.bytes;
		else
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
			b.desc = bottomDescriptor(e.header, &b.triangles, &b.geometries);
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
			id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
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

RayStats
getRayStats(void)
{
	RayStats s = rt.stats;
	s.bottomMegabytes = (float32)(rt.usedBytes/1048576.0);
	s.scratchMegabytes = rt.scratch ? (float32)(rt.scratch.length/1048576.0) : 0.0f;
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

}
}
#endif
