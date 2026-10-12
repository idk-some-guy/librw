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

void openIm2DUV2(void);
void closeIm2DUV2(void);
void openIm3D(void);
void closeIm3D(void);
bool32 openSkin(void);
void closeSkin(void);
bool32 openMatFX(void);
void closeMatFX(void);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void im3DEnd(void);

struct MetalGlobals
{
	MetalHost *host;
	int numDisplays;
	int currentDisplay;
	int surfaceDisplay;

	MetalHostMode *modes;
	int numModes;
	int currentMode;

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
	uint32 renderPasses;
	uint32 copies;
	uint32 depthDetached;
	uint32 drawableWaitMaxUs;
};
FrameStats getFrameStats(void);
void resetDrawableWaitMax(void);

struct RasterStats
{
	uint32 directUploads;
	uint32 stagedUploads;
	uint32 mipmapBlits;
	uint32 gpuWaits;
};
extern RasterStats rasterStats;
RasterStats getRasterStats(void);

struct InstanceStats
{
	uint32 stripRestartMeshes;
	uint32 normalStreams;
	uint32 normalPending;
	uint32 normalLive;
	uint32 normalBytes;
};
InstanceStats getInstanceStats(void);
void setNormalBudgetForTest(float32 ms);
void setNormalBuildCostForTest(float32 ms);
void setShadowFactorForTest(float32 v);
void setFaceFetchForTest(bool32 on);
void setFaceSkipForTest(bool32 on);
void setRelitAtFullWeightForTest(bool32 on);
void setPlainTwinFailForTest(bool32 fail);
uint32 lightsBlockForTest(void *dst, uint32 size);
double lastFrameGpuMsForTest(void);
void setRtBudgetForTest(uint64 bytes);
void setRtBuildBudgetForTest(uint32 triangles);
void setRtFrameForTest(uint64 frame);
void rtFlushForTest(void);
bool32 rtBottomForTest(InstanceDataHeader *h, uint32 *triangles, uint32 *geometries);
uint32 rtQueuedForTest(void);
uint32 rtBudgetLogsForTest(void);
const char *rtRefusalLogForTest(void);
uint32 rtRetiredForTest(void);
uint64 rtCasterAddsForTest(void);
uint64 rtPaletteCopiesForTest(void);
uint64 rtHeapBytesForTest(void);
uint64 rtLastUsedForTest(InstanceDataHeader *h);
void setRtScratchFailForTest(bool32 fail);

enum { RT_TIMING_USAGES = 4 };
struct RtBuildTiming
{
	uint32 rounds;
	uint32 levels;
	uint32 triangles;
	uint32 usages;
	const char *usageName[RT_TIMING_USAGES];
	float32 batchMs[RT_TIMING_USAGES];
	uint64 bytes[RT_TIMING_USAGES];
	float32 tinyMs;
	float32 splitMs;
	float32 fixedUs;
	float32 perTriangleNs;
	uint32 compactedLevels;
	uint32 sampleBytes;
	uint32 compactedBytes;
	uint32 instanceCounts[3];
	float32 instanceMs[3];
	uint32 instanceBytes[3];
	float32 nsPerTick;
	float32 bufferMs;
};
bool32 rtMeasureBuildsForTest(RtBuildTiming *t);

enum { RT_SKIN_DEFAULT, RT_SKIN_REBUILD, RT_SKIN_REFIT };
struct RtFrameTiming
{
	float32 skinMs, skinnedMs, instanceMs, walkMs;
	uint32 instances, skinned;
};
bool32 rtTraceForTest(const float32 origin[3], const float32 dir[3], float32 maxDistance, uint32 mask);
uint32 rtTraceDeferredForTest(const float32 origin[3], const float32 dir[3], float32 maxDistance, uint32 mask);
bool32 rtDeferredHitForTest(uint32 index);
const void *rtInstanceLevelForTest(void);
void setRtInstanceCapForTest(uint32 cap);
void setRtFrameWorkForTest(bool32 on);
void setRtSkinModeForTest(int32 mode);
bool32 rtSkinnedForTest(const void *object);
void rtWaitForTest(void);
void rtResetForTest(void);
void setRtTimingForTest(bool32 on);
bool32 rtFrameTimingForTest(RtFrameTiming *t);
bool32 rtShadowTimingForTest(uint32 width, uint32 height, int32 quality, const float32 eye[3], const float32 target[3],
	const float32 toSun[3], float32 *ms, float32 *ground, float32 *shadowed);
void setRtAlphaCapForTest(uint32 entries);
void setShadowAlphaKernelForTest(int32 mode);
void setShadowTerminatorRuleForTest(bool32 on);
void setShadowOwnerRuleForTest(bool32 on);
void setShadowOwnerListCapForTest(uint32 cap);
void setRtAlphaFunctionForTest(int32 mode);
const void *rtAlphaTableForTest(void);
void setRtSkinnedLimitForTest(uint32 cap, float32 distance);
void setRtAlphaIndexPastForTest(bool32 on);
void setRtAlphaTextureLostForTest(bool32 on);
void rtWatchTextureForTest(Raster *raster);
bool32 rtWatchedTextureAliveForTest(void);
void rtWatchAlphaTableForTest(void);
bool32 rtWatchedTableAliveForTest(void);

bool32 encodeTextureUpload(void *texture, int32 level, int32 width, int32 height,
	const uint8 *bytes, uint32 bytesPerRow, uint32 bytesPerImage);
void encodeMipmapGeneration(void *texture);
void waitForGPUWrites(uint64 frameId);

void forgetRasterTarget(Raster *raster);
void clearNewRasterTarget(Raster *raster);
bool32 readRasterPixels(Raster *raster, uint8 *dst);
bool32 writeRasterPixels(Raster *raster, const uint8 *src);
void resolveRasterTarget(Raster *raster);
bool32 rasterHasPendingWork(Raster *raster);
void *getRasterSampleTexture(Raster *raster);
void *getRasterTargetTexture(Raster *raster, uint32 samples);
void *getWhiteTexture(void);
void termRaster(void);

bool32 beginDraw(void);
bool32 drawIndexed(InstanceDataHeader *header, InstanceData *inst);
void allocInstanceVertices(InstanceDataHeader *header, const AttribDesc *attribs, int32 numAttribs);
void instanceDefaultAttribs(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
void uploadInstanceVertices(InstanceDataHeader *header);

int32 getMaxFramesInFlight(void);
void holdFrameForTest(double seconds);
void setSurfaceHiddenForTest(bool32 hidden);
#ifdef LIBRW_COCOA
bool32 cocoaFullscreenSizeForTest(int32 mode, int32 *width, int32 *height);
#endif
bool32 passIsOpen(void);
bool32 readDepthPixel(Raster *zbuffer, int32 x, int32 y, float32 *depth);
bool32 compositeCameraPixels(Raster *raster, uint8 *dst);
bool32 compositeCameraPixelsHalf(Raster *raster, uint16 *dst);
void setTierCapsForTest(const TierCaps *caps);
uint32 gradeBuildsForTest(void);
uint32 hostPrewarmRecordsForTest(void);
void setBloomKarisForTest(bool32 on);
void setScalerFailForTest(bool32 fail);
void setDeringCopyForTest(bool32 copy);
void scalerCountsForTest(uint32 *attempts, uint32 *logs);
void setDeringNanForTest(int32 x, int32 y);
void setUpscaledFailForTest(bool32 fail);
void upscaledCountsForTest(uint32 *attempts, uint32 *logs);
void setAntiAliasFailForTest(bool32 fail);
void antiAliasCountsForTest(uint32 *allocs, uint32 *logs);
bool32 antiAliasTextureForTest(void);
bool32 readFrameOutput(int32 x, int32 y, float32 rgba[4]);
bool32 readScenePixel(int32 x, int32 y, float32 rgba[4]);
bool32 readPrepassForTest(int32 x, int32 y, float32 normal[3], float32 motion[2], float32 *depth);
bool32 readLayerForTest(int32 x, int32 y, float32 normal[3], float32 motion[2], float32 *depth);
bool32 readShadowForTest(int32 x, int32 y, float32 *value, float32 *length);
void forcePrepassEqualForTest(void);
bool32 readReactiveForTest(int32 x, int32 y, float32 *v);
void setTemporalFailForTest(bool32 fail);
void setPrepassTargetsFailForTest(bool32 fail);
void setPrepassNormalsUnwrittenForTest(bool32 on);
uint32 temporalLogsForTest(void);
void sceneProjectionForTest(float32 proj[16]);
void setJitterIndexForTest(uint32 index);
const void *reactivePassForTest(void);
void setTaaFailForTest(bool32 fail);
void taaCountsForTest(uint32 *allocs, uint32 *logs, uint32 *requests);
bool32 taaHistoryForTest(void);
void setEdrForTest(float32 current, float32 potential, bool32 notify);
bool32 layerIsEdrForTest(void);
void setEdrScreenLostForTest(bool32 lost);
uint32 edrLogsForTest(void);
Raster *createSceneRaster(int32 width, int32 height);
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
