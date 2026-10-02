#ifndef RW_METAL_METALNORMALS_H
#define RW_METAL_METALNORMALS_H

#include <stdint.h>

namespace rw {
namespace metal {

uint32_t stripToList(const uint16_t *strip, uint32_t numIndices, uint32_t *out);
uint32_t listToList(const uint16_t *list, uint32_t numIndices, uint32_t *out);
void deriveNormals(const uint8_t *vertices, uint32_t stride, uint32_t posOffset, uint32_t numVertices,
                   const uint32_t *list, uint32_t numIndices, float *normals);

}
}

#endif
