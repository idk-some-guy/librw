#ifdef RW_METAL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace rw {

struct EngineOpenParams
{
	GLFWwindow **window;
	int width, height;
	const char *windowtitle;
	bool hidden = false;
};

namespace metal {

void registerPlatformPlugins(void);

extern Device renderdevice;

enum AttribFormat
{
	ATTRIBFMT_FLOAT2 = 0,
	ATTRIBFMT_FLOAT3,
	ATTRIBFMT_FLOAT4,
	ATTRIBFMT_UCHAR4,
	ATTRIBFMT_UCHAR4_NORM,
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
};

struct MetalCaps
{
	bool bcSupported;
	float maxAnisotropy;
	uint32 maxSamples;
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

}
}
#endif
