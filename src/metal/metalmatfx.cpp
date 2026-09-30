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
#include "metalstate.h"
#include "metalkeys.h"

namespace rw {
namespace metal {

static ObjPipeline *matfxPipe;
Shader *matfxEnvShader;

bool32
openMatFX(void)
{
#include "shaders/matfx_env_metal.inc"
	const char *src[] = { header_metal_src, matfx_env_metal_src, nil };
	matfxEnvShader = Shader::create(src, "matfxEnvVS", "matfxEnvFS", VARIANT_ALL);
	return matfxEnvShader != nil;
}

void
closeMatFX(void)
{
	if(matfxEnvShader)
		matfxEnvShader->destroy();
	matfxEnvShader = nil;
}

void
matfxEnvMatrix(Frame *frame, RawMatrix *out)
{
	RawMatrix normal2texcoord = {
		{ 0.5f,  0.0f, 0.0f }, 0.0f,
		{ 0.0f, -0.5f, 0.0f }, 0.0f,
		{ 0.0f,  0.0f, 1.0f }, 0.0f,
		{ 0.5f,  0.5f, 0.0f }, 1.0f
	};
	RawMatrix invMtx;
	Matrix invMat;

	if(frame == nil && engine->currentCamera)
		frame = ((Camera*)engine->currentCamera)->getFrame();
	if(frame)
		Matrix::invert(&invMat, frame->getLTM());
	else
		invMat.setIdentity();
	convMatrix(&invMtx, &invMat);
	invMtx.pos.set(0.0f, 0.0f, 0.0f);
	if(MatFX::envMapFlipU)
		normal2texcoord.right.x = -0.5f;
	RawMatrix::mult(out, &invMtx, &normal2texcoord);
}

static void
matfxDefaultRender(InstanceDataHeader *header, InstanceData *inst, int32 vsBits, uint32 flags)
{
	Material *m = inst->material;
	setMaterial(flags, m->color, m->surfaceProps);
	setTexture(0, m->texture);
	rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
	defaultShader->use(drawVariant(vsBits));
	drawInst(header, inst);
}

static void
matfxEnvRender(InstanceDataHeader *header, InstanceData *inst, int32 vsBits, uint32 flags, MatFX::Env *env)
{
	static const RGBAf zero = { 0.0f, 0.0f, 0.0f, 0.0f };
	static const RGBAf one = { 1.0f, 1.0f, 1.0f, 1.0f };
	Material *m = inst->material;
	RawMatrix envMtx;
	RGBAf envcol;

	if(env->tex == nil || env->coefficient == 0.0f){
		matfxDefaultRender(header, inst, vsBits, flags);
		return;
	}
	setTexture(0, m->texture);
	setTexture(1, env->tex);
	matfxEnvMatrix(env->frame, &envMtx);
	setMaterial(flags, m->color, m->surfaceProps);
	float32 fxparams[4] = { env->coefficient, env->fbAlpha ? 0.0f : 1.0f, 0.0f, 0.0f };
	convColor(&envcol, MatFX::envMapUseMatColor ? &m->color : &MatFX::envMapColor);
	setMatFXConstants(&envMtx, fxparams, MatFX::envMapApplyLight ? &zero : &one, &envcol);
	rw::SetRenderState(VERTEXALPHA, 1);
	rw::SetRenderState(SRCBLEND, BLENDONE);
	matfxEnvShader->use(drawVariant(vsBits));
	drawInst(header, inst);
	rw::SetRenderState(SRCBLEND, BLENDSRCALPHA);
}

void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM(), atomic);
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	while(n--){
		MatFX *matfx = MatFX::get(inst->material);
		if(matfx && matfx->type == MatFX::ENVMAP)
			matfxEnvRender(header, inst, vsBits, flags, &matfx->fx[0].env);
		else
			matfxDefaultRender(header, inst, vsBits, flags);
		inst++;
	}
	teardownVertexInput(header);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = matfxRenderCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

static void*
matfxOpen(void *o, int32, int32)
{
	if(matfxPipe == nil)
		matfxPipe = makeMatFXPipeline();
	matFXGlobals.pipelines[PLATFORM_METAL] = matfxPipe;
	return o;
}

// atomics keep this pipeline across an engine restart, so it lives for the process
static void*
matfxClose(void *o, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_METAL] = nil;
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_METAL, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

}
}
#endif
