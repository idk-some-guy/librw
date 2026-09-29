#ifdef RW_METAL
#include "metalobjc.h"
#include "metalformat.h"
#include "metalstate.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace metal {

static_assert(RWFMT_C1555 == Raster::C1555 && RWFMT_C565 == Raster::C565 &&
              RWFMT_C4444 == Raster::C4444 && RWFMT_LUM8 == Raster::LUM8 &&
              RWFMT_C8888 == Raster::C8888 && RWFMT_C888 == Raster::C888 &&
              RWFMT_AUTOMIPMAP == Raster::AUTOMIPMAP && RWFMT_PAL8 == Raster::PAL8 &&
              RWFMT_PAL4 == Raster::PAL4 && RWFMT_MIPMAP == Raster::MIPMAP,
              "metalformat.h raster formats");

int32 nativeRasterOffset;

static id<MTLCommandBuffer> pendingMipmaps;
static id<MTLTexture> whiteTexture;

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

static MTLPixelFormat
getPixelFormat(int32 format)
{
	switch(format){
	case TEXFMT_RGBA8: return MTLPixelFormatRGBA8Unorm;
	case TEXFMT_BC1: return MTLPixelFormatBC1_RGBA;
	case TEXFMT_BC2: return MTLPixelFormatBC2_RGBA;
	case TEXFMT_BC3: return MTLPixelFormatBC3_RGBA;
	}
	return MTLPixelFormatInvalid;
}

static bool32
createSampledTexture(Raster *raster, const TexFormat &fmt, int32 numLevels)
{
	MetalContext *ctx = getContext();
	MetalRaster *natras = GETMETALRASTEREXT(raster);
	MTLTextureDescriptor *desc;
	id<MTLTexture> tex;

	if(ctx == nil)
		return 0;
	@autoreleasepool {
		desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:getPixelFormat(fmt.format)
			width:raster->width height:raster->height mipmapped:NO];
		desc.mipmapLevelCount = numLevels;
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		if(fmt.alphaOne)
			desc.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,
				MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
		tex = [ctx->device newTextureWithDescriptor:desc];
		if(tex == nil)
			return 0;
		natras->texture = (__bridge_retained void*)tex;
	}
	return 1;
}

static void
initSampler(MetalRaster *natras)
{
	natras->filterMode = 0;
	natras->addressU = 0;
	natras->addressV = 0;
	natras->maxAnisotropy = 1;
}

static int32
getDXT(int32 format)
{
	switch(format){
	case MTLPixelFormatBC1_RGBA: return 1;
	case MTLPixelFormatBC2_RGBA: return 3;
	case MTLPixelFormatBC3_RGBA: return 5;
	}
	return 0;
}

static bool
getLockFormat(Raster *raster, TexFormat *fmt)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster->parent);
	if(natras->isCompressed)
		return findDXTFormat(getDXT(natras->format), natras->hasAlpha, fmt);
	return findTexFormat(raster->format, fmt) && fmt->bpp != 0;
}

static void
waitForMipmaps(void)
{
	if(pendingMipmaps){
		[pendingMipmaps waitUntilCompleted];
		pendingMipmaps = nil;
	}
}

static void
generateMipmaps(id<MTLTexture> tex)
{
	MetalContext *ctx = getContext();
	id<MTLCommandBuffer> cb;
	id<MTLBlitCommandEncoder> blit;

	if(tex.mipmapLevelCount < 2)
		return;
	cb = [ctx->queue commandBuffer];
	blit = [cb blitCommandEncoder];
	[blit generateMipmapsForTexture:tex];
	[blit endEncoding];
	[cb commit];
	pendingMipmaps = cb;
}

static Raster*
createTextureLevels(Raster *raster, int32 numLevels)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster);
	TexFormat fmt;
	int32 texLevels;

	if(raster->format & (Raster::PAL4 | Raster::PAL8)){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}
	if(!findTexFormat(raster->format, &fmt)){
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	natras->format = getPixelFormat(fmt.format);
	natras->hasAlpha = fmt.hasAlpha;
	natras->bpp = fmt.bpp;
	raster->depth = fmt.depth;
	raster->stride = raster->width*natras->bpp;

	natras->autogenMipmap = (raster->format & (Raster::MIPMAP|Raster::AUTOMIPMAP)) == (Raster::MIPMAP|Raster::AUTOMIPMAP);
	texLevels = numTexLevels(raster->format, raster->width, raster->height);
	if(!natras->autogenMipmap && numLevels < texLevels)
		texLevels = numLevels < 1 ? 1 : numLevels;
	natras->numLevels = numLockLevels(raster->format, raster->width, raster->height);
	if(natras->numLevels > texLevels)
		natras->numLevels = texLevels;
	initSampler(natras);

	if(!createSampledTexture(raster, fmt, texLevels)){
		RWERROR((ERR_GENERAL, "can't create texture"));
		return nil;
	}
	return raster;
}

static Raster*
rasterCreateTexture(Raster *raster)
{
	return createTextureLevels(raster, numTexLevels(raster->format, raster->width, raster->height));
}

void
allocateTexture(Raster *raster, int32 numLevels)
{
	assert(raster->type == Raster::TEXTURE);
	if(createTextureLevels(raster, numLevels) == nil)
		return;
	raster->originalStride = raster->stride;
	raster->flags &= ~Raster::DONTALLOCATE;
}

static Raster*
rasterCreateCameraTexture(Raster *raster)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster);
	TexFormat fmt;

	if(raster->format & (Raster::PAL4 | Raster::PAL8)){
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}

	switch(raster->format & 0xF00){
	case Raster::C8888:
	case Raster::C888:
	case Raster::C1555:
		break;
	default:
		raster->format = (raster->format & ~0xF00) | Raster::C888;
		break;
	}
	findTexFormat(raster->format, &fmt);

	natras->format = MTLPixelFormatRGBA8Unorm;
	natras->hasAlpha = fmt.hasAlpha;
	natras->bpp = fmt.bpp;
	raster->stride = raster->width*natras->bpp;
	natras->autogenMipmap = (raster->format & (Raster::MIPMAP|Raster::AUTOMIPMAP)) == (Raster::MIPMAP|Raster::AUTOMIPMAP);
	initSampler(natras);

	if(!createTexture(raster, MTLPixelFormatRGBA8Unorm)){
		RWERROR((ERR_GENERAL, "can't create camera texture"));
		return nil;
	}
	if(!natras->hasAlpha){
		id<MTLTexture> tex = getRasterTexture(raster);
		id<MTLTexture> view = [tex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
			textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
			swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,
				MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne)];
		natras->sampleTexture = (__bridge_retained void*)view;
	}
	clearNewRasterTarget(raster);
	return raster;
}

void
allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster);
	TexFormat fmt;

	assert(raster->type == Raster::TEXTURE);
	if(!findDXTFormat(dxt, hasAlpha, &fmt)){
		RWERROR((ERR_INVRASTER));
		return;
	}
	if(!metalCaps.bcSupported){
		RWERROR((ERR_GENERAL, "no BC texture support"));
		return;
	}

	natras->format = getPixelFormat(fmt.format);
	natras->hasAlpha = hasAlpha;
	natras->bpp = 2;
	raster->depth = 16;
	raster->stride = levelStride(fmt, raster->width)/4;

	natras->isCompressed = 1;
	natras->numLevels = 1;
	if(raster->format & Raster::MIPMAP)
		natras->numLevels = numLevels;
	if(natras->numLevels > mipChainLength(raster->width, raster->height))
		natras->numLevels = mipChainLength(raster->width, raster->height);
	if(natras->numLevels < 1)
		natras->numLevels = 1;
	natras->autogenMipmap = 0;
	initSampler(natras);

	if(!createSampledTexture(raster, fmt, natras->numLevels)){
		RWERROR((ERR_GENERAL, "can't create texture"));
		return;
	}

	raster->originalStride = raster->stride;
	raster->flags &= ~Raster::DONTALLOCATE;
}

static bool32
readLevel(Raster *raster, int32 level, const TexFormat &fmt, uint8 *px)
{
	id<MTLTexture> tex = getRasterTexture(raster->parent);
	int32 n = raster->width*raster->height;
	uint8 *rgba;
	bool32 ok = 1;

	if(fmt.isCompressed){
		waitForMipmaps();
		[tex getBytes:px bytesPerRow:levelStride(fmt, raster->width)
			fromRegion:MTLRegionMake2D(0, 0, raster->width, raster->height) mipmapLevel:level];
		return 1;
	}
	rgba = fmt.conv == TEXCONV_NONE ? px : (uint8*)rwMalloc(n*4, MEMDUR_FUNCTION | ID_DRIVER);
	if(raster->type == Raster::CAMERATEXTURE)
		ok = readRasterPixels(raster, rgba);
	else{
		waitForMipmaps();
		[tex getBytes:rgba bytesPerRow:raster->width*4
			fromRegion:MTLRegionMake2D(0, 0, raster->width, raster->height) mipmapLevel:level];
	}
	if(rgba != px){
		if(ok)
			convertFromRGBA8(fmt.conv, px, rgba, n);
		rwFree(rgba);
	}
	return ok;
}

static void
markLevelFilled(MetalRaster *natras, int32 level)
{
	natras->filledMask |= 1u << level;
	natras->filledLevels = filledPrefix(natras->filledMask);
}

static void
writeLevel(Raster *raster, int32 level, const TexFormat &fmt, uint8 *px)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster->parent);
	id<MTLTexture> tex = getRasterTexture(raster->parent);
	int32 n = raster->width*raster->height;
	uint8 *rgba;

	if(fmt.isCompressed){
		waitForMipmaps();
		[tex replaceRegion:MTLRegionMake2D(0, 0, raster->width, raster->height) mipmapLevel:level
			withBytes:px bytesPerRow:levelStride(fmt, raster->width)];
		markLevelFilled(natras, level);
		return;
	}
	rgba = px;
	if(fmt.conv != TEXCONV_NONE){
		rgba = (uint8*)rwMalloc(n*4, MEMDUR_FUNCTION | ID_DRIVER);
		convertToRGBA8(fmt.conv, rgba, px, n);
	}
	if(raster->type == Raster::CAMERATEXTURE)
		writeRasterPixels(raster, rgba);
	else{
		waitForMipmaps();
		[tex replaceRegion:MTLRegionMake2D(0, 0, raster->width, raster->height) mipmapLevel:level
			withBytes:rgba bytesPerRow:raster->width*4];
		markLevelFilled(natras, level);
		if(level == 0 && natras->autogenMipmap){
			generateMipmaps(tex);
			natras->filledMask = (1u << tex.mipmapLevelCount) - 1;
			natras->filledLevels = (int8)tex.mipmapLevelCount;
		}
	}
	if(rgba != px)
		rwFree(rgba);
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
	natras->filledLevels = 0;
	natras->filledMask = 0;

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
	case Raster::NORMAL:
	case Raster::TEXTURE:
		ret = rasterCreateTexture(raster);
		break;
	case Raster::CAMERATEXTURE:
		ret = rasterCreateCameraTexture(raster);
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

	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE: {
		id<MTLTexture> tex = getRasterTexture(raster->parent);
		TexFormat fmt;
		int32 w, h;
		if(tex == nil || level < 0 || level >= (int32)tex.mipmapLevelCount ||
		   !getLockFormat(raster, &fmt)){
			RWERROR((ERR_INVRASTER));
			return nil;
		}
		if(raster->type == Raster::CAMERATEXTURE){
			raster->originalWidth = raster->width;
			raster->originalHeight = raster->height;
			raster->originalStride = raster->stride;
			raster->originalPixels = raster->pixels;
			w = raster->width;
			h = raster->height;
		}else
			mipLevelDims(raster->originalWidth, raster->originalHeight, level, &w, &h);
		raster->width = w;
		raster->height = h;
		raster->stride = fmt.isCompressed ? levelStride(fmt, w)/4 : levelStride(fmt, w);
		px = (uint8*)rwMalloc(levelSize(fmt, w, h), MEMDUR_EVENT | ID_DRIVER);
		if((lockMode & Raster::LOCKREAD || !(lockMode & Raster::LOCKNOFETCH)) &&
		   !readLevel(raster, level, fmt, px)){
			rwFree(px);
			raster->width = raster->originalWidth;
			raster->height = raster->originalHeight;
			raster->stride = raster->originalStride;
			return nil;
		}
		raster->pixels = px;
		raster->privateFlags = lockMode;
		return px;
	}

	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}
}

void
rasterUnlock(Raster *raster, int32 level)
{
	TexFormat fmt;

	if(raster->pixels == nil)
		return;

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		if(raster->privateFlags & Raster::LOCKWRITE && getLockFormat(raster, &fmt))
			@autoreleasepool {
				writeLevel(raster, level, fmt, raster->pixels);
			}
		break;
	}

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

bool32
imageFindRasterFormat(Image *img, int32 type,
	int32 *pWidth, int32 *pHeight, int32 *pDepth, int32 *pFormat)
{
	int32 width, height, depth, format;

	assert((type&0xF) == Raster::TEXTURE);

	width = img->width;
	height = img->height;

	depth = img->depth;

	if(depth <= 8)
		depth = 32;

	switch(depth){
	case 32:
		if(img->hasAlpha())
			format = Raster::C8888;
		else{
			format = Raster::C888;
			depth = 24;
		}
		break;
	case 24:
		format = Raster::C888;
		break;
	case 16:
		format = Raster::C1555;
		break;

	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	format |= type;

	*pWidth = width;
	*pHeight = height;
	*pDepth = depth;
	*pFormat = format;

	return 1;
}

static int32
getImageConv(int32 depth)
{
	switch(depth){
	case 32: return TEXCONV_NONE;
	case 24: return TEXCONV_RGB888;
	case 16: return TEXCONV_ARGB1555;
	}
	return -1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster->parent);
	TexFormat fmt;
	int32 conv, y;

	if((raster->type&0xF) != Raster::TEXTURE)
		return 0;
	if(natras->isCompressed || !getLockFormat(raster, &fmt)){
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	Image *truecolimg = nil;
	if(image->depth <= 8){
		truecolimg = Image::create(image->width, image->height, image->depth);
		truecolimg->pixels = image->pixels;
		truecolimg->stride = image->stride;
		truecolimg->palette = image->palette;
		truecolimg->unpalettize();
		image = truecolimg;
	}

	conv = getImageConv(image->depth);
	if(conv < 0){
		RWERROR((ERR_INVRASTER));
		if(truecolimg)
			truecolimg->destroy();
		return 0;
	}

	natras->hasAlpha = image->hasAlpha();

	bool unlock = false;
	if(raster->pixels == nil){
		if(raster->lock(0, Raster::LOCKWRITE|Raster::LOCKNOFETCH) == nil){
			if(truecolimg)
				truecolimg->destroy();
			return 0;
		}
		unlock = true;
	}

	uint8 *pixels = raster->pixels;
	uint8 *imgpixels = image->pixels;
	assert(pixels);
	assert(image->width == raster->width);
	assert(image->height == raster->height);
	uint8 *row = (uint8*)rwMalloc(image->width*4, MEMDUR_FUNCTION | ID_DRIVER);
	for(y = 0; y < image->height; y++){
		convertToRGBA8(conv, row, imgpixels, image->width);
		convertFromRGBA8(fmt.conv, pixels, row, image->width);
		imgpixels += image->stride;
		pixels += raster->stride;
	}
	rwFree(row);
	if(unlock)
		raster->unlock(0);

	if(truecolimg)
		truecolimg->destroy();

	return 1;
}

static Image*
textureToImage(Raster *raster)
{
	MetalRaster *natras = GETMETALRASTEREXT(raster->parent);
	TexFormat fmt;
	Image *image;
	uint8 *in, *out;
	int32 depth, y;

	if(natras->isCompressed || !getLockFormat(raster, &fmt)){
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	switch(fmt.conv){
	case TEXCONV_RGB888: depth = 24; break;
	case TEXCONV_ARGB1555: depth = 16; break;
	default: depth = 32; break;
	}

	bool unlock = false;
	if(raster->pixels == nil){
		if(raster->lock(0, Raster::LOCKREAD) == nil)
			return nil;
		unlock = true;
	}

	image = Image::create(raster->width, raster->height, depth);
	image->allocate();
	in = raster->pixels;
	out = image->pixels;
	for(y = 0; y < image->height; y++){
		if(depth == 32)
			convertToRGBA8(fmt.conv, out, in, image->width);
		else
			memcpy(out, in, image->width*image->bpp);
		in += raster->stride;
		out += image->stride;
	}

	if(unlock)
		raster->unlock(0);

	return image;
}

Image*
rasterToImage(Raster *raster)
{
	Image *image;
	uint8 *in, *out;
	int32 y;

	if(raster->type != Raster::CAMERA)
		return textureToImage(raster);
	if((raster->format & 0xF00) != Raster::C8888){
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
	ras->sampleTexture = nil;
	ras->format = 0;
	ras->bpp = 0;
	ras->isCompressed = 0;
	ras->hasAlpha = 0;
	ras->autogenMipmap = 0;
	ras->numLevels = 1;
	ras->filledLevels = 0;
	ras->filledMask = 0;
	initSampler(ras);
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	Raster *raster = (Raster*)object;
	MetalRaster *natras = PLUGINOFFSET(MetalRaster, object, offset);

	evictRaster(raster);
	@autoreleasepool {
		forgetRasterTarget(raster);
		if(natras->texture){
			id<MTLTexture> tex = (__bridge_transfer id<MTLTexture>)natras->texture;
			tex = nil;
			natras->texture = nil;
		}
		if(natras->sampleTexture){
			id<MTLTexture> view = (__bridge_transfer id<MTLTexture>)natras->sampleTexture;
			view = nil;
			natras->sampleTexture = nil;
		}
	}
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	MetalRaster *d = PLUGINOFFSET(MetalRaster, dst, offset);
	d->texture = nil;
	d->sampleTexture = nil;
	return dst;
}

void*
getRasterSampleTexture(Raster *raster)
{
	MetalRaster *natras;

	if(raster == nil)
		return nil;
	natras = GETMETALRASTEREXT(raster->parent);
	return natras->sampleTexture ? natras->sampleTexture : natras->texture;
}

void*
getWhiteTexture(void)
{
	MetalContext *ctx = getContext();
	static const uint8 white[4] = { 255, 255, 255, 255 };

	if(ctx == nil)
		return nil;
	if(whiteTexture == nil || whiteTexture.device != ctx->device) @autoreleasepool {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			width:1 height:1 mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		whiteTexture = [ctx->device newTextureWithDescriptor:desc];
		[whiteTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
			withBytes:white bytesPerRow:4];
	}
	return (__bridge void*)whiteTexture;
}

void
termRaster(void)
{
	waitForMipmaps();
	whiteTexture = nil;
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
