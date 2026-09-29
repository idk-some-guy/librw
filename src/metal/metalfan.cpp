#include "metalfan.h"

namespace rw {
namespace metal {

int32_t
primitiveCount(int32_t primType, int32_t numElements)
{
	if(numElements <= 0)
		return 0;
	switch(primType){
	case IMPRIM_NONE:
	case IMPRIM_POINTLIST:
		return numElements;
	case IMPRIM_LINELIST:
		return numElements/2;
	case IMPRIM_POLYLINE:
		return numElements >= 2 ? numElements-1 : 0;
	case IMPRIM_TRILIST:
		return numElements/3;
	case IMPRIM_TRISTRIP:
	case IMPRIM_TRIFAN:
		return numElements >= 3 ? numElements-2 : 0;
	}
	return 0;
}

int32_t
drawElementCount(int32_t primType, int32_t numElements)
{
	int32_t n = primitiveCount(primType, numElements);
	if(n == 0)
		return 0;
	switch(primType){
	case IMPRIM_LINELIST:
		return n*2;
	case IMPRIM_TRILIST:
	case IMPRIM_TRIFAN:
		return n*3;
	}
	return numElements;
}

template <typename T> static int32_t
writeFan(T *dst, int32_t numVertices)
{
	int32_t i, n;
	n = primitiveCount(IMPRIM_TRIFAN, numVertices);
	for(i = 0; i < n; i++){
		dst[i*3] = 0;
		dst[i*3+1] = (T)(i+1);
		dst[i*3+2] = (T)(i+2);
	}
	return n*3;
}

int32_t fanToList(uint16_t *dst, int32_t numVertices) { return writeFan(dst, numVertices); }
int32_t fanToList(uint32_t *dst, int32_t numVertices) { return writeFan(dst, numVertices); }

int32_t
indexedFanToList(uint16_t *dst, const uint16_t *indices, int32_t numIndices)
{
	int32_t i, n;
	n = primitiveCount(IMPRIM_TRIFAN, numIndices);
	for(i = 0; i < n; i++){
		dst[i*3] = indices[0];
		dst[i*3+1] = indices[i+1];
		dst[i*3+2] = indices[i+2];
	}
	return n*3;
}

}
}
