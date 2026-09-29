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
