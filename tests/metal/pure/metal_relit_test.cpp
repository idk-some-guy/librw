#include <cstdio>
#include <cmath>
#include "metalrelit.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestWeight(void)
{
	CHECK(relitClampWeight(NAN) == 1.0f);
	CHECK(relitClampWeight(-0.5f) == 0.0f && relitClampWeight(1.5f) == 1.0f && relitClampWeight(0.6f) == 0.6f);
	CHECK(relitPrelitScale(0.6f, true) == 0.6f && relitPrelitScale(0.6f, false) == 1.0f);
	CHECK(Near(relitLiveShare(0.6f, true), 0.4f, 1e-7f) && relitLiveShare(0.6f, false) == 1.0f);
	CHECK(relitLiveShare(1.0f, true) == 0.0f && relitPrelitScale(1.0f, true) == 1.0f);
}

static void
TestHemisphere(void)
{
	RLColor top = { 0.2f, 0.4f, 0.8f }, bottom = { 0.6f, 0.5f, 0.4f };
	RLColor up = relitHemisphere(1.0f, top, bottom);
	RLColor down = relitHemisphere(-1.0f, top, bottom);
	RLColor side = relitHemisphere(0.0f, top, bottom);
	CHECK(Near(up.r, 0.2f, 1e-6f) && Near(up.b, 0.8f, 1e-6f));
	CHECK(Near(down.r, 0.6f, 1e-6f) && Near(down.b, 0.4f, 1e-6f));
	CHECK(Near(side.g, 0.45f, 1e-6f));
	RLColor beyond = relitHemisphere(3.0f, top, bottom);
	CHECK(Near(beyond.r, 0.2f, 1e-6f));
	RLColor c = { 0.9f, 0.3f, 0.1f };
	RLColor same = relitDesaturate(c, 1.0f);
	RLColor grey = relitDesaturate(c, 0.0f);
	CHECK(Near(same.r, 0.9f, 1e-6f) && Near(same.b, 0.1f, 1e-6f));
	CHECK(Near(grey.r, grey.g, 1e-6f) && Near(grey.g, grey.b, 1e-6f) &&
		Near(grey.r, 0.299f*0.9f + 0.587f*0.3f + 0.114f*0.1f, 1e-6f));
}

static void
TestSplit(void)
{
	RLSplit s = relitSplit(0.4f, 0.0f, 0.5f);
	CHECK(Near(s.full, 0.2f, 1e-7f) && s.sun == 0.0f);
	s = relitSplit(0.4f, 0.3f, 1.0f);
	CHECK(Near(s.full, 0.7f, 1e-6f) && Near(s.sun, 0.3f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 1.0f), 0.7f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 0.0f), 0.4f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 0.5f), 0.55f, 1e-6f));
	s = relitSplit(0.9f, 0.3f, 1.0f);
	CHECK(s.full == 1.0f && Near(s.sun, 0.1f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 0.0f), 0.9f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 2.0f), 1.0f, 1e-6f) && Near(relitShadowed(s.full, s.sun, -1.0f), 0.9f, 1e-6f));
	s = relitSplit(-0.2f, 0.1f, 1.0f);
	CHECK(s.full == 0.0f && s.sun == 0.0f);
	s = relitSplit(0.9f, 0.5f, 0.5f);
	CHECK(Near(s.full, 0.5f, 1e-6f) && Near(s.sun, 0.05f, 1e-6f));
}

static bool
Vec(RLVec3 v, float x, float y, float z, float tol)
{
	return Near(v.x, x, tol) && Near(v.y, y, tol) && Near(v.z, z, tol);
}

static void
TestFillNormal(void)
{
	RLVec3 g = { 0.0f, 0.0f, -4.0f }, up = { 0.0f, 1.0f, 0.0f };
	float c = cosf(0.6f), s = sinf(0.6f);
	RLVec3 unit = { s, 0.0f, -c };
	CHECK(Vec(relitFillNormal(unit, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	RLVec3 none = { 0.0f, 0.0f, 0.0f };
	CHECK(Vec(relitFillNormal(none, 0.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	CHECK(Vec(relitFillNormal(unit, 0.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	RLVec3 half = { 0.5f*s, 0.0f, -0.5f*c };
	CHECK(Vec(relitFillNormal(half, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	float hx = 0.5f*s, hz = -0.5f*c - 0.5f, hl = sqrtf(hx*hx + hz*hz);
	CHECK(Vec(relitFillNormal(half, 0.5f, g, up), hx/hl, 0.0f, hz/hl, 1e-6f));
	CHECK(Vec(relitFillNormal(unit, 0.5f, g, up), hx/hl, 0.0f, hz/hl, 1e-6f));
	RLVec3 fading = { 1e-4f*s, 0.0f, -1e-4f*c };
	CHECK(Vec(relitFillNormal(fading, 1e-4f, g, up), 0.0f, 0.0f, -1.0f, 2e-4f));
	RLVec3 longer = { 2.0f*s, 0.0f, -2.0f*c };
	CHECK(Vec(relitFillNormal(longer, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	CHECK(Vec(relitFillNormal(unit, 2.0f, g, up), s, 0.0f, -c, 1e-6f) && Vec(relitFillNormal(unit, -1.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	RLVec3 behind = { -s, 0.0f, c };
	CHECK(Vec(relitFillNormal(behind, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	CHECK(Vec(relitFillNormal(none, 1.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	RLVec3 flat = { 0.0f, 0.0f, 0.0f };
	CHECK(Vec(relitFillNormal(none, 0.0f, flat, up), 0.0f, 1.0f, 0.0f, 1e-6f));
	RLVec3 tiny = { 0.0f, 0.0f, -1e-20f };
	float tl = sqrtf(0.25f*s*s + 0.25f + 0.25f*c*c);
	CHECK(Vec(relitFillNormal(half, 0.5f, tiny, up), 0.5f*s/tl, 0.5f/tl, -0.5f*c/tl, 1e-6f));
}

static void
TestFillMidpoint(void)
{
	float s = sinf(30.0f*3.14159265f/180.0f), c = cosf(30.0f*3.14159265f/180.0f);
	RLVec3 mid = { 0.5f*(s - s), 0.0f, -c };
	RLVec3 g = { 0.70710678f, 0.0f, -0.70710678f }, up = { 0.0f, 1.0f, 0.0f };
	CHECK(Vec(relitFillNormal(mid, 1.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
}

int
main(void)
{
	TestWeight();
	TestHemisphere();
	TestSplit();
	TestFillNormal();
	TestFillMidpoint();
	if(failures == 0)
		printf("all relit tests passed\n");
	return failures != 0;
}
