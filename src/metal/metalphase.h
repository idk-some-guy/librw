#ifndef RW_METAL_METALPHASE_H
#define RW_METAL_METALPHASE_H

#include <stdint.h>
#include "metalpass.h"
#include "metaldrawable.h"
#include "metalkeys.h"

namespace rw {
namespace metal {

enum
{
	PHASE_CLASSIC = 0,
	PHASE_SCENE,
	PHASE_POST,
	PHASE_SHOWN,
	PHASE_PREPASS
};

struct FrameRoute
{
	int phase;
	const void *mainColor, *mainDepth;
	const void *sceneColor, *sceneDepth;
};

inline PassTarget
routeTarget(const FrameRoute &r, const PassTarget &t)
{
	if(r.phase != PHASE_SCENE || t.color == nullptr || t.color != r.mainColor)
		return t;
	PassTarget s;
	s.color = r.sceneColor;
	s.depth = t.depth == r.mainDepth ? r.sceneDepth : t.depth;
	return s;
}

inline uint32_t
routeColorFormat(const FrameRoute &r, const void *color)
{
	if(color == nullptr)
		return COLORFMT_RGBA8;
	if(r.phase == PHASE_SCENE && color == r.sceneColor)
		return COLORFMT_SCENE;
	if((r.phase == PHASE_POST || r.phase == PHASE_SHOWN) && color == r.mainColor)
		return COLORFMT_OVERLAY;
	return COLORFMT_RGBA8;
}

inline int
phaseOnBeginScene(int, bool enhanced)
{
	return enhanced ? PHASE_SCENE : PHASE_CLASSIC;
}

inline int
phaseOnEndScene(int phase)
{
	return phase == PHASE_SCENE ? PHASE_POST : phase;
}

inline int
phaseOnShow(int phase)
{
	return phase == PHASE_CLASSIC ? PHASE_CLASSIC : PHASE_SHOWN;
}

inline int
phaseOnMainUpdate(int phase)
{
	return phase == PHASE_SHOWN ? PHASE_CLASSIC : phase;
}

inline int32_t
scaleEdge(int32_t v, int32_t from, int32_t to)
{
	if(from <= 0)
		return v;
	return (int32_t)(((int64_t)v*to*2 + from) / (2*(int64_t)from));
}

inline HostRect
scaleRect(HostRect r, int32_t fromW, int32_t fromH, int32_t toW, int32_t toH)
{
	HostRect s;
	s.x = scaleEdge(r.x, fromW, toW);
	s.y = scaleEdge(r.y, fromH, toH);
	s.width = scaleEdge(r.x + r.width, fromW, toW) - s.x;
	s.height = scaleEdge(r.y + r.height, fromH, toH) - s.y;
	return s;
}

}
}

#endif
