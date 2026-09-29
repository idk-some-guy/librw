#ifndef RW_METAL_METALKEYS_H
#define RW_METAL_METALKEYS_H

#include <stdint.h>

namespace rw {
namespace metal {

enum MtlBlendFactor
{
	MTLBLEND_ZERO = 0,
	MTLBLEND_ONE,
	MTLBLEND_SRCCOLOR,
	MTLBLEND_INVSRCCOLOR,
	MTLBLEND_SRCALPHA,
	MTLBLEND_INVSRCALPHA,
	MTLBLEND_DESTCOLOR,
	MTLBLEND_INVDESTCOLOR,
	MTLBLEND_DESTALPHA,
	MTLBLEND_INVDESTALPHA,
	MTLBLEND_SRCALPHASAT
};

enum MtlCompare
{
	MTLCMP_NEVER = 0,
	MTLCMP_LESS,
	MTLCMP_EQUAL,
	MTLCMP_LESSEQUAL,
	MTLCMP_GREATER,
	MTLCMP_NOTEQUAL,
	MTLCMP_GREATEREQUAL,
	MTLCMP_ALWAYS
};

enum MtlStencilOp
{
	MTLSTENCIL_KEEP = 0,
	MTLSTENCIL_ZERO,
	MTLSTENCIL_REPLACE,
	MTLSTENCIL_INCCLAMP,
	MTLSTENCIL_DECCLAMP,
	MTLSTENCIL_INVERT,
	MTLSTENCIL_INCWRAP,
	MTLSTENCIL_DECWRAP
};

enum MtlCull
{
	MTLCULL_NONE = 0,
	MTLCULL_FRONT,
	MTLCULL_BACK
};

enum MtlFilter
{
	MTLFILTER_NEAREST = 0,
	MTLFILTER_LINEAR
};

enum MtlMipFilter
{
	MTLMIP_NONE = 0,
	MTLMIP_NEAREST,
	MTLMIP_LINEAR
};

enum MtlAddress
{
	MTLADDR_CLAMPTOEDGE = 0,
	MTLADDR_MIRRORCLAMPTOEDGE,
	MTLADDR_REPEAT,
	MTLADDR_MIRRORREPEAT,
	MTLADDR_CLAMPTOZERO,
	MTLADDR_CLAMPTOBORDER
};

enum ColorFormatId
{
	COLORFMT_NONE = 0,
	COLORFMT_RGBA8,
	COLORFMT_BGRA8
};

enum DepthFormatId
{
	DEPTHFMT_NONE = 0,
	DEPTHFMT_D32S8
};

uint32_t blendFactor(uint32_t rwBlend);
uint32_t compareFunction(uint32_t rwStencilFunc);
uint32_t stencilOperation(uint32_t rwStencilOp);
uint32_t cullMode(uint32_t rwCull);
uint32_t addressMode(uint32_t rwAddressing);

void alphaTestRange(uint32_t alphaFunc, float alphaRef, float range[2]);

extern const bool gl3Addressing;
uint32_t addressOnSet(uint32_t applied, uint32_t requested, bool gl3Mode);

struct PipelineDesc
{
	uint32_t shader;
	uint32_t variant;
	uint32_t vertexLayout;
	bool blendEnable;
	uint32_t srcBlend, destBlend;
	uint32_t writeMask;
	uint32_t colorFormat;
	uint32_t depthFormat;
	uint32_t sampleCount;
};

enum
{
	PIPEKEY_SHADERBITS = 10,
	PIPEKEY_VARIANTBITS = 6,
	PIPEKEY_LAYOUTBITS = 12
};

uint64_t pipelineKey(const PipelineDesc &d);

struct DepthStencilDesc
{
	bool hasDepth;
	bool ztest, zwrite;
	bool stencilEnable;
	uint32_t stencilFunc;
	uint32_t stencilFail, stencilZFail, stencilPass;
	uint32_t stencilMask, stencilWriteMask;
};

struct DepthStencilResolved
{
	uint32_t depthCompare;
	bool depthWrite;
	bool stencilEnable;
	uint32_t stencilCompare;
	uint32_t stencilFail, depthFail, depthStencilPass;
	uint32_t readMask, writeMask;
};

DepthStencilResolved resolveDepthStencil(const DepthStencilDesc &d);
uint64_t depthStencilKey(const DepthStencilDesc &d);

struct SamplerDesc
{
	uint32_t filter;
	uint32_t addressU, addressV;
	uint32_t maxAnisotropy;
	bool hasMips;
	uint32_t maxLevel;
};

struct SamplerResolved
{
	uint32_t minFilter, magFilter, mipFilter;
	uint32_t addressU, addressV;
	uint32_t maxAnisotropy;
	uint32_t maxLevel;
};

SamplerResolved resolveSampler(const SamplerDesc &d);
uint32_t samplerKey(const SamplerDesc &d);

}
}

#endif
