#ifdef RW_METAL
#include "metalobjc.h"
#include "rwmetalshader.h"
#include "metalstate.h"
#include "metalkeys.h"
#include "metalfan.h"

#define PLUGIN_ID 0

namespace rw {
namespace metal {

static_assert((int)IMPRIM_NONE == PRIMTYPENONE && (int)IMPRIM_LINELIST == PRIMTYPELINELIST &&
	(int)IMPRIM_POLYLINE == PRIMTYPEPOLYLINE && (int)IMPRIM_TRILIST == PRIMTYPETRILIST &&
	(int)IMPRIM_TRISTRIP == PRIMTYPETRISTRIP && (int)IMPRIM_TRIFAN == PRIMTYPETRIFAN &&
	(int)IMPRIM_POINTLIST == PRIMTYPEPOINTLIST, "primitive types in metalfan.h");

Shader *im2dOverrideShader;

#include "shaders/im2d_uv2_metal.inc"

uint32 im2dUV2VertexLayout;
static_assert(sizeof(Im2DVertexUV2) == sizeof(Im2DVertex) + 2*sizeof(float32), "two-uv im2d vertex");
static AttribDesc im2dUV2AttribDesc[4] = {
	{ ATTRIB_POS,        ATTRIBFMT_FLOAT4,      sizeof(Im2DVertexUV2), 0 },
	{ ATTRIB_COLOR,      ATTRIBFMT_UCHAR4_NORM, sizeof(Im2DVertexUV2), offsetof(Im2DVertex, r) },
	{ ATTRIB_TEXCOORDS0, ATTRIBFMT_FLOAT2,      sizeof(Im2DVertexUV2), offsetof(Im2DVertex, u) },
	{ ATTRIB_TEXCOORDS1, ATTRIBFMT_FLOAT2,      sizeof(Im2DVertexUV2), sizeof(Im2DVertex) },
};

void
openIm2DUV2(void)
{
	im2dUV2VertexLayout = registerVertexLayout(im2dUV2AttribDesc, nelem(im2dUV2AttribDesc));
}

void
closeIm2DUV2(void)
{
	im2dUV2VertexLayout = 0;
}

static MTLPrimitiveType primTypeMap[] = {
	MTLPrimitiveTypePoint,
	MTLPrimitiveTypeLine,
	MTLPrimitiveTypeLineStrip,
	MTLPrimitiveTypeTriangle,
	MTLPrimitiveTypeTriangleStrip,
	MTLPrimitiveTypeTriangle,
	MTLPrimitiveTypePoint
};

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
im2DBegin(Shader *shader, uint32 layout, void *vertices, int32 numVertices, uint32 stride)
{
	Camera *cam = (Camera*)engine->currentCamera;
	RingSpace space;
	float32 xform[4];
	uint32 size;

	if(cam == nil || cam->frameBuffer == nil)
		return 0;
	shader->use();
	if(!beginDraw())
		return 0;
	setVertexLayout(layout);
	xform[0] = 2.0f/cam->frameBuffer->width;
	xform[1] = -2.0f/cam->frameBuffer->height;
	xform[2] = -1.0f;
	xform[3] = 1.0f;
	setIm2DXform(xform);
	if(!flushCache())
		return 0;

	size = numVertices*stride;
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
	int32 count;

	count = drawElementCount(primType, numVertices);
	if(ctx == nil || count == 0)
		return;
	@autoreleasepool {
		if(!im2DBegin(im2dOverrideShader ? im2dOverrideShader : im2dShader, im2dVertexLayout,
		              vertices, numVertices, sizeof(Im2DVertex)))
			return;
		drawRingPrimitive(ctx, primType, count, numVertices);
	}
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
	void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
	MetalContext *ctx = getContext();
	int32 count;

	count = drawElementCount(primType, numIndices);
	if(ctx == nil || count == 0 || numVertices <= 0)
		return;
	@autoreleasepool {
		if(!im2DBegin(im2dOverrideShader ? im2dOverrideShader : im2dShader, im2dVertexLayout,
		              vertices, numVertices, sizeof(Im2DVertex)))
			return;
		drawRingIndexedPrimitive(ctx, primType, count, indices, numIndices);
	}
}

void
im2DRenderIndexedPrimitiveUV2(PrimitiveType primType,
	void *vertices, int32 numVertices, void *indices, int32 numIndices)
{
	MetalContext *ctx = getContext();
	int32 count;

	count = drawElementCount(primType, numIndices);
	if(ctx == nil || count == 0 || numVertices <= 0)
		return;
	if(im2dOverrideShader == nil || im2dUV2VertexLayout == 0){
		countDroppedDraw();
		return;
	}
	@autoreleasepool {
		if(!im2DBegin(im2dOverrideShader, im2dUV2VertexLayout, vertices, numVertices, sizeof(Im2DVertexUV2)))
			return;
		drawRingIndexedPrimitive(ctx, primType, count, indices, numIndices);
	}
}

Shader *im3dShader;
uint32 im3dVertexLayout;
static AttribDesc im3dAttribDesc[4] = {
	{ ATTRIB_POS,        ATTRIBFMT_FLOAT3,      sizeof(Im3DVertex), 0 },
	{ ATTRIB_NORMAL,     ATTRIBFMT_FLOAT3,      sizeof(Im3DVertex), offsetof(Im3DVertex, normal) },
	{ ATTRIB_COLOR,      ATTRIBFMT_UCHAR4_NORM, sizeof(Im3DVertex), offsetof(Im3DVertex, r) },
	{ ATTRIB_TEXCOORDS0, ATTRIBFMT_FLOAT2,      sizeof(Im3DVertex), offsetof(Im3DVertex, u) },
};
static Im3DVertex *im3dVertices;
static int32 im3dNumVertices;
static int32 im3dMaxVertices;
static bool im3dLit;
static int32 im3dBits;
static uint64 im3dUploadFrame;
static RingSpace im3dUpload;

void
openIm3D(void)
{
#include "shaders/im3d_metal.inc"
#include "shaders/simple_metal.inc"
	const char *src[] = { header_metal_src, im3d_metal_src, simple_metal_src, nil };
	@autoreleasepool {
		im3dShader = Shader::create(src, "im3dVS", "simpleFS", VARIANT_ALPHATEST);
	}
	assert(im3dShader);
	im3dVertexLayout = registerVertexLayout(im3dAttribDesc, nelem(im3dAttribDesc));
}

void
closeIm3D(void)
{
	if(im3dShader)
		im3dShader->destroy();
	im3dShader = nil;
	im3dVertexLayout = 0;
	rwFree(im3dVertices);
	im3dVertices = nil;
	im3dNumVertices = 0;
	im3dMaxVertices = 0;
	im3dUploadFrame = 0;
}

RGBA im3dMaterialColor = { 255, 255, 255, 255 };
SurfaceProperties im3dSurfaceProps = { 1.0f, 1.0f, 1.0f };

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	if(world == nil){
		static Matrix ident;
		ident.setIdentity();
		world = &ident;
	}
	setWorldMatrix(world);
	im3dLit = (flags & im3d::LIGHTING) != 0;
	if(im3dLit){
		setMaterial(im3dMaterialColor, im3dSurfaceProps);
		im3dBits = lightingCB();
	}

	if((flags & im3d::VERTEXUV) == 0)
		SetRenderStatePtr(TEXTURERASTER, nil);

	if(numVertices < 0)
		numVertices = 0;
	if(numVertices > im3dMaxVertices){
		im3dVertices = (Im3DVertex*)rwResize(im3dVertices, numVertices*sizeof(Im3DVertex), MEMDUR_EVENT | ID_DRIVER);
		im3dMaxVertices = numVertices;
	}
	memcpy(im3dVertices, vertices, numVertices*sizeof(Im3DVertex));
	im3dNumVertices = numVertices;
	im3dUploadFrame = 0;
}

static bool32
im3DBegin(void)
{
	uint32 size;

	if(im3dShader == nil || im3dNumVertices == 0)
		return 0;
	if(im3dLit)
		defaultShader->use(shaderVariant(im3dBits & VSLIGHT_MASK, getAlphaTest()));
	else
		im3dShader->use(getAlphaTest() ? VARIANT_ALPHATEST : 0);
	if(!beginDraw())
		return 0;
	setVertexLayout(im3dVertexLayout);
	if(!flushCache())
		return 0;

	if(im3dUploadFrame != getFrameId()){
		size = im3dNumVertices*sizeof(Im3DVertex);
		if(!ringAlloc(size, 16, &im3dUpload))
			return 0;
		memcpy(im3dUpload.cpu, im3dVertices, size);
		im3dUploadFrame = getFrameId();
	}
	bindVertexBuffer(im3dUpload.buffer, im3dUpload.offset);
	return 1;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	MetalContext *ctx = getContext();
	int32 count;

	count = drawElementCount(primType, im3dNumVertices);
	if(ctx == nil || count == 0)
		return;
	@autoreleasepool {
		if(!im3DBegin())
			return;
		drawRingPrimitive(ctx, primType, count, im3dNumVertices);
	}
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	MetalContext *ctx = getContext();
	int32 count;

	count = drawElementCount(primType, numIndices);
	if(ctx == nil || count == 0)
		return;
	@autoreleasepool {
		if(!im3DBegin())
			return;
		drawRingIndexedPrimitive(ctx, primType, count, indices, numIndices);
	}
}

void
im3DEnd(void)
{
}

}
}
#endif
