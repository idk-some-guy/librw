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

Texture *readNativeTexture(Stream *stream) { return nil; }
void writeNativeTexture(Texture *tex, Stream *stream) { }
uint32 getSizeNativeTexture(Texture *tex) { return 0; }

RGBA im3dMaterialColor;
SurfaceProperties im3dSurfaceProps;

void openIm3D(void) { }
void closeIm3D(void) { }
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags) { }
void im3DRenderPrimitive(PrimitiveType primType) { }
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices) { }
void im3DEnd(void) { }

void initMatFX(void) { }
ObjPipeline *makeMatFXPipeline(void) { return ObjPipeline::create(); }

void initSkin(void) { }
ObjPipeline *makeSkinPipeline(void) { return ObjPipeline::create(); }
void skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance) { }
void skinRenderCB(Atomic *atomic, InstanceDataHeader *header) { }
void uploadSkinMatrices(Atomic *atomic) { }

void matfxRenderCB(Atomic *atomic, InstanceDataHeader *header) { }

void im2DRenderIndexedPrimitiveUV2(PrimitiveType primType,
   void *vertices, int32 numVertices, void *indices, int32 numIndices) { }

}
}
#endif
