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

namespace rw {
namespace metal {

static ObjPipeline *matfxPipe;

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
	matfxPipe = makeMatFXPipeline();
	matFXGlobals.pipelines[PLATFORM_METAL] = matfxPipe;
	return o;
}

// skinClose clears this slot first while the skin pipeline is still the dummy
static void*
matfxClose(void *o, int32, int32)
{
	if(matFXGlobals.pipelines[PLATFORM_METAL] == matfxPipe)
		matFXGlobals.pipelines[PLATFORM_METAL] = nil;
	matfxPipe->destroy();
	matfxPipe = nil;
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
