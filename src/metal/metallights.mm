#ifdef RW_METAL
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unistd.h>
#include <vector>
#include "metalobjc.h"
#include "metalstate.h"
#include "metalframe.h"
#include "metallights.h"
#include "shaders/lightsh_metal.inc"
#include "shaders/lights_metal.inc"

namespace rw {
namespace metal {

static_assert(sizeof(LightDesc) == 72, "LightDesc is 18 plain words");

enum { CLUSTERS = LT_CLUSTER_X*LT_CLUSTER_Y*LT_CLUSTER_Z };

struct Ranked
{
	float32 dist2;
	int32 index;
};

struct ClusterResult
{
	int32 pass;
	uint32 loss, worst;
	uint64 ticks;
};

static struct LightState
{
	std::vector<LightDesc> kept;
	std::vector<Ranked> order;
	uint32 submitted = 0, dropped = 0;
	Camera *cam = nil;
	float32 zNear = 0.1f, zFar = 1000.0f;
	LightFrameParams params = { 0.0f, 1.0f };
	id<MTLComputePipelineState> pipe = nil;
	bool pipeTried = false;
	id<MTLBuffer> indices[MAXFRAMESINFLIGHT], counts[MAXFRAMESINFLIGHT];
	id<MTLCounterSampleBuffer> timing[MAXFRAMESINFLIGHT];
	int32 sceneSlot = -1, scenePass = 0;
	uint32 lastIds[LT_MAX_LIGHTS];
	uint32 lastIdCount = 0;
	int32 passes = 0, waited = 0;
	std::mutex lock;
	ClusterResult result = { 0, 0, 0, 0 };
	MTLTimestamp gpu0 = 0;
	std::chrono::steady_clock::time_point wall0;
} lt;

void
lightsInit(void)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || lt.pipe || lt.pipeTried)
		return;
	lt.pipeTried = true;
	NSMutableString *src = [NSMutableString stringWithUTF8String:"#include <metal_stdlib>\nusing namespace metal;\n"];
	[src appendString:[NSString stringWithUTF8String:lightsh_metal_src]];
	[src appendString:[NSString stringWithUTF8String:lights_metal_src]];
	MTLCompileOptions *o = [MTLCompileOptions new];
	if(@available(macOS 15.0, *))
		o.mathMode = MTLMathModeRelaxed;
	else
		o.fastMathEnabled = NO;
	NSError *err = nil;
	id<MTLLibrary> lib = [ctx->device newLibraryWithSource:src options:o error:&err];
	id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"lightCluster"] : nil;
	lt.pipe = fn ? [ctx->device newComputePipelineStateWithFunction:fn error:&err] : nil;
	if(lt.pipe == nil)
		fprintf(stderr, "rw::metal: the light cluster kernel failed to build; lights are off: %s\n",
			err ? err.localizedDescription.UTF8String : "");
}

static bool
ensureBuffers(MetalContext *ctx)
{
	if(lt.indices[0])
		return true;
	id<MTLCounterSet> timestamps = nil;
	if([ctx->device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
		for(id<MTLCounterSet> s in ctx->device.counterSets)
			if([s.name isEqualToString:MTLCommonCounterSetTimestamp])
				timestamps = s;
	for(uint32 i = 0; i < MAXFRAMESINFLIGHT; i++){
		lt.indices[i] = [ctx->device newBufferWithLength:CLUSTERS*LT_PER_CLUSTER*2 options:MTLResourceStorageModeShared];
		lt.counts[i] = [ctx->device newBufferWithLength:CLUSTERS*4 options:MTLResourceStorageModeShared];
		if(timestamps){
			MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
			sd.counterSet = timestamps;
			sd.storageMode = MTLStorageModeShared;
			sd.sampleCount = 2;
			lt.timing[i] = [ctx->device newCounterSampleBufferWithDescriptor:sd error:nil];
		}
		if(lt.indices[i] == nil || lt.counts[i] == nil){
			for(uint32 k = 0; k < MAXFRAMESINFLIGHT; k++){
				lt.indices[k] = lt.counts[k] = nil;
				lt.timing[k] = nil;
			}
			return false;
		}
	}
	MTLTimestamp cpu;
	[ctx->device sampleTimestamps:&cpu gpuTimestamp:&lt.gpu0];
	lt.wall0 = std::chrono::steady_clock::now();
	return true;
}

static void
clearSubmission(void)
{
	lt.kept.clear();
	lt.submitted = lt.dropped = 0;
}

void
lightsSceneBegin(Camera *cam)
{
	clearSubmission();
	lt.cam = cam;
	lt.scenePass = 0;
	lt.sceneSlot = -1;
}

static void
cameraEye(float32 eye[3])
{
	eye[0] = eye[1] = eye[2] = 0.0f;
	if(lt.cam && lt.cam->getFrame()){
		const V3d &p = lt.cam->getFrame()->getLTM()->pos;
		eye[0] = p.x;
		eye[1] = p.y;
		eye[2] = p.z;
	}
}

void
setLights(const LightDesc *lights, int32 n)
{
	clearSubmission();
	if(lights == nil || n <= 0 || !frameLightsLive())
		return;
	float32 eye[3];
	cameraEye(eye);
	lt.zNear = lt.cam ? lt.cam->nearPlane : 0.1f;
	lt.zFar = lt.cam ? lt.cam->farPlane : 1000.0f;
	if(lt.kept.capacity() < LT_MAX_LIGHTS)
		lt.kept.reserve(LT_MAX_LIGHTS);
	lt.order.clear();
	for(int32 i = 0; i < n; i++){
		if(!(lights[i].radius >= 0.0f))
			continue;
		float32 dx = lights[i].pos[0] - eye[0], dy = lights[i].pos[1] - eye[1], dz = lights[i].pos[2] - eye[2];
		lt.order.push_back({ dx*dx + dy*dy + dz*dz, i });
	}
	size_t keep = std::min(lt.order.size(), (size_t)LT_MAX_LIGHTS);
	auto nearer = [](const Ranked &a, const Ranked &b){ return a.dist2 < b.dist2 || (a.dist2 == b.dist2 && a.index < b.index); };
	std::partial_sort(lt.order.begin(), lt.order.begin() + keep, lt.order.end(), nearer);
	lt.submitted = (uint32)n;
	lt.dropped = (uint32)(n - (int32)keep);
	for(size_t i = 0; i < keep; i++){
		LightDesc d = lights[lt.order[i].index];
		float32 len2 = d.dir[0]*d.dir[0] + d.dir[1]*d.dir[1] + d.dir[2]*d.dir[2];
		if(!(len2 > 0.0f) || !std::isfinite(len2))
			d.flags &= ~(uint32)LIGHT_SPOT;
		lt.kept.push_back(d);
	}
}

void
setLightFrameParams(const LightFrameParams *p)
{
	if(p)
		lt.params = *p;
}

bool
lightsEncode(id<MTLCommandBuffer> cb, const RawMatrix &view, const RawMatrix &viewProj, int32 width, int32 height)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || lt.kept.empty())
		return false;
	lightsInit();
	if(lt.pipe == nil || !ensureBuffers(ctx))
		return false;
	uint32 n = (uint32)lt.kept.size();
	RingSpace listSpace, blockSpace, statsSpace;
	if(!ringAlloc(n*sizeof(LTLight), 256, &listSpace) || !ringAlloc(sizeof(LTFrame), 256, &blockSpace) || !ringAlloc(16, 256, &statsSpace))
		return false;
	simd_float4x4 v, vp;
	memcpy(&v, &view, 64);
	memcpy(&vp, &viewProj, 64);
	const float32 *m = (const float32*)&view;
	float32 eye[3];
	for(int i = 0; i < 3; i++)
		eye[i] = -(m[i*4]*m[12] + m[i*4 + 1]*m[13] + m[i*4 + 2]*m[14]);
	LTLight *gpu = (LTLight*)listSpace.cpu;
	for(uint32 i = 0; i < n; i++){
		const LightDesc &d = lt.kept[i];
		gpu[i].posRadius = simd_make_float4(d.pos[0] - eye[0], d.pos[1] - eye[1], d.pos[2] - eye[2], d.radius);
		gpu[i].colorGloss = simd_make_float4(d.color[0], d.color[1], d.color[2], d.gloss);
		gpu[i].dirOuter = simd_make_float4(d.dir[0], d.dir[1], d.dir[2], d.cosOuter);
		gpu[i].spot = simd_make_float4(d.cosInner, d.softness, 0.0f, 0.0f);
		gpu[i].ids = simd_make_uint4(d.id, d.flags, d.owner, d.group);
		lt.lastIds[i] = d.id;
	}
	lt.lastIdCount = n;
	LTFrame *fb = (LTFrame*)blockSpace.cpu;
	fb->view = v;
	fb->invProj = simd_mul(v, simd_inverse(vp));
	fb->eye = simd_make_float4(eye[0], eye[1], eye[2], 0.0f);
	fb->axis = simd_make_float4(v.columns[0].z, v.columns[1].z, v.columns[2].z, lt.zNear);
	fb->grid = simd_make_float4((float32)width, (float32)height, lt.zFar, (float32)n);
	fb->params = simd_make_float4(lt.params.wetGloss, lt.params.lightScale, 0.0f, 0.0f);
	uint32 slot = (uint32)(getFrameId() % MAXFRAMESINFLIGHT);
	uint32 *stats = (uint32*)statsSpace.cpu;
	memset(stats, 0, 16);
	id<MTLCounterSampleBuffer> sb = lt.timing[slot];
	id<MTLComputeCommandEncoder> e;
	if(sb){
		MTLComputePassDescriptor *pd = [MTLComputePassDescriptor computePassDescriptor];
		pd.sampleBufferAttachments[0].sampleBuffer = sb;
		pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = 0;
		pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = 1;
		e = [cb computeCommandEncoderWithDescriptor:pd];
	}else
		e = [cb computeCommandEncoder];
	[e setComputePipelineState:lt.pipe];
	[e setBuffer:(__bridge id<MTLBuffer>)blockSpace.buffer offset:blockSpace.offset atIndex:0];
	[e setBuffer:(__bridge id<MTLBuffer>)listSpace.buffer offset:listSpace.offset atIndex:1];
	[e setBuffer:lt.indices[slot] offset:0 atIndex:2];
	[e setBuffer:lt.counts[slot] offset:0 atIndex:3];
	[e setBuffer:(__bridge id<MTLBuffer>)statsSpace.buffer offset:statsSpace.offset atIndex:4];
	[e dispatchThreads:MTLSizeMake(LT_CLUSTER_X, LT_CLUSTER_Y, LT_CLUSTER_Z) threadsPerThreadgroup:MTLSizeMake(LT_CLUSTER_X, LT_CLUSTER_Y, 1)];
	[e endEncoding];
	int32 pass = ++lt.passes;
	[cb addCompletedHandler:^(id<MTLCommandBuffer>){
		ClusterResult r = { pass, stats[0], stats[1], 0 };
		if(sb){
			NSData *data = [sb resolveCounterRange:NSMakeRange(0, 2)];
			if(data && data.length >= 2*sizeof(MTLCounterResultTimestamp)){
				const MTLCounterResultTimestamp *t = (const MTLCounterResultTimestamp*)data.bytes;
				if(t[0].timestamp != MTLCounterErrorValue && t[1].timestamp != MTLCounterErrorValue && t[1].timestamp >= t[0].timestamp)
					r.ticks = t[1].timestamp - t[0].timestamp;
			}
		}
		std::lock_guard<std::mutex> g(lt.lock);
		if(r.pass > lt.result.pass)
			lt.result = r;
	}];
	lt.sceneSlot = (int32)slot;
	lt.scenePass = pass;
	return true;
}

LightStats
getLightStats(void)
{
	LightStats s;
	memset(&s, 0, sizeof(s));
	s.submitted = lt.submitted;
	s.kept = (uint32)lt.kept.size();
	s.dropped = lt.dropped;
	s.clusterMs = -1.0f;
	ClusterResult r;
	{
		std::lock_guard<std::mutex> g(lt.lock);
		r = lt.result;
	}
	if(lt.scenePass == 0 || r.pass != lt.scenePass)
		return s;
	s.clusterLoss = r.loss;
	s.maxPerCluster = r.worst;
	MetalContext *ctx = getContext();
	if(ctx && lt.timing[0] != nil){
		MTLTimestamp cpu1, gpu1;
		[ctx->device sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
		double wall = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - lt.wall0).count();
		double nsPerTick = gpu1 > lt.gpu0 ? wall/(double)(gpu1 - lt.gpu0) : 1.0;
		uint64 t = r.ticks;
		s.clusterMs = t ? (float32)(t*nsPerTick/1e6) : -1.0f;
	}
	return s;
}

int32
lightClusterPassesForTest(void)
{
	return lt.passes;
}

int32
lightClusterSlotForTest(void)
{
	return lt.sceneSlot;
}

bool32
readClusterForTest(int32 cx, int32 cy, int32 cz, uint32 *ids, int32 max, int32 *count)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || lt.scenePass == 0 || cx < 0 || cy < 0 || cz < 0 || cx >= LT_CLUSTER_X || cy >= LT_CLUSTER_Y || cz >= LT_CLUSTER_Z)
		return 0;
	if(lt.waited != lt.passes){
		flushPasses();
		if(!commitAndWait(frameCommandBuffer()))
			return 0;
		lt.waited = lt.passes;
	}
	for(int i = 0; i < 2000; i++){
		{
			std::lock_guard<std::mutex> g(lt.lock);
			if(lt.result.pass >= lt.scenePass)
				break;
		}
		usleep(1000);
	}
	uint32 cluster = (uint32)(cx + LT_CLUSTER_X*(cy + LT_CLUSTER_Y*cz));
	uint32 listed = ((const uint32*)lt.counts[lt.sceneSlot].contents)[cluster];
	const uint16 *idx = (const uint16*)lt.indices[lt.sceneSlot].contents + cluster*LT_PER_CLUSTER;
	*count = (int32)listed;
	for(uint32 i = 0; i < listed && (int32)i < max; i++)
		ids[i] = idx[i] < lt.lastIdCount ? lt.lastIds[idx[i]] : ~0u;
	return 1;
}

void
lightsTerm(void)
{
	clearSubmission();
	lt.cam = nil;
	lt.pipe = nil;
	lt.pipeTried = false;
	for(uint32 i = 0; i < MAXFRAMESINFLIGHT; i++){
		lt.indices[i] = lt.counts[i] = nil;
		lt.timing[i] = nil;
	}
	lt.sceneSlot = -1;
	lt.scenePass = 0;
	lt.lastIdCount = 0;
	lt.waited = lt.passes;
	std::lock_guard<std::mutex> g(lt.lock);
	lt.result = { 0, 0, 0, 0 };
}

}
}
#endif
