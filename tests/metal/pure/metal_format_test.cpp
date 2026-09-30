#include <cstdio>
#include <cstring>
#include "metalformat.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

struct Want
{
	int32_t rasterFormat;
	int32_t format;
	int32_t conv;
	int32_t bpp;
	int32_t texBpp;
	int32_t depth;
	bool hasAlpha;
};

static void
CheckUncompressed(const Want &w, int line)
{
	TexFormat f;
	memset(&f, 0xAB, sizeof(f));
	bool ok = findTexFormat(w.rasterFormat, &f);
	if(!ok){
		printf("FAIL line %d: findTexFormat(0x%x) returned false\n", line, w.rasterFormat);
		failures++;
		return;
	}
	if(f.format != w.format || f.conv != w.conv || f.bpp != w.bpp || f.texBpp != w.texBpp ||
	   f.depth != w.depth || f.hasAlpha != w.hasAlpha || f.isCompressed || f.alphaOne || f.blockSize != 0){
		printf("FAIL line %d: format 0x%x gave format %d conv %d bpp %d texBpp %d depth %d alpha %d compressed %d alphaOne %d block %d\n",
		       line, w.rasterFormat, f.format, f.conv, f.bpp, f.texBpp, f.depth,
		       f.hasAlpha, f.isCompressed, f.alphaOne, f.blockSize);
		failures++;
	}
}

#define UNCOMPRESSED(...) do { Want w = { __VA_ARGS__ }; CheckUncompressed(w, __LINE__); } while(0)

static void
TestUncompressedFormats(void)
{
	UNCOMPRESSED(RWFMT_C8888, TEXFMT_RGBA8, TEXCONV_NONE, 4, 4, 32, true);
	UNCOMPRESSED(RWFMT_C888, TEXFMT_RGBA8, TEXCONV_RGB888, 3, 4, 24, false);
	UNCOMPRESSED(RWFMT_C1555, TEXFMT_RGBA8, TEXCONV_ARGB1555, 2, 4, 16, true);
	UNCOMPRESSED(RWFMT_C565, TEXFMT_RGBA8, TEXCONV_RGB565, 2, 4, 16, false);
	UNCOMPRESSED(RWFMT_C4444, TEXFMT_RGBA8, TEXCONV_ARGB4444, 2, 4, 16, true);
	UNCOMPRESSED(RWFMT_LUM8, TEXFMT_RGBA8, TEXCONV_LUM8, 1, 4, 8, false);
}

static void
TestMipmapFlagsDoNotChangeFormat(void)
{
	UNCOMPRESSED(RWFMT_C8888|RWFMT_MIPMAP, TEXFMT_RGBA8, TEXCONV_NONE, 4, 4, 32, true);
	UNCOMPRESSED(RWFMT_C888|RWFMT_MIPMAP|RWFMT_AUTOMIPMAP, TEXFMT_RGBA8, TEXCONV_RGB888, 3, 4, 24, false);
	UNCOMPRESSED(RWFMT_C1555|RWFMT_MIPMAP, TEXFMT_RGBA8, TEXCONV_ARGB1555, 2, 4, 16, true);
	UNCOMPRESSED(RWFMT_C565|RWFMT_MIPMAP, TEXFMT_RGBA8, TEXCONV_RGB565, 2, 4, 16, false);
	UNCOMPRESSED(RWFMT_C4444|RWFMT_MIPMAP, TEXFMT_RGBA8, TEXCONV_ARGB4444, 2, 4, 16, true);
	UNCOMPRESSED(RWFMT_LUM8|RWFMT_MIPMAP, TEXFMT_RGBA8, TEXCONV_LUM8, 1, 4, 8, false);
}

static void
TestPalettisedFormatsGoThroughImagePath(void)
{
	UNCOMPRESSED(RWFMT_PAL8|RWFMT_C8888, TEXFMT_RGBA8, TEXCONV_PAL8, 0, 4, 8, true);
	UNCOMPRESSED(RWFMT_PAL8|RWFMT_C888, TEXFMT_RGBA8, TEXCONV_PAL8, 0, 4, 8, false);
	UNCOMPRESSED(RWFMT_PAL4|RWFMT_C8888, TEXFMT_RGBA8, TEXCONV_PAL4, 0, 4, 4, true);
	UNCOMPRESSED(RWFMT_PAL4|RWFMT_C888, TEXFMT_RGBA8, TEXCONV_PAL4, 0, 4, 4, false);
	UNCOMPRESSED(RWFMT_PAL8|RWFMT_C8888|RWFMT_MIPMAP, TEXFMT_RGBA8, TEXCONV_PAL8, 0, 4, 8, true);
	UNCOMPRESSED(RWFMT_PAL4|RWFMT_C888|RWFMT_MIPMAP, TEXFMT_RGBA8, TEXCONV_PAL4, 0, 4, 4, false);
}

static void
TestUnsupportedFormatsFail(void)
{
	TexFormat f;
	CHECK(!findTexFormat(0, &f));
	CHECK(!findTexFormat(0x0700, &f));
	CHECK(!findTexFormat(0x0800, &f));
	CHECK(!findTexFormat(0x0900, &f));
	CHECK(!findTexFormat(0x0B00, &f));
	CHECK(!findTexFormat(RWFMT_PAL8|RWFMT_PAL4|RWFMT_C8888, &f));
	CHECK(!findTexFormat(RWFMT_PAL8|RWFMT_LUM8, &f));
}

static void
TestDXTFormats(void)
{
	TexFormat f;
	CHECK(findDXTFormat(1, true, &f));
	CHECK(f.format == TEXFMT_BC1 && f.isCompressed && f.blockSize == 8 && f.hasAlpha && !f.alphaOne);
	CHECK(f.bpp == 0 && f.texBpp == 0 && f.conv == TEXCONV_NONE && f.depth == 16);

	CHECK(findDXTFormat(1, false, &f));
	CHECK(f.format == TEXFMT_BC1 && f.isCompressed && f.blockSize == 8 && !f.hasAlpha && f.alphaOne);

	CHECK(findDXTFormat(3, true, &f));
	CHECK(f.format == TEXFMT_BC2 && f.isCompressed && f.blockSize == 16 && f.hasAlpha && !f.alphaOne);

	CHECK(findDXTFormat(5, true, &f));
	CHECK(f.format == TEXFMT_BC3 && f.isCompressed && f.blockSize == 16 && f.hasAlpha && !f.alphaOne);

	CHECK(findDXTFormat(5, false, &f));
	CHECK(f.format == TEXFMT_BC3 && !f.hasAlpha && !f.alphaOne);

	CHECK(!findDXTFormat(0, true, &f));
	CHECK(!findDXTFormat(2, true, &f));
	CHECK(!findDXTFormat(4, true, &f));
}

static void
TestMipChain(void)
{
	CHECK(mipChainLength(1, 1) == 1);
	CHECK(mipChainLength(256, 256) == 9);
	CHECK(mipChainLength(8, 2) == 4);
	CHECK(mipChainLength(2, 8) == 4);
	CHECK(mipChainLength(5, 3) == 3);
	CHECK(mipChainLength(100, 37) == 7);

	int32_t w, h;
	mipLevelDims(5, 3, 0, &w, &h);
	CHECK(w == 5 && h == 3);
	mipLevelDims(5, 3, 1, &w, &h);
	CHECK(w == 2 && h == 1);
	mipLevelDims(5, 3, 2, &w, &h);
	CHECK(w == 1 && h == 1);
	mipLevelDims(8, 2, 2, &w, &h);
	CHECK(w == 2 && h == 1);
	mipLevelDims(8, 2, 3, &w, &h);
	CHECK(w == 1 && h == 1);
	mipLevelDims(100, 37, 3, &w, &h);
	CHECK(w == 12 && h == 4);
}

static void
TestNumLevels(void)
{
	CHECK(numTexLevels(RWFMT_C8888, 64, 32) == 1);
	CHECK(numTexLevels(RWFMT_C8888|RWFMT_MIPMAP, 64, 32) == 7);
	CHECK(numTexLevels(RWFMT_C8888|RWFMT_MIPMAP|RWFMT_AUTOMIPMAP, 64, 32) == 7);
	CHECK(numTexLevels(RWFMT_C8888|RWFMT_AUTOMIPMAP, 64, 32) == 1);
	CHECK(numTexLevels(RWFMT_C1555|RWFMT_MIPMAP, 5, 3) == 3);
	CHECK(numLockLevels(RWFMT_C8888, 64, 32) == 1);
	CHECK(numLockLevels(RWFMT_C8888|RWFMT_MIPMAP, 64, 32) == 7);
	CHECK(numLockLevels(RWFMT_C8888|RWFMT_MIPMAP|RWFMT_AUTOMIPMAP, 64, 32) == 1);
}

static void
TestUncompressedLevelSizes(void)
{
	TexFormat f;
	findTexFormat(RWFMT_C8888, &f);
	CHECK(levelStride(f, 5) == 20);
	CHECK(levelSize(f, 5, 3) == 60);
	CHECK(levelSize(f, 2, 1) == 8);
	CHECK(levelSize(f, 1, 1) == 4);

	findTexFormat(RWFMT_C888, &f);
	CHECK(levelStride(f, 5) == 15);
	CHECK(levelSize(f, 5, 3) == 45);

	findTexFormat(RWFMT_C1555, &f);
	CHECK(levelStride(f, 3) == 6);
	CHECK(levelSize(f, 3, 3) == 18);

	findTexFormat(RWFMT_LUM8, &f);
	CHECK(levelSize(f, 7, 3) == 21);
}

static void
TestCompressedLevelSizes(void)
{
	TexFormat f;
	findDXTFormat(1, false, &f);
	CHECK(levelStride(f, 8) == 16);
	CHECK(levelSize(f, 8, 8) == 32);
	CHECK(levelSize(f, 4, 4) == 8);
	CHECK(levelSize(f, 2, 2) == 8);
	CHECK(levelSize(f, 1, 1) == 8);
	CHECK(levelSize(f, 8, 2) == 16);
	CHECK(levelSize(f, 5, 3) == 16);
	CHECK(levelStride(f, 1) == 8);

	findDXTFormat(3, true, &f);
	CHECK(levelStride(f, 8) == 32);
	CHECK(levelSize(f, 8, 8) == 64);
	CHECK(levelSize(f, 2, 1) == 16);

	findDXTFormat(5, true, &f);
	CHECK(levelSize(f, 6, 2) == 32);
	CHECK(levelSize(f, 12, 4) == 48);
}

static bool
Same4(const uint8_t *p, int r, int g, int b, int a)
{
	return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static void
TestConvertToRGBA8(void)
{
	uint8_t out[16];

	const uint8_t rgba[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	CHECK(convertToRGBA8(TEXCONV_NONE, out, rgba, 2));
	CHECK(memcmp(out, rgba, 8) == 0);

	const uint8_t rgb[6] = { 10, 20, 30, 40, 50, 60 };
	CHECK(convertToRGBA8(TEXCONV_RGB888, out, rgb, 2));
	CHECK(Same4(out, 10, 20, 30, 255) && Same4(out+4, 40, 50, 60, 255));

	const uint8_t argb1555[4] = { 0x00, 0xFC, 0xE1, 0x03 };
	CHECK(convertToRGBA8(TEXCONV_ARGB1555, out, argb1555, 2));
	CHECK(Same4(out, 255, 0, 0, 255) && Same4(out+4, 0, 255, 8, 0));

	const uint8_t rgb565[4] = { 0xE0, 0x07, 0x01, 0xF8 };
	CHECK(convertToRGBA8(TEXCONV_RGB565, out, rgb565, 2));
	CHECK(Same4(out, 0, 255, 0, 255) && Same4(out+4, 255, 0, 8, 255));

	const uint8_t argb4444[2] = { 0x34, 0x12 };
	CHECK(convertToRGBA8(TEXCONV_ARGB4444, out, argb4444, 1));
	CHECK(Same4(out, 34, 51, 68, 17));

	const uint8_t lum[2] = { 0, 200 };
	CHECK(convertToRGBA8(TEXCONV_LUM8, out, lum, 2));
	CHECK(Same4(out, 0, 0, 0, 255) && Same4(out+4, 200, 200, 200, 255));

	CHECK(!convertToRGBA8(TEXCONV_PAL8, out, lum, 2));
	CHECK(!convertToRGBA8(TEXCONV_PAL4, out, lum, 2));
}

static void
TestSixteenBitRoundTripsExactly(void)
{
	const int32_t convs[3] = { TEXCONV_ARGB1555, TEXCONV_RGB565, TEXCONV_ARGB4444 };
	for(int c = 0; c < 3; c++){
		int bad = 0;
		for(uint32_t v = 0; v < 0x10000; v++){
			uint8_t in[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
			uint8_t mid[4], back[2];
			convertToRGBA8(convs[c], mid, in, 1);
			convertFromRGBA8(convs[c], back, mid, 1);
			if(back[0] != in[0] || back[1] != in[1])
				bad++;
		}
		if(bad){
			printf("FAIL: conv %d round trip wrong for %d of 65536 values\n", convs[c], bad);
			failures++;
		}
	}
}

static void
TestConvertFromRGBA8(void)
{
	const uint8_t px[8] = { 10, 20, 30, 40, 200, 100, 50, 255 };
	uint8_t out[8];

	CHECK(convertFromRGBA8(TEXCONV_NONE, out, px, 2));
	CHECK(memcmp(out, px, 8) == 0);

	CHECK(convertFromRGBA8(TEXCONV_RGB888, out, px, 2));
	CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 200 && out[4] == 100 && out[5] == 50);

	uint8_t lum[4] = { 0, 0, 0, 0 };
	const uint8_t grey[8] = { 77, 77, 77, 255, 9, 9, 9, 255 };
	CHECK(convertFromRGBA8(TEXCONV_LUM8, lum, grey, 2));
	CHECK(lum[0] == 77 && lum[1] == 9 && lum[2] == 0);

	CHECK(!convertFromRGBA8(TEXCONV_PAL8, out, px, 2));
}

static void
TestFilledPrefix(void)
{
	CHECK(filledPrefix(0) == 0);
	CHECK(filledPrefix(1) == 1);
	CHECK(filledPrefix(0x5) == 1);
	CHECK(filledPrefix(0x6) == 0);
	CHECK(filledPrefix(0x7) == 3);
	CHECK(filledPrefix(0x7FFF) == 15);
	CHECK(filledPrefix(0xFFFF) == 16);
	CHECK(filledPrefix(0x1FFFF) == 16);
}

int
main(void)
{
	TestUncompressedFormats();
	TestMipmapFlagsDoNotChangeFormat();
	TestPalettisedFormatsGoThroughImagePath();
	TestUnsupportedFormatsFail();
	TestDXTFormats();
	TestMipChain();
	TestNumLevels();
	TestUncompressedLevelSizes();
	TestCompressedLevelSizes();
	TestConvertToRGBA8();
	TestSixteenBitRoundTripsExactly();
	TestConvertFromRGBA8();
	TestFilledPrefix();
	if(failures == 0)
		printf("all tests passed\n");
	return failures == 0 ? 0 : 1;
}
