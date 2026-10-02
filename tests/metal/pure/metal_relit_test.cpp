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
}

int
main(void)
{
	TestWeight();
	TestHemisphere();
	TestSplit();
	if(failures == 0)
		printf("all relit tests passed\n");
	return failures != 0;
}
