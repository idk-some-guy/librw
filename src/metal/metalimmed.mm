#ifdef RW_METAL
#include "metalobjc.h"
#include "rwmetalshader.h"
#include "metalstate.h"
#include "metalfan.h"

#define PLUGIN_ID 0

namespace rw {
namespace metal {

static_assert((int)IMPRIM_NONE == PRIMTYPENONE && (int)IMPRIM_LINELIST == PRIMTYPELINELIST &&
	(int)IMPRIM_POLYLINE == PRIMTYPEPOLYLINE && (int)IMPRIM_TRILIST == PRIMTYPETRILIST &&
	(int)IMPRIM_TRISTRIP == PRIMTYPETRISTRIP && (int)IMPRIM_TRIFAN == PRIMTYPETRIFAN &&
	(int)IMPRIM_POINTLIST == PRIMTYPEPOINTLIST, "primitive types in metalfan.h");

Shader *im2dOverrideShader;

static MTLPrimitiveType primTypeMap[] = {
	MTLPrimitiveTypePoint,
	MTLPrimitiveTypeLine,
	MTLPrimitiveTypeLineStrip,
	MTLPrimitiveTypeTriangle,
	MTLPrimitiveTypeTriangleStrip,
	MTLPrimitiveTypeTriangle,
	MTLPrimitiveTypePoint
};

void
openIm2D(void)
{
}

void
closeIm2D(void)
{
}

static Im2DVertex tmpprimbuf[3];

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	im2DRenderPrimitive(PRIMTYPELINELIST, tmpprimbuf, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	tmpprimbuf[2] = verts[vert3];
	im2DRenderPrimitive(PRIMTYPETRILIST, tmpprimbuf, 3);
}

static void
drawRingPrimitive(MetalContext *ctx, PrimitiveType primType, int32 count, int32 numVertices)
{
	RingSpace space;
	bool wide;

	if(primType != PRIMTYPETRIFAN){
		[ctx->encoder drawPrimitives:primTypeMap[primType] vertexStart:0 vertexCount:count];
		return;
	}
	wide = numVertices > 0x10000;
	if(!ringAlloc(count*(wide ? 4 : 2), 16, &space))
		return;
	if(wide)
		fanToList((uint32_t*)space.cpu, numVertices);
	else
		fanToList((uint16_t*)space.cpu, numVertices);
	[ctx->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:count
		indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
		indexBuffer:(__bridge id<MTLBuffer>)space.buffer indexBufferOffset:space.offset];
}

static void
drawRingIndexedPrimitive(MetalContext *ctx, PrimitiveType primType, int32 count, void *indices, int32 numIndices)
{
	RingSpace space;

	if(!ringAlloc(count*2, 16, &space))
		return;
	if(primType == PRIMTYPETRIFAN)
		indexedFanToList((uint16_t*)space.cpu, (uint16_t*)indices, numIndices);
	else
		memcpy(space.cpu, indices, count*2);
	[ctx->encoder drawIndexedPrimitives:primTypeMap[primType] indexCount:count
		indexType:MTLIndexTypeUInt16
		indexBuffer:(__bridge id<MTLBuffer>)space.buffer indexBufferOffset:space.offset];
}

static bool32
im2DBegin(void *vertices, int32 numVertices)
{
	Camera *cam = (Camera*)engine->currentCamera;
	RingSpace space;
	float32 xform[4];
	uint32 size;

	if(cam == nil || cam->frameBuffer == nil || !beginDraw())
		return 0;
	if(im2dOverrideShader)
		im2dOverrideShader->use();
	else
		im2dShader->use();
	setVertexLayout(im2dVertexLayout);
	xform[0] = 2.0f/cam->frameBuffer->width;
	xform[1] = -2.0f/cam->frameBuffer->height;
	xform[2] = -1.0f;
	xform[3] = 1.0f;
	setIm2DXform(xform);
	if(!flushCache())
		return 0;

	size = numVertices*sizeof(Im2DVertex);
	if(!ringAlloc(size, 16, &space))
		return 0;
	memcpy(space.cpu, vertices, size);
	bindVertexBuffer(space.buffer, space.offset);
	return 1;
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	MetalContext *ctx = getContext();
	RingSpace space;
	int32 count;
	bool wide;

	count = drawElementCount(primType, numVertices);
	if(ctx == nil || count == 0)
		return;
	@autoreleasepool {
		if(!im2DBegin(vertices, numVertices))
			return;
		if(primType != PRIMTYPETRIFAN){
			[ctx->encoder drawPrimitives:primTypeMap[primType] vertexStart:0 vertexCount:count];
			return;
		}
		wide = numVertices > 0x10000;
		if(!ringAlloc(count*(wide ? 4 : 2), 16, &space))
			return;
		if(wide)
			fanToList((uint32_t*)space.cpu, numVertices);
		else
			fanToList((uint16_t*)space.cpu, numVertices);
		[ctx->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:count
			indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
			indexBuffer:(__bridge id<MTLBuffer>)space.buffer indexBufferOffset:space.offset];
	}
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
	void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
	MetalContext *ctx = getContext();
	RingSpace space;
	int32 count;

	count = drawElementCount(primType, numIndices);
	if(ctx == nil || count == 0 || numVertices <= 0)
		return;
	@autoreleasepool {
		if(!im2DBegin(vertices, numVertices))
			return;
		if(!ringAlloc(count*2, 16, &space))
			return;
		if(primType == PRIMTYPETRIFAN)
			indexedFanToList((uint16_t*)space.cpu, (uint16_t*)indices, numIndices);
		else
			memcpy(space.cpu, indices, count*2);
		[ctx->encoder drawIndexedPrimitives:primTypeMap[primType] indexCount:count
			indexType:MTLIndexTypeUInt16
			indexBuffer:(__bridge id<MTLBuffer>)space.buffer indexBufferOffset:space.offset];
	}
}

}
}
#endif
