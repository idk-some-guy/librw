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

#ifdef __OBJC__
void rtEncodeBuilds(id<MTLCommandBuffer> cb);
id<MTLAccelerationStructure> rtBottomLevel(const InstanceDataHeader *h);
#endif

}
}

#endif

#endif
