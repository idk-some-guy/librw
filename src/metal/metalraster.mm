#ifdef RW_METAL
#include "metalobjc.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace metal {

int32 nativeRasterOffset;

static bool32
createTexture(Raster *raster, MTLPixelFormat format)
{
	MetalContext *ctx = getContext();
	MetalRaster *natras = GETMETALRASTEREXT(raster);
	MTLTextureDescriptor *desc;
	id<MTLTexture> tex;

	if(ctx == nil)
		return 0;
	@autoreleasepool {
		desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
			width:raster->width height:raster->height mipmapped:NO];
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModePrivate;
		tex = [ctx->device newTextureWithDescriptor:desc];
		if(tex == nil)
			return 0;
		natras->texture = (__bridge_retained void*)tex;
	}
	return 1;
}

static Raster*
rasterCreateCamera(Raster *raster)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster);

	raster->format = Raster::C8888;
	natras->format = MTLPixelFormatRGBA8Unorm;
	natras->hasAlpha = 1;
	natras->bpp = 4;
	natras->autogenMipmap = 0;

	if(!createTexture(raster, MTLPixelFormatRGBA8Unorm)){
		RWERROR((ERR_GENERAL, "can't create camera texture"));
		return nil;
	}
	return raster;
}

static Raster*
rasterCreateZbuffer(Raster *raster)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster);

	natras->format = MTLPixelFormatDepth32Float_Stencil8;
	natras->bpp = 5;
	natras->autogenMipmap = 0;

	if(!createTexture(raster, MTLPixelFormatDepth32Float_Stencil8)){
		RWERROR((ERR_GENERAL, "can't create depth texture"));
		return nil;
	}
	return raster;
}

Raster*
rasterCreate(Raster *raster)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster);

	natras->isCompressed = 0;
	natras->hasAlpha = 0;
	natras->numLevels = 1;

	Raster *ret = raster;

	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		goto ret;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		goto ret;

	switch(raster->type){
	case Raster::CAMERA:
		ret = rasterCreateCamera(raster);
		break;
	case Raster::ZBUFFER:
		ret = rasterCreateZbuffer(raster);
		break;

	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	if(ret == nil)
		return nil;

ret:
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	raster->originalPixels = raster->pixels;
	return ret;
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster->parent);
	uint8 *px;

	if(raster->pixels != nil || raster->privateFlags != 0){
		RWERROR((ERR_GENERAL, "raster is already locked"));
		return nil;
	}

	switch(raster->type){
	case Raster::CAMERA:
		if(level != 0 || natras->texture == nil){
			RWERROR((ERR_INVRASTER));
			return nil;
		}
		raster->originalWidth = raster->width;
		raster->originalHeight = raster->height;
		raster->originalStride = raster->stride;
		raster->originalPixels = raster->pixels;
		raster->stride = raster->width*natras->bpp;
		px = (uint8*)rwMalloc(raster->height*raster->stride, MEMDUR_EVENT | ID_DRIVER);
		if(!readRasterPixels(raster, px)){
			rwFree(px);
			raster->stride = raster->originalStride;
			return nil;
		}
		raster->pixels = px;
		raster->privateFlags = lockMode;
		return px;

	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}
}

void
rasterUnlock(Raster *raster, int32 level)
{
	if(raster->pixels == nil)
		return;

	rwFree(raster->pixels);
	raster->pixels = nil;
	raster->width = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->pixels = raster->originalPixels;
	raster->privateFlags = 0;
}

int32
rasterNumLevels(Raster *raster)
{
	return GETMETALRASTEREXT(raster)->numLevels;
}

Image*
rasterToImage(Raster *raster)
{
	Image *image;
	uint8 *in, *out;
	int32 y;

	if(raster->type != Raster::CAMERA || (raster->format & 0xF00) != Raster::C8888){
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	bool unlock = false;
	if(raster->pixels == nil){
		if(raster->lock(0, Raster::LOCKREAD) == nil)
			return nil;
		unlock = true;
	}

	image = Image::create(raster->width, raster->height, 32);
	image->allocate();
	in = raster->pixels;
	out = image->pixels;
	for(y = 0; y < image->height; y++){
		memcpy(out, in, image->width*4);
		in += raster->stride;
		out += image->stride;
	}

	if(unlock)
		raster->unlock(0);

	return image;
}

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	MetalRaster *ras = PLUGINOFFSET(MetalRaster, object, offset);
	ras->texture = nil;
	ras->fboMate = nil;
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	Raster *raster = (Raster*)object;
	MetalRaster *natras = PLUGINOFFSET(MetalRaster, object, offset);

	@autoreleasepool {
		forgetRasterTarget(raster);
		if(natras->texture){
			id<MTLTexture> tex = (__bridge_transfer id<MTLTexture>)natras->texture;
			tex = nil;
			natras->texture = nil;
		}
	}
	natras->fboMate = nil;
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	MetalRaster *d = PLUGINOFFSET(MetalRaster, dst, offset);
	d->texture = nil;
	d->fboMate = nil;
	return dst;
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(MetalRaster),
	                                            ID_RASTERMETAL,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

}
}
#endif
