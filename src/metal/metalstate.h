#ifdef RW_METAL

namespace rw {
namespace metal {

struct Shader;

#define MAXNUMSTAGES 8
enum { MAXSKINBONES = 64 };

bool32 initState(void);
void prewarmPipelines(void);
void openIm2DEmit(void);
void openPrepass(void);
void prewarmFormats(uint32 mask);
void termState(void);
void startFrame(void);
void beginFrameState(void);
uint64 getFrameId(void);
void frameCompleted(uint64 frameId);
uint64 getCompletedFrameId(void);
void invalidateEncoderState(void);
void setRenderState(int32 state, void *pvalue);
void *getRenderState(int32 state);
void setEncoderViewport(double x, double y, double w, double h);
void setFogPlanes(float32 fogStart, float32 fogEnd);
void setIm2DXform(const float32 *xform);
void setSkinMatrices(const RawMatrix *bones, int32 numBones);
void setMatFXConstants(const RawMatrix *texMatrix, const float32 *fxParams, const RGBAf *colorClamp, const RGBAf *envColor);
void evictRaster(Raster *raster);
Raster *getStageRaster(int32 stage);
void forgetShaderPipelines(uint32 shaderId);

uint32 registerVertexLayout(const AttribDesc *attribs, int32 numAttribs);
void setVertexLayout(uint32 layout);

struct RingSpace
{
	uint8 *cpu;
	void *buffer;
	uint32 offset;
};
bool32 ringAlloc(uint32 size, uint32 align, RingSpace *space);
void bindVertexBuffer(void *buffer, uint32 offset);

enum DropCause
{
	DROP_NOENCODER,
	DROP_NOSHADER,
	DROP_NOPIPELINE,
	DROP_NORINGSPACE,
	DROP_NOTARGET,
	DROP_FEEDBACK,
	DROP_NOOVERRIDESHADER,
	NUMDROPCAUSES
};
bool32 countDroppedDraw(int32 cause, Shader *shader);
void countFeedbackDraw(void);
void countSkinnedUnrouted(void);
void logStats(void);
void logStatsIfDue(void);

extern Shader *im2dShader;
extern Shader *im2dEmitShader;
extern Shader *skinShader;
extern Shader *matfxEnvShader;
extern Shader *im3dShader;
extern uint32 im2dVertexLayout;
extern uint32 im2dUV2VertexLayout;
extern uint32 im3dVertexLayout;

struct StateStats
{
	uint32 pipelinesAtInit;
	uint32 pipelinesLate;
	uint32 pipelinesHost;
	uint32 pipelineFailures;
	uint32 blockSizeMismatches;
	uint32 customBlockBinds;
	uint32 ringSize;
	uint32 ringEarlyReuses;
	uint32 framesInFlightAtTerm;
	uint32 skinnedUnrouted;
	uint32 textureStageBinds;
	uint32 draws;
	uint32 droppedDraws;
	uint32 feedbackDraws;
	uint32 ringGrows;
	uint32 ringPeakBytes;
	uint32 frameRingBytes;
	uint32 blockUploads[9];
	uint32 vertexBlockBinds[9];
	uint32 fragmentBlockBinds[9];
	uint64 hostPrewarmUs;
	uint32 formatPrewarms;
	uint32 prepassDraws;
	uint32 prepassSkipped;
	uint32 prepassEqual;
	uint32 prepassOrphans;
	uint32 prepassOrphanLogs;
	uint32 prepassUnkeyed;
};
StateStats getStateStats(void);
const char *getPrewarmLine(void);
const char *getStatsLine(void);
const char *getDropLine(void);
uint32 checkShaderBlockSizes(Shader *shader, uint32 variant);
bool32 pipelineCached(uint64 key);
struct PipelineBlendInfo
{
	bool32 cached;
	bool32 fixedBlend;
	bool32 shaderBlend;
};
PipelineBlendInfo pipelineBlendInfo(uint64 key);
void setDrawEmissiveScale(float32 s);
void setDrawInstance(const void *inst);
bool prepassShadersReady(void);
void beginPrepassState(bool continuous);
struct PrepassMatrices
{
	RawMatrix viewProj;
	RawMatrix prevViewProj;
	RawMatrix uploadedViewProj;
};
const PrepassMatrices &prepassMatrices(void);
uint32 endPrepassState(void);
uint32 prepassDrawsSoFar(void);
void setUnjitteredProjection(float32 *mat);
void forgetObjectHistory(const void *object);
void noteImmediateDraw(uint32 primitive, uint32 count, void *indexBuffer, uint32 indexOffset, uint32 indexType, bool indexed,
	bool emissive);
void finishPrepassFrame(bool countOrphans);
int32 getVertexLayout(uint32 layout, AttribDesc *attribs, int32 maxAttribs);

}
}

#endif
