#include <math.h>
#include <string.h>
#include "metalnormals.h"

namespace rw {
namespace metal {

uint32_t
stripToList(const uint16_t *s, uint32_t n, uint32_t *out)
{
	uint32_t k = 0;
	for(uint32_t i = 2; i < n; i++){
		uint32_t a = s[i-2], b = s[i-1], c = s[i];
		if(a == b || b == c || a == c)
			continue;
		if(i & 1){
			out[k++] = b;
			out[k++] = a;
		}else{
			out[k++] = a;
			out[k++] = b;
		}
		out[k++] = c;
	}
	return k;
}

uint32_t
listToList(const uint16_t *l, uint32_t n, uint32_t *out)
{
	uint32_t k = 0;
	for(uint32_t i = 0; i + 2 < n; i += 3){
		if(l[i] == l[i+1] || l[i+1] == l[i+2] || l[i] == l[i+2])
			continue;
		out[k++] = l[i];
		out[k++] = l[i+1];
		out[k++] = l[i+2];
	}
	return k;
}

static void
position(const uint8_t *v, uint32_t stride, uint32_t posOffset, uint32_t i, float p[3])
{
	memcpy(p, v + (size_t)i*stride + posOffset, 3*sizeof(float));
}

void
deriveNormals(const uint8_t *v, uint32_t stride, uint32_t posOffset, uint32_t numVertices,
              const uint32_t *list, uint32_t numIndices, float *n)
{
	memset(n, 0, (size_t)numVertices*3*sizeof(float));
	for(uint32_t t = 0; t + 2 < numIndices; t += 3){
		uint32_t idx[3] = { list[t], list[t+1], list[t+2] };
		if(idx[0] >= numVertices || idx[1] >= numVertices || idx[2] >= numVertices)
			continue;
		float a[3], b[3], c[3];
		position(v, stride, posOffset, idx[0], a);
		position(v, stride, posOffset, idx[1], b);
		position(v, stride, posOffset, idx[2], c);
		float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		float f[3] = { e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0] };
		for(int k = 0; k < 3; k++)
			for(int j = 0; j < 3; j++)
				n[(size_t)idx[k]*3+j] += f[j];
	}
	for(uint32_t i = 0; i < numVertices; i++){
		float *p = &n[(size_t)i*3];
		float l = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
		if(l > 0.0f){
			p[0] /= l;
			p[1] /= l;
			p[2] /= l;
		}
	}
}

}
}
