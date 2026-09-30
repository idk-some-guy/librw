#include "metalinst.h"

namespace rw {
namespace metal {

static void
setAttrib(InstAttrib *a, uint32_t index, int32_t format, uint32_t offset)
{
	a->index = index;
	a->format = format;
	a->stride = 0;
	a->offset = offset;
}

int32_t
defaultVertexLayout(bool normals, bool prelit, int32_t numTexCoordSets, InstAttrib *out)
{
	InstAttrib *a = out;
	uint32_t stride = 0;
	int32_t i, n;

	if(numTexCoordSets < 0)
		numTexCoordSets = 0;
	if(numTexCoordSets > 8)
		numTexCoordSets = 8;

	setAttrib(a++, INSTATTRIB_POS, INSTFMT_FLOAT3, stride);
	stride += 12;
	if(normals){
		setAttrib(a++, INSTATTRIB_NORMAL, INSTFMT_FLOAT3, stride);
		stride += 12;
	}
	if(prelit){
		setAttrib(a++, INSTATTRIB_COLOR, INSTFMT_UCHAR4_NORM, stride);
		stride += 4;
	}
	for(i = 0; i < numTexCoordSets; i++){
		setAttrib(a++, INSTATTRIB_TEXCOORDS0+i, INSTFMT_FLOAT2, stride);
		stride += 8;
	}

	n = a - out;
	for(i = 0; i < n; i++)
		out[i].stride = stride;
	return n;
}

int32_t
skinVertexLayout(bool normals, bool prelit, int32_t numTexCoordSets, InstAttrib *out)
{
	int32_t i, n = defaultVertexLayout(normals, prelit, numTexCoordSets, out);
	uint32_t stride = out[0].stride;

	setAttrib(&out[n++], INSTATTRIB_WEIGHTS, INSTFMT_FLOAT4, stride);
	stride += 16;
	setAttrib(&out[n++], INSTATTRIB_INDICES, INSTFMT_UCHAR4, stride);
	stride += 4;

	for(i = 0; i < n; i++)
		out[i].stride = stride;
	return n;
}

uint32_t
meshIndexOffsets(const uint32_t *numIndices, int32_t numMeshes, uint32_t *offsets)
{
	uint32_t offset = 0;
	int32_t i;
	for(i = 0; i < numMeshes; i++){
		offsets[i] = offset;
		offset += (numIndices[i]*2 + 3) & ~3u;
	}
	return offset;
}

bool
stripHasRestartIndex(const uint16_t *indices, uint32_t numIndices)
{
	uint32_t i;
	for(i = 0; i < numIndices; i++)
		if(indices[i] == 0xFFFF)
			return true;
	return false;
}

}
}
