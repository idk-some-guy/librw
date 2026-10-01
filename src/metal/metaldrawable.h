#ifndef RW_METAL_METALDRAWABLE_H
#define RW_METAL_METALDRAWABLE_H

#include <stdint.h>

namespace rw {
namespace metal {

struct HostRect
{
	int32_t x, y, width, height;
};

inline int32_t
hostPixels(float points, float scale)
{
	if(!(points > 0.0f))
		return 0;
	if(!(scale > 0.0f))
		scale = 1.0f;
	int32_t px = (int32_t)(points*scale + 0.5f);
	return px < 1 ? 1 : px;
}

inline void
hostWindowDrawable(float pointsWidth, float pointsHeight, float scale, int32_t *width, int32_t *height)
{
	*width = hostPixels(pointsWidth, scale);
	*height = hostPixels(pointsHeight, scale);
}

inline void
hostRenderSize(int32_t width, int32_t height, int32_t nativeWidth, int32_t nativeHeight,
	int32_t *outWidth, int32_t *outHeight)
{
	bool chosen = width > 0 && height > 0;
	if(nativeWidth <= 0 || nativeHeight <= 0){
		*outWidth = chosen ? width : 0;
		*outHeight = chosen ? height : 0;
		return;
	}
	if(!chosen || width > nativeWidth || height > nativeHeight){
		*outWidth = nativeWidth;
		*outHeight = nativeHeight;
		return;
	}
	*outWidth = width;
	*outHeight = height;
}

inline HostRect
hostFitRect(int32_t srcWidth, int32_t srcHeight, int32_t dstWidth, int32_t dstHeight)
{
	HostRect r;
	r.x = 0;
	r.y = 0;
	r.width = dstWidth > 0 ? dstWidth : 0;
	r.height = dstHeight > 0 ? dstHeight : 0;
	if(srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0)
		return r;
	int64_t a = (int64_t)srcWidth * dstHeight;
	int64_t b = (int64_t)dstWidth * srcHeight;
	if(a > b){
		r.height = (int32_t)((2*(int64_t)dstWidth*srcHeight + srcWidth) / (2*(int64_t)srcWidth));
		r.y = (dstHeight - r.height) / 2;
	}else if(a < b){
		r.width = (int32_t)((2*(int64_t)dstHeight*srcWidth + srcHeight) / (2*(int64_t)srcHeight));
		r.x = (dstWidth - r.width) / 2;
	}
	return r;
}

inline void
hostMapToRender(float x, float y, HostRect fit, int32_t renderWidth, int32_t renderHeight, float *rx, float *ry)
{
	if(fit.width <= 0 || fit.height <= 0){
		*rx = 0.0f;
		*ry = 0.0f;
		return;
	}
	float u = (x - fit.x) * renderWidth / fit.width;
	float v = (y - fit.y) * renderHeight / fit.height;
	*rx = u < 0.0f ? 0.0f : u > renderWidth ? (float)renderWidth : u;
	*ry = v < 0.0f ? 0.0f : v > renderHeight ? (float)renderHeight : v;
}

}
}

#endif
