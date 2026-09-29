#include "metalkeys.h"

namespace rw {
namespace metal {

#ifdef RW_METAL_GL3_ADDRESSING
const bool gl3Addressing = true;
#else
const bool gl3Addressing = false;
#endif

static const uint32_t blendMap[] = {
	MTLBLEND_ZERO,
	MTLBLEND_ZERO,
	MTLBLEND_ONE,
	MTLBLEND_SRCCOLOR,
	MTLBLEND_INVSRCCOLOR,
	MTLBLEND_SRCALPHA,
	MTLBLEND_INVSRCALPHA,
	MTLBLEND_DESTALPHA,
	MTLBLEND_INVDESTALPHA,
	MTLBLEND_DESTCOLOR,
	MTLBLEND_INVDESTCOLOR,
	MTLBLEND_SRCALPHASAT
};

static const uint32_t stencilOpMap[] = {
	MTLSTENCIL_KEEP,
	MTLSTENCIL_KEEP,
	MTLSTENCIL_ZERO,
	MTLSTENCIL_REPLACE,
	MTLSTENCIL_INCCLAMP,
	MTLSTENCIL_DECCLAMP,
	MTLSTENCIL_INVERT,
	MTLSTENCIL_INCWRAP,
	MTLSTENCIL_DECWRAP
};

static const uint32_t stencilFuncMap[] = {
	MTLCMP_NEVER,
	MTLCMP_NEVER,
	MTLCMP_LESS,
	MTLCMP_EQUAL,
	MTLCMP_LESSEQUAL,
	MTLCMP_GREATER,
	MTLCMP_NOTEQUAL,
	MTLCMP_GREATEREQUAL,
	MTLCMP_ALWAYS
};

static const uint32_t addressMap[] = {
	MTLADDR_REPEAT,
	MTLADDR_REPEAT,
	MTLADDR_MIRRORREPEAT,
	MTLADDR_CLAMPTOEDGE,
	MTLADDR_CLAMPTOBORDER
};

struct FilterMap { uint32_t min, mip; };

static const FilterMap filterMap[] = {
	{ MTLFILTER_NEAREST, MTLMIP_NONE },
	{ MTLFILTER_NEAREST, MTLMIP_NONE },
	{ MTLFILTER_LINEAR, MTLMIP_NONE },
	{ MTLFILTER_NEAREST, MTLMIP_NEAREST },
	{ MTLFILTER_LINEAR, MTLMIP_NEAREST },
	{ MTLFILTER_NEAREST, MTLMIP_LINEAR },
	{ MTLFILTER_LINEAR, MTLMIP_LINEAR }
};

#define NELEM(a) (sizeof(a)/sizeof(a[0]))

static uint32_t
lookup(const uint32_t *map, uint32_t n, uint32_t value)
{
	return value < n ? map[value] : map[0];
}

uint32_t
blendFactor(uint32_t rwBlend)
{
	return lookup(blendMap, NELEM(blendMap), rwBlend);
}

uint32_t
compareFunction(uint32_t rwStencilFunc)
{
	return lookup(stencilFuncMap, NELEM(stencilFuncMap), rwStencilFunc);
}

uint32_t
stencilOperation(uint32_t rwStencilOp)
{
	return lookup(stencilOpMap, NELEM(stencilOpMap), rwStencilOp);
}

uint32_t
cullMode(uint32_t rwCull)
{
	if(rwCull == 1)
		return MTLCULL_NONE;
	return rwCull == 2 ? MTLCULL_BACK : MTLCULL_FRONT;
}

uint32_t
addressMode(uint32_t rwAddressing)
{
	return lookup(addressMap, NELEM(addressMap), rwAddressing);
}

void
alphaTestRange(uint32_t alphaFunc, float alphaRef, float range[2])
{
	switch(alphaFunc){
	case 0:
	default:
		range[0] = -1000.0f;
		range[1] = 1000.0f;
		break;
	case 1:
		range[0] = alphaRef;
		range[1] = 1000.0f;
		break;
	case 2:
		range[0] = -1000.0f;
		range[1] = alphaRef;
		break;
	}
}

uint32_t
addressOnSet(uint32_t applied, uint32_t requested, bool gl3Mode)
{
	return gl3Mode ? applied : requested;
}

static uint32_t
log2Samples(uint32_t n)
{
	uint32_t l = 0;
	while(n > 1 && l < 7){
		n >>= 1;
		l++;
	}
	return l;
}

uint64_t
pipelineKey(const PipelineDesc &d)
{
	uint64_t key = 0;
	uint32_t src = 0, dst = 0;
	int shift = 0;

	if(d.blendEnable){
		src = blendFactor(d.srcBlend);
		dst = blendFactor(d.destBlend);
	}
#define PUT(v, bits) key |= (uint64_t)((v) & ((1u<<(bits))-1)) << shift; shift += bits
	PUT(d.shader, PIPEKEY_SHADERBITS);
	PUT(d.variant, PIPEKEY_VARIANTBITS);
	PUT(d.vertexLayout, PIPEKEY_LAYOUTBITS);
	PUT(d.blendEnable ? 1 : 0, 1);
	PUT(src, 4);
	PUT(dst, 4);
	PUT(d.writeMask, 4);
	PUT(d.colorFormat, 4);
	PUT(d.depthFormat, 3);
	PUT(log2Samples(d.sampleCount), 3);
	return key;
}

uint32_t
shaderVariant(uint32_t lightBits, bool alphaTest)
{
	return (alphaTest ? 1u : 0u) | (lightBits & 7u) << 1;
}

DepthStencilResolved
resolveDepthStencil(const DepthStencilDesc &d)
{
	DepthStencilResolved r = {};

	r.depthCompare = MTLCMP_ALWAYS;
	if(!d.hasDepth)
		return r;
	r.depthCompare = d.ztest ? MTLCMP_LESSEQUAL : MTLCMP_ALWAYS;
	r.depthWrite = d.zwrite;
	if(d.stencilEnable){
		r.stencilEnable = true;
		r.stencilCompare = compareFunction(d.stencilFunc);
		r.stencilFail = stencilOperation(d.stencilFail);
		r.depthFail = stencilOperation(d.stencilZFail);
		r.depthStencilPass = stencilOperation(d.stencilPass);
		r.readMask = d.stencilMask & 0xFF;
		r.writeMask = d.stencilWriteMask & 0xFF;
	}
	return r;
}

uint64_t
depthStencilKey(const DepthStencilDesc &d)
{
	DepthStencilResolved r = resolveDepthStencil(d);
	uint64_t key = 0;
	int shift = 0;

	PUT(r.depthCompare, 3);
	PUT(r.depthWrite ? 1 : 0, 1);
	PUT(r.stencilEnable ? 1 : 0, 1);
	PUT(r.stencilCompare, 3);
	PUT(r.stencilFail, 3);
	PUT(r.depthFail, 3);
	PUT(r.depthStencilPass, 3);
	PUT(r.readMask, 8);
	PUT(r.writeMask, 8);
	return key;
}

SamplerResolved
resolveSampler(const SamplerDesc &d)
{
	SamplerResolved r;
	FilterMap f = d.filter < NELEM(filterMap) ? filterMap[d.filter] : filterMap[0];

	r.minFilter = f.min;
	r.magFilter = f.min;
	r.mipFilter = d.hasMips ? f.mip : MTLMIP_NONE;
	r.addressU = addressMode(d.addressU);
	r.addressV = addressMode(d.addressV);
	r.maxAnisotropy = d.maxAnisotropy < 1 ? 1 : d.maxAnisotropy > 16 ? 16 : d.maxAnisotropy;
	r.maxLevel = !d.hasMips ? 0 : d.maxLevel > 15 ? 15 : d.maxLevel;
	return r;
}

uint32_t
samplerKey(const SamplerDesc &d)
{
	SamplerResolved r = resolveSampler(d);
	uint64_t key = 0;
	int shift = 0;

	PUT(r.minFilter, 1);
	PUT(r.magFilter, 1);
	PUT(r.mipFilter, 2);
	PUT(r.addressU, 3);
	PUT(r.addressV, 3);
	PUT(r.maxAnisotropy-1, 4);
	PUT(r.maxLevel, 4);
#undef PUT
	return (uint32_t)key;
}

}
}
