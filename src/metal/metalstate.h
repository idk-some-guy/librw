#ifdef RW_METAL

namespace rw {
namespace metal {

struct Shader;

#define MAXNUMSTAGES 8

bool32 initState(void);
void termState(void);
void beginFrameState(void);
uint64 getFrameId(void);
void frameCompleted(uint64 frameId);
void invalidateEncoderState(void);
void setRenderState(int32 state, void *pvalue);
void *getRenderState(int32 state);
void setEncoderViewport(double x, double y, double w, double h);
void setFogPlanes(float32 fogStart, float32 fogEnd);
void setIm2DXform(const float32 *xform);
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

extern Shader *im2dShader;
extern uint32 im2dVertexLayout;

struct StateStats
{
	uint32 pipelinesAtInit;
	uint32 pipelinesLate;
	uint32 pipelineFailures;
	uint32 blockSizeMismatches;
	uint32 customBlockBinds;
	uint32 ringSize;
	uint32 ringEarlyReuses;
	uint32 framesInFlightAtTerm;
	uint32 blockUploads[8];
	uint32 vertexBlockBinds[8];
	uint32 fragmentBlockBinds[8];
};
StateStats getStateStats(void);
uint32 checkShaderBlockSizes(Shader *shader, uint32 variant);
bool32 pipelineCached(uint64 key);

}
}

#endif
