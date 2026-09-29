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

static ObjPipeline *skinPipe;

ObjPipeline*
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
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
