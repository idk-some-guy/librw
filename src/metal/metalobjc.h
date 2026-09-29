#ifndef RW_METAL_METALOBJC_H
#define RW_METAL_METALOBJC_H

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

#include "rwmetal.h"
#include "rwmetalplg.h"
#include "rwmetalimpl.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace rw {
namespace metal {

enum { MAXFRAMESINFLIGHT = 3 };

struct MetalContext
{
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;

	GLFWwindow *window;
	CAMetalLayer *layer;

	dispatch_semaphore_t frameSemaphore;
	bool frameStarted;
	id<MTLCommandBuffer> commandBuffer;
	id<MTLCommandBuffer> lastCommitted;
	id<MTLRenderCommandEncoder> encoder;
	bool encoderHasDepth;
	uint32 encoderWidth, encoderHeight;

	id<MTLRenderPipelineState> compositePipeline;
	id<MTLRenderPipelineState> clearPipelines[2][2];
	id<MTLDepthStencilState> clearDepthStates[2][2];
};

inline MetalContext *getContext(void) { return (MetalContext*)metalGlobals.context; }

inline id<MTLTexture>
getRasterTexture(Raster *raster)
{
	if(raster == nil)
		return nil;
	return (__bridge id<MTLTexture>)GETMETALRASTEREXT(raster)->texture;
}

}
}

#endif
