#include <cstdio>
#include "metalkeys.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

// RenderWare values from rwrender.h and rwobjects.h
enum { BLENDZERO = 1, BLENDONE, BLENDSRCCOLOR, BLENDINVSRCCOLOR, BLENDSRCALPHA, BLENDINVSRCALPHA,
	BLENDDESTALPHA, BLENDINVDESTALPHA, BLENDDESTCOLOR, BLENDINVDESTCOLOR, BLENDSRCALPHASAT };
enum { STENCILKEEP = 1, STENCILZERO, STENCILREPLACE, STENCILINCSAT, STENCILDECSAT, STENCILINVERT,
	STENCILINC, STENCILDEC };
enum { STENCILNEVER = 1, STENCILLESS, STENCILEQUAL, STENCILLESSEQUAL, STENCILGREATER,
	STENCILNOTEQUAL, STENCILGREATEREQUAL, STENCILALWAYS };
enum { CULLNONE = 1, CULLBACK, CULLFRONT };
enum { ALPHAALWAYS, ALPHAGREATEREQUAL, ALPHALESS };
enum { NEAREST = 1, LINEAR, MIPNEAREST, MIPLINEAR, LINEARMIPNEAREST, LINEARMIPLINEAR };
enum { WRAP = 1, MIRROR, CLAMP, BORDER };

static void
TestBlendFactors(void)
{
	CHECK(blendFactor(BLENDZERO) == MTLBLEND_ZERO);
	CHECK(blendFactor(BLENDONE) == MTLBLEND_ONE);
	CHECK(blendFactor(BLENDSRCCOLOR) == MTLBLEND_SRCCOLOR);
	CHECK(blendFactor(BLENDINVSRCCOLOR) == MTLBLEND_INVSRCCOLOR);
	CHECK(blendFactor(BLENDSRCALPHA) == MTLBLEND_SRCALPHA);
	CHECK(blendFactor(BLENDINVSRCALPHA) == MTLBLEND_INVSRCALPHA);
	CHECK(blendFactor(BLENDDESTALPHA) == MTLBLEND_DESTALPHA);
	CHECK(blendFactor(BLENDINVDESTALPHA) == MTLBLEND_INVDESTALPHA);
	CHECK(blendFactor(BLENDDESTCOLOR) == MTLBLEND_DESTCOLOR);
	CHECK(blendFactor(BLENDINVDESTCOLOR) == MTLBLEND_INVDESTCOLOR);
	CHECK(blendFactor(BLENDSRCALPHASAT) == MTLBLEND_SRCALPHASAT);
	CHECK(blendFactor(0) == MTLBLEND_ZERO);
	CHECK(blendFactor(12) == MTLBLEND_ZERO);
	CHECK(blendFactor(0xFFFFFFFF) == MTLBLEND_ZERO);
}

static void
TestCompareFunctions(void)
{
	CHECK(compareFunction(STENCILNEVER) == MTLCMP_NEVER);
	CHECK(compareFunction(STENCILLESS) == MTLCMP_LESS);
	CHECK(compareFunction(STENCILEQUAL) == MTLCMP_EQUAL);
	CHECK(compareFunction(STENCILLESSEQUAL) == MTLCMP_LESSEQUAL);
	CHECK(compareFunction(STENCILGREATER) == MTLCMP_GREATER);
	CHECK(compareFunction(STENCILNOTEQUAL) == MTLCMP_NOTEQUAL);
	CHECK(compareFunction(STENCILGREATEREQUAL) == MTLCMP_GREATEREQUAL);
	CHECK(compareFunction(STENCILALWAYS) == MTLCMP_ALWAYS);
	CHECK(compareFunction(0) == MTLCMP_NEVER);
	CHECK(compareFunction(9) == MTLCMP_NEVER);
}

static void
TestStencilOps(void)
{
	CHECK(stencilOperation(STENCILKEEP) == MTLSTENCIL_KEEP);
	CHECK(stencilOperation(STENCILZERO) == MTLSTENCIL_ZERO);
	CHECK(stencilOperation(STENCILREPLACE) == MTLSTENCIL_REPLACE);
	CHECK(stencilOperation(STENCILINCSAT) == MTLSTENCIL_INCCLAMP);
	CHECK(stencilOperation(STENCILDECSAT) == MTLSTENCIL_DECCLAMP);
	CHECK(stencilOperation(STENCILINVERT) == MTLSTENCIL_INVERT);
	CHECK(stencilOperation(STENCILINC) == MTLSTENCIL_INCWRAP);
	CHECK(stencilOperation(STENCILDEC) == MTLSTENCIL_DECWRAP);
	CHECK(stencilOperation(0) == MTLSTENCIL_KEEP);
	CHECK(stencilOperation(9) == MTLSTENCIL_KEEP);
}

static void
TestCullModes(void)
{
	CHECK(cullMode(CULLNONE) == MTLCULL_NONE);
	CHECK(cullMode(CULLBACK) == MTLCULL_BACK);
	CHECK(cullMode(CULLFRONT) == MTLCULL_FRONT);
	CHECK(cullMode(0) == MTLCULL_FRONT);
	CHECK(cullMode(7) == MTLCULL_FRONT);
}

static SamplerResolved
Filter(uint32_t filter, bool mips)
{
	SamplerDesc d = { filter, WRAP, WRAP, 1, mips, 0 };
	return resolveSampler(d);
}

static bool
FilterIs(SamplerResolved r, uint32_t min, uint32_t mag, uint32_t mip)
{
	return r.minFilter == min && r.magFilter == mag && r.mipFilter == mip;
}

static void
TestFilterModes(void)
{
	CHECK(FilterIs(Filter(NEAREST, true), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_NONE));
	CHECK(FilterIs(Filter(LINEAR, true), MTLFILTER_LINEAR, MTLFILTER_LINEAR, MTLMIP_NONE));
	CHECK(FilterIs(Filter(MIPNEAREST, true), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_NEAREST));
	CHECK(FilterIs(Filter(MIPLINEAR, true), MTLFILTER_LINEAR, MTLFILTER_LINEAR, MTLMIP_NEAREST));
	CHECK(FilterIs(Filter(LINEARMIPNEAREST, true), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_LINEAR));
	CHECK(FilterIs(Filter(LINEARMIPLINEAR, true), MTLFILTER_LINEAR, MTLFILTER_LINEAR, MTLMIP_LINEAR));

	CHECK(FilterIs(Filter(NEAREST, false), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_NONE));
	CHECK(FilterIs(Filter(LINEAR, false), MTLFILTER_LINEAR, MTLFILTER_LINEAR, MTLMIP_NONE));
	CHECK(FilterIs(Filter(MIPNEAREST, false), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_NONE));
	CHECK(FilterIs(Filter(MIPLINEAR, false), MTLFILTER_LINEAR, MTLFILTER_LINEAR, MTLMIP_NONE));
	CHECK(FilterIs(Filter(LINEARMIPNEAREST, false), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_NONE));
	CHECK(FilterIs(Filter(LINEARMIPLINEAR, false), MTLFILTER_LINEAR, MTLFILTER_LINEAR, MTLMIP_NONE));

	CHECK(FilterIs(Filter(0, true), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_NONE));
	CHECK(FilterIs(Filter(7, true), MTLFILTER_NEAREST, MTLFILTER_NEAREST, MTLMIP_NONE));
}

static void
TestAddressModes(void)
{
	CHECK(addressMode(WRAP) == MTLADDR_REPEAT);
	CHECK(addressMode(MIRROR) == MTLADDR_MIRRORREPEAT);
	CHECK(addressMode(CLAMP) == MTLADDR_CLAMPTOEDGE);
	CHECK(addressMode(BORDER) == MTLADDR_CLAMPTOBORDER);
	CHECK(addressMode(0) == MTLADDR_REPEAT);
	CHECK(addressMode(5) == MTLADDR_REPEAT);

	SamplerDesc d = { LINEAR, CLAMP, MIRROR, 1, false, 0 };
	SamplerResolved r = resolveSampler(d);
	CHECK(r.addressU == MTLADDR_CLAMPTOEDGE);
	CHECK(r.addressV == MTLADDR_MIRRORREPEAT);
}

static void
TestAnisotropy(void)
{
	SamplerDesc d = { LINEAR, WRAP, WRAP, 0, true, 0 };
	CHECK(resolveSampler(d).maxAnisotropy == 1);
	d.maxAnisotropy = 8;
	CHECK(resolveSampler(d).maxAnisotropy == 8);
	d.maxAnisotropy = 16;
	CHECK(resolveSampler(d).maxAnisotropy == 16);
	d.maxAnisotropy = 64;
	CHECK(resolveSampler(d).maxAnisotropy == 16);
}

static bool
RangeIs(uint32_t func, uint32_t ref, float lo, float hi)
{
	float r[2] = { 123.0f, 456.0f };
	alphaTestRange(func, ref/255.0f, r);
	if(r[0] == lo && r[1] == hi)
		return true;
	printf("  func %u ref %u: [%g, %g), expected [%g, %g)\n", func, ref, r[0], r[1], lo, hi);
	return false;
}

static void
TestAlphaTestRange(void)
{
	uint32_t refs[3] = { 0, 10, 255 };
	for(int i = 0; i < 3; i++){
		float ref = refs[i]/255.0f;
		CHECK(RangeIs(ALPHAALWAYS, refs[i], -1000.0f, 1000.0f));
		CHECK(RangeIs(ALPHAGREATEREQUAL, refs[i], ref, 1000.0f));
		CHECK(RangeIs(ALPHALESS, refs[i], -1000.0f, ref));
		CHECK(RangeIs(3, refs[i], -1000.0f, 1000.0f));
	}
}

static void
TestAddressing(void)
{
	CHECK(gl3Addressing == false);

	CHECK(addressOnSet(WRAP, CLAMP, false) == CLAMP);
	CHECK(addressOnSet(0, CLAMP, false) == CLAMP);
	CHECK(addressOnSet(CLAMP, CLAMP, false) == CLAMP);

	CHECK(addressOnSet(WRAP, CLAMP, true) == WRAP);
	CHECK(addressOnSet(0, CLAMP, true) == 0);
	CHECK(addressOnSet(CLAMP, CLAMP, true) == CLAMP);
	CHECK(addressOnSet(MIRROR, WRAP, true) == MIRROR);
}

static PipelineDesc
BasePipe(void)
{
	PipelineDesc d = {};
	d.shader = 1;
	d.variant = 1;
	d.vertexLayout = 1;
	d.blendEnable = true;
	d.srcBlend = BLENDSRCALPHA;
	d.destBlend = BLENDINVSRCALPHA;
	d.writeMask = 0xF;
	d.colorFormat = COLORFMT_RGBA8;
	d.depthFormat = DEPTHFMT_D32S8;
	d.sampleCount = 1;
	return d;
}

static void
TestPipelineKey(void)
{
	PipelineDesc v[32];
	int n = 0;
	v[n++] = BasePipe();
	v[n] = BasePipe(); v[n++].shader = 2;
	v[n] = BasePipe(); v[n++].shader = (1<<PIPEKEY_SHADERBITS)-1;
	v[n] = BasePipe(); v[n++].variant = 0;
	v[n] = BasePipe(); v[n++].variant = (1<<PIPEKEY_VARIANTBITS)-1;
	v[n] = BasePipe(); v[n++].vertexLayout = 2;
	v[n] = BasePipe(); v[n++].vertexLayout = (1<<PIPEKEY_LAYOUTBITS)-1;
	v[n] = BasePipe(); v[n++].blendEnable = false;
	v[n] = BasePipe(); v[n++].srcBlend = BLENDONE;
	v[n] = BasePipe(); v[n++].srcBlend = BLENDSRCALPHASAT;
	v[n] = BasePipe(); v[n++].destBlend = BLENDONE;
	v[n] = BasePipe(); v[n++].destBlend = BLENDSRCALPHASAT;
	v[n] = BasePipe(); v[n++].writeMask = 0x7;
	v[n] = BasePipe(); v[n++].writeMask = 0x8;
	v[n] = BasePipe(); v[n++].colorFormat = COLORFMT_BGRA8;
	v[n] = BasePipe(); v[n++].colorFormat = 15;
	v[n] = BasePipe(); v[n++].depthFormat = DEPTHFMT_NONE;
	v[n] = BasePipe(); v[n++].depthFormat = 7;
	v[n] = BasePipe(); v[n++].sampleCount = 2;
	v[n] = BasePipe(); v[n++].sampleCount = 4;
	v[n] = BasePipe(); v[n++].sampleCount = 8;
	for(int i = 0; i < n; i++)
		for(int j = i+1; j < n; j++)
			if(pipelineKey(v[i]) == pipelineKey(v[j])){
				printf("  pipeline variations %d and %d share a key\n", i, j);
				failures++;
			}

	PipelineDesc a = BasePipe(), b = BasePipe();
	a.blendEnable = b.blendEnable = false;
	b.srcBlend = BLENDONE;
	b.destBlend = BLENDZERO;
	CHECK(pipelineKey(a) == pipelineKey(b));

	a = BasePipe();
	b = BasePipe();
	a.srcBlend = 0;
	b.srcBlend = BLENDZERO;
	CHECK(pipelineKey(a) == pipelineKey(b));

	CHECK(pipelineKey(BasePipe()) == pipelineKey(BasePipe()));
}

static DepthStencilDesc
BaseDepth(void)
{
	DepthStencilDesc d = {};
	d.hasDepth = true;
	d.ztest = true;
	d.zwrite = true;
	d.stencilEnable = true;
	d.stencilFunc = STENCILEQUAL;
	d.stencilFail = STENCILKEEP;
	d.stencilZFail = STENCILKEEP;
	d.stencilPass = STENCILREPLACE;
	d.stencilMask = 0xFF;
	d.stencilWriteMask = 0xFF;
	return d;
}

static void
TestDepthState(void)
{
	DepthStencilDesc d = BaseDepth();
	DepthStencilResolved r = resolveDepthStencil(d);
	CHECK(r.depthCompare == MTLCMP_LESSEQUAL && r.depthWrite);

	d.zwrite = false;
	r = resolveDepthStencil(d);
	CHECK(r.depthCompare == MTLCMP_LESSEQUAL && !r.depthWrite);

	d.ztest = false;
	d.zwrite = true;
	r = resolveDepthStencil(d);
	CHECK(r.depthCompare == MTLCMP_ALWAYS && r.depthWrite);

	d.zwrite = false;
	r = resolveDepthStencil(d);
	CHECK(r.depthCompare == MTLCMP_ALWAYS && !r.depthWrite);

	d = BaseDepth();
	r = resolveDepthStencil(d);
	CHECK(r.stencilEnable);
	CHECK(r.stencilCompare == MTLCMP_EQUAL);
	CHECK(r.stencilFail == MTLSTENCIL_KEEP && r.depthFail == MTLSTENCIL_KEEP);
	CHECK(r.depthStencilPass == MTLSTENCIL_REPLACE);
	CHECK(r.readMask == 0xFF && r.writeMask == 0xFF);

	d.stencilMask = 0xFFFFFFFF;
	d.stencilWriteMask = 0xFFFFFF0F;
	r = resolveDepthStencil(d);
	CHECK(r.readMask == 0xFF && r.writeMask == 0x0F);

	d = BaseDepth();
	d.hasDepth = false;
	r = resolveDepthStencil(d);
	CHECK(r.depthCompare == MTLCMP_ALWAYS && !r.depthWrite && !r.stencilEnable);
}

static void
TestDepthStencilKey(void)
{
	DepthStencilDesc v[32];
	int n = 0;
	v[n++] = BaseDepth();
	v[n] = BaseDepth(); v[n++].ztest = false;
	v[n] = BaseDepth(); v[n++].zwrite = false;
	v[n] = BaseDepth(); v[n].ztest = false; v[n++].zwrite = false;
	v[n] = BaseDepth(); v[n++].stencilEnable = false;
	v[n] = BaseDepth(); v[n++].stencilFunc = STENCILNEVER;
	v[n] = BaseDepth(); v[n++].stencilFunc = STENCILALWAYS;
	v[n] = BaseDepth(); v[n++].stencilFail = STENCILDEC;
	v[n] = BaseDepth(); v[n++].stencilZFail = STENCILDEC;
	v[n] = BaseDepth(); v[n++].stencilPass = STENCILDEC;
	v[n] = BaseDepth(); v[n++].stencilPass = STENCILKEEP;
	v[n] = BaseDepth(); v[n++].stencilMask = 0x80;
	v[n] = BaseDepth(); v[n++].stencilMask = 0x01;
	v[n] = BaseDepth(); v[n++].stencilWriteMask = 0x80;
	v[n] = BaseDepth(); v[n++].stencilWriteMask = 0x01;
	for(int i = 0; i < n; i++)
		for(int j = i+1; j < n; j++)
			if(depthStencilKey(v[i]) == depthStencilKey(v[j])){
				printf("  depth-stencil variations %d and %d share a key\n", i, j);
				failures++;
			}

	DepthStencilDesc a = BaseDepth(), b = BaseDepth();
	a.stencilEnable = b.stencilEnable = false;
	b.stencilFunc = STENCILNEVER;
	b.stencilFail = STENCILZERO;
	b.stencilZFail = STENCILZERO;
	b.stencilPass = STENCILZERO;
	b.stencilMask = 0;
	b.stencilWriteMask = 0;
	CHECK(depthStencilKey(a) == depthStencilKey(b));

	a = BaseDepth();
	b = BaseDepth();
	a.stencilMask = 0xFFFFFFFF;
	CHECK(depthStencilKey(a) == depthStencilKey(b));

	a = BaseDepth();
	b = BaseDepth();
	a.hasDepth = b.hasDepth = false;
	b.ztest = false;
	b.zwrite = false;
	b.stencilEnable = false;
	CHECK(depthStencilKey(a) == depthStencilKey(b));
}

static SamplerDesc
BaseSampler(void)
{
	SamplerDesc d = { LINEARMIPLINEAR, WRAP, WRAP, 1, true, 0 };
	return d;
}

static void
TestSamplerKey(void)
{
	SamplerDesc v[32];
	int n = 0;
	v[n++] = BaseSampler();
	v[n] = BaseSampler(); v[n++].filter = NEAREST;
	v[n] = BaseSampler(); v[n++].filter = LINEAR;
	v[n] = BaseSampler(); v[n++].filter = MIPNEAREST;
	v[n] = BaseSampler(); v[n++].filter = MIPLINEAR;
	v[n] = BaseSampler(); v[n++].filter = LINEARMIPNEAREST;
	v[n] = BaseSampler(); v[n++].addressU = MIRROR;
	v[n] = BaseSampler(); v[n++].addressU = CLAMP;
	v[n] = BaseSampler(); v[n++].addressU = BORDER;
	v[n] = BaseSampler(); v[n++].addressV = MIRROR;
	v[n] = BaseSampler(); v[n++].addressV = CLAMP;
	v[n] = BaseSampler(); v[n++].addressV = BORDER;
	v[n] = BaseSampler(); v[n++].maxAnisotropy = 2;
	v[n] = BaseSampler(); v[n++].maxAnisotropy = 16;
	for(int i = 0; i < n; i++)
		for(int j = i+1; j < n; j++)
			if(samplerKey(v[i]) == samplerKey(v[j])){
				printf("  sampler variations %d and %d share a key\n", i, j);
				failures++;
			}

	SamplerDesc a = BaseSampler(), b = BaseSampler();
	a.hasMips = b.hasMips = false;
	a.filter = LINEAR;
	b.filter = LINEARMIPLINEAR;
	CHECK(samplerKey(a) == samplerKey(b));
	a.hasMips = true;
	CHECK(samplerKey(a) == samplerKey(b));
	CHECK(samplerKey(BaseSampler()) != samplerKey(b));

	a = BaseSampler();
	b = BaseSampler();
	a.addressU = 0;
	CHECK(samplerKey(a) == samplerKey(b));

	a = BaseSampler();
	b = BaseSampler();
	a.maxAnisotropy = 0;
	CHECK(samplerKey(a) == samplerKey(b));
}

static void
TestSamplerMaxLevel(void)
{
	SamplerDesc a = BaseSampler(), b = BaseSampler();
	a.maxLevel = 1;
	b.maxLevel = 4;
	CHECK(resolveSampler(a).maxLevel == 1);
	CHECK(resolveSampler(b).maxLevel == 4);
	CHECK(samplerKey(a) != samplerKey(b));
	CHECK(samplerKey(a) != samplerKey(BaseSampler()));

	b.maxLevel = 1;
	CHECK(samplerKey(a) == samplerKey(b));

	a.maxLevel = 15;
	b.maxLevel = 14;
	CHECK(resolveSampler(a).maxLevel == 15);
	CHECK(samplerKey(a) != samplerKey(b));
	a.maxLevel = 40;
	CHECK(resolveSampler(a).maxLevel == 15);

	a.hasMips = b.hasMips = false;
	a.maxLevel = 3;
	b.maxLevel = 0;
	CHECK(resolveSampler(a).maxLevel == 0);
	CHECK(samplerKey(a) == samplerKey(b));

	a = BaseSampler();
	a.maxLevel = 15;
	a.filter = LINEARMIPLINEAR;
	a.addressU = BORDER;
	a.addressV = BORDER;
	a.maxAnisotropy = 16;
	b = a;
	b.maxLevel = 0;
	CHECK(samplerKey(a) != samplerKey(b));
}

static void
TestShaderVariant(void)
{
	uint32_t bits;
	int at;

	for(bits = 0; bits < 16; bits++)
		for(at = 0; at < 2; at++)
			CHECK(shaderVariant(bits, at != 0) == ((at ? 1u : 0u) | (bits & 7u) << 1));
	CHECK(shaderVariant(8, false) == 0);
	CHECK(shaderVariant(1, true) == 3);
	CHECK(shaderVariant(LIGHTBIT_DIRECT | LIGHTBIT_POINT | LIGHTBIT_SPOT, false) == 14);
}

static void
TestLateDepthOnlyIm2dKey(void)
{
	PipelineDesc d = {};

	d.shader = 1;
	d.variant = 1;
	d.vertexLayout = 1;
	d.blendEnable = true;
	d.srcBlend = BLENDZERO;
	d.destBlend = BLENDONE;
	d.writeMask = 15;
	d.colorFormat = COLORFMT_RGBA8;
	d.depthFormat = DEPTHFMT_D32S8;
	d.sampleCount = 1;
	CHECK(pipelineKey(d) == 0x000023e210010401ull);
}

static void
TestSupportedSampleCount(void)
{
	CHECK(supportedSampleCount(0, 4) == 1);
	CHECK(supportedSampleCount(1, 4) == 1);
	CHECK(supportedSampleCount(2, 4) == 2);
	CHECK(supportedSampleCount(3, 4) == 2);
	CHECK(supportedSampleCount(4, 4) == 4);
	CHECK(supportedSampleCount(8, 4) == 4);
	CHECK(supportedSampleCount(8, 8) == 8);
	CHECK(supportedSampleCount(16, 8) == 8);
	CHECK(supportedSampleCount(16, 16) == 8);
	CHECK(supportedSampleCount(4, 1) == 1);
	CHECK(supportedSampleCount(4, 0) == 1);
}

static void
TestPrewarmSampleCounts(void)
{
	uint32_t c[2] = { 0, 0 };
	CHECK(prewarmSampleCounts(0, c) == 1 && c[0] == 1);
	CHECK(prewarmSampleCounts(1, c) == 1 && c[0] == 1);
	c[0] = c[1] = 0;
	CHECK(prewarmSampleCounts(4, c) == 2 && c[0] == 1 && c[1] == 4);
	CHECK(prewarmSampleCounts(8, c) == 2 && c[0] == 1 && c[1] == 8);
}

int
main(void)
{
	TestBlendFactors();
	TestCompareFunctions();
	TestStencilOps();
	TestCullModes();
	TestFilterModes();
	TestAddressModes();
	TestAnisotropy();
	TestAlphaTestRange();
	TestAddressing();
	TestPipelineKey();
	TestDepthState();
	TestDepthStencilKey();
	TestSamplerKey();
	TestSamplerMaxLevel();
	TestShaderVariant();
	TestLateDepthOnlyIm2dKey();
	TestSupportedSampleCount();
	TestPrewarmSampleCounts();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
