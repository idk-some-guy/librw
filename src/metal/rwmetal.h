#ifdef RW_METAL
#include "metalsettings.h"
#ifdef LIBRW_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

namespace rw {

namespace metal { struct MetalHost; }

struct EngineOpenParams
{
#ifdef LIBRW_GLFW
	GLFWwindow **window;
#endif
#ifdef LIBRW_COCOA
	void **cocoaWindow = nil;
#endif
	int width, height;
	const char *windowtitle;
	bool hidden = false;
	metal::MetalHost *host = nil;
};

namespace metal {

struct MetalHostMode
{
	int32 width, height;
	int32 depth;
	int32 refresh;
	uint32 flags;
};

struct MetalHost
{
	bool32 (*open)(EngineOpenParams *params);
	void (*close)(void);
	int32 (*numDisplays)(void);
	const char *(*displayName)(int32 display);
	bool32 (*displayMode)(int32 display, MetalHostMode *mode);
	const MetalHostMode *(*getModes)(int32 display, int32 *numModes);
	void *(*createSurface)(int32 display, int32 mode, bool32 windowed, bool32 hidden);
	void (*destroySurface)(void);
	void (*drawableSize)(int32 *width, int32 *height);
	float32 (*backingScale)(void);
	int32 (*refreshRate)(void);
	bool32 (*visible)(void);
	bool32 (*pollSizeChange)(void);
	void *(*surfaceScreen)(void);
};

#ifdef LIBRW_GLFW
extern MetalHost glfwHost;
#endif
#ifdef LIBRW_COCOA
extern MetalHost cocoaHost;
#endif

void registerPlatformPlugins(void);
void setStatsInterval(uint32 seconds);

extern Device renderdevice;

enum AttribFormat
{
	ATTRIBFMT_FLOAT2 = 0,
	ATTRIBFMT_FLOAT3,
	ATTRIBFMT_FLOAT4,
	ATTRIBFMT_UCHAR4,
	ATTRIBFMT_UCHAR4_NORM,
	ATTRIBFMT_CHAR4_NORM,
};

struct AttribDesc
{
	uint32 index;
	int32  format;
	uint32 stride;
	uint32 offset;
};

enum AttribIndices
{
	ATTRIB_POS = 0,
	ATTRIB_NORMAL,
	ATTRIB_COLOR,
	ATTRIB_WEIGHTS,
	ATTRIB_INDICES,
	ATTRIB_TEXCOORDS0,
	ATTRIB_TEXCOORDS1,
	ATTRIB_TEXCOORDS2,
	ATTRIB_TEXCOORDS3,
	ATTRIB_TEXCOORDS4,
	ATTRIB_TEXCOORDS5,
	ATTRIB_TEXCOORDS6,
	ATTRIB_TEXCOORDS7,
};

enum { MAXVERTEXATTRIBS = 13 };

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;
	int32     numVertices;
	Material *material;
	bool32    vertexAlpha;
	uint32    offset;
	float32   castAlphaRef;
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32      serialNumber;
	uint32      numMeshes;
	uint16     *indexBuffer;
	uint32      primType;
	uint8      *vertexBuffer;
	int32       numAttribs;
	AttribDesc *attribDesc;
	uint32      totalNumVertex;

	uint32      vertexLayout;
	void       *mtlIndexBuffer;
	void       *mtlVertexBuffer;
	void       *mtlNormalBuffer;
	int32       normalSlot;
	bool32      stripRestart;

	InstanceData *inst;
};

struct Shader;

extern Shader *defaultShader, *defaultShader_noAT;
extern Shader *defaultShader_fullLight, *defaultShader_fullLight_noAT;

struct Im3DVertex
{
	V3d     position;
	V3d     normal;
	uint8   r, g, b, a;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setNormalX(float32 x) { this->normal.x = x; }
	void setNormalY(float32 y) { this->normal.y = y; }
	void setNormalZ(float32 z) { this->normal.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->position.x; }
	float getY(void) { return this->position.y; }
	float getZ(void) { return this->position.z; }
	float getNormalX(void) { return this->normal.x; }
	float getNormalY(void) { return this->normal.y; }
	float getNormalZ(void) { return this->normal.z; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};
extern RGBA im3dMaterialColor;
extern SurfaceProperties im3dSurfaceProps;

struct Im2DVertex
{
	float32 x, y, z, w;
	uint8   r, g, b, a;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float recipz) { this->u = u; }
	void setV(float32 v, float recipz) { this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

struct Im2DVertexUV2 : Im2DVertex
{
	float32 u2, v2;
};

void setupVertexInput(InstanceDataHeader *header);
void teardownVertexInput(InstanceDataHeader *header);

enum
{
	VSLIGHT_DIRECT	= 1,
	VSLIGHT_POINT	= 2,
	VSLIGHT_SPOT	= 4,
	VSLIGHT_MASK	= 7,
	VSLIGHT_AMBIENT = 8,
};

extern const char *header_metal_src;
extern const char *im2d_metal_src;
extern const char *im2d_uv2_metal_src;
extern const char *default_metal_src;
extern const char *skin_metal_src;
extern const char *simple_metal_src;

extern Shader *im2dOverrideShader;

void im2DRenderIndexedPrimitiveUV2(PrimitiveType primType,
   void *vertices, int32 numVertices, void *indices, int32 numIndices);

uint32 drawVariant(int32 vsBits);
int32 defaultVertexAttribs(bool32 normals, bool32 prelit, int32 numTexCoordSets, AttribDesc *out);
int32 skinVertexAttribs(bool32 normals, bool32 prelit, int32 numTexCoordSets, AttribDesc *out);
bool32 prewarmShader(Shader *shader, const AttribDesc *attribs, int32 numAttribs, uint32 variant,
                     bool32 blend, int32 srcBlend, int32 destBlend, bool32 depth);
bool32 prewarmIm2DShader(Shader *shader, bool32 uv2, bool32 blend, int32 srcBlend, int32 destBlend, bool32 depth);

void setProjectionMatrix(float32*);
void setViewMatrix(float32*);

void setWorldMatrix(Matrix *mat, const void *object = nil);
int32 setLights(WorldLights *lightData);

void setTexture(int32 n, Texture *tex);
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f);
inline void setMaterial(uint32 flags, const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f)
{
	static RGBA white = { 255, 255, 255, 255 };
	if(flags & Geometry::MODULATE)
		setMaterial(color, surfaceprops, extraSurfProp);
	else
		setMaterial(white, surfaceprops, extraSurfProp);
}

void setAlphaBlend(bool32 enable);
bool32 getAlphaBlend(void);

bool32 getAlphaTest(void);

void setCustomConstants(const void *data, uint32 size);

bool32 flushCache(void);

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);
void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header);
int32 lightingCB(Atomic *atomic);
int32 lightingCB(void);

void drawInst_simple(InstanceDataHeader *header, InstanceData *inst);
void drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst);
void drawInst(InstanceDataHeader *header, InstanceData *inst);

void *destroyNativeData(void *object, int32, int32);

ObjPipeline *makeDefaultPipeline(void);


struct MetalRaster
{
	void *texture;
	void *sampleTexture;
	int32 format;
	int32 bpp;

	bool isCompressed;
	bool hasAlpha;
	bool autogenMipmap;
	int8 numLevels;
	int8 filledLevels;
	uint16 filledMask;
	uint8 filterMode;
	uint8 addressU;
	uint8 addressV;
	int32 maxAnisotropy;
	uint64 lastUseFrame;
	uint64 gpuWriteFrame;
	void *msaaTexture;
	uint32 numSamples;
	bool32 msaaCurrent;
};

struct MetalCaps
{
	bool bcSupported;
	float maxAnisotropy;
	uint32 maxSamples;
	bool appleGPU;
	bool apple9;
	bool raytracing;
};
extern MetalCaps metalCaps;

void allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha);
void allocateTexture(Raster *raster, int32 numLevels);

Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);

extern int32 nativeRasterOffset;
void registerNativeRaster(void);
#define GETMETALRASTEREXT(raster) PLUGINOFFSET(MetalRaster, raster, rw::metal::nativeRasterOffset)

struct RenderSettings
{
	int32 tier;
	int32 upscale;
	bool32 dynamicResolution;
	int32 hdrOutput;
	float32 emitterPeakNits;
	float32 softness;
	int32 antiAliasing;
	int32 shadowQuality;
	int32 lights;
	int32 shadowedLights;
};

struct FrameInfo
{
	int32 tier;
	int32 tierReason;
	int32 internalWidth, internalHeight;
	int32 outputWidth, outputHeight;
	int32 upscaler;
	bool32 edr;
	float32 headroom, potentialHeadroom, peak;
	float32 bloom;
	int32 antiAliasing;
	float32 jitterX, jitterY;
	bool32 prepassValid;
	bool32 temporalReset;
};

void setRenderSettings(const RenderSettings *s);
RenderSettings getRenderSettings(void);
int32 getSupportedTier(void);
FrameInfo getFrameInfo(void);

struct RayStats
{
	uint32 bottomLevels;
	float32 bottomMegabytes;
	uint32 instances;
	uint32 raysPerFrame;
	float32 passMs;
	uint32 builds;
	uint32 refits;
	uint32 budgetFull;
	uint32 levelsSkipped;
	uint32 levelsEmpty;
	float32 scratchMegabytes;
	uint32 casters, hostCasters, castersCapped, castersWaiting, skinnedCasters, skinnedLevels, skinnedBuilds, rayFrames, castersDropped;
	float32 skinnedMegabytes, topMegabytes;
	uint32 frameBuilds, frameRefits;
	float32 walkMs, shadowMegabytes;
	uint32 alphaInstances, alphaEntries, alphaTextures, alphaCapped, alphaKernel, alphaSkipped;
	float32 topMs, buildMs;
	uint32 castersLarge;
	uint32 ownedInstances;
	uint32 ownerListed, ownerOverflow;
	uint32 lightSlots, lightFrame;
	uint32 lightRays[8], lightSlotId[8];
};
RayStats getRayStats(void);

enum { MAXGRADECONSTANTS = 16 };

struct GradeParams
{
	bool32 grade;
	const float32 *constants;
	uint32 numConstants;
	float32 bloomWeight;
};

void beginScene(Camera *cam);
void endScene(const GradeParams *params);
bool32 beginPrepass(void);
void endPrepass(void);
void cameraCut(void);

struct RelitParams
{
	float32 weight;
	float32 sunDir[3];
	float32 skyTop[3];
	float32 skyBottom[3];
	float32 hemiScale;
	float32 hemiSaturation;
	float32 sunScale;
	float32 sunRadius;
	float32 shadowStrength;
};

void setRelitParams(const RelitParams *p);
RelitParams getRelitParams(void);
void setSunLight(const Light *light);
void setRelitDraw(bool32 on);
enum { SHADOWCASTER_STATIC = 1, SHADOWCASTER_MOVING = 2, SHADOWCASTER_SKINNED = 4 };
void addShadowCaster(Atomic *atomic, uint32 kind);
void addSkinnedShadowCaster(Atomic *atomic);
void setShadowOwner(uint32 owner);
void requestShadowLevel(Atomic *atomic);
void setShadowCaster(bool32 on);
bool32 shadowSourceOn(void);
bool32 lightShadowsPlanned(void);
void setShadowCasterMaxRadius(float32 metres);
void setRelitTwoSided(bool32 on);
void beginReactive(void);
void endReactive(void);
bool32 enhancedFrame(void);
void setEmissiveScale(float32 s);
float32 getEmissiveScale(void);
bool32 setGradeShader(const char *src);
void displayChanged(void);
void setDebugView(int32 view);
int32 getDebugView(void);

enum { LIGHTS_OFF = 0, LIGHTS_ON, LIGHTS_SHADOWED };
enum { LIGHT_SPOT = 1, LIGHT_PINNED = 2 };

struct LightDesc
{
	float32 pos[3];
	float32 radius;
	float32 color[3];
	float32 gloss;
	float32 dir[3];
	float32 cosOuter;
	float32 cosInner;
	float32 softness;
	uint32 id;
	uint32 flags;
	uint32 owner;
	uint32 group;
};

struct LightFrameParams
{
	float32 wetGloss;
	float32 lightScale;
};

struct LightStats
{
	uint32 submitted, kept, dropped, clusterLoss, maxPerCluster, shadowed, lightRays;
	float32 clusterMs, lightShadowMs;
};

void setLights(const LightDesc *lights, int32 n);
void setLightFrameParams(const LightFrameParams *p);
void setDrawEmissiveLuma(float32 scale);
LightStats getLightStats(void);

}
}
#endif
