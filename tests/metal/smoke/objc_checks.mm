#include <src/metal/metalobjc.h>
#include "objc_checks.h"

using namespace rw::metal;

static __weak id watched;
static id<MTLCommandBuffer> testCommandBuffer;

bool
TextureTexel(void *texture, int *width, int *height, unsigned char rgba[4])
{
	id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
	if(tex == nil)
		return false;
	*width = (int)tex.width;
	*height = (int)tex.height;
	[tex getBytes:rgba bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
	return true;
}

bool
TextureTexelAt(void *texture, int level, int x, int y, unsigned char rgba[4])
{
	id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
	if(tex == nil || level >= (int)tex.mipmapLevelCount)
		return false;
	[tex getBytes:rgba bytesPerRow:4 fromRegion:MTLRegionMake2D(x, y, 1, 1) mipmapLevel:level];
	return true;
}

int
TextureLevels(void *texture)
{
	id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
	return tex ? (int)tex.mipmapLevelCount : 0;
}

bool
BufferBytes(void *buffer, unsigned offset, void *dst, unsigned size)
{
	id<MTLBuffer> buf = (__bridge id<MTLBuffer>)buffer;
	if(buf == nil || buf.storageMode != MTLStorageModeShared || (NSUInteger)offset + size > buf.length)
		return false;
	memcpy(dst, (uint8*)buf.contents + offset, size);
	return true;
}

bool
BufferOnCurrentDevice(void *buffer)
{
	MetalContext *ctx = getContext();
	id<MTLBuffer> buf = (__bridge id<MTLBuffer>)buffer;
	return ctx != nil && buf != nil && buf.device == ctx->device;
}

void
WatchObject(void *object)
{
	watched = (__bridge id)object;
}

bool
WatchedObjectAlive(void)
{
	return watched != nil;
}

bool
BeginTestEncoder(void)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || ctx->encoder != nil)
		return false;
	@autoreleasepool {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			width:4 height:4 mipmapped:NO];
		desc.usage = MTLTextureUsageRenderTarget;
		desc.storageMode = MTLStorageModePrivate;
		id<MTLTexture> target = [ctx->device newTextureWithDescriptor:desc];
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = target;
		pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
		testCommandBuffer = [ctx->queue commandBuffer];
		ctx->encoder = [testCommandBuffer renderCommandEncoderWithDescriptor:pass];
		ctx->encoderHasDepth = false;
		ctx->encoderSamples = 1;
		ctx->encoderWidth = 4;
		ctx->encoderHeight = 4;
	}
	return ctx->encoder != nil;
}

void
RunInPool(void (*fn)(void *arg), void *arg)
{
	@autoreleasepool {
		fn(arg);
	}
}

bool
DefaultSamplePositions(int count, float *xy)
{
	MetalContext *ctx = getContext();
	MTLSamplePosition pos[8];
	if(ctx == nil || count < 1 || count > 8)
		return false;
	[ctx->device getDefaultSamplePositions:pos count:count];
	for(int i = 0; i < count; i++){
		xy[2*i] = pos[i].x;
		xy[2*i+1] = pos[i].y;
	}
	return true;
}

void
EndTestEncoder(void)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || testCommandBuffer == nil)
		return;
	@autoreleasepool {
		[ctx->encoder endEncoding];
		ctx->encoder = nil;
		[testCommandBuffer commit];
		[testCommandBuffer waitUntilCompleted];
		testCommandBuffer = nil;
	}
}

bool
LayerDrawableSize(int *width, int *height, double *scale)
{
	MetalContext *ctx = getContext();
	if(ctx == nil || ctx->layer == nil)
		return false;
	*width = (int)ctx->layer.drawableSize.width;
	*height = (int)ctx->layer.drawableSize.height;
	*scale = ctx->layer.contentsScale;
	return true;
}
