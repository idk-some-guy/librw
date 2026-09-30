#include <cstdio>
#include "metalinst.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static void
CheckAttrib(const InstAttrib &a, uint32_t index, int32_t format, uint32_t offset, uint32_t stride,
            bool normals, bool prelit, int sets)
{
	if(a.index == index && a.format == format && a.offset == offset && a.stride == stride)
		return;
	printf("FAIL normals %d prelit %d sets %d: attrib {%u, %d, stride %u, offset %u}, expected {%u, %d, stride %u, offset %u}\n",
	       normals, prelit, sets, a.index, a.format, a.stride, a.offset, index, format, stride, offset);
	failures++;
}

static void
TestLayout(bool normals, bool prelit, int sets)
{
	InstAttrib out[MAXINSTATTRIBS];
	uint32_t stride = 12 + (normals ? 12 : 0) + (prelit ? 4 : 0) + 8*sets;
	int32_t want = 1 + normals + prelit + sets;
	int32_t n = defaultVertexLayout(normals, prelit, sets, out);
	if(n != want){
		printf("FAIL normals %d prelit %d sets %d: %d attribs, expected %d\n", normals, prelit, sets, n, want);
		failures++;
		return;
	}
	int i = 0;
	uint32_t off = 0;
	CheckAttrib(out[i++], INSTATTRIB_POS, INSTFMT_FLOAT3, off, stride, normals, prelit, sets);
	off += 12;
	if(normals){
		CheckAttrib(out[i++], INSTATTRIB_NORMAL, INSTFMT_FLOAT3, off, stride, normals, prelit, sets);
		off += 12;
	}
	if(prelit){
		CheckAttrib(out[i++], INSTATTRIB_COLOR, INSTFMT_UCHAR4_NORM, off, stride, normals, prelit, sets);
		off += 4;
	}
	for(int s = 0; s < sets; s++){
		CheckAttrib(out[i++], INSTATTRIB_TEXCOORDS0+s, INSTFMT_FLOAT2, off, stride, normals, prelit, sets);
		off += 8;
	}
}

static void
TestAllLayouts(void)
{
	for(int normals = 0; normals < 2; normals++)
		for(int prelit = 0; prelit < 2; prelit++)
			for(int sets = 0; sets < 3; sets++)
				TestLayout(normals, prelit, sets);
}

static void
TestWorkedExample(void)
{
	InstAttrib out[MAXINSTATTRIBS];
	CHECK(defaultVertexLayout(true, true, 1, out) == 4);
	CHECK(out[0].index == 0 && out[0].offset == 0);
	CHECK(out[1].index == 1 && out[1].offset == 12);
	CHECK(out[2].index == 2 && out[2].offset == 24);
	CHECK(out[3].index == 5 && out[3].offset == 28);
	for(int i = 0; i < 4; i++)
		CHECK(out[i].stride == 36);
}

static void
TestEightSets(void)
{
	InstAttrib out[MAXINSTATTRIBS];
	CHECK(defaultVertexLayout(true, true, 8, out) == 11);
	CHECK(out[10].index == INSTATTRIB_TEXCOORDS0+7);
	CHECK(out[10].offset == 28 + 7*8);
	CHECK(out[10].stride == 28 + 8*8);
}

static void
TestSetsClamped(void)
{
	InstAttrib out[MAXINSTATTRIBS+4];
	CHECK(defaultVertexLayout(true, true, 12, out) == 11);
	CHECK(out[0].stride == 28 + 8*8);
	CHECK(defaultVertexLayout(false, false, -1, out) == 1);
	CHECK(out[0].stride == 12);
}

struct SkinCase
{
	bool normals, prelit;
	int sets;
	int32_t n;
	uint32_t index[MAXINSTATTRIBS], offset[MAXINSTATTRIBS];
	uint32_t stride;
};

static void
TestSkinLayout(const SkinCase &c)
{
	InstAttrib out[MAXINSTATTRIBS];
	int32_t n = skinVertexLayout(c.normals, c.prelit, c.sets, out);
	if(n != c.n){
		printf("FAIL skin normals %d prelit %d sets %d: %d attribs, expected %d\n",
		       c.normals, c.prelit, c.sets, n, c.n);
		failures++;
		return;
	}
	for(int i = 0; i < n; i++){
		int32_t format = c.index[i] == INSTATTRIB_WEIGHTS ? INSTFMT_FLOAT4 :
		                 c.index[i] == INSTATTRIB_INDICES ? INSTFMT_UCHAR4 :
		                 c.index[i] == INSTATTRIB_COLOR ? INSTFMT_UCHAR4_NORM :
		                 c.index[i] >= INSTATTRIB_TEXCOORDS0 ? INSTFMT_FLOAT2 : INSTFMT_FLOAT3;
		CheckAttrib(out[i], c.index[i], format, c.offset[i], c.stride, c.normals, c.prelit, c.sets);
	}
}

static void
TestSkinLayouts(void)
{
	SkinCase ped = { true, true, 1, 6, { 0, 1, 2, 5, 3, 4 }, { 0, 12, 24, 28, 36, 52 }, 56 };
	SkinCase bare = { false, false, 0, 3, { 0, 3, 4 }, { 0, 12, 28 }, 32 };
	SkinCase eight = { true, true, 8, 13,
		{ 0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12, 3, 4 },
		{ 0, 12, 24, 28, 36, 44, 52, 60, 68, 76, 84, 92, 108 }, 112 };
	SkinCase noNormals = { false, true, 1, 5, { 0, 2, 5, 3, 4 }, { 0, 12, 16, 24, 40 }, 44 };
	TestSkinLayout(ped);
	TestSkinLayout(bare);
	TestSkinLayout(eight);
	TestSkinLayout(noNormals);
}

static void
TestDefaultUnchangedBySkin(void)
{
	InstAttrib skin[MAXINSTATTRIBS], def[MAXINSTATTRIBS];
	CHECK(skinVertexLayout(true, true, 1, skin) == 6);
	CHECK(defaultVertexLayout(true, true, 1, def) == 4);
	CHECK(def[3].index == INSTATTRIB_TEXCOORDS0 && def[3].offset == 28 && def[3].stride == 36);
	CHECK(skin[3].stride == 56);
}

static void
TestIndexOffsets(void)
{
	uint32_t counts[] = { 3, 6, 4, 1 };
	uint32_t offsets[4] = { 99, 99, 99, 99 };
	CHECK(meshIndexOffsets(counts, 4, offsets) == 32);
	CHECK(offsets[0] == 0);
	CHECK(offsets[1] == 8);
	CHECK(offsets[2] == 20);
	CHECK(offsets[3] == 28);

	CHECK(meshIndexOffsets(nullptr, 0, nullptr) == 0);

	uint32_t two[] = { 2 };
	uint32_t off1 = 99;
	CHECK(meshIndexOffsets(two, 1, &off1) == 4);
	CHECK(off1 == 0);
}

static void
TestRestartIndex(void)
{
	uint16_t plain[] = { 0, 1, 2, 3, 0xFFFE };
	uint16_t restart[] = { 65533, 65534, 0xFFFF };
	uint16_t first[] = { 0xFFFF, 1, 2 };
	CHECK(!stripHasRestartIndex(plain, 5));
	CHECK(stripHasRestartIndex(restart, 3));
	CHECK(!stripHasRestartIndex(restart, 2));
	CHECK(stripHasRestartIndex(first, 3));
	CHECK(!stripHasRestartIndex(nullptr, 0));
}

int
main(void)
{
	TestAllLayouts();
	TestWorkedExample();
	TestEightSets();
	TestSetsClamped();
	TestSkinLayouts();
	TestDefaultUnchangedBySkin();
	TestIndexOffsets();
	TestRestartIndex();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all inst tests passed\n");
	return 0;
}
