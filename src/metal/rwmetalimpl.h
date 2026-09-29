namespace rw {
namespace metal {

#ifdef RW_METAL

void openIm2D(void);
void closeIm2D(void);
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

void forgetRasterTarget(Raster *raster);
bool32 readRasterPixels(Raster *raster, uint8 *dst);
bool32 compositeCameraPixels(Raster *raster, uint8 *dst);
bool32 writeRasterPixels(Raster *raster, const uint8 *src);
void resolveRasterTarget(Raster *raster);
bool32 beginDraw(void);
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
