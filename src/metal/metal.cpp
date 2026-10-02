#ifdef RW_METAL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"

#include "rwmetal.h"

#include "rwmetalimpl.h"
#include "metalstate.h"

namespace rw {
namespace metal {

static ObjPipeline *defaultPipe;

static void*
driverOpen(void *o, int32, int32)
{
	if(defaultPipe == nil)
		defaultPipe = makeDefaultPipeline();
	engine->driver[PLATFORM_METAL]->defaultPipeline = defaultPipe;
	engine->driver[PLATFORM_METAL]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_METAL]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_METAL]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_METAL]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_METAL]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_METAL]->imageFindRasterFormat = imageFindRasterFormat;
	engine->driver[PLATFORM_METAL]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_METAL]->rasterToImage      = rasterToImage;

	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	return o;
}

static void*
destroyAtomicHistory(void *object, int32, int32)
{
	forgetObjectHistory(object);
	return object;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_METAL, 0, PLATFORM_METAL,
	                       driverOpen, driverClose);
	Atomic::registerPlugin(0, MAKEPLUGINID(VEND_DRIVER, PLATFORM_METAL), nil, destroyAtomicHistory, nil);
	registerNativeRaster();
}

}
}
#endif
