#ifdef RW_METAL
#include <stddef.h>

#include "metalobjc.h"
#include "metalstate.h"
#include "metalinst.h"

#define PLUGIN_ID 0

namespace rw {
namespace metal {

static_assert(sizeof(InstAttrib) == sizeof(AttribDesc) &&
	offsetof(InstAttrib, index) == offsetof(AttribDesc, index) &&
	offsetof(InstAttrib, format) == offsetof(AttribDesc, format) &&
	offsetof(InstAttrib, stride) == offsetof(AttribDesc, stride) &&
	offsetof(InstAttrib, offset) == offsetof(AttribDesc, offset), "InstAttrib layout in metalinst.h");
static_assert((int)INSTATTRIB_POS == ATTRIB_POS && (int)INSTATTRIB_NORMAL == ATTRIB_NORMAL &&
	(int)INSTATTRIB_COLOR == ATTRIB_COLOR && (int)INSTATTRIB_TEXCOORDS0 == ATTRIB_TEXCOORDS0,
	"attribute indices in metalinst.h");
static_assert((int)INSTFMT_FLOAT2 == ATTRIBFMT_FLOAT2 && (int)INSTFMT_FLOAT3 == ATTRIBFMT_FLOAT3 &&
	(int)INSTFMT_UCHAR4_NORM == ATTRIBFMT_UCHAR4_NORM, "attribute formats in metalinst.h");

static InstanceStats instanceStats;

InstanceStats
getInstanceStats(void)
{
	return instanceStats;
}

static void
releaseBuffer(void **buffer)
{
	if(*buffer){
		CFBridgingRelease(*buffer);
		*buffer = nil;
	}
}

static void*
newBuffer(const void *bytes, uint32 size)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || size == 0)
		return nil;
	id<MTLBuffer> buf = [ctx->device newBufferWithBytes:bytes length:size options:MTLResourceStorageModeShared];
	return buf ? (__bridge_retained void*)buf : nil;
}

static void
freeInstanceData(Geometry *geometry)
{
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_METAL)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	releaseBuffer(&header->mtlIndexBuffer);
	releaseBuffer(&header->mtlVertexBuffer);
	rwFree(header->indexBuffer);
	rwFree(header->vertexBuffer);
	rwFree(header->attribDesc);
	rwFree(header->inst);
	rwFree(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static InstanceDataHeader*
instanceMesh(Geometry *geo)
{
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_METAL;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->primType = meshh->flags == 1 ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle;
	header->totalNumVertex = geo->numVertices;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

	uint32 *counts = rwNewT(uint32, header->numMeshes*2, MEMDUR_FUNCTION | ID_GEOMETRY);
	uint32 *offsets = counts + header->numMeshes;
	Mesh *mesh = meshh->getMeshes();
	for(uint32 i = 0; i < header->numMeshes; i++)
		counts[i] = mesh[i].numIndices;
	uint32 indexBytes = meshIndexOffsets(counts, header->numMeshes, offsets);

	header->indexBuffer = (uint16*)rwNew(indexBytes, MEMDUR_EVENT | ID_GEOMETRY);
	memset(header->indexBuffer, 0, indexBytes);
	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		assert(inst->minVert != 0xFFFFFFFF);
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->offset = offsets[i];
		memcpy((uint8*)header->indexBuffer + inst->offset,
		       mesh->indices, inst->numIndex*2);
		if(meshh->flags == 1 && stripHasRestartIndex(mesh->indices, mesh->numIndices) &&
		   instanceStats.stripRestartMeshes++ == 0)
			RWERROR((ERR_GENERAL, "a triangle strip uses index 0xFFFF, which restarts the strip in Metal"));
		mesh++;
		inst++;
	}
	rwFree(counts);

	header->vertexBuffer = nil;
	header->numAttribs = 0;
	header->attribDesc = nil;
	header->vertexLayout = 0;
	header->mtlVertexBuffer = nil;
	header->mtlIndexBuffer = newBuffer(header->indexBuffer, indexBytes);

	return header;
}

static void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	if(geo->flags & Geometry::NATIVE)
		return;

	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(geo->instData){
		assert(header->platform == PLATFORM_METAL);
		if(header->serialNumber != geo->meshHeader->serialNum)
			freeInstanceData(geo);
	}

	if(geo->instData == nil){
		geo->instData = instanceMesh(geo);
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 0);
	}else if(geo->lockedSinceInst)
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 1);

	geo->lockedSinceInst = 0;
}

static void
uninstance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	assert(0 && "can't uninstance");
}

static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	pipe->instance(atomic);
	assert(geo->instData != nil);
	assert(geo->instData->platform == PLATFORM_METAL);
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

bool32
drawIndexed(InstanceDataHeader *header, InstanceData *inst)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || inst->numIndex == 0 || header->mtlIndexBuffer == nil || header->mtlVertexBuffer == nil)
		return 0;
	@autoreleasepool {
		if(!beginDraw() || !flushCache())
			return 0;
		bindVertexBuffer(header->mtlVertexBuffer, 0);
		[ctx->encoder drawIndexedPrimitives:(MTLPrimitiveType)header->primType indexCount:inst->numIndex
			indexType:MTLIndexTypeUInt16 indexBuffer:(__bridge id<MTLBuffer>)header->mtlIndexBuffer
			indexBufferOffset:inst->offset];
	}
	return 1;
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_METAL);
	this->impl.instance = metal::instance;
	this->impl.uninstance = metal::uninstance;
	this->impl.render = metal::render;
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

void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	AttribDesc *attribs, *a;

	bool isPrelit = !!(geo->flags & Geometry::PRELIT);
	bool hasNormals = !!(geo->flags & Geometry::NORMALS);

	if(!reinstance){
		InstAttrib tmpAttribs[MAXINSTATTRIBS];
		header->numAttribs = defaultVertexLayout(hasNormals, isPrelit, geo->numTexCoordSets, tmpAttribs);
		header->attribDesc = rwNewT(AttribDesc, header->numAttribs, MEMDUR_EVENT | ID_GEOMETRY);
		memcpy(header->attribDesc, tmpAttribs,
		       header->numAttribs*sizeof(AttribDesc));
		header->vertexLayout = registerVertexLayout(header->attribDesc, header->numAttribs);
		header->vertexBuffer = rwNewT(uint8, header->totalNumVertex*header->attribDesc[0].stride,
		                              MEMDUR_EVENT | ID_GEOMETRY);
	}

	attribs = header->attribDesc;
	AttribDesc *end = attribs + header->numAttribs;
	uint8 *verts = header->vertexBuffer;

	if(!reinstance || geo->lockedSinceInst&Geometry::LOCKVERTICES){
		for(a = attribs; a->index != ATTRIB_POS; a++)
			;
		instV3d(VERT_FLOAT3, verts + a->offset,
			geo->morphTargets[0].vertices,
			header->totalNumVertex, a->stride);
	}

	if(hasNormals && (!reinstance || geo->lockedSinceInst&Geometry::LOCKNORMALS)){
		for(a = attribs; a->index != ATTRIB_NORMAL; a++)
			;
		instV3d(VERT_FLOAT3, verts + a->offset,
			geo->morphTargets[0].normals,
			header->totalNumVertex, a->stride);
	}

	if(isPrelit && (!reinstance || geo->lockedSinceInst&Geometry::LOCKPRELIGHT)){
		for(a = attribs; a->index != ATTRIB_COLOR; a++)
			;
		int n = header->numMeshes;
		InstanceData *inst = header->inst;
		while(n--){
			assert(inst->minVert != 0xFFFFFFFF);
			inst->vertexAlpha = instColor(VERT_RGBA,
				verts + a->offset + a->stride*inst->minVert,
				geo->colors + inst->minVert,
				inst->numVertices, a->stride);
			inst++;
		}
	}

	for(a = attribs; a != end; a++){
		if(a->index < ATTRIB_TEXCOORDS0)
			continue;
		int32 n = a->index - ATTRIB_TEXCOORDS0;
		if(!reinstance || geo->lockedSinceInst&(Geometry::LOCKTEXCOORDS<<n))
			instTexCoords(VERT_FLOAT2, verts + a->offset,
				geo->texCoords[n],
				header->totalNumVertex, a->stride);
	}

	void *old = header->mtlVertexBuffer;
	header->mtlVertexBuffer = newBuffer(header->vertexBuffer, header->totalNumVertex*attribs[0].stride);
	releaseBuffer(&old);
}

void
defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	assert(0 && "can't uninstance");
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

}
}
#endif
