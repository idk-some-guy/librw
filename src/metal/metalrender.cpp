#ifdef RW_METAL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"

#include "rwmetal.h"
#include "rwmetalshader.h"
#include "metalstate.h"
#include "metalkeys.h"

#include "rwmetalimpl.h"

namespace rw {
namespace metal {

void
drawInst_simple(InstanceDataHeader *header, InstanceData *inst)
{
	drawIndexed(header, inst);
}

void
drawInst(InstanceDataHeader *header, InstanceData *inst)
{
	drawInst_simple(header, inst);
}

void
setupVertexInput(InstanceDataHeader *header)
{
	setVertexLayout(header->vertexLayout);
}

void
teardownVertexInput(InstanceDataHeader *header)
{
}

void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	while(n--){
		m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);

		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		defaultShader->use(shaderVariant(vsBits & VSLIGHT_MASK, getAlphaTest()));

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
}

}
}

#endif
