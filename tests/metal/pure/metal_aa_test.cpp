#include <cstdio>
#include <cmath>
#include "metalaa.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestLuma(void)
{
	CHECK(aaLuma(0.0f, 0.0f, 0.0f) == 0.0f);
	CHECK(Near(aaLuma(1.0f, 1.0f, 1.0f), 1.0f, 1e-6f));
	CHECK(Near(aaLuma(1.0f, 0.0f, 0.0f), 0.299f, 1e-6f));
	CHECK(Near(aaLuma(0.0f, 1.0f, 0.0f), 0.587f, 1e-6f));
	CHECK(Near(aaLuma(0.0f, 0.0f, 1.0f), 0.114f, 1e-6f));
	for(int i = 0; i <= 255; i++){
		float g = i/255.0f;
		CHECK(Near(aaLuma(g, g, g), g, 1e-6f));
	}
	CHECK(aaLuma(5.0f, 5.0f, 5.0f) == aaLuma(1.0f, 1.0f, 1.0f));
	CHECK(aaLuma(-0.5f, 0.0f, 0.0f) == 0.0f);
}

static void
TestEdge(void)
{
	for(int i = 0; i <= 255; i++){
		float g = i/255.0f;
		CHECK(!aaIsEdge(g, g));
	}
	CHECK(aaIsEdge(0.0312f, 0.0f));
	CHECK(!aaIsEdge(0.031f, 0.0f));
	CHECK(!aaIsEdge(0.2f, 0.17f));
	CHECK(aaIsEdge(0.2f, 0.165f));
	CHECK(!aaIsEdge(0.8f, 0.71f));
	CHECK(aaIsEdge(0.8f, 0.69f));
	CHECK(!aaIsEdge(40/255.0f, 34/255.0f));
	CHECK(aaIsEdge(192/255.0f, 64/255.0f));
	CHECK(FXAA_EDGE_THRESHOLD == 0.125f && FXAA_EDGE_THRESHOLD_MIN == 0.0312f);
}

static void
TestSteps(void)
{
	float sum = 0.0f;
	for(int i = 0; i < FXAA_STEPS; i++)
		sum += aaStep(i);
	CHECK(FXAA_STEPS == 12);
	CHECK(sum == 26.5f);
	CHECK(aaStep(0) == 1.0f && aaStep(4) == 1.0f && aaStep(5) == 1.5f);
	CHECK(aaStep(6) == 2.0f && aaStep(9) == 2.0f && aaStep(10) == 4.0f && aaStep(11) == 8.0f);
}

static void
TestSubpix(void)
{
	CHECK(aaSubpixBlend(0.4f, 0.4f, 0.5f) == 0.0f);
	CHECK(aaSubpixBlend(1.0f, 0.0f, 1.0f) == 0.75f);
	CHECK(aaSubpixBlend(0.0f, 1.0f, 0.5f) == 0.75f);
	CHECK(Near(aaSubpixBlend(0.5f, 0.0f, 1.0f), 0.1875f, 1e-6f));
	CHECK(Near(aaSubpixBlend(1.0f/3.0f, 0.0f, 1.0f), 0.75f*49.0f/729.0f, 1e-6f));
	CHECK(FXAA_SUBPIX == 0.75f);
}

int
main(void)
{
	TestLuma();
	TestEdge();
	TestSteps();
	TestSubpix();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all aa tests passed\n");
	return 0;
}
