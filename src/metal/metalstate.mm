#ifdef RW_METAL
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <chrono>
#include "metalobjc.h"
#include "rwmetalshader.h"
#include "metalstate.h"
#include "metalkeys.h"
#include "metalframe.h"
#include "metalprepass.h"
#include "metalprepasskeys.h"

#define PLUGIN_ID 0

namespace rw {
namespace metal {

static_assert(MTLBLEND_SRCALPHASAT == (int)MTLBlendFactorSourceAlphaSaturated &&
	MTLBLEND_INVDESTALPHA == (int)MTLBlendFactorOneMinusDestinationAlpha &&
	MTLBLEND_DESTCOLOR == (int)MTLBlendFactorDestinationColor, "blend factors");
static_assert(MTLCMP_ALWAYS == (int)MTLCompareFunctionAlways &&
	MTLCMP_LESSEQUAL == (int)MTLCompareFunctionLessEqual, "compare functions");
static_assert(MTLSTENCIL_DECWRAP == (int)MTLStencilOperationDecrementWrap &&
	MTLSTENCIL_INCCLAMP == (int)MTLStencilOperationIncrementClamp, "stencil operations");
static_assert(MTLCULL_FRONT == (int)MTLCullModeFront && MTLCULL_BACK == (int)MTLCullModeBack, "cull modes");
static_assert(MTLFILTER_LINEAR == (int)MTLSamplerMinMagFilterLinear &&
	MTLMIP_LINEAR == (int)MTLSamplerMipFilterLinear, "filters");
static_assert(MTLADDR_REPEAT == (int)MTLSamplerAddressModeRepeat &&
	MTLADDR_MIRRORREPEAT == (int)MTLSamplerAddressModeMirrorRepeat &&
	MTLADDR_CLAMPTOBORDER == (int)MTLSamplerAddressModeClampToBorderColor, "address modes");
static_assert(BLENDSRCALPHASAT == 11 && STENCILDEC == 8 && STENCILALWAYS == 8 && CULLFRONT == 3 &&
	Texture::BORDER == 4 && Texture::LINEARMIPLINEAR == 6 && ALPHALESS == 2, "RenderWare values in metalkeys.cpp");
static_assert(VARIANT_DIRECTIONALS == LIGHTBIT_DIRECT << 1 && VARIANT_POINTLIGHTS == LIGHTBIT_POINT << 1 &&
	VARIANT_SPOTLIGHTS == LIGHTBIT_SPOT << 1, "variants");

#include "shaders/im2d_metal.inc"
#include "shaders/simple_metal.inc"
#include "shaders/default_metal.inc"
#include "shaders/emit_metal.inc"
#include "shaders/prepassh_metal.inc"
#include "shaders/prepass_metal.inc"
#include "shaders/prepass_skin_metal.inc"

static int32   alphaFunc;
static float32 alphaRef;

struct UniformState
{
	float32 alphaRefLow;
	float32 alphaRefHigh;
	int32   pad[2];

	float32 fogStart;
	float32 fogEnd;
	float32 fogRange;
	float32 fogDisable;

	RGBAf   fogColor;
};

struct UniformScene
{
	float32 proj[16];
	float32 view[16];
	float32 xform[4];
};

#define MAX_LIGHTS 8

struct UniformObject
{
	RawMatrix    world;
};

struct UniformLights
{
	RGBAf        ambLight;
	struct {
		float type;
		float radius;
		float minusCosAngle;
		float hardSpot;
	} lightParams[MAX_LIGHTS];
	V4d lightPosition[MAX_LIGHTS];
	V4d lightDirection[MAX_LIGHTS];
	RGBAf lightColor[MAX_LIGHTS];
};

struct UniformMaterial
{
	RGBAf   matColor;
	float32 surfProps[4];
};

struct UniformSkin
{
	RawMatrix bones[MAXSKINBONES];
};

struct UniformMatFX
{
	RawMatrix texMatrix;
	float32   fxParams[4];
	RGBAf     colorClamp;
	RGBAf     envColor;
};

static_assert(sizeof(UniformSkin) == 4096, "skin block size");
static_assert(sizeof(UniformMatFX) == 112, "matfx block size");
static_assert(offsetof(UniformMatFX, fxParams) == 64 && offsetof(UniformMatFX, colorClamp) == 80 &&
	offsetof(UniformMatFX, envColor) == 96, "UniformMatFX layout");
static_assert(sizeof(UniformObject) == 64 && sizeof(UniformLights) == 528 && sizeof(UniformMaterial) == 32, "block sizes");
static_assert(offsetof(UniformLights, lightParams) == 16 && offsetof(UniformLights, lightPosition) == 144 &&
	offsetof(UniformLights, lightDirection) == 272 && offsetof(UniformLights, lightColor) == 400, "UniformLights layout");
static_assert(VSLIGHT_DIRECT == LIGHTBIT_DIRECT && VSLIGHT_POINT == LIGHTBIT_POINT && VSLIGHT_SPOT == LIGHTBIT_SPOT, "light bits");

static UniformState uniformState;
static UniformScene uniformScene;
static UniformObject uniformObject;
static UniformLights uniformLights;
static UniformMaterial uniformMaterial;
static UniformSkin uniformSkin;
static UniformMatFX uniformMatFX;
static uint8 customConstants[MAXCUSTOMCONSTANTS];

enum
{
	BLOCK_SCENE,
	BLOCK_OBJECT,
	BLOCK_LIGHTS,
	BLOCK_MATERIAL,
	BLOCK_STATE,
	BLOCK_CUSTOM,
	BLOCK_SKIN,
	BLOCK_MATFX,
	NUMBLOCKS
};

static_assert(NUMBLOCKS <= 8, "block masks are uint8");
static_assert(BUFFER_MATFX < sizeof(StateStats::blockUploads)/sizeof(StateStats::blockUploads[0]), "stat arrays");

struct BlockInfo
{
	uint32 index;
	void *data;
	uint32 size;
};

static BlockInfo blockInfo[NUMBLOCKS] = {
	{ BUFFER_SCENE, &uniformScene, sizeof(UniformScene) },
	{ BUFFER_OBJECT, &uniformObject, sizeof(UniformObject) },
	{ BUFFER_LIGHTS, &uniformLights, sizeof(UniformLights) },
	{ BUFFER_MATERIAL, &uniformMaterial, sizeof(UniformMaterial) },
	{ BUFFER_STATE, &uniformState, sizeof(UniformState) },
	{ BUFFER_CUSTOM, customConstants, 0 },
	{ BUFFER_SKIN, &uniformSkin, sizeof(UniformSkin) },
	{ BUFFER_MATFX, &uniformMatFX, sizeof(UniformMatFX) },
};

static bool32 stateDirty = 1;
static bool blockDirty[NUMBLOCKS];
static id<MTLBuffer> blockBuffer[NUMBLOCKS];
static uint32 blockOffset[NUMBLOCKS];
static uint32 blockBufferSize[NUMBLOCKS];

struct RwRasterStateCache {
	Raster *raster;
	Texture::Addressing addressingU;
	Texture::Addressing addressingV;
	Texture::FilterMode filter;
};

struct RwStateCache {
	bool32 vertexAlpha;
	uint32 alphaTestEnable;
	uint32 alphaFunc;
	bool32 textureAlpha;
	bool32 blendEnable;
	uint32 srcblend, destblend;
	uint32 zwrite;
	uint32 ztest;
	uint32 cullmode;
	uint32 stencilenable;
	uint32 stencilpass;
	uint32 stencilfail;
	uint32 stencilzfail;
	uint32 stencilfunc;
	uint32 stencilref;
	uint32 stencilmask;
	uint32 stencilwritemask;
	uint32 fogEnable;
	float32 fogStart;
	float32 fogEnd;

	bool32 gsalpha;
	uint32 gsalpharef;

	RwRasterStateCache texstage[MAXNUMSTAGES];
};
static RwStateCache rwStateCache;
static int32 numStagesUsed = 1;

Shader *im2dShader;
Shader *im2dEmitShader;
Shader *defaultShader, *defaultShader_noAT;
Shader *defaultShader_fullLight, *defaultShader_fullLight_noAT;
uint32 im2dVertexLayout;
static uint32 currentLayout;

static AttribDesc im2dAttribDesc[3] = {
	{ ATTRIB_POS,        ATTRIBFMT_FLOAT4,      sizeof(Im2DVertex), 0 },
	{ ATTRIB_COLOR,      ATTRIBFMT_UCHAR4_NORM, sizeof(Im2DVertex), offsetof(Im2DVertex, r) },
	{ ATTRIB_TEXCOORDS0, ATTRIBFMT_FLOAT2,      sizeof(Im2DVertex), offsetof(Im2DVertex, u) },
};

enum { MAXATTRIBS = 16 };

struct VertexLayout
{
	AttribDesc attribs[MAXATTRIBS];
	int32 numAttribs;
	MTLVertexDescriptor *desc;
};
static std::vector<VertexLayout> vertexLayouts;

struct PipelineEntry
{
	id<MTLRenderPipelineState> state;
	bool defaultAttribs;
	uint8 vertexBlocks;
	uint8 fragmentBlocks;
	uint32 blockSizes[NUMBLOCKS];
	uint8 textureStages;
	bool fixedBlend;
	bool shaderBlend;
	bool emissive;
};
static std::unordered_map<uint64_t, PipelineEntry> pipelineCache;
static std::unordered_map<uint64_t, id<MTLDepthStencilState>> depthStencilCache;
static uint64 lastPipeKey;
static PipelineEntry *lastPipe;
static uint64 lastDepthKey;
static id<MTLDepthStencilState> lastDepth;
static std::unordered_map<uint32_t, id<MTLSamplerState>> samplerCache;
static bool prewarming;
static bool hostPrewarming;
static uint32 warmedFormats;
static float32 drawEmissive = 1.0f;

struct HostPrewarmRecord
{
	Shader *shader;
	uint32 layout;
	uint32 variant;
	bool32 blend;
	int32 srcBlend, destBlend;
	bool32 depth;
	uint32 formats;
};
static std::vector<HostPrewarmRecord> hostRecords;

static bool
im2DLayout(uint32 layout)
{
	return layout == im2dVertexLayout || (im2dUV2VertexLayout != 0 && layout == im2dUV2VertexLayout);
}
static StateStats stats;

Shader *prepassShader, *prepassSkinShader;
static const void *currentDrawInst;
static const void *currentObject;
enum { PREPASS_KEYS = 8192 };
static PrepassKeySet prepassKeys;
static bool prepassFrame;
static bool orphanReported;
static uint32 prepassDrawsThisFrame;
static float32 prepassView[16], prepassProj[16];
static bool prepassViewSame;
static bool forceEqualNext;
enum { PREV_WORLDS = 8192, PREV_PALETTES = 256 };
static PrevMap<RawMatrix> worldsPrev, worldsCur;
static PrevMap<UniformSkin> bonesPrev, bonesCur;
static float32 projBase[16];
static RawMatrix lastViewProj;
static bool haveLastViewProj;
static PrepassMatrices prepassMats;

struct PrepassFrameBlock
{
	RawMatrix viewProj;
	RawMatrix prevViewProj;
};
static PrepassFrameBlock prepassFrameBlock;
static bool prepassFrameBound;

static void
mul44(float32 *out, const float32 *a, const float32 *b)
{
	for(int c = 0; c < 4; c++)
		for(int r = 0; r < 4; r++){
			float32 s = 0.0f;
			for(int k = 0; k < 4; k++)
				s += a[k*4+r]*b[c*4+k];
			out[c*4+r] = s;
		}
}

static bool
layoutHasAttrib(uint32 layout, uint32 index)
{
	if(layout == 0 || layout >= vertexLayouts.size())
		return false;
	const VertexLayout &l = vertexLayouts[layout];
	for(int32 i = 0; i < l.numAttribs; i++)
		if(l.attribs[i].index == index)
			return true;
	return false;
}

bool
prepassShadersReady(void)
{
	return prepassShader != nil && prepassSkinShader != nil;
}

static void
comparePrepassView(void)
{
	if(prepassFrame)
		prepassViewSame = memcmp(prepassView, uniformScene.view, sizeof(prepassView)) == 0 &&
			memcmp(prepassProj, uniformScene.proj, sizeof(prepassProj)) == 0;
}

void
beginPrepassState(bool continuous)
{
	if(prepassKeys.capacity() == 0){
		prepassKeys.reserve(PREPASS_KEYS);
		worldsPrev.reserve(PREV_WORLDS);
		worldsCur.reserve(PREV_WORLDS);
		bonesPrev.reserve(PREV_PALETTES);
		bonesCur.reserve(PREV_PALETTES);
	}
	prepassKeys.clear();
	prepassFrame = true;
	prepassFrameBound = false;
	prepassDrawsThisFrame = 0;
	memcpy(prepassView, uniformScene.view, sizeof(prepassView));
	memcpy(prepassProj, uniformScene.proj, sizeof(prepassProj));
	prepassViewSame = true;
	bool cut = !haveLastViewProj || !continuous;
	std::swap(worldsPrev, worldsCur);
	worldsCur.clear();
	std::swap(bonesPrev, bonesCur);
	bonesCur.clear();
	if(cut){
		worldsPrev.clear();
		bonesPrev.clear();
	}
	RawMatrix vp;
	mul44((float32*)&vp, projBase, uniformScene.view);
	mul44((float32*)&prepassMats.uploadedViewProj, uniformScene.proj, uniformScene.view);
	prepassMats.viewProj = vp;
	prepassMats.prevViewProj = cut ? vp : lastViewProj;
	prepassFrameBlock.viewProj = vp;
	prepassFrameBlock.prevViewProj = prepassMats.prevViewProj;
	lastViewProj = vp;
	haveLastViewProj = true;
}

const PrepassMatrices&
prepassMatrices(void)
{
	return prepassMats;
}

uint32
endPrepassState(void)
{
	return prepassDrawsThisFrame;
}

void
finishPrepassFrame(bool countOrphans)
{
	if(!prepassFrame)
		return;
	uint32 orphans = countOrphans ? prepassKeys.unused() : 0;
	stats.prepassOrphans += orphans;
	if(orphans && !orphanReported){
		orphanReported = true;
		stats.prepassOrphanLogs++;
		fprintf(stderr, "rw::metal: %u prepass draws had no forward draw this frame; their depth hides what lies behind them\n", orphans);
	}
	prepassKeys.clear();
	prepassFrame = false;
	forceEqualNext = false;
}

void
setDrawInstance(const void *inst)
{
	currentDrawInst = inst;
}

void
cameraCut(void)
{
	haveLastViewProj = false;
}

void
forcePrepassEqualForTest(void)
{
	forceEqualNext = true;
}

struct DefaultAttribs
{
	float32 f[4];
	uint8 u[4];
};
static const DefaultAttribs defaultAttribs = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0, 0, 0, 1 } };

struct EncoderState
{
	id<MTLRenderCommandEncoder> encoder;
	id<MTLRenderPipelineState> pipeline;
	id<MTLDepthStencilState> depthStencil;
	int32 cull;
	bool winding;
	bool scissorKnown;
	int32 stencilRef;
	bool viewportKnown;
	MTLViewport viewport;
	bool defaultAttribs;
	id<MTLBuffer> vertexBuffers[NUMBLOCKS];
	id<MTLBuffer> fragmentBuffers[NUMBLOCKS];
	uint32 vertexOffsets[NUMBLOCKS];
	uint32 fragmentOffsets[NUMBLOCKS];
	id<MTLTexture> textures[MAXNUMSTAGES];
	id<MTLSamplerState> samplers[MAXNUMSTAGES];
	id<MTLBuffer> vertexData;
	uint32 vertexDataOffset;
	float32 emissive;
};
static EncoderState enc;
static bool haveViewport;
static MTLViewport wantViewport;

enum { RINGSTARTSIZE = 8<<20 };

struct Ring
{
	id<MTLBuffer> buffers[MAXFRAMESINFLIGHT];
	std::vector<id<MTLBuffer>> retired[MAXFRAMESINFLIGHT];
	uint32 frame;
	uint32 used;
	uint64 frameId;
	uint64 slotFrame[MAXFRAMESINFLIGHT];
	std::atomic<uint64> slotDone[MAXFRAMESINFLIGHT];
};
static Ring ring;
static std::atomic<uint64> completedFrameId;

struct StatsLog
{
	std::chrono::steady_clock::time_point time;
	uint32 framesAtStart;
	uint32 frames;
	uint32 draws;
	uint32 frameBytes;
	uint32 ringPeak;
	uint32 renderPasses, copies;
	uint32 customUploads, materialUploads, drawablesAcquired;
	RasterStats raster;
	InstanceStats instance;
};
static StatsLog statsLog;
static uint32 statsInterval;
static void resetStatsLog(void);
static char prewarmLine[128];
static char statsLine[1024];
static char dropLine[256];
static std::unordered_set<uint32> dropsReported;

void
beginFrameState(void)
{
	int i;
	ring.frameId++;
	ring.frame = ring.frameId % MAXFRAMESINFLIGHT;
	if(ring.slotDone[ring.frame].load() != ring.slotFrame[ring.frame]){
		stats.ringEarlyReuses++;
		assert(0 && "ring slot reused before its frame completed");
	}
	if(statsLog.frameBytes > statsLog.ringPeak)
		statsLog.ringPeak = statsLog.frameBytes;
	if(statsLog.frameBytes > stats.ringPeakBytes)
		stats.ringPeakBytes = statsLog.frameBytes;
	statsLog.frameBytes = 0;
	ring.slotFrame[ring.frame] = ring.frameId;
	ring.used = 0;
	ring.retired[ring.frame].clear();
	for(i = 0; i < NUMBLOCKS; i++)
		blockDirty[i] = true;
}

uint64
getFrameId(void)
{
	return ring.frameId;
}

void
frameCompleted(uint64 frameId)
{
	uint64 done;

	ring.slotDone[frameId % MAXFRAMESINFLIGHT].store(frameId);
	done = completedFrameId.load();
	while(done < frameId && !completedFrameId.compare_exchange_weak(done, frameId))
		;
}

uint64
getCompletedFrameId(void)
{
	return completedFrameId.load();
}

bool32
ringAlloc(uint32 size, uint32 align, RingSpace *space)
{
	MetalContext *ctx = getContext();
	id<MTLBuffer> buf;
	NSUInteger len;
	uint32 off, usedBefore;

	if(ctx == nil)
		return 0;
	startFrame();
	buf = ring.buffers[ring.frame];
	if(align == 0)
		align = 4;
	assert((align & (align-1)) == 0);
	usedBefore = ring.used;
	off = (ring.used + align-1) & ~(align-1);
	if(buf == nil || off + size > buf.length){
		len = buf ? buf.length*2 : RINGSTARTSIZE;
		while(len < size)
			len *= 2;
		buf = [ctx->device newBufferWithLength:len
			options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
		if(buf == nil)
			return 0;
		if(ring.buffers[ring.frame]){
			ring.retired[ring.frame].push_back(ring.buffers[ring.frame]);
			stats.ringGrows++;
		}
		ring.buffers[ring.frame] = buf;
		off = 0;
		usedBefore = 0;
	}
	ring.used = off + size;
	statsLog.frameBytes += ring.used - usedBefore;
	space->cpu = (uint8*)buf.contents + off;
	space->buffer = (__bridge void*)buf;
	space->offset = off;
	return 1;
}

void
invalidateEncoderState(void)
{
	MetalContext *ctx = getContext();
	int i;

	enc.encoder = ctx ? ctx->encoder : nil;
	enc.pipeline = nil;
	enc.depthStencil = nil;
	enc.cull = -1;
	enc.winding = false;
	enc.scissorKnown = false;
	enc.stencilRef = -1;
	enc.viewportKnown = false;
	enc.defaultAttribs = false;
	for(i = 0; i < NUMBLOCKS; i++){
		enc.vertexBuffers[i] = nil;
		enc.fragmentBuffers[i] = nil;
	}
	for(i = 0; i < MAXNUMSTAGES; i++){
		enc.textures[i] = nil;
		enc.samplers[i] = nil;
	}
	enc.vertexData = nil;
	enc.emissive = -1.0f;
	prepassFrameBound = false;
}

static void
syncEncoder(MetalContext *ctx)
{
	if(ctx->encoder != enc.encoder)
		invalidateEncoderState();
}

static bool
sameViewport(const MTLViewport &a, const MTLViewport &b)
{
	return a.originX == b.originX && a.originY == b.originY &&
		a.width == b.width && a.height == b.height &&
		a.znear == b.znear && a.zfar == b.zfar;
}

static void
applyViewport(MetalContext *ctx)
{
	if(!haveViewport || ctx->encoder == nil)
		return;
	syncEncoder(ctx);
	if(enc.viewportKnown && sameViewport(enc.viewport, wantViewport))
		return;
	[ctx->encoder setViewport:wantViewport];
	enc.viewport = wantViewport;
	enc.viewportKnown = true;
}

void
setEncoderViewport(double x, double y, double w, double h)
{
	MetalContext *ctx = getContext();
	wantViewport.originX = x;
	wantViewport.originY = y;
	wantViewport.width = w;
	wantViewport.height = h;
	wantViewport.znear = 0.0;
	wantViewport.zfar = 1.0;
	haveViewport = true;
	if(ctx)
		applyViewport(ctx);
}

static MTLVertexFormat attribFormatMap[] = {
	MTLVertexFormatFloat2,
	MTLVertexFormatFloat3,
	MTLVertexFormatFloat4,
	MTLVertexFormatUChar4,
	MTLVertexFormatUChar4Normalized
};

uint32
registerVertexLayout(const AttribDesc *attribs, int32 numAttribs)
{
	VertexLayout l;
	uint32 i;
	int32 j;

	if(numAttribs > MAXATTRIBS)
		return 0;
	if(vertexLayouts.empty())
		vertexLayouts.push_back(VertexLayout());
	for(i = 1; i < vertexLayouts.size(); i++)
		if(vertexLayouts[i].numAttribs == numAttribs &&
		   memcmp(vertexLayouts[i].attribs, attribs, numAttribs*sizeof(AttribDesc)) == 0)
			return i;
	if(vertexLayouts.size() >= (1<<PIPEKEY_LAYOUTBITS))
		return 0;

	memset(l.attribs, 0, sizeof(l.attribs));
	memcpy(l.attribs, attribs, numAttribs*sizeof(AttribDesc));
	l.numAttribs = numAttribs;
	l.desc = [MTLVertexDescriptor vertexDescriptor];
	for(j = 0; j < numAttribs; j++){
		l.desc.attributes[attribs[j].index].format = attribFormatMap[attribs[j].format];
		l.desc.attributes[attribs[j].index].offset = attribs[j].offset;
		l.desc.attributes[attribs[j].index].bufferIndex = BUFFER_VERTEX;
		l.desc.layouts[BUFFER_VERTEX].stride = attribs[j].stride;
	}
	vertexLayouts.push_back(l);
	return vertexLayouts.size()-1;
}

void
setVertexLayout(uint32 layout)
{
	currentLayout = layout;
}

int32
getVertexLayout(uint32 layout, AttribDesc *attribs, int32 maxAttribs)
{
	if(layout == 0 || layout >= vertexLayouts.size() ||
	   vertexLayouts[layout].numAttribs > maxAttribs)
		return 0;
	memcpy(attribs, vertexLayouts[layout].attribs, vertexLayouts[layout].numAttribs*sizeof(AttribDesc));
	return vertexLayouts[layout].numAttribs;
}

static bool
isFloatType(MTLDataType t)
{
	return (t >= MTLDataTypeFloat && t <= MTLDataTypeFloat4) ||
		(t >= MTLDataTypeHalf && t <= MTLDataTypeHalf4);
}

static MTLVertexDescriptor*
makeVertexDescriptor(uint32 layout, id<MTLFunction> vs, bool *usesDefaults)
{
	MTLVertexDescriptor *vd;
	NSUInteger i;
	bool integer;

	if(layout > 0 && layout < vertexLayouts.size())
		vd = [vertexLayouts[layout].desc copy];
	else
		vd = [MTLVertexDescriptor vertexDescriptor];
	*usesDefaults = false;
	for(MTLVertexAttribute *a in vs.vertexAttributes){
		i = a.attributeIndex;
		if(vd.attributes[i].format != MTLVertexFormatInvalid)
			continue;
		integer = !isFloatType(a.attributeType);
		vd.attributes[i].format = integer ? MTLVertexFormatUChar4 : MTLVertexFormatFloat4;
		vd.attributes[i].offset = integer ? offsetof(DefaultAttribs, u) : 0;
		vd.attributes[i].bufferIndex = BUFFER_DEFAULTATTRIBS;
		*usesDefaults = true;
	}
	if(*usesDefaults){
		vd.layouts[BUFFER_DEFAULTATTRIBS].stride = sizeof(defaultAttribs);
		vd.layouts[BUFFER_DEFAULTATTRIBS].stepFunction = MTLVertexStepFunctionConstant;
		vd.layouts[BUFFER_DEFAULTATTRIBS].stepRate = 0;
	}
	return vd;
}

static MTLPixelFormat colorFormatMap[] = {
	MTLPixelFormatInvalid,
	MTLPixelFormatRGBA8Unorm,
	MTLPixelFormatBGRA8Unorm,
	MTLPixelFormatRGBA16Float,
	MTLPixelFormatRGBA8Unorm,
	MTLPixelFormatRG16Snorm
};

static MTLPixelFormat depthFormatMap[] = {
	MTLPixelFormatInvalid,
	MTLPixelFormatDepth32Float_Stencil8
};

static MTLRenderPipelineDescriptor*
makePipelineDescriptor(Shader *shader, const PipelineDesc &d, bool *usesDefaults, bool *shaderBlend)
{
	MTLRenderPipelineDescriptor *pd;
	void *vfn, *ffn;
	id<MTLFunction> vs, fs;

	if(!shader->getFunctions(d.variant, &vfn, &ffn))
		return nil;
	*shaderBlend = false;
	if(d.colorFormat == COLORFMT_SCENE)
		*shaderBlend = shader->getSceneFunction(d.variant, d.blendEnable, blendFactor(d.srcBlend), blendFactor(d.destBlend), &ffn) &&
			d.blendEnable;
	vs = (__bridge id<MTLFunction>)vfn;
	fs = (__bridge id<MTLFunction>)ffn;

	pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = vs;
	pd.fragmentFunction = fs;
	pd.vertexDescriptor = makeVertexDescriptor(d.vertexLayout, vs, usesDefaults);
	pd.colorAttachments[0].pixelFormat = colorFormatMap[d.colorFormat];
	pd.colorAttachments[0].writeMask = (MTLColorWriteMask)d.writeMask;
	if(d.blendEnable && !*shaderBlend){
		pd.colorAttachments[0].blendingEnabled = YES;
		pd.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)blendFactor(d.srcBlend);
		pd.colorAttachments[0].sourceAlphaBlendFactor = (MTLBlendFactor)blendFactor(d.srcBlend);
		pd.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)blendFactor(d.destBlend);
		pd.colorAttachments[0].destinationAlphaBlendFactor = (MTLBlendFactor)blendFactor(d.destBlend);
	}
	if(d.colorFormat == COLORFMT_OVERLAY && d.blendEnable){
		bool over = d.destBlend == BLENDINVSRCALPHA;
		pd.colorAttachments[0].sourceAlphaBlendFactor = over ? MTLBlendFactorOne : MTLBlendFactorZero;
		pd.colorAttachments[0].destinationAlphaBlendFactor = over ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
	}
	if(d.colorFormat == COLORFMT_PREPASS){
		pd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
		pd.colorAttachments[1].writeMask = MTLColorWriteMaskAll;
	}
	pd.depthAttachmentPixelFormat = depthFormatMap[d.depthFormat];
	pd.stencilAttachmentPixelFormat = depthFormatMap[d.depthFormat];
	pd.rasterSampleCount = d.sampleCount;
	return pd;
}

static void
reflectBlocks(MTLRenderPipelineReflection *refl, PipelineEntry *e)
{
	uint8 *masks[2] = { &e->vertexBlocks, &e->fragmentBlocks };
	NSArray<id<MTLBinding>> *stages[2] = { refl.vertexBindings, refl.fragmentBindings };
	uint32 size;
	int s, i;

	for(s = 0; s < 2; s++)
		for(id<MTLBinding> b in stages[s]){
			if(!b.used)
				continue;
			if(s == 1 && (b.type == MTLBindingTypeTexture || b.type == MTLBindingTypeSampler) &&
			   b.index < MAXNUMSTAGES)
				e->textureStages |= 1<<b.index;
			if(b.type != MTLBindingTypeBuffer)
				continue;
			if(s == 1 && b.index == BUFFER_EMISSIVE)
				e->emissive = true;
			for(i = 0; i < NUMBLOCKS; i++){
				if(blockInfo[i].index != b.index)
					continue;
				*masks[s] |= 1<<i;
				size = (uint32)((id<MTLBufferBinding>)b).bufferDataSize;
				if(size > e->blockSizes[i])
					e->blockSizes[i] = size;
				if(i == BLOCK_CUSTOM)
					continue;
				if(size == blockInfo[i].size)
					continue;
				fprintf(stderr, "rw::metal: uniform block %d is %u bytes, shader expects %u\n", i,
					blockInfo[i].size, size);
				stats.blockSizeMismatches++;
			}
		}
}

static void
noteTextureStages(Shader *shader, uint8 stages)
{
	if(shader->textureStages < 0)
		shader->textureStages = 0;
	shader->textureStages |= stages;
}

static PipelineEntry*
getPipeline(Shader *shader, const PipelineDesc &d, uint64 key)
{
	MetalContext *ctx = getContext();
	MTLRenderPipelineDescriptor *pd;
	MTLRenderPipelineReflection *refl = nil;
	PipelineEntry e = { nil, false, 0, 0 };
	NSError *err = nil;

	auto it = pipelineCache.find(key);
	if(it != pipelineCache.end()){
		if(it->second.state == nil)
			return nil;
		noteTextureStages(shader, it->second.textureStages);
		return &it->second;
	}

	@autoreleasepool {
		pd = makePipelineDescriptor(shader, d, &e.defaultAttribs, &e.shaderBlend);
		e.fixedBlend = pd && pd.colorAttachments[0].blendingEnabled;
		if(pd)
			e.state = [ctx->device newRenderPipelineStateWithDescriptor:pd
				options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
				reflection:&refl error:&err];
		if(e.state){
			reflectBlocks(refl, &e);
			if(e.blockSizes[BLOCK_CUSTOM] > MAXCUSTOMCONSTANTS){
				e.state = nil;
				stats.pipelineFailures++;
				RWERROR((ERR_GENERAL, "custom constant block larger than MAXCUSTOMCONSTANTS"));
			}
		}else{
			stats.pipelineFailures++;
			RWERROR((ERR_GENERAL, err ? err.localizedDescription.UTF8String : "no shader functions"));
		}
	}
	if(prewarming)
		stats.pipelinesAtInit++;
	else if(hostPrewarming)
		stats.pipelinesHost++;
	else{
		stats.pipelinesLate++;
		fprintf(stderr, "rw::metal: pipeline %016llx created after init\n", (unsigned long long)key);
	}
	pipelineCache[key] = e;
	if(e.state == nil)
		return nil;
	noteTextureStages(shader, e.textureStages);
	return &pipelineCache[key];
}

bool32
pipelineCached(uint64 key)
{
	auto it = pipelineCache.find(key);
	return it != pipelineCache.end() && it->second.state != nil;
}

PipelineBlendInfo
pipelineBlendInfo(uint64 key)
{
	PipelineBlendInfo b = { 0, 0, 0 };
	auto it = pipelineCache.find(key);
	if(it == pipelineCache.end() || it->second.state == nil)
		return b;
	b.cached = 1;
	b.fixedBlend = it->second.fixedBlend;
	b.shaderBlend = it->second.shaderBlend;
	return b;
}

void
setDrawEmissiveScale(float32 s)
{
	drawEmissive = s;
}

void
forgetShaderPipelines(uint32 shaderId)
{
	hostRecords.erase(std::remove_if(hostRecords.begin(), hostRecords.end(),
		[shaderId](const HostPrewarmRecord &r){ return r.shader->shaderId == shaderId; }), hostRecords.end());
	lastPipeKey = 0;
	lastPipe = nil;
	lastDepthKey = 0;
	lastDepth = nil;
	auto it = pipelineCache.begin();
	while(it != pipelineCache.end()){
		if((it->first & ((1<<PIPEKEY_SHADERBITS)-1)) == shaderId)
			it = pipelineCache.erase(it);
		else
			++it;
	}
}

static id<MTLDepthStencilState>
getDepthStencil(uint64 key, const DepthStencilResolved &r)
{
	MetalContext *ctx = getContext();
	MTLDepthStencilDescriptor *desc;
	MTLStencilDescriptor *stencil;
	id<MTLDepthStencilState> ds;

	auto it = depthStencilCache.find(key);
	if(it != depthStencilCache.end())
		return it->second;

	desc = [MTLDepthStencilDescriptor new];
	desc.depthCompareFunction = (MTLCompareFunction)r.depthCompare;
	desc.depthWriteEnabled = r.depthWrite;
	if(r.stencilEnable){
		stencil = [MTLStencilDescriptor new];
		stencil.stencilCompareFunction = (MTLCompareFunction)r.stencilCompare;
		stencil.stencilFailureOperation = (MTLStencilOperation)r.stencilFail;
		stencil.depthFailureOperation = (MTLStencilOperation)r.depthFail;
		stencil.depthStencilPassOperation = (MTLStencilOperation)r.depthStencilPass;
		stencil.readMask = r.readMask;
		stencil.writeMask = r.writeMask;
		desc.frontFaceStencil = stencil;
		desc.backFaceStencil = stencil;
	}
	ds = [ctx->device newDepthStencilStateWithDescriptor:desc];
	depthStencilCache[key] = ds;
	return ds;
}

static id<MTLSamplerState>
getSampler(const SamplerDesc &d)
{
	MetalContext *ctx = getContext();
	MTLSamplerDescriptor *desc;
	id<MTLSamplerState> smp;
	uint32_t key = samplerKey(d);
	SamplerResolved r;

	auto it = samplerCache.find(key);
	if(it != samplerCache.end())
		return it->second;

	r = resolveSampler(d);
	desc = [MTLSamplerDescriptor new];
	desc.minFilter = (MTLSamplerMinMagFilter)r.minFilter;
	desc.magFilter = (MTLSamplerMinMagFilter)r.magFilter;
	desc.mipFilter = (MTLSamplerMipFilter)r.mipFilter;
	desc.sAddressMode = (MTLSamplerAddressMode)r.addressU;
	desc.tAddressMode = (MTLSamplerAddressMode)r.addressV;
	desc.maxAnisotropy = r.maxAnisotropy;
	desc.lodMaxClamp = r.maxLevel;
	desc.borderColor = MTLSamplerBorderColorTransparentBlack;
	smp = [ctx->device newSamplerStateWithDescriptor:desc];
	samplerCache[key] = smp;
	return smp;
}

void
setAlphaBlend(bool32 enable)
{
	if(rwStateCache.blendEnable != enable)
		rwStateCache.blendEnable = enable;
}

bool32
getAlphaBlend(void)
{
	return rwStateCache.blendEnable;
}

bool32 getAlphaTest(void) { return rwStateCache.alphaTestEnable; }

static void
setDepthTest(bool32 enable)
{
	if(rwStateCache.ztest != enable)
		rwStateCache.ztest = enable;
}

static void
setDepthWrite(bool32 enable)
{
	enable = enable ? 1 : 0;
	if(rwStateCache.zwrite != enable)
		rwStateCache.zwrite = enable;
}

static void
setAlphaTest(bool32 enable)
{
	uint32 shaderfunc;
	if(rwStateCache.alphaTestEnable != enable){
		rwStateCache.alphaTestEnable = enable;
		shaderfunc = rwStateCache.alphaTestEnable ? rwStateCache.alphaFunc : ALPHAALWAYS;
		if(alphaFunc != shaderfunc){
			alphaFunc = shaderfunc;
			stateDirty = 1;
		}
	}
}

static void
setAlphaTestFunction(uint32 function)
{
	uint32 shaderfunc;
	if(rwStateCache.alphaFunc != function){
		rwStateCache.alphaFunc = function;
		shaderfunc = rwStateCache.alphaTestEnable ? rwStateCache.alphaFunc : ALPHAALWAYS;
		if(alphaFunc != shaderfunc){
			alphaFunc = shaderfunc;
			stateDirty = 1;
		}
	}
}

static void
setVertexAlpha(bool32 enable)
{
	if(rwStateCache.vertexAlpha != enable){
		if(!rwStateCache.textureAlpha){
			setAlphaBlend(enable);
			setAlphaTest(enable);
		}
		rwStateCache.vertexAlpha = enable;
	}
}

static MetalRaster*
stageExt(Raster *raster)
{
	if(raster == nil || raster->platform != PLATFORM_METAL)
		return nil;
	return GETMETALRASTEREXT(raster);
}

static void
reportForeignRaster(Raster *raster)
{
	static bool reported;
	if(reported)
		return;
	reported = true;
	RWERROR((ERR_PLATFORM, raster->platform));
}

static void
setFilterMode(uint32 stage, int32 filter, int32 maxAniso = 1)
{
	if(rwStateCache.texstage[stage].filter != (Texture::FilterMode)filter){
		rwStateCache.texstage[stage].filter = (Texture::FilterMode)filter;
		MetalRaster *natras = stageExt(rwStateCache.texstage[stage].raster);
		if(natras){
			if(natras->filterMode != filter)
				natras->filterMode = filter;
			if(natras->maxAnisotropy != maxAniso)
				natras->maxAnisotropy = maxAniso;
		}
	}
}

static void
setAddressU(uint32 stage, int32 addressing)
{
	if(rwStateCache.texstage[stage].addressingU != (Texture::Addressing)addressing){
		rwStateCache.texstage[stage].addressingU = (Texture::Addressing)addressing;
		MetalRaster *natras = stageExt(rwStateCache.texstage[stage].raster);
		if(natras)
			natras->addressU = addressOnSet(natras->addressU, addressing, gl3Addressing);
	}
}

static void
setAddressV(uint32 stage, int32 addressing)
{
	if(rwStateCache.texstage[stage].addressingV != (Texture::Addressing)addressing){
		rwStateCache.texstage[stage].addressingV = (Texture::Addressing)addressing;
		MetalRaster *natras = stageExt(rwStateCache.texstage[stage].raster);
		if(natras)
			natras->addressV = addressOnSet(natras->addressV, addressing, gl3Addressing);
	}
}

static void
setStageAlpha(uint32 stage, bool32 alpha)
{
	if(stage == 0){
		if(alpha != rwStateCache.textureAlpha){
			rwStateCache.textureAlpha = alpha;
			if(!rwStateCache.vertexAlpha){
				setAlphaBlend(alpha);
				setAlphaTest(alpha);
			}
		}
	}
}

static void
setRasterStageOnly(uint32 stage, Raster *raster)
{
	bool32 alpha;
	if(raster != rwStateCache.texstage[stage].raster){
		rwStateCache.texstage[stage].raster = raster;
		if((int32)stage >= numStagesUsed)
			numStagesUsed = stage+1;
		MetalRaster *natras = stageExt(raster);
		if(raster && natras == nil)
			reportForeignRaster(raster);
		if(natras){
			rwStateCache.texstage[stage].filter = (rw::Texture::FilterMode)natras->filterMode;
			rwStateCache.texstage[stage].addressingU = (rw::Texture::Addressing)natras->addressU;
			rwStateCache.texstage[stage].addressingV = (rw::Texture::Addressing)natras->addressV;

			alpha = natras->hasAlpha;
		}else
			alpha = 0;
		setStageAlpha(stage, alpha);
	}
}

static void
setRasterStage(uint32 stage, Raster *raster)
{
	bool32 alpha;
	if(raster != rwStateCache.texstage[stage].raster){
		rwStateCache.texstage[stage].raster = raster;
		if((int32)stage >= numStagesUsed)
			numStagesUsed = stage+1;
		MetalRaster *natras = stageExt(raster);
		if(raster && natras == nil)
			reportForeignRaster(raster);
		if(natras){
			natras->filterMode = rwStateCache.texstage[stage].filter;
			natras->addressU = rwStateCache.texstage[stage].addressingU;
			natras->addressV = rwStateCache.texstage[stage].addressingV;
			alpha = natras->hasAlpha;
		}else
			alpha = 0;
		setStageAlpha(stage, alpha);
	}
}

Raster*
getStageRaster(int32 stage)
{
	if(stage < 0 || stage >= numStagesUsed)
		return nil;
	return rwStateCache.texstage[stage].raster;
}

void
evictRaster(Raster *raster)
{
	int i;
	for(i = 0; i < MAXNUMSTAGES; i++){
		if(rwStateCache.texstage[i].raster != raster)
			continue;
		setRasterStage(i, nil);
	}
}

void
setTexture(int32 stage, Texture *tex)
{
	if(tex == nil || tex->raster == nil){
		setRasterStage(stage, nil);
		return;
	}
	setRasterStageOnly(stage, tex->raster);
	setFilterMode(stage, tex->getFilter(), tex->getMaxAnisotropy());
	setAddressU(stage, tex->getAddressU());
	setAddressV(stage, tex->getAddressV());
}

void
setRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;
	switch(state){
	case TEXTURERASTER:
		setRasterStage(0, (Raster*)pvalue);
		break;
	case TEXTUREADDRESS:
		setAddressU(0, value);
		setAddressV(0, value);
		break;
	case TEXTUREADDRESSU:
		setAddressU(0, value);
		break;
	case TEXTUREADDRESSV:
		setAddressV(0, value);
		break;
	case TEXTUREFILTER:
		setFilterMode(0, value);
		break;
	case VERTEXALPHA:
		setVertexAlpha(value);
		break;
	case SRCBLEND:
		rwStateCache.srcblend = value;
		break;
	case DESTBLEND:
		rwStateCache.destblend = value;
		break;
	case ZTESTENABLE:
		setDepthTest(value);
		break;
	case ZWRITEENABLE:
		setDepthWrite(value);
		break;
	case FOGENABLE:
		if(rwStateCache.fogEnable != value){
			rwStateCache.fogEnable = value;
			stateDirty = 1;
		}
		break;
	case FOGCOLOR:
		RGBA c;
		c.red = value;
		c.green = value>>8;
		c.blue = value>>16;
		c.alpha = value>>24;
		convColor(&uniformState.fogColor, &c);
		stateDirty = 1;
		break;
	case CULLMODE:
		rwStateCache.cullmode = value;
		break;

	case STENCILENABLE:
		rwStateCache.stencilenable = value;
		break;
	case STENCILFAIL:
		rwStateCache.stencilfail = value;
		break;
	case STENCILZFAIL:
		rwStateCache.stencilzfail = value;
		break;
	case STENCILPASS:
		rwStateCache.stencilpass = value;
		break;
	case STENCILFUNCTION:
		rwStateCache.stencilfunc = value;
		break;
	case STENCILFUNCTIONREF:
		rwStateCache.stencilref = value;
		break;
	case STENCILFUNCTIONMASK:
		rwStateCache.stencilmask = value;
		break;
	case STENCILFUNCTIONWRITEMASK:
		rwStateCache.stencilwritemask = value;
		break;

	case ALPHATESTFUNC:
		setAlphaTestFunction(value);
		break;
	case ALPHATESTREF:
		if(alphaRef != value/255.0f){
			alphaRef = value/255.0f;
			stateDirty = 1;
		}
		break;
	case GSALPHATEST:
		rwStateCache.gsalpha = value;
		break;
	case GSALPHATESTREF:
		rwStateCache.gsalpharef = value;
	}
}

void*
getRenderState(int32 state)
{
	uint32 val;
	RGBA rgba;
	switch(state){
	case TEXTURERASTER:
		return rwStateCache.texstage[0].raster;
	case TEXTUREADDRESS:
		if(rwStateCache.texstage[0].addressingU == rwStateCache.texstage[0].addressingV)
			val = rwStateCache.texstage[0].addressingU;
		else
			val = 0;
		break;
	case TEXTUREADDRESSU:
		val = rwStateCache.texstage[0].addressingU;
		break;
	case TEXTUREADDRESSV:
		val = rwStateCache.texstage[0].addressingV;
		break;
	case TEXTUREFILTER:
		val = rwStateCache.texstage[0].filter;
		break;

	case VERTEXALPHA:
		val = rwStateCache.vertexAlpha;
		break;
	case SRCBLEND:
		val = rwStateCache.srcblend;
		break;
	case DESTBLEND:
		val = rwStateCache.destblend;
		break;
	case ZTESTENABLE:
		val = rwStateCache.ztest;
		break;
	case ZWRITEENABLE:
		val = rwStateCache.zwrite;
		break;
	case FOGENABLE:
		val = rwStateCache.fogEnable;
		break;
	case FOGCOLOR:
		convColor(&rgba, &uniformState.fogColor);
		val = RWRGBAINT(rgba.red, rgba.green, rgba.blue, rgba.alpha);
		break;
	case CULLMODE:
		val = rwStateCache.cullmode;
		break;

	case STENCILENABLE:
		val = rwStateCache.stencilenable;
		break;
	case STENCILFAIL:
		val = rwStateCache.stencilfail;
		break;
	case STENCILZFAIL:
		val = rwStateCache.stencilzfail;
		break;
	case STENCILPASS:
		val = rwStateCache.stencilpass;
		break;
	case STENCILFUNCTION:
		val = rwStateCache.stencilfunc;
		break;
	case STENCILFUNCTIONREF:
		val = rwStateCache.stencilref;
		break;
	case STENCILFUNCTIONMASK:
		val = rwStateCache.stencilmask;
		break;
	case STENCILFUNCTIONWRITEMASK:
		val = rwStateCache.stencilwritemask;
		break;

	case ALPHATESTFUNC:
		val = rwStateCache.alphaFunc;
		break;
	case ALPHATESTREF:
		val = (uint32)(alphaRef*255.0f);
		break;
	case GSALPHATEST:
		val = rwStateCache.gsalpha;
		break;
	case GSALPHATESTREF:
		val = rwStateCache.gsalpharef;
		break;
	default:
		val = 0;
	}
	return (void*)(uintptr)val;
}

static void
resetRenderState(void)
{
	int i;

	rwStateCache.alphaFunc = ALPHAGREATEREQUAL;
	alphaFunc = 0;
	alphaRef = 10.0f/255.0f;
	uniformState.fogDisable = 1.0f;
	uniformState.fogStart = 0.0f;
	uniformState.fogEnd = 0.0f;
	uniformState.fogRange = 0.0f;
	uniformState.fogColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	rwStateCache.gsalpha = 0;
	rwStateCache.gsalpharef = 128;
	stateDirty = 1;

	rwStateCache.vertexAlpha = 0;
	rwStateCache.textureAlpha = 0;
	rwStateCache.alphaTestEnable = 0;

	rwStateCache.blendEnable = 0;
	rwStateCache.srcblend = BLENDSRCALPHA;
	rwStateCache.destblend = BLENDINVSRCALPHA;

	rwStateCache.zwrite = 1;
	rwStateCache.ztest = 1;

	rwStateCache.cullmode = CULLNONE;

	rwStateCache.stencilenable = 0;
	rwStateCache.stencilfail = STENCILKEEP;
	rwStateCache.stencilzfail = STENCILKEEP;
	rwStateCache.stencilpass = STENCILKEEP;
	rwStateCache.stencilfunc = STENCILALWAYS;
	rwStateCache.stencilref = 0;
	rwStateCache.stencilmask = 0xFFFFFFFF;
	rwStateCache.stencilwritemask = 0xFFFFFFFF;

	for(i = 0; i < MAXNUMSTAGES; i++)
		rwStateCache.texstage[i].raster = nil;
	numStagesUsed = 1;
	for(i = 0; i < NUMBLOCKS; i++)
		blockDirty[i] = true;
}

void
setWorldMatrix(Matrix *mat, const void *object)
{
	RawMatrix world;

	currentObject = object;
	convMatrix(&world, mat);
	if(memcmp(&uniformObject.world, &world, sizeof(world)) != 0){
		uniformObject.world = world;
		blockDirty[BLOCK_OBJECT] = true;
	}
}

int32
setLights(WorldLights *lightData)
{
	int i, n;
	Light *l;
	int32 bits;
	UniformLights lights = uniformLights;

	lights.ambLight = lightData->ambient;

	bits = 0;

	if(lightData->numAmbients)
		bits |= VSLIGHT_AMBIENT;

	n = 0;
	for(i = 0; i < lightData->numDirectionals && i < 8; i++){
		l = lightData->directionals[i];
		lights.lightParams[n].type = 1.0f;
		lights.lightColor[n] = l->color;
		memcpy(&lights.lightDirection[n], &l->getFrame()->getLTM()->at, sizeof(V3d));
		bits |= VSLIGHT_DIRECT;
		n++;
		if(n >= MAX_LIGHTS)
			goto out;
	}

	for(i = 0; i < lightData->numLocals; i++){
		Light *l = lightData->locals[i];

		switch(l->getType()){
		case Light::POINT:
			lights.lightParams[n].type = 2.0f;
			lights.lightParams[n].radius = l->radius;
			lights.lightColor[n] = l->color;
			memcpy(&lights.lightPosition[n], &l->getFrame()->getLTM()->pos, sizeof(V3d));
			bits |= VSLIGHT_POINT;
			n++;
			if(n >= MAX_LIGHTS)
				goto out;
			break;
		case Light::SPOT:
		case Light::SOFTSPOT:
			lights.lightParams[n].type = 3.0f;
			lights.lightParams[n].minusCosAngle = l->minusCosAngle;
			lights.lightParams[n].radius = l->radius;
			lights.lightColor[n] = l->color;
			memcpy(&lights.lightPosition[n], &l->getFrame()->getLTM()->pos, sizeof(V3d));
			memcpy(&lights.lightDirection[n], &l->getFrame()->getLTM()->at, sizeof(V3d));
			if(l->getType() == Light::SOFTSPOT)
				lights.lightParams[n].hardSpot = 0.0f;
			else
				lights.lightParams[n].hardSpot = 1.0f;
			bits |= VSLIGHT_SPOT;
			n++;
			if(n >= MAX_LIGHTS)
				goto out;
			break;
		}
	}

	lights.lightParams[n].type = 0.0f;
out:
	if(memcmp(&uniformLights, &lights, sizeof(lights)) != 0){
		uniformLights = lights;
		blockDirty[BLOCK_LIGHTS] = true;
	}
	return bits;
}

void
setProjectionMatrix(float32 *mat)
{
	memcpy(projBase, mat, 64);
	memcpy(&uniformScene.proj, mat, 64);
	blockDirty[BLOCK_SCENE] = true;
	comparePrepassView();
}

void
setViewMatrix(float32 *mat)
{
	memcpy(&uniformScene.view, mat, 64);
	blockDirty[BLOCK_SCENE] = true;
	comparePrepassView();
}

void
setIm2DXform(const float32 *xform)
{
	if(memcmp(uniformScene.xform, xform, sizeof(uniformScene.xform)) != 0){
		memcpy(uniformScene.xform, xform, sizeof(uniformScene.xform));
		blockDirty[BLOCK_SCENE] = true;
	}
}

void
setFogPlanes(float32 fogStart, float32 fogEnd)
{
	if(rwStateCache.fogStart != fogStart){
		rwStateCache.fogStart = fogStart;
		stateDirty = 1;
	}
	if(rwStateCache.fogEnd != fogEnd){
		rwStateCache.fogEnd = fogEnd;
		stateDirty = 1;
	}
}

void
setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp)
{
	UniformMaterial mat;

	convColor(&mat.matColor, &color);
	mat.surfProps[0] = surfaceprops.ambient;
	mat.surfProps[1] = surfaceprops.specular;
	mat.surfProps[2] = surfaceprops.diffuse;
	mat.surfProps[3] = extraSurfProp;
	if(memcmp(&uniformMaterial, &mat, sizeof(mat)) != 0){
		uniformMaterial = mat;
		blockDirty[BLOCK_MATERIAL] = true;
	}
}

void
setMatFXConstants(const RawMatrix *texMatrix, const float32 *fxParams, const RGBAf *colorClamp, const RGBAf *envColor)
{
	UniformMatFX fx;

	fx.texMatrix = *texMatrix;
	memcpy(fx.fxParams, fxParams, sizeof(fx.fxParams));
	fx.colorClamp = *colorClamp;
	fx.envColor = *envColor;
	if(memcmp(&uniformMatFX, &fx, sizeof(fx)) != 0){
		uniformMatFX = fx;
		blockDirty[BLOCK_MATFX] = true;
	}
}

void
setCustomConstants(const void *data, uint32 size)
{
	if(size > MAXCUSTOMCONSTANTS)
		size = MAXCUSTOMCONSTANTS;
	if(size)
		memcpy(customConstants, data, size);
	blockInfo[BLOCK_CUSTOM].size = size;
	blockDirty[BLOCK_CUSTOM] = true;
}

static void
updateStateBlock(void)
{
	float32 range[2];

	if(!stateDirty)
		return;
	alphaTestRange(alphaFunc, alphaRef, range);
	uniformState.alphaRefLow = range[0];
	uniformState.alphaRefHigh = range[1];
	uniformState.fogDisable = rwStateCache.fogEnable ? 0.0f : 1.0f;
	uniformState.fogStart = rwStateCache.fogStart;
	uniformState.fogEnd = rwStateCache.fogEnd;
	uniformState.fogRange = 1.0f/(rwStateCache.fogStart - rwStateCache.fogEnd);
	blockDirty[BLOCK_STATE] = true;
	stateDirty = 0;
}

static bool
bindBlocks(id<MTLRenderCommandEncoder> e, const PipelineEntry *pipe)
{
	RingSpace space;
	id<MTLBuffer> buf;
	uint32 off, size;
	int i;

	for(i = 0; i < NUMBLOCKS; i++){
		if(((pipe->vertexBlocks | pipe->fragmentBlocks) & (1<<i)) == 0)
			continue;
		size = blockInfo[i].size;
		if(pipe->blockSizes[i] > size)
			size = pipe->blockSizes[i];
		if(blockDirty[i] || blockBuffer[i] == nil || blockBufferSize[i] < size){
			if(!ringAlloc(size, 256, &space))
				return false;
			memcpy(space.cpu, blockInfo[i].data, blockInfo[i].size);
			memset(space.cpu + blockInfo[i].size, 0, size - blockInfo[i].size);
			blockBuffer[i] = (__bridge id<MTLBuffer>)space.buffer;
			blockOffset[i] = space.offset;
			blockBufferSize[i] = size;
			blockDirty[i] = false;
			stats.blockUploads[blockInfo[i].index]++;
		}
		if(i == BLOCK_CUSTOM)
			stats.customBlockBinds++;
		buf = blockBuffer[i];
		off = blockOffset[i];
		if(pipe->vertexBlocks & (1<<i)){
			stats.vertexBlockBinds[blockInfo[i].index]++;
			if(enc.vertexBuffers[i] == buf){
				if(enc.vertexOffsets[i] != off)
					[e setVertexBufferOffset:off atIndex:blockInfo[i].index];
			}else
				[e setVertexBuffer:buf offset:off atIndex:blockInfo[i].index];
			enc.vertexBuffers[i] = buf;
			enc.vertexOffsets[i] = off;
		}
		if(pipe->fragmentBlocks & (1<<i)){
			stats.fragmentBlockBinds[blockInfo[i].index]++;
			if(enc.fragmentBuffers[i] == buf){
				if(enc.fragmentOffsets[i] != off)
					[e setFragmentBufferOffset:off atIndex:blockInfo[i].index];
			}else
				[e setFragmentBuffer:buf offset:off atIndex:blockInfo[i].index];
			enc.fragmentBuffers[i] = buf;
			enc.fragmentOffsets[i] = off;
		}
	}
	return true;
}

static void
bindTextures(id<MTLRenderCommandEncoder> e, const PipelineEntry *pipe)
{
	id<MTLTexture> tex;
	id<MTLSamplerState> smp;
	MetalRaster *natras;
	SamplerDesc sd;
	Raster *raster;
	int i;

	for(i = 0; i < MAXNUMSTAGES; i++){
		if((pipe->textureStages & 1<<i) == 0)
			continue;
		stats.textureStageBinds++;
		raster = rwStateCache.texstage[i].raster;
		natras = stageExt(raster);
		tex = natras ? (__bridge id<MTLTexture>)getRasterSampleTexture(raster) : nil;
		if(tex){
			GETMETALRASTEREXT(raster->parent)->lastUseFrame = getFrameId();
			sd.filter = natras->filterMode;
			sd.addressU = natras->addressU;
			sd.addressV = natras->addressV;
			sd.maxAnisotropy = natras->maxAnisotropy;
			sd.hasMips = natras->autogenMipmap || natras->numLevels > 1;
			sd.maxLevel = natras->filledLevels > 1 ? natras->filledLevels-1 : 0;
		}else{
			tex = (__bridge id<MTLTexture>)getWhiteTexture();
			sd.filter = Texture::NEAREST;
			sd.addressU = Texture::WRAP;
			sd.addressV = Texture::WRAP;
			sd.maxAnisotropy = 1;
			sd.hasMips = false;
			sd.maxLevel = 0;
		}
		smp = getSampler(sd);
		if(enc.textures[i] != tex){
			[e setFragmentTexture:tex atIndex:i];
			enc.textures[i] = tex;
		}
		if(enc.samplers[i] != smp){
			[e setFragmentSamplerState:smp atIndex:i];
			enc.samplers[i] = smp;
		}
	}
}

static bool32
encodeDraw(MetalContext *ctx, id<MTLRenderCommandEncoder> e, PipelineEntry *pipe, bool depthEqual)
{
	DepthStencilDesc dd;
	DepthStencilResolved dr;
	id<MTLDepthStencilState> ds;
	uint64 key;
	int32 cull;

	if(enc.pipeline != pipe->state){
		[e setRenderPipelineState:pipe->state];
		enc.pipeline = pipe->state;
	}
	if(pipe->defaultAttribs && !enc.defaultAttribs){
		[e setVertexBytes:&defaultAttribs length:sizeof(defaultAttribs) atIndex:BUFFER_DEFAULTATTRIBS];
		enc.defaultAttribs = true;
	}
	if(pipe->emissive && enc.emissive != drawEmissive){
		[e setFragmentBytes:&drawEmissive length:sizeof(drawEmissive) atIndex:BUFFER_EMISSIVE];
		enc.emissive = drawEmissive;
	}

	dd.hasDepth = ctx->encoderHasDepth;
	dd.ztest = rwStateCache.ztest;
	dd.zwrite = rwStateCache.zwrite;
	dd.stencilEnable = rwStateCache.stencilenable;
	dd.stencilFunc = rwStateCache.stencilfunc;
	dd.stencilFail = rwStateCache.stencilfail;
	dd.stencilZFail = rwStateCache.stencilzfail;
	dd.stencilPass = rwStateCache.stencilpass;
	dd.stencilMask = rwStateCache.stencilmask;
	dd.stencilWriteMask = rwStateCache.stencilwritemask;
	dd.depthEqual = depthEqual;
	dr = resolveDepthStencil(dd);
	key = depthStencilKey(dd);
	if(key == lastDepthKey && lastDepth)
		ds = lastDepth;
	else{
		ds = getDepthStencil(key, dr);
		lastDepthKey = key;
		lastDepth = ds;
	}
	if(enc.depthStencil != ds){
		[e setDepthStencilState:ds];
		enc.depthStencil = ds;
	}
	if(dr.stencilEnable && enc.stencilRef != (int32)(rwStateCache.stencilref & 0xFF)){
		enc.stencilRef = rwStateCache.stencilref & 0xFF;
		[e setStencilReferenceValue:enc.stencilRef];
	}

	if(!enc.winding){
		[e setFrontFacingWinding:MTLWindingCounterClockwise];
		enc.winding = true;
	}
	if(!enc.scissorKnown){
		MTLScissorRect full = { 0, 0, ctx->encoderWidth, ctx->encoderHeight };
		[e setScissorRect:full];
		enc.scissorKnown = true;
	}
	cull = cullMode(rwStateCache.cullmode);
	if(enc.cull != cull){
		[e setCullMode:(MTLCullMode)cull];
		enc.cull = cull;
	}
	applyViewport(ctx);

	bindTextures(e, pipe);
	updateStateBlock();
	if(!bindBlocks(e, pipe)){
		countDroppedDraw(DROP_NORINGSPACE, currentShader);
		return 0;
	}
	stats.draws++;
	return 1;
}

static RawMatrix
previousWorld(const void *object, const RawMatrix &current)
{
	if(object == nil)
		return current;
	RawMatrix *cur = worldsCur.put(object);
	if(cur)
		*cur = current;
	const RawMatrix *prev = worldsPrev.find(object);
	return prev ? *prev : current;
}

static bool
bindPreviousBones(id<MTLRenderCommandEncoder> e, const void *object)
{
	RingSpace space;
	if(!ringAlloc(sizeof(uniformSkin), 256, &space))
		return false;
	const UniformSkin *src = &uniformSkin;
	if(object){
		UniformSkin *cur = bonesCur.put(object);
		if(cur)
			*cur = uniformSkin;
		const UniformSkin *prev = bonesPrev.find(object);
		if(prev)
			src = prev;
	}
	memcpy(space.cpu, src, sizeof(uniformSkin));
	[e setVertexBuffer:(__bridge id<MTLBuffer>)space.buffer offset:space.offset atIndex:BUFFER_PREVSKIN];
	return true;
}

static bool32
flushPrepass(MetalContext *ctx, id<MTLRenderCommandEncoder> e, uint64 key)
{
	bool skinned = layoutHasAttrib(currentLayout, ATTRIB_WEIGHTS);
	Shader *sh = skinned ? prepassSkinShader : prepassShader;
	PipelineDesc pd = {};
	pd.shader = sh->shaderId;
	pd.variant = 0;
	pd.vertexLayout = currentLayout;
	pd.blendEnable = false;
	pd.writeMask = MTLColorWriteMaskAll;
	pd.colorFormat = COLORFMT_PREPASS;
	pd.depthFormat = DEPTHFMT_D32S8;
	pd.sampleCount = ctx->encoderSamples;
	uint64 pkey = pipelineKey(pd);
	PipelineEntry *pipe = pkey == lastPipeKey && lastPipe ? lastPipe : getPipeline(sh, pd, pkey);
	lastPipeKey = pkey;
	lastPipe = pipe;
	if(pipe == nil){
		stats.prepassSkipped++;
		return 0;
	}
	if(!prepassFrameBound){
		RingSpace space;
		if(!ringAlloc(sizeof(prepassFrameBlock), 256, &space)){
			countDroppedDraw(DROP_NORINGSPACE, currentShader);
			return 0;
		}
		memcpy(space.cpu, &prepassFrameBlock, sizeof(prepassFrameBlock));
		[e setVertexBuffer:(__bridge id<MTLBuffer>)space.buffer offset:space.offset atIndex:BUFFER_PREPASSFRAME];
		prepassFrameBound = true;
	}
	RawMatrix prevWorld = previousWorld(currentObject, uniformObject.world);
	[e setVertexBytes:&prevWorld length:sizeof(prevWorld) atIndex:BUFFER_PREPASSOBJECT];
	if(skinned && !bindPreviousBones(e, currentObject)){
		countDroppedDraw(DROP_NORINGSPACE, currentShader);
		return 0;
	}
	if(!encodeDraw(ctx, e, pipe, false))
		return 0;
	if(!prepassKeys.insert(key))
		stats.prepassUnkeyed++;
	prepassDrawsThisFrame++;
	stats.prepassDraws++;
	return 1;
}

bool32
flushCache(void)
{
	MetalContext *ctx = getContext();
	id<MTLRenderCommandEncoder> e;
	PipelineEntry *pipe;
	PipelineDesc pd;
	uint64 key;

	if(ctx == nil || ctx->encoder == nil || currentShader == nil){
		countDroppedDraw(currentShader ? DROP_NOENCODER : DROP_NOSHADER, currentShader);
		return 0;
	}
	syncEncoder(ctx);
	e = ctx->encoder;

	bool prepassPass = ctx->encoderColorFormat == COLORFMT_PREPASS;
	bool keyed = false;
	uint64 drawKey = 0;
	if(prepassViewSame && (prepassPass || (prepassFrame && ctx->encoderColorFormat == COLORFMT_SCENE))){
		PrepassDraw pdraw = { rwStateCache.blendEnable != 0, rwStateCache.ztest != 0, rwStateCache.zwrite != 0,
			ctx->encoderHasDepth, currentDrawInst != nil };
		if(prepassEligible(pdraw)){
			bool skinned = layoutHasAttrib(currentLayout, ATTRIB_WEIGHTS);
			drawKey = prepassKey(currentDrawInst, (const float*)&uniformObject.world,
				skinned ? (const float*)&uniformSkin.bones[0] : nil);
			keyed = true;
		}
	}
	if(prepassPass)
		return keyed ? flushPrepass(ctx, e, drawKey) : (stats.prepassSkipped++, 0);

	pd.shader = currentShader->shaderId;
	pd.variant = currentVariant;
	pd.vertexLayout = currentLayout;
	pd.blendEnable = rwStateCache.blendEnable;
	pd.srcBlend = rwStateCache.srcblend;
	pd.destBlend = rwStateCache.destblend;
	pd.writeMask = MTLColorWriteMaskAll;
	pd.colorFormat = ctx->encoderColorFormat;
	pd.depthFormat = ctx->encoderHasDepth ? DEPTHFMT_D32S8 : DEPTHFMT_NONE;
	pd.sampleCount = ctx->encoderSamples;
	key = pipelineKey(pd);
	if(key == lastPipeKey && lastPipe)
		pipe = lastPipe;
	else{
		pipe = getPipeline(currentShader, pd, key);
		lastPipeKey = key;
		lastPipe = pipe;
	}
	if(pipe == nil){
		countDroppedDraw(DROP_NOPIPELINE, currentShader);
		return 0;
	}
	bool depthEqual = keyed && prepassKeys.markUsed(drawKey);
	if(depthEqual)
		stats.prepassEqual++;
	if(forceEqualNext && ctx->encoderColorFormat == COLORFMT_SCENE){
		forceEqualNext = false;
		depthEqual = true;
	}
	return encodeDraw(ctx, e, pipe, depthEqual);
}

static uint32
checkBlockSizes(MetalContext *ctx, Shader *shader, const PipelineDesc &d)
{
	MTLRenderPipelineDescriptor *pd;
	MTLRenderPipelineReflection *refl = nil;
	PipelineEntry e = { nil, false, 0, 0 };
	uint32 used = 0;
	int i;

	pd = makePipelineDescriptor(shader, d, &e.defaultAttribs, &e.shaderBlend);
	if(pd == nil)
		return 0;
	[ctx->device newRenderPipelineStateWithDescriptor:pd
		options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
		reflection:&refl error:nil];
	if(refl == nil)
		return 0;
	reflectBlocks(refl, &e);
	for(i = 0; i < NUMBLOCKS; i++)
		if((e.vertexBlocks | e.fragmentBlocks) & (1<<i))
			used |= 1<<blockInfo[i].index;
	return used;
}

uint32
checkShaderBlockSizes(Shader *shader, uint32 variant)
{
	MetalContext *ctx = getContext();
	PipelineDesc d;
	uint32 used;

	if(ctx == nil || shader == nil)
		return 0;
	d.shader = shader->shaderId;
	d.variant = variant;
	d.vertexLayout = 0;
	d.blendEnable = 0;
	d.srcBlend = 0;
	d.destBlend = 0;
	d.writeMask = MTLColorWriteMaskAll;
	d.colorFormat = COLORFMT_RGBA8;
	d.depthFormat = DEPTHFMT_NONE;
	d.sampleCount = 1;
	@autoreleasepool {
		used = checkBlockSizes(ctx, shader, d);
	}
	return used;
}

enum
{
	BLEND_OFF,
	BLEND_ALPHA,
	BLEND_ALPHAADD,
	BLEND_ADD,
	BLEND_DEPTHONLY,
	BLEND_DARKEN,
	BLEND_REPLACE,
	BLEND_INVERT,
	BLEND_PREMUL,
	BLEND_MODULATE,
	NUMPREWARMBLENDS
};

static const uint32 prewarmBlends[NUMPREWARMBLENDS][2] = {
	{ 0, 0 },
	{ BLENDSRCALPHA, BLENDINVSRCALPHA },
	{ BLENDSRCALPHA, BLENDONE },
	{ BLENDONE, BLENDONE },
	{ BLENDZERO, BLENDONE },
	{ BLENDZERO, BLENDINVSRCCOLOR },
	{ BLENDONE, BLENDZERO },
	{ BLENDINVDESTCOLOR, BLENDZERO },
	{ BLENDONE, BLENDINVSRCALPHA },
	{ BLENDZERO, BLENDSRCCOLOR },
};

struct PrewarmState
{
	uint32 variant;
	int32 blend;
};

struct WorldLayout
{
	bool normals;
	bool prelit;
	int32 numTexCoords;
	const PrewarmState *states;
	int32 numStates;
};

struct EnvLayout
{
	bool normals;
	bool prelit;
	int32 numStates;
};

static const int32 im2dBlends[] = { BLEND_OFF, BLEND_ALPHA, BLEND_ALPHAADD, BLEND_ADD, BLEND_DEPTHONLY };
static const int32 im2dTargetBlends[] = { BLEND_INVERT, BLEND_REPLACE, BLEND_MODULATE };
static const PrewarmState prelitStates[] = {
	{ 0, BLEND_OFF },
	{ VARIANT_ALPHATEST, BLEND_ALPHA },
	{ VARIANT_ALPHATEST, BLEND_ALPHAADD },
};
static const PrewarmState litStates[] = {
	{ 0, BLEND_OFF },
	{ VARIANT_ALPHATEST, BLEND_ALPHA },
	{ VARIANT_DIRECTIONALS, BLEND_OFF },
	{ VARIANT_DIRECTIONALS | VARIANT_ALPHATEST, BLEND_ALPHA },
};
static const PrewarmState litPrelitStates[] = {
	{ 0, BLEND_OFF },
	{ VARIANT_ALPHATEST, BLEND_ALPHA },
	{ VARIANT_ALPHATEST, BLEND_REPLACE },
};
static const PrewarmState unlitStates[] = {
	{ 0, BLEND_OFF },
	{ VARIANT_ALPHATEST, BLEND_ALPHA },
};
static const WorldLayout worldLayouts[] = {
	{ false, true, 1, prelitStates, nelem(prelitStates) },
	{ true, false, 1, litStates, nelem(litStates) },
	{ true, true, 1, litPrelitStates, nelem(litPrelitStates) },
	{ false, true, 0, unlitStates, nelem(unlitStates) },
	{ true, false, 0, litStates, nelem(litStates) },
};
static const PrewarmState envStates[] = {
	{ VARIANT_ALPHATEST, BLEND_PREMUL },
	{ VARIANT_DIRECTIONALS | VARIANT_ALPHATEST, BLEND_PREMUL },
};
static const EnvLayout envLayouts[] = {
	{ true, false, 2 },
	{ false, true, 2 },
	{ true, true, 1 },
};
static const PrewarmState im3dStates[] = {
	{ 0, BLEND_OFF },
	{ VARIANT_ALPHATEST, BLEND_ALPHA },
	{ VARIANT_ALPHATEST, BLEND_ADD },
	{ VARIANT_ALPHATEST, BLEND_DEPTHONLY },
	{ VARIANT_ALPHATEST, BLEND_DARKEN },
	{ VARIANT_ALPHATEST, BLEND_REPLACE },
};
static const WorldLayout uv2WorldLayouts[] = {
	{ false, true, 2, prelitStates, nelem(prelitStates) },
};

static void
prewarm(Shader *shader, uint32 layout, uint32 variant, int32 blend, int32 depthFormat, uint32 samples, uint32 colorFormat)
{
	PipelineDesc d;

	d.shader = shader->shaderId;
	d.variant = variant & shader->variantMask;
	d.vertexLayout = layout;
	d.blendEnable = blend != BLEND_OFF;
	d.srcBlend = prewarmBlends[blend][0];
	d.destBlend = prewarmBlends[blend][1];
	d.writeMask = MTLColorWriteMaskAll;
	d.colorFormat = colorFormat;
	d.depthFormat = depthFormat;
	d.sampleCount = samples;
	getPipeline(shader, d, pipelineKey(d));
}

static void
prewarmEngineLayouts(uint32 samples, uint32 fmt)
{
	AttribDesc descs[MAXVERTEXATTRIBS];
	uint32 layout;
	int32 i, j, n, depth;

	if(fmt == COLORFMT_OVERLAY){
		for(depth = 0; depth < 2; depth++)
			for(i = 0; i < (int32)nelem(im2dBlends); i++)
				prewarm(im2dShader, im2dVertexLayout, VARIANT_ALPHATEST, im2dBlends[i],
					depth ? DEPTHFMT_D32S8 : DEPTHFMT_NONE, samples, fmt);
		return;
	}
	for(depth = 0; depth < 2; depth++)
		for(i = 0; i < (int32)nelem(im2dBlends); i++)
			prewarm(im2dShader, im2dVertexLayout, VARIANT_ALPHATEST, im2dBlends[i],
				depth ? DEPTHFMT_D32S8 : DEPTHFMT_NONE, samples, fmt);
	if(fmt == COLORFMT_SCENE && im2dEmitShader)
		for(depth = 0; depth < 2; depth++)
			for(i = 0; i < (int32)nelem(im2dBlends); i++)
				prewarm(im2dEmitShader, im2dVertexLayout, VARIANT_ALPHATEST, im2dBlends[i],
					depth ? DEPTHFMT_D32S8 : DEPTHFMT_NONE, samples, fmt);
	for(i = 0; i < (int32)nelem(im2dTargetBlends); i++)
		prewarm(im2dShader, im2dVertexLayout, VARIANT_ALPHATEST, im2dTargetBlends[i], DEPTHFMT_D32S8, samples, fmt);
	for(i = 0; i < (int32)nelem(worldLayouts); i++){
		const WorldLayout &w = worldLayouts[i];
		n = defaultVertexAttribs(w.normals, w.prelit, w.numTexCoords, descs);
		layout = registerVertexLayout(descs, n);
		for(j = 0; j < w.numStates; j++)
			prewarm(defaultShader, layout, w.states[j].variant, w.states[j].blend, DEPTHFMT_D32S8, samples, fmt);
	}
	if(skinShader)
		for(i = 0; i < 2; i++){
			n = skinVertexAttribs(true, i == 0, 1, descs);
			layout = registerVertexLayout(descs, n);
			for(j = 0; j < (int32)nelem(litStates); j++)
				prewarm(skinShader, layout, litStates[j].variant, litStates[j].blend, DEPTHFMT_D32S8, samples, fmt);
		}
	if(matfxEnvShader)
		for(i = 0; i < (int32)nelem(envLayouts); i++){
			n = defaultVertexAttribs(envLayouts[i].normals, envLayouts[i].prelit, 1, descs);
			layout = registerVertexLayout(descs, n);
			for(j = 0; j < envLayouts[i].numStates; j++)
				prewarm(matfxEnvShader, layout, envStates[j].variant, envStates[j].blend, DEPTHFMT_D32S8, samples, fmt);
		}
	if(im3dShader)
		for(i = 0; i < (int32)nelem(im3dStates); i++)
			prewarm(im3dShader, im3dVertexLayout, im3dStates[i].variant, im3dStates[i].blend, DEPTHFMT_D32S8, samples, fmt);
}

static void
prewarmUV2Layouts(uint32 samples, uint32 fmt)
{
	AttribDesc descs[MAXVERTEXATTRIBS];
	uint32 layout;
	int32 i, j, n;

	for(i = 0; i < (int32)nelem(uv2WorldLayouts); i++){
		const WorldLayout &w = uv2WorldLayouts[i];
		n = defaultVertexAttribs(w.normals, w.prelit, w.numTexCoords, descs);
		layout = registerVertexLayout(descs, n);
		for(j = 0; j < w.numStates; j++)
			prewarm(defaultShader, layout, w.states[j].variant, w.states[j].blend, DEPTHFMT_D32S8, samples, fmt);
	}
}

static void
prewarmPrepassLayout(uint32 layout, uint32 samples)
{
	prewarm(layoutHasAttrib(layout, ATTRIB_WEIGHTS) ? prepassSkinShader : prepassShader, layout, 0, BLEND_OFF, DEPTHFMT_D32S8,
		samples, COLORFMT_PREPASS);
}

static void
prewarmPrepass(uint32 samples)
{
	AttribDesc descs[MAXVERTEXATTRIBS];
	int32 i, n;

	for(i = 0; i < (int32)nelem(worldLayouts); i++){
		n = defaultVertexAttribs(worldLayouts[i].normals, worldLayouts[i].prelit, worldLayouts[i].numTexCoords, descs);
		prewarmPrepassLayout(registerVertexLayout(descs, n), samples);
	}
	for(i = 0; i < (int32)nelem(uv2WorldLayouts); i++){
		n = defaultVertexAttribs(uv2WorldLayouts[i].normals, uv2WorldLayouts[i].prelit, uv2WorldLayouts[i].numTexCoords, descs);
		prewarmPrepassLayout(registerVertexLayout(descs, n), samples);
	}
	for(i = 0; i < 2; i++){
		n = skinVertexAttribs(true, i == 0, 1, descs);
		prewarmPrepassLayout(registerVertexLayout(descs, n), samples);
	}
}

static bool32
hostPrewarmPrepass(const HostPrewarmRecord &r)
{
	uint32 counts[2];
	int32 i, n = prewarmSampleCounts(metalGlobals.numSamples, counts);
	bool32 ok = 1;

	if(r.blend || !r.depth || im2DLayout(r.layout) || !prepassShadersReady())
		return 1;
	for(i = 0; i < n; i++){
		Shader *sh = layoutHasAttrib(r.layout, ATTRIB_WEIGHTS) ? prepassSkinShader : prepassShader;
		PipelineDesc d = {};
		d.shader = sh->shaderId;
		d.vertexLayout = r.layout;
		d.writeMask = MTLColorWriteMaskAll;
		d.colorFormat = COLORFMT_PREPASS;
		d.depthFormat = DEPTHFMT_D32S8;
		d.sampleCount = counts[i];
		if(getPipeline(sh, d, pipelineKey(d)) == nil)
			ok = 0;
	}
	return ok;
}

static void
prewarmFormatSet(uint32 mask)
{
	uint32 counts[2];
	int32 i, numCounts = prewarmSampleCounts(metalGlobals.numSamples, counts);
	uint32 fmt;

	for(fmt = COLORFMT_RGBA8; fmt <= COLORFMT_OVERLAY; fmt++){
		if((mask & (1u << fmt)) == 0)
			continue;
		for(i = 0; i < numCounts; i++)
			prewarmEngineLayouts(counts[i], fmt);
		openIm2DUV2();
		if(fmt != COLORFMT_OVERLAY)
			for(i = 0; i < numCounts; i++)
				prewarmUV2Layouts(counts[i], fmt);
	}
	if((mask & (1u << COLORFMT_PREPASS)) && prepassShadersReady())
		for(i = 0; i < numCounts; i++)
			prewarmPrepass(counts[i]);
}

void
prewarmPipelines(void)
{
	uint32 mask = activeFormatMask();

	if(getContext() == nil || im2dShader == nil || defaultShader == nil)
		return;
	auto start = std::chrono::steady_clock::now();
	uint32 before = stats.pipelinesAtInit;
	prewarming = true;
	@autoreleasepool {
		prewarmFormatSet(mask);
	}
	prewarming = false;
	warmedFormats = mask;
	snprintf(prewarmLine, sizeof(prewarmLine), "rw::metal: prewarm %u pipelines in %.1f ms\n", stats.pipelinesAtInit - before,
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	fprintf(stderr, "%s", prewarmLine);
}

static bool32
hostPrewarmFormat(const HostPrewarmRecord &r, uint32 fmt)
{
	PipelineDesc d;
	uint32 counts[2];
	int32 i, n = prewarmSampleCounts(metalGlobals.numSamples, counts);
	bool32 ok = 1;

	if(fmt == COLORFMT_OVERLAY && !im2DLayout(r.layout))
		return 1;
	if(fmt == COLORFMT_PREPASS)
		return hostPrewarmPrepass(r);
	d.shader = r.shader->shaderId;
	d.variant = r.variant & r.shader->variantMask;
	d.vertexLayout = r.layout;
	d.blendEnable = r.blend != 0;
	d.srcBlend = r.srcBlend;
	d.destBlend = r.destBlend;
	d.writeMask = MTLColorWriteMaskAll;
	d.colorFormat = fmt;
	d.depthFormat = r.depth ? DEPTHFMT_D32S8 : DEPTHFMT_NONE;
	for(i = 0; i < n; i++){
		d.sampleCount = counts[i];
		if(getPipeline(r.shader, d, pipelineKey(d)) == nil)
			ok = 0;
	}
	return ok;
}

void
prewarmFormats(uint32 mask)
{
	uint32 fresh = mask & ~warmedFormats;
	uint32 fmt;
	bool missing = false;

	for(const HostPrewarmRecord &r : hostRecords)
		if(mask & ~r.formats)
			missing = true;
	if((fresh == 0 && !missing) || getContext() == nil || im2dShader == nil)
		return;
	auto start = std::chrono::steady_clock::now();
	uint32 before = stats.pipelinesAtInit;
	stats.formatPrewarms++;
	prewarming = true;
	@autoreleasepool {
		prewarmFormatSet(fresh);
		for(HostPrewarmRecord &r : hostRecords){
			for(fmt = COLORFMT_RGBA8; fmt <= COLORFMT_PREPASS; fmt++)
				if(mask & ~r.formats & (1u << fmt))
					hostPrewarmFormat(r, fmt);
			r.formats |= mask;
		}
	}
	prewarming = false;
	warmedFormats |= fresh;
	fprintf(stderr, "rw::metal: prewarm %u pipelines in %.1f ms for the %s tier\n", stats.pipelinesAtInit - before,
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
		tierName(getFrameInfo().tier));
}

static bool32
hostPrewarm(Shader *shader, uint32 layout, uint32 variant, bool32 blend, int32 srcBlend, int32 destBlend, bool32 depth)
{
	uint32 mask = activeFormatMask();
	HostPrewarmRecord r = { shader, layout, variant, blend, srcBlend, destBlend, depth, mask };
	uint32 fmt;
	bool32 ok = 1;

	hostRecords.push_back(r);
	auto start = std::chrono::steady_clock::now();
	hostPrewarming = true;
	@autoreleasepool {
		for(fmt = COLORFMT_RGBA8; fmt <= COLORFMT_PREPASS; fmt++)
			if(mask & (1u << fmt))
				if(!hostPrewarmFormat(r, fmt))
					ok = 0;
	}
	hostPrewarming = false;
	stats.hostPrewarmUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	return ok;
}

bool32
prewarmIm2DShader(Shader *shader, bool32 uv2, bool32 blend, int32 srcBlend, int32 destBlend, bool32 depth)
{
	uint32 layout = uv2 ? im2dUV2VertexLayout : im2dVertexLayout;
	if(getContext() == nil || shader == nil || layout == 0)
		return 0;
	return hostPrewarm(shader, layout, VARIANT_ALPHATEST, blend, srcBlend, destBlend, depth);
}

bool32
prewarmShader(Shader *shader, const AttribDesc *attribs, int32 numAttribs, uint32 variant,
              bool32 blend, int32 srcBlend, int32 destBlend, bool32 depth)
{
	uint32 layout;
	if(getContext() == nil || shader == nil || attribs == nil || numAttribs <= 0)
		return 0;
	for(int32 i = 0; i < numAttribs; i++)
		if(attribs[i].format < ATTRIBFMT_FLOAT2 || attribs[i].format > ATTRIBFMT_UCHAR4_NORM || attribs[i].index >= 31){
			RWERROR((ERR_GENERAL, "vertex attribute format or index out of range"));
			return 0;
		}
	layout = registerVertexLayout(attribs, numAttribs);
	if(layout == 0)
		return 0;
	return hostPrewarm(shader, layout, variant, blend, srcBlend, destBlend, depth);
}

void
openPrepass(void)
{
	const char *ppSrc[] = { header_metal_src, prepassh_metal_src, prepass_metal_src, nil };
	const char *ppSkinSrc[] = { header_metal_src, skin_metal_src, prepassh_metal_src, prepass_metal_src,
		prepass_skin_metal_src, nil };

	@autoreleasepool {
		prepassShader = Shader::create(ppSrc, "prepassVS", "prepassFS", 0);
		prepassSkinShader = Shader::create(ppSkinSrc, "prepassSkinVS", "prepassFS", 0);
	}
	if(prepassShader == nil || prepassSkinShader == nil)
		fprintf(stderr, "rw::metal: prepass shaders failed to build; modern frames draw without the prepass\n");
}

void
openIm2DEmit(void)
{
	const char *src[] = { header_metal_src, im2d_metal_src, emit_metal_src, nil };

	@autoreleasepool {
		im2dEmitShader = Shader::create(src, "im2dVS", "emitFS", VARIANT_ALPHATEST);
	}
}

bool32
initState(void)
{
	MetalContext *ctx = getContext();
	const char *im2dSrc[] = { header_metal_src, im2d_metal_src, simple_metal_src, nil };
	const char *defaultSrc[] = { header_metal_src, default_metal_src, simple_metal_src, nil };

	if(ctx == nil)
		return 0;
	memset(&stats, 0, sizeof(stats));
	dropsReported.clear();
	resetStatsLog();
	@autoreleasepool {
		im2dShader = Shader::create(im2dSrc, "im2dVS", "simpleFS", VARIANT_ALPHATEST);
		if(im2dShader == nil)
			return 0;
		defaultShader = Shader::create(defaultSrc, "defaultVS", "simpleFS", VARIANT_ALL);
		if(defaultShader == nil)
			return 0;
		defaultShader_noAT = defaultShader;
		defaultShader_fullLight = defaultShader;
		defaultShader_fullLight_noAT = defaultShader;
		im2dVertexLayout = registerVertexLayout(im2dAttribDesc, nelem(im2dAttribDesc));
		resetRenderState();
		invalidateEncoderState();
	}
	return 1;
}

void
termState(void)
{
	int i;

	if(im2dShader){
		im2dShader->destroy();
		im2dShader = nil;
	}
	if(im2dEmitShader){
		im2dEmitShader->destroy();
		im2dEmitShader = nil;
	}
	if(defaultShader)
		defaultShader->destroy();
	defaultShader = nil;
	defaultShader_noAT = nil;
	defaultShader_fullLight = nil;
	defaultShader_fullLight_noAT = nil;
	if(prepassShader)
		prepassShader->destroy();
	prepassShader = nil;
	if(prepassSkinShader)
		prepassSkinShader->destroy();
	prepassSkinShader = nil;
	prepassKeys = PrepassKeySet();
	prepassFrame = false;
	forceEqualNext = false;
	worldsPrev = PrevMap<RawMatrix>();
	worldsCur = PrevMap<RawMatrix>();
	bonesPrev = PrevMap<UniformSkin>();
	bonesCur = PrevMap<UniformSkin>();
	haveLastViewProj = false;
	currentObject = nil;
	currentDrawInst = nil;
	currentShader = nil;
	lastPipeKey = 0;
	lastPipe = nil;
	lastDepthKey = 0;
	lastDepth = nil;
	pipelineCache.clear();
	depthStencilCache.clear();
	samplerCache.clear();
	warmedFormats = 0;
	drawEmissive = 1.0f;
	hostRecords.clear();
	currentLayout = 0;
	im2dVertexLayout = 0;
	for(i = 0; i < NUMBLOCKS; i++)
		blockBuffer[i] = nil;
	stats.framesInFlightAtTerm = 0;
	for(i = 0; i < MAXFRAMESINFLIGHT; i++){
		if(ring.slotDone[i].load() != ring.slotFrame[i])
			stats.framesInFlightAtTerm++;
		ring.buffers[i] = nil;
		ring.retired[i].clear();
	}
	ring.used = 0;
	blockInfo[BLOCK_CUSTOM].size = 0;
	haveViewport = false;
	enc = EncoderState();
}

void
bindVertexBuffer(void *buffer, uint32 offset)
{
	MetalContext *ctx = getContext();
	id<MTLBuffer> buf = (__bridge id<MTLBuffer>)buffer;

	if(ctx == nil || ctx->encoder == nil)
		return;
	syncEncoder(ctx);
	if(enc.vertexData == buf){
		if(enc.vertexDataOffset != offset)
			[ctx->encoder setVertexBufferOffset:offset atIndex:BUFFER_VERTEX];
	}else
		[ctx->encoder setVertexBuffer:buf offset:offset atIndex:BUFFER_VERTEX];
	enc.vertexData = buf;
	enc.vertexDataOffset = offset;
}

StateStats
getStateStats(void)
{
	stats.ringSize = ring.buffers[ring.frame] ? (uint32)ring.buffers[ring.frame].length : 0;
	stats.frameRingBytes = statsLog.frameBytes;
	return stats;
}

void
setSkinMatrices(const RawMatrix *bones, int32 numBones)
{
	if(numBones > MAXSKINBONES)
		numBones = MAXSKINBONES;
	if(numBones <= 0 || memcmp(uniformSkin.bones, bones, numBones*sizeof(RawMatrix)) == 0)
		return;
	memcpy(uniformSkin.bones, bones, numBones*sizeof(RawMatrix));
	blockDirty[BLOCK_SKIN] = true;
}

void
countSkinnedUnrouted(void)
{
	stats.skinnedUnrouted++;
}

bool32
countDroppedDraw(int32 cause, Shader *shader)
{
	static const char *causes[NUMDROPCAUSES] = {
		"no render encoder is open",
		"no shader is set",
		"its pipeline failed to build",
		"no ring space for its uniform blocks",
		"no render target is set",
		"it samples the raster it renders into",
		"a two-uv im2d draw has no override shader or layout",
	};
	stats.droppedDraws++;
	if(!dropsReported.insert((uint32)cause<<16 | (shader ? shader->shaderId : 0)).second)
		return 0;
	if(shader)
		snprintf(dropLine, sizeof(dropLine), "rw::metal: draw dropped, %s (shader %s/%s)\n", causes[cause],
			shader->vertexName, shader->fragmentName);
	else
		snprintf(dropLine, sizeof(dropLine), "rw::metal: draw dropped, %s (shader none)\n", causes[cause]);
	fprintf(stderr, "%s", dropLine);
	return 1;
}

void
countFeedbackDraw(void)
{
	stats.feedbackDraws++;
}

static void
resetStatsLog(void)
{
	FrameStats f = getFrameStats();
	statsLog.time = std::chrono::steady_clock::now();
	statsLog.framesAtStart = f.framesShown;
	statsLog.frames = statsLog.framesAtStart;
	statsLog.renderPasses = f.renderPasses;
	statsLog.copies = f.copies;
	statsLog.customUploads = stats.blockUploads[BUFFER_CUSTOM];
	statsLog.materialUploads = stats.blockUploads[BUFFER_MATERIAL];
	statsLog.drawablesAcquired = f.drawablesAcquired;
	statsLog.draws = 0;
	statsLog.frameBytes = 0;
	statsLog.ringPeak = 0;
	statsLog.raster = getRasterStats();
	statsLog.instance = getInstanceStats();
}

void
logStats(void)
{
	FrameStats f = getFrameStats();
	uint32 frames = f.framesShown;
	uint32 interval = frames - statsLog.frames;
	uint32 peak = statsLog.frameBytes > statsLog.ringPeak ? statsLog.frameBytes : statsLog.ringPeak;
	RasterStats r = getRasterStats();
	InstanceStats in = getInstanceStats();

	snprintf(statsLine, sizeof(statsLine), "rw::metal: stats frames %u draws/frame %.1f ring peak %u ring grows %u "
		"late pipelines %u host pipelines %u skinned unrouted %u strip restarts %u staged uploads %u direct uploads %u "
		"mipmap blits %u gpu waits %u block mismatches %u dropped draws %u passes/frame %.1f copies/frame %.1f "
		"custom uploads/frame %.1f material uploads/frame %.1f frames without drawable %u drawable wait max %.1f ms "
		"host prewarm %.1f ms samples %u%s\n",
		frames - statsLog.framesAtStart,
		interval ? (double)(stats.draws - statsLog.draws)/interval : 0.0,
		(peak + 1023)/1024, stats.ringGrows, stats.pipelinesLate, stats.pipelinesHost, stats.skinnedUnrouted,
		in.stripRestartMeshes - statsLog.instance.stripRestartMeshes,
		r.stagedUploads - statsLog.raster.stagedUploads, r.directUploads - statsLog.raster.directUploads,
		r.mipmapBlits - statsLog.raster.mipmapBlits, r.gpuWaits - statsLog.raster.gpuWaits,
		stats.blockSizeMismatches, stats.droppedDraws,
		interval ? (double)(f.renderPasses - statsLog.renderPasses)/interval : 0.0,
		interval ? (double)(f.copies - statsLog.copies)/interval : 0.0,
		interval ? (double)(stats.blockUploads[BUFFER_CUSTOM] - statsLog.customUploads)/interval : 0.0,
		interval ? (double)(stats.blockUploads[BUFFER_MATERIAL] - statsLog.materialUploads)/interval : 0.0,
		interval - (f.drawablesAcquired - statsLog.drawablesAcquired),
		f.drawableWaitMaxUs/1000.0, stats.hostPrewarmUs/1000.0,
		metalGlobals.numSamples ? metalGlobals.numSamples : 1, frameStatsTail());
	resetDrawableWaitMax();
	fprintf(stderr, "%s", statsLine);
	statsLog.time = std::chrono::steady_clock::now();
	statsLog.frames = frames;
	statsLog.draws = stats.draws;
	statsLog.ringPeak = 0;
	statsLog.renderPasses = f.renderPasses;
	statsLog.copies = f.copies;
	statsLog.customUploads = stats.blockUploads[BUFFER_CUSTOM];
	statsLog.materialUploads = stats.blockUploads[BUFFER_MATERIAL];
	statsLog.drawablesAcquired = f.drawablesAcquired;
}

void
setStatsInterval(uint32 seconds)
{
	statsInterval = seconds;
}

void
logStatsIfDue(void)
{
	if(statsInterval && std::chrono::steady_clock::now() - statsLog.time >= std::chrono::seconds(statsInterval))
		logStats();
}

const char*
getPrewarmLine(void)
{
	return prewarmLine;
}

const char*
getStatsLine(void)
{
	return statsLine;
}

const char*
getDropLine(void)
{
	return dropLine;
}

}
}
#endif
