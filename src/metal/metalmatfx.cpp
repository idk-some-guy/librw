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

void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	defaultRenderCB(atomic, header);
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
