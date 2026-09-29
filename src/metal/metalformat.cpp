#include <string.h>
#include "metalformat.h"

namespace rw {
namespace metal {

static void
setFormat(TexFormat *fmt, int32_t format, int32_t conv, int32_t bpp, int32_t depth, bool hasAlpha)
{
	fmt->format = format;
	fmt->conv = conv;
	fmt->bpp = bpp;
	fmt->texBpp = 4;
	fmt->blockSize = 0;
	fmt->depth = depth;
	fmt->hasAlpha = hasAlpha;
	fmt->isCompressed = false;
	fmt->alphaOne = false;
}

static bool
findColorFormat(int32_t color, TexFormat *fmt)
{
	switch(color){
	case RWFMT_C8888:
		setFormat(fmt, TEXFMT_RGBA8, TEXCONV_NONE, 4, 32, true);
		return true;
	case RWFMT_C888:
		setFormat(fmt, TEXFMT_RGBA8, TEXCONV_RGB888, 3, 24, false);
		return true;
	case RWFMT_C1555:
		setFormat(fmt, TEXFMT_RGBA8, TEXCONV_ARGB1555, 2, 16, true);
		return true;
	case RWFMT_C565:
		setFormat(fmt, TEXFMT_RGBA8, TEXCONV_RGB565, 2, 16, false);
		return true;
	case RWFMT_C4444:
		setFormat(fmt, TEXFMT_RGBA8, TEXCONV_ARGB4444, 2, 16, true);
		return true;
	case RWFMT_LUM8:
		setFormat(fmt, TEXFMT_RGBA8, TEXCONV_LUM8, 1, 8, false);
		return true;
	}
	return false;
}

bool
findTexFormat(int32_t rasterFormat, TexFormat *fmt)
{
	int32_t pal = rasterFormat & (RWFMT_PAL8|RWFMT_PAL4);

	if(!findColorFormat(rasterFormat & 0xF00, fmt))
		return false;
	if(pal == 0)
		return true;
	if(pal == (RWFMT_PAL8|RWFMT_PAL4) || fmt->conv == TEXCONV_LUM8)
		return false;
	fmt->conv = pal == RWFMT_PAL8 ? TEXCONV_PAL8 : TEXCONV_PAL4;
	fmt->bpp = 0;
	fmt->depth = pal == RWFMT_PAL8 ? 8 : 4;
	return true;
}

bool
findDXTFormat(int32_t dxt, bool hasAlpha, TexFormat *fmt)
{
	switch(dxt){
	case 1:
		fmt->format = TEXFMT_BC1;
		fmt->blockSize = 8;
		break;
	case 3:
		fmt->format = TEXFMT_BC2;
		fmt->blockSize = 16;
		break;
	case 5:
		fmt->format = TEXFMT_BC3;
		fmt->blockSize = 16;
		break;
	default:
		return false;
	}
	fmt->conv = TEXCONV_NONE;
	fmt->bpp = 0;
	fmt->texBpp = 0;
	fmt->depth = 16;
	fmt->hasAlpha = hasAlpha;
	fmt->isCompressed = true;
	fmt->alphaOne = dxt == 1 && !hasAlpha;
	return true;
}

int32_t
mipChainLength(int32_t width, int32_t height)
{
	int32_t n = 1;
	while(width > 1 || height > 1){
		width /= 2;
		height /= 2;
		n++;
	}
	return n;
}

void
mipLevelDims(int32_t width, int32_t height, int32_t level, int32_t *levelWidth, int32_t *levelHeight)
{
	width >>= level;
	height >>= level;
	*levelWidth = width > 0 ? width : 1;
	*levelHeight = height > 0 ? height : 1;
}

int32_t
numTexLevels(int32_t rasterFormat, int32_t width, int32_t height)
{
	if(rasterFormat & RWFMT_MIPMAP)
		return mipChainLength(width, height);
	return 1;
}

int32_t
numLockLevels(int32_t rasterFormat, int32_t width, int32_t height)
{
	if(rasterFormat & RWFMT_AUTOMIPMAP)
		return 1;
	return numTexLevels(rasterFormat, width, height);
}

uint32_t
levelStride(const TexFormat &fmt, int32_t levelWidth)
{
	if(fmt.isCompressed)
		return (uint32_t)((levelWidth+3)/4 * fmt.blockSize);
	return (uint32_t)(levelWidth * fmt.bpp);
}

uint32_t
levelSize(const TexFormat &fmt, int32_t levelWidth, int32_t levelHeight)
{
	if(fmt.isCompressed)
		return levelStride(fmt, levelWidth) * (uint32_t)((levelHeight+3)/4);
	return levelStride(fmt, levelWidth) * (uint32_t)levelHeight;
}

static uint8_t
expand5(uint32_t v)
{
	return (uint8_t)(v*0xFF/0x1F);
}

bool
convertToRGBA8(int32_t conv, uint8_t *dst, const uint8_t *src, int32_t n)
{
	int32_t i;
	uint32_t v;

	switch(conv){
	case TEXCONV_NONE:
		memcpy(dst, src, n*4);
		return true;
	case TEXCONV_RGB888:
		for(i = 0; i < n; i++, dst += 4, src += 3){
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = 0xFF;
		}
		return true;
	case TEXCONV_ARGB1555:
		for(i = 0; i < n; i++, dst += 4, src += 2){
			v = src[0] | src[1]<<8;
			dst[0] = expand5((v>>10) & 0x1F);
			dst[1] = expand5((v>>5) & 0x1F);
			dst[2] = expand5(v & 0x1F);
			dst[3] = v & 0x8000 ? 0xFF : 0;
		}
		return true;
	case TEXCONV_RGB565:
		for(i = 0; i < n; i++, dst += 4, src += 2){
			v = src[0] | src[1]<<8;
			dst[0] = expand5((v>>11) & 0x1F);
			dst[1] = (uint8_t)(((v>>5) & 0x3F)*0xFF/0x3F);
			dst[2] = expand5(v & 0x1F);
			dst[3] = 0xFF;
		}
		return true;
	case TEXCONV_ARGB4444:
		for(i = 0; i < n; i++, dst += 4, src += 2){
			v = src[0] | src[1]<<8;
			dst[0] = ((v>>8) & 0xF)*0x11;
			dst[1] = ((v>>4) & 0xF)*0x11;
			dst[2] = (v & 0xF)*0x11;
			dst[3] = ((v>>12) & 0xF)*0x11;
		}
		return true;
	case TEXCONV_LUM8:
		for(i = 0; i < n; i++, dst += 4, src++){
			dst[0] = dst[1] = dst[2] = src[0];
			dst[3] = 0xFF;
		}
		return true;
	}
	return false;
}

bool
convertFromRGBA8(int32_t conv, uint8_t *dst, const uint8_t *src, int32_t n)
{
	int32_t i;
	uint32_t v;

	switch(conv){
	case TEXCONV_NONE:
		memcpy(dst, src, n*4);
		return true;
	case TEXCONV_RGB888:
		for(i = 0; i < n; i++, dst += 3, src += 4){
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}
		return true;
	case TEXCONV_ARGB1555:
		for(i = 0; i < n; i++, dst += 2, src += 4){
			v = (src[3] >= 0x80) << 15 | (src[0]>>3) << 10 | (src[1]>>3) << 5 | src[2]>>3;
			dst[0] = v & 0xFF;
			dst[1] = v >> 8;
		}
		return true;
	case TEXCONV_RGB565:
		for(i = 0; i < n; i++, dst += 2, src += 4){
			v = (src[0]>>3) << 11 | (src[1]>>2) << 5 | src[2]>>3;
			dst[0] = v & 0xFF;
			dst[1] = v >> 8;
		}
		return true;
	case TEXCONV_ARGB4444:
		for(i = 0; i < n; i++, dst += 2, src += 4){
			v = (src[3]>>4) << 12 | (src[0]>>4) << 8 | (src[1]>>4) << 4 | src[2]>>4;
			dst[0] = v & 0xFF;
			dst[1] = v >> 8;
		}
		return true;
	case TEXCONV_LUM8:
		for(i = 0; i < n; i++, dst++, src += 4)
			dst[0] = src[0];
		return true;
	}
	return false;
}

}
}
