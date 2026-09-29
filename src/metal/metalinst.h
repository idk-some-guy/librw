#ifndef RW_METAL_METALINST_H
#define RW_METAL_METALINST_H

#include <stdint.h>

namespace rw {
namespace metal {

struct InstAttrib
{
	uint32_t index;
	int32_t format;
	uint32_t stride;
	uint32_t offset;
};

enum { INSTATTRIB_POS = 0, INSTATTRIB_NORMAL = 1, INSTATTRIB_COLOR = 2, INSTATTRIB_TEXCOORDS0 = 5 };
enum { INSTFMT_FLOAT2 = 0, INSTFMT_FLOAT3 = 1, INSTFMT_UCHAR4_NORM = 4 };
enum { MAXINSTATTRIBS = 11 };

int32_t defaultVertexLayout(bool normals, bool prelit, int32_t numTexCoordSets, InstAttrib *out);
uint32_t meshIndexOffsets(const uint32_t *numIndices, int32_t numMeshes, uint32_t *offsets);
bool stripHasRestartIndex(const uint16_t *indices, uint32_t numIndices);

}
}

#endif
