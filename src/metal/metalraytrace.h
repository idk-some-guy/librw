#ifndef RW_METAL_METALRAYTRACE_H
#define RW_METAL_METALRAYTRACE_H

#ifdef RW_METAL

#ifdef __OBJC__
#include <simd/simd.h>
#endif

namespace rw {
namespace metal {

struct InstanceDataHeader;

bool rtInit(void);
void rtTerm(void);
bool rtAvailable(void);
bool rtEnsureBottom(InstanceDataHeader *h);
void rtForgetHeader(InstanceDataHeader *h);
void rtBeginFrame(void);
void rtCameraCut(void);
uint64 rtFrame(void);
void rtReleaseLevels(void);
void rtBeginCasters(const float32 eye[3], bool passRuns);
void rtEndCasters(void);
bool rtCastersOpen(void);
void rtSceneBegin(void);
bool rtFrameWorkEnabled(void);
void rtAddDrawCaster(InstanceDataHeader *h, const RawMatrix &world, const void *object, const RawMatrix *bones);
bool rtAddHostCaster(InstanceDataHeader *h, const RawMatrix &world, const void *object, const float32 sphere[4], uint32 mask);
bool rtShadowsSupported(void);
bool rtAlphaCastersSupported(void);
void rtDropShadowTargets(void);
void rtDropLightTargets(void);

#ifdef __OBJC__
struct ShadowInputs
{
	id<MTLTexture> depth, normals, motion;
	simd_float4x4 viewProj, view;
	uint32 width, height, samples;
	int32 quality;
	float32 toSun[3];
	float32 radius;
	bool history;
};

id<MTLTexture> rtEncodeShadows(id<MTLCommandBuffer> cb, const ShadowInputs &in);

struct LightRaySlotHost
{
	simd_float4 posRadius, dirOuter, misc;
	simd_uint4 ids;
};

void rtEncodeLightRays(id<MTLCommandBuffer> cb, const ShadowInputs &in, const LightRaySlotHost *slots, uint32 count, uint32 budget);
id<MTLTexture> rtLightRawTexture(void);
id<MTLTexture> rtLightShadowTexture(void);
uint32 rtLightSlotCount(void);
void rtEncodeBuilds(id<MTLCommandBuffer> cb);
id<MTLAccelerationStructure> rtBottomLevel(const InstanceDataHeader *h);
void rtEncodeFrame(id<MTLCommandBuffer> cb, bool casters);
id<MTLAccelerationStructure> rtInstanceLevel(void);
void rtUseHeaps(id<MTLComputeCommandEncoder> e);
void rtUseHeapsForBuild(id<MTLAccelerationStructureCommandEncoder> e);
#endif

}
}

#endif

#endif
