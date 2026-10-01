#ifndef RW_METAL_METALMODES_H
#define RW_METAL_METALMODES_H

#include <stdint.h>

namespace rw {
namespace metal {

enum
{
	HOSTMODE_EXCLUSIVE = 1,
	HOSTMODE_DEPTH = 32
};

enum
{
	HOSTCAND_USABLE = 1,
	HOSTCAND_NATIVE = 2
};

struct HostModeCandidate
{
	int32_t width, height;
	int32_t refresh;
	uint32_t flags;
};

struct HostMode
{
	int32_t width, height;
	int32_t depth;
	int32_t refresh;
	uint32_t flags;
};

inline int32_t
hostRefreshHz(double hz)
{
	if(!(hz > 0.0))
		return 0;
	return (int32_t)(hz + 0.5);
}

inline bool
hostNativeSize(const HostModeCandidate *cands, int32_t num, int32_t *width, int32_t *height)
{
	int32_t i, best = -1;
	int64_t bestArea = 0;
	for(i = 0; i < num; i++)
		if((cands[i].flags & HOSTCAND_NATIVE) && cands[i].width > 0 && cands[i].height > 0){
			*width = cands[i].width;
			*height = cands[i].height;
			return true;
		}
	for(i = 0; i < num; i++){
		const HostModeCandidate *c = &cands[i];
		if(!(c->flags & HOSTCAND_USABLE) || c->width <= 0 || c->height <= 0)
			continue;
		int64_t area = (int64_t)c->width * c->height;
		if(best < 0 || area > bestArea || (area == bestArea && c->width > cands[best].width)){
			best = i;
			bestArea = area;
		}
	}
	if(best < 0){
		*width = 0;
		*height = 0;
		return false;
	}
	*width = cands[best].width;
	*height = cands[best].height;
	return true;
}

inline int32_t
hostAddMode(HostMode *out, int32_t num, int32_t maxOut, int32_t width, int32_t height, int32_t refresh)
{
	int32_t i;
	if(refresh < 0)
		refresh = 0;
	for(i = 1; i < num; i++)
		if(out[i].width == width && out[i].height == height){
			if(refresh > out[i].refresh)
				out[i].refresh = refresh;
			return num;
		}
	if(num >= maxOut)
		return num;
	out[num].width = width;
	out[num].height = height;
	out[num].depth = HOSTMODE_DEPTH;
	out[num].refresh = refresh;
	out[num].flags = HOSTMODE_EXCLUSIVE;
	return num + 1;
}

inline bool
hostModeBefore(const HostMode &a, const HostMode &b)
{
	if(a.width != b.width)
		return a.width < b.width;
	return a.height < b.height;
}

inline int32_t
hostBuildModeList(const HostModeCandidate *cands, int32_t num, int32_t nativeWidth, int32_t nativeHeight,
	const HostMode &current, HostMode *out, int32_t maxOut)
{
	int32_t i, j, n;
	bool capped = nativeWidth > 0 && nativeHeight > 0;
	if(maxOut < 1)
		return 0;
	out[0] = current;
	out[0].depth = HOSTMODE_DEPTH;
	out[0].flags = 0;
	if(out[0].refresh < 0)
		out[0].refresh = 0;
	n = 1;
	for(i = 0; i < num; i++){
		const HostModeCandidate *c = &cands[i];
		if(!(c->flags & HOSTCAND_USABLE) || c->width <= 0 || c->height <= 0)
			continue;
		if(capped && (c->width > nativeWidth || c->height > nativeHeight))
			continue;
		n = hostAddMode(out, n, maxOut, c->width, c->height, c->refresh);
	}
	if(capped)
		n = hostAddMode(out, n, maxOut, nativeWidth, nativeHeight, 0);
	for(i = 2; i < n; i++){
		HostMode m = out[i];
		for(j = i; j > 1 && hostModeBefore(m, out[j-1]); j--)
			out[j] = out[j-1];
		out[j] = m;
	}
	return n;
}

inline int32_t
hostFindMode(const HostMode *modes, int32_t num, int32_t width, int32_t height)
{
	int32_t i;
	for(i = 1; i < num; i++)
		if(modes[i].width == width && modes[i].height == height)
			return i;
	return num > 1 ? num - 1 : 0;
}

}
}

#endif
