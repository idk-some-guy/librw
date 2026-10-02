#ifndef RW_METAL_METALNORMALS_H
#define RW_METAL_METALNORMALS_H

#include <stdint.h>

namespace rw {
namespace metal {

struct NormalRule
{
	float smoothCos;
	float quantum;
	float sliverArea;
};

struct NormalCounts
{
	uint32_t vertices;
	uint32_t welded;
	uint32_t ambiguous;
	uint32_t facesFlat;
	uint32_t facesSmooth;
	uint32_t facesMixed;
};

extern const float normalSmoothAngle;
extern const NormalRule defaultNormalRule;

uint32_t stripToList(const uint16_t *strip, uint32_t numIndices, uint32_t *out);
uint32_t listToList(const uint16_t *list, uint32_t numIndices, uint32_t *out);
void deriveNormals(const uint8_t *vertices, uint32_t stride, uint32_t posOffset, uint32_t numVertices,
                   const uint32_t *list, uint32_t numIndices, float *normals,
                   const NormalRule *rule = 0, NormalCounts *counts = 0);
void packNormals(const float *normals, uint32_t numVertices, int8_t *packed);

}
}

#endif
