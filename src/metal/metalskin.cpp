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
#include "rwmetalshader.h"
#include "rwmetalplg.h"
#include "rwmetalimpl.h"
#include "metalinst.h"
#include "metalstate.h"
#include "metalkeys.h"

#define PLUGIN_ID ID_SKIN

namespace rw {
namespace metal {

static ObjPipeline *skinPipe;
Shader *skinShader;
static RawMatrix skinMatrices[MAXSKINBONES];

bool32
openSkin(void)
{
#include "shaders/skin_metal.inc"
#include "shaders/simple_metal.inc"
	const char *src[] = { header_metal_src, skin_metal_src, simple_metal_src, nil };
	skinShader = Shader::create(src, "skinVS", "simpleFS", VARIANT_ALL);
	return skinShader != nil;
}

void
closeSkin(void)
{
	if(skinShader)
		skinShader->destroy();
	skinShader = nil;
}

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

void
uploadSkinMatrices(Atomic *a)
{
	static bool reported;
	int32 i, n;
	Skin *skin = Skin::get(a->geometry);
	HAnimHierarchy *hier = Skin::getHierarchy(a);
	Matrix m, tmp;

	if(hier){
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		n = hier->numNodes;
		if((n != skin->numBones || n > MAXSKINBONES) && !reported){
			reported = true;
			RWERROR((ERR_GENERAL, "skin bone count differs from its hierarchy or exceeds 64"));
		}
		if(n > skin->numBones)
			n = skin->numBones;
		if(n > MAXSKINBONES)
			n = MAXSKINBONES;
		if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(i = 0; i < n; i++){
				invMats[i].flags = 0;
				Matrix::mult(&m, &invMats[i], &hier->matrices[i]);
				convMatrix(&skinMatrices[i], &m);
			}
		}else{
			Matrix invAtmMat;
			Matrix::invert(&invAtmMat, a->getFrame()->getLTM());
			for(i = 0; i < n; i++){
				invMats[i].flags = 0;
				Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
				Matrix::mult(&m, &invMats[i], &tmp);
				convMatrix(&skinMatrices[i], &m);
			}
		}
	}else{
		n = skin->numBones < MAXSKINBONES ? skin->numBones : MAXSKINBONES;
		m.setIdentity();
		for(i = 0; i < n; i++)
			convMatrix(&skinMatrices[i], &m);
	}
	setSkinMatrices(skinMatrices, n);
}

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Material *m;

	if(Skin::get(atomic->geometry) == nil){
		defaultRenderCB(atomic, header);
		return;
	}
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	uploadSkinMatrices(atomic);

	while(n--){
		m = inst->material;
		setMaterial(flags, m->color, m->surfaceProps);
		setTexture(0, m->texture);
		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		skinShader->use(shaderVariant(vsBits & VSLIGHT_MASK, getAlphaTest()));
		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
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
