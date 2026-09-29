#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace rw {
namespace metal {

struct MetalContext
{
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;

	GLFWwindow *window;
	CAMetalLayer *layer;

	id<MTLCommandBuffer> commandBuffer;
	id<MTLRenderCommandEncoder> encoder;
};

inline MetalContext *getContext(void) { return (MetalContext*)metalGlobals.context; }

}
}
