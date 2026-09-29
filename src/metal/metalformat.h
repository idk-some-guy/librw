#ifndef RW_METAL_METALFORMAT_H
#define RW_METAL_METALFORMAT_H

#include <stdint.h>

namespace rw {
namespace metal {

enum
{
	RWFMT_C1555      = 0x0100,
	RWFMT_C565       = 0x0200,
	RWFMT_C4444      = 0x0300,
	RWFMT_LUM8       = 0x0400,
	RWFMT_C8888      = 0x0500,
	RWFMT_C888       = 0x0600,
	RWFMT_AUTOMIPMAP = 0x1000,
	RWFMT_PAL8       = 0x2000,
	RWFMT_PAL4       = 0x4000,
	RWFMT_MIPMAP     = 0x8000,
};

enum TexPixelFormat
{
	TEXFMT_INVALID = 0,
	TEXFMT_RGBA8,
	TEXFMT_BC1,
	TEXFMT_BC2,
	TEXFMT_BC3,
};

enum TexConv
{
	TEXCONV_NONE = 0,
	TEXCONV_RGB888,
	TEXCONV_ARGB1555,
	TEXCONV_RGB565,
	TEXCONV_ARGB4444,
	TEXCONV_LUM8,
	TEXCONV_PAL8,
	TEXCONV_PAL4,
};

struct TexFormat
{
	int32_t format;
	int32_t conv;
	int32_t bpp;
	int32_t texBpp;
	int32_t blockSize;
	int32_t depth;
	bool hasAlpha;
	bool isCompressed;
	bool alphaOne;
};

bool findTexFormat(int32_t rasterFormat, TexFormat *fmt);
bool findDXTFormat(int32_t dxt, bool hasAlpha, TexFormat *fmt);

int32_t mipChainLength(int32_t width, int32_t height);
void mipLevelDims(int32_t width, int32_t height, int32_t level, int32_t *levelWidth, int32_t *levelHeight);
int32_t numTexLevels(int32_t rasterFormat, int32_t width, int32_t height);
int32_t numLockLevels(int32_t rasterFormat, int32_t width, int32_t height);
uint32_t levelStride(const TexFormat &fmt, int32_t levelWidth);
uint32_t levelSize(const TexFormat &fmt, int32_t levelWidth, int32_t levelHeight);
int32_t filledPrefix(uint32_t mask);

bool convertToRGBA8(int32_t conv, uint8_t *dst, const uint8_t *src, int32_t n);
bool convertFromRGBA8(int32_t conv, uint8_t *dst, const uint8_t *src, int32_t n);

}
}

#endif
