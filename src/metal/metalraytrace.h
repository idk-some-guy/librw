#ifndef RW_METAL_METALRAYTRACE_H
#define RW_METAL_METALRAYTRACE_H

#ifdef RW_METAL

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
void rtBeginCasters(const float32 eye[3]);
void rtEndCasters(void);
bool rtCastersOpen(void);
void rtSceneBegin(void);
bool rtFrameWorkEnabled(void);
void rtAddDrawCaster(InstanceDataHeader *h, const RawMatrix &world, const void *object, const RawMatrix *bones);
bool rtAddHostCaster(InstanceDataHeader *h, const RawMatrix &world, const void *object, const float32 sphere[4], uint32 mask);

#ifdef __OBJC__
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
