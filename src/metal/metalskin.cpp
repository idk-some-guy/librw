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
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwmetal.h"
#include "rwmetalplg.h"
#include "rwmetalimpl.h"
#include "metalinst.h"

namespace rw {
namespace metal {

static ObjPipeline *skinPipe;

void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	AttribDesc *a;
	Skin *skin = Skin::get(geo);
	if(skin == nil){
		defaultInstanceCB(geo, header, reinstance);
		return;
	}
	if(!reinstance){
		InstAttrib tmp[MAXINSTATTRIBS];
		AttribDesc descs[MAXINSTATTRIBS];
		int32 n = skinVertexLayout(!!(geo->flags & Geometry::NORMALS), !!(geo->flags & Geometry::PRELIT),
		                           geo->numTexCoordSets, tmp);
		memcpy(descs, tmp, n*sizeof(AttribDesc));
		allocInstanceVertices(header, descs, n);
	}
	instanceDefaultAttribs(geo, header, reinstance);
	for(uint32 i = 0; i < header->numMeshes; i++)
		header->inst[i].vertexAlpha = 0;
	if(!reinstance){
		for(a = header->attribDesc; a->index != ATTRIB_WEIGHTS; a++)
			;
		instV4d(VERT_FLOAT4, header->vertexBuffer + a->offset, (V4d*)skin->weights,
			header->totalNumVertex, a->stride);
		for(a = header->attribDesc; a->index != ATTRIB_INDICES; a++)
			;
		instColor(VERT_RGBA, header->vertexBuffer + a->offset, (RGBA*)skin->indices,
			header->totalNumVertex, a->stride);
	}
	uploadInstanceVertices(header);
}

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

static void*
skinOpen(void *o, int32, int32)
{
	if(skinPipe == nil)
		skinPipe = makeSkinPipeline();
	skinGlobals.pipelines[PLATFORM_METAL] = skinPipe;
	return o;
}

// atomics keep this pipeline across an engine restart, so it lives for the process
static void*
skinClose(void *o, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_METAL] = nil;
	return o;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_METAL, 0, ID_SKIN,
	                       skinOpen, skinClose);
}

}
}
#endif
