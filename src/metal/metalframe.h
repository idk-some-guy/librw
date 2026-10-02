#ifndef RW_METAL_METALFRAME_H
#define RW_METAL_METALFRAME_H

#include "metalobjc.h"
#include "metalphase.h"

namespace rw {
namespace metal {

enum
{
	COPY_DIRECT = 0,
	COPY_RENDER
};

void flushPasses(void);
void clearPassTarget(const PassTarget &t, const PassClear &c);
id<MTLCommandBuffer> frameCommandBuffer(void);
bool32 commitAndWait(id<MTLCommandBuffer> cb);

bool32 frameInit(void);
void frameTerm(void);
void frameBuildReactive(void);
const char *frameStatsTail(void);
void frameTierChanged(void);
uint32 activeFormatMask(void);
void applyPendingSettings(void);
float32 frameEmissiveScale(void);

PassTarget frameRoute(const PassTarget &t);
void frameNoteUpdate(const PassTarget &t);
uint32 frameColorFormat(Raster *color);
Raster *frameSceneBase(void);
bool frameSubViewport(Raster *fb, Raster *target, HostRect *r);
void frameScaleClear(PassClear *c);
void frameShow(void);
bool frameEncodeComposite(Raster *raster, id<MTLTexture> target);
void frameUpdateOutput(CAMetalLayer *layer, uint32 frameCount);
int32 frameCopyRoute(Raster *src);
bool frameReadsComposite(Raster *src);
bool32 frameRenderCopy(Raster *src, id<MTLTexture> dst, int32 dx, int32 dy, int32 w, int32 h);
void frameForget(Raster *raster);
bool frameIsPrepassTarget(Raster *r);
void framePrepassAttachments(MTLRenderPassDescriptor *desc, uint32 samples);
void framePrepassPassEnded(void);
PassTarget currentPassTarget(void);
void resumePassTarget(const PassTarget &t);
void setPassDepthSplit(Raster *color);

struct ReactiveDraw
{
	id<MTLBuffer> vertices;
	uint32 vertexOffset;
	id<MTLBuffer> indices;
	uint32 indexOffset;
	uint32 count;
	uint32 primitive;
	uint32 indexType;
	bool indexed;
	uint32 layout;
	id<MTLBuffer> blocks[3];
	uint32 blockOffsets[3];
	id<MTLTexture> texture;
	id<MTLSamplerState> sampler;
	MTLViewport viewport;
};

bool frameReactiveOpen(void);
bool frameTemporalActive(void);
void frameAddReactive(const ReactiveDraw &r);
void frameJitterProjection(Camera *cam, float32 *proj);
bool frameJitterIm2D(Camera *cam, float32 *nx, float32 *ny);
void frameCameraCut(void);
bool frameModernTier(void);
bool frameRelitActive(void);
id<MTLTexture> frameShadowTexture(void);
MTLVertexDescriptor *layoutVertexDescriptor(uint32 layout);

}
}

#endif
