#ifdef RW_METAL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#include "rwmetal.h"
#include "rwmetalplg.h"
#include "rwmetalimpl.h"

namespace rw {
namespace metal {

// M1 window, device, pass manager, present

// M2 rasters, textures, state, shaders, im2d

int32 nativeRasterOffset;

void registerNativeRaster(void) { }
Raster *rasterCreate(Raster *raster) { return nil; }
uint8 *rasterLock(Raster*, int32 level, int32 lockMode) { return nil; }
void rasterUnlock(Raster*, int32) { }
int32 rasterNumLevels(Raster*) { return 0; }
bool32 imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format) { return 0; }
bool32 rasterFromImage(Raster *raster, Image *image) { return 0; }
Image *rasterToImage(Raster *raster) { return nil; }
void allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha) { }

Texture *readNativeTexture(Stream *stream) { return nil; }
void writeNativeTexture(Texture *tex, Stream *stream) { }
uint32 getSizeNativeTexture(Texture *tex) { return 0; }

const char *header_metal_src = "";
Shader *im2dOverrideShader;

void setTexture(int32 n, Texture *tex) { }
void setAlphaBlend(bool32 enable) { }
bool32 getAlphaBlend(void) { return 0; }
bool32 getAlphaTest(void) { return 0; }
void setCustomConstants(const void *data, uint32 size) { }
void flushCache(void) { }

void openIm2D(void) { }
void closeIm2D(void) { }
void im2DRenderLine(void *vertices, int32 numVertices,
  int32 vert1, int32 vert2) { }
void im2DRenderTriangle(void *vertices, int32 numVertices,
  int32 vert1, int32 vert2, int32 vert3) { }
void im2DRenderPrimitive(PrimitiveType primType,
   void *vertices, int32 numVertices) { }
void im2DRenderIndexedPrimitive(PrimitiveType primType,
   void *vertices, int32 numVertices, void *indices, int32 numIndices) { }

// M3 instancing, default pipeline, lighting, im3d, matfx fallback

Shader *defaultShader, *defaultShader_noAT;
Shader *defaultShader_fullLight, *defaultShader_fullLight_noAT;

RGBA im3dMaterialColor;
SurfaceProperties im3dSurfaceProps;

void openIm3D(void) { }
void closeIm3D(void) { }
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags) { }
void im3DRenderPrimitive(PrimitiveType primType) { }
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices) { }
void im3DEnd(void) { }

void setupVertexInput(InstanceDataHeader *header) { }
void teardownVertexInput(InstanceDataHeader *header) { }

void setProjectionMatrix(float32*) { }
void setViewMatrix(float32*) { }
void setWorldMatrix(Matrix *mat, const void *object) { }
int32 setLights(WorldLights *lightData) { return 0; }
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp) { }

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_METAL);
	this->instanceCB = nil;
	this->uninstanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance) { }
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header) { }
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header) { }
int32 lightingCB(Atomic *atomic) { return 0; }
int32 lightingCB(void) { return 0; }

void drawInst_simple(InstanceDataHeader *header, InstanceData *inst) { }
void drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst) { }
void drawInst(InstanceDataHeader *header, InstanceData *inst) { }

void *destroyNativeData(void *object, int32, int32) { return object; }

ObjPipeline *makeDefaultPipeline(void) { return ObjPipeline::create(); }

void initMatFX(void) { }
ObjPipeline *makeMatFXPipeline(void) { return ObjPipeline::create(); }

// M4 skinning

void initSkin(void) { }
ObjPipeline *makeSkinPipeline(void) { return ObjPipeline::create(); }
void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance) { }
void skinRenderCB(Atomic *atomic, InstanceDataHeader *header) { }
void uploadSkinMatrices(Atomic *atomic) { }

// M5 matfx environment map

void matfxRenderCB(Atomic *atomic, InstanceDataHeader *header) { }

// M7 post effects and droplets

void im2DRenderIndexedPrimitiveUV2(PrimitiveType primType,
   void *vertices, int32 numVertices, void *indices, int32 numIndices) { }

}
}
#endif
