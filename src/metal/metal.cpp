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

namespace rw {
namespace metal {

static void*
driverOpen(void *o, int32, int32)
{
	engine->driver[PLATFORM_METAL]->defaultPipeline = makeDefaultPipeline();
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

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_METAL, 0, PLATFORM_METAL,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

}
}
#endif
