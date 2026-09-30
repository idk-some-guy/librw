namespace rw {
namespace metal {

#ifdef RW_METAL

void im2DRenderLine(void *vertices, int32 numVertices,
  int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices,
  int32 vert1, int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType,
   void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType,
   void *vertices, int32 numVertices, void *indices, int32 numIndices);

void openIm3D(void);
void closeIm3D(void);
bool32 openSkin(void);
void closeSkin(void);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void im3DEnd(void);

struct DisplayMode
{
	GLFWvidmode mode;
	int32 depth;
	uint32 flags;
};

struct MetalGlobals
{
	GLFWwindow **pWindow;
	GLFWwindow *window;

	GLFWmonitor *monitor;
	int numMonitors;
	int currentMonitor;

	DisplayMode *modes;
	int numModes;
	int currentMode;

	int winWidth, winHeight;
	const char *winTitle;
	bool winHidden;
	uint32 numSamples;

	void *context;
};

extern MetalGlobals metalGlobals;

struct FrameStats
{
	uint32 framesShown;
	uint32 drawablesAcquired;
	uint32 framesPresented;
};
FrameStats getFrameStats(void);

struct RasterStats
{
	uint32 directUploads;
	uint32 stagedUploads;
	uint32 mipmapBlits;
	uint32 gpuWaits;
};
extern RasterStats rasterStats;
RasterStats getRasterStats(void);

bool32 encodeTextureUpload(void *texture, int32 level, int32 width, int32 height,
	const uint8 *bytes, uint32 bytesPerRow, uint32 bytesPerImage);
void encodeMipmapGeneration(void *texture);
void waitForGPUWrites(uint64 frameId);

void forgetRasterTarget(Raster *raster);
void clearNewRasterTarget(Raster *raster);
bool32 readRasterPixels(Raster *raster, uint8 *dst);
bool32 readDepthPixel(Raster *zbuffer, int32 x, int32 y, float32 *depth);
bool32 compositeCameraPixels(Raster *raster, uint8 *dst);
bool32 writeRasterPixels(Raster *raster, const uint8 *src);
void resolveRasterTarget(Raster *raster);
bool32 beginDraw(void);
bool32 drawIndexed(InstanceDataHeader *header, InstanceData *inst);
void allocInstanceVertices(InstanceDataHeader *header, const AttribDesc *attribs, int32 numAttribs);
void instanceDefaultAttribs(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void uploadInstanceVertices(InstanceDataHeader *header);

struct InstanceStats
{
	uint32 stripRestartMeshes;
};
InstanceStats getInstanceStats(void);
bool32 rasterHasPendingWork(Raster *raster);
void *getRasterSampleTexture(Raster *raster);
void *getWhiteTexture(void);
void termRaster(void);
#endif

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster*, int32 level, int32 lockMode);
void rasterUnlock(Raster*, int32);
int32 rasterNumLevels(Raster*);
bool32 imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);

}
}
