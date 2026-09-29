#ifndef RW_METAL_METALFAN_H
#define RW_METAL_METALFAN_H

#include <stdint.h>

namespace rw {
namespace metal {

enum ImPrimType
{
	IMPRIM_NONE = 0,
	IMPRIM_LINELIST,
	IMPRIM_POLYLINE,
	IMPRIM_TRILIST,
	IMPRIM_TRISTRIP,
	IMPRIM_TRIFAN,
	IMPRIM_POINTLIST
};

int32_t primitiveCount(int32_t primType, int32_t numElements);
int32_t drawElementCount(int32_t primType, int32_t numElements);
int32_t fanToList(uint16_t *dst, int32_t numVertices);
int32_t fanToList(uint32_t *dst, int32_t numVertices);
int32_t indexedFanToList(uint16_t *dst, const uint16_t *indices, int32_t numIndices);

}
}

#endif
