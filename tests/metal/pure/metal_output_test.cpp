#include <cstdio>
#include <cmath>
#include "metaloutput.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestShoulderUnity(void)
{
	for(int i = 0; i <= 255; i++){
		float x = i/255.0f;
		CHECK(sdrShoulder(x, 1.0f) == x);
		CHECK(sdrShoulder(x, x) == x);
		CHECK(sdrShoulder(x, 0.5f) == x);
	}
	CHECK(sdrShoulder(1.5f, 1.0f) == 1.5f);
}

static void
TestShoulderAbove(void)
{
	CHECK(sdrShoulder(2.0f, 2.0f) == 1.0f);
	CHECK(Near(sdrShoulder(0.5f, 2.0f), 0.75f, 1e-6f));
	CHECK(Near(sdrShoulder(0.2f, 2.0f), 0.6f, 1e-6f));
	CHECK(Near(sdrShoulder(0.0f, 3.0f), 2.0f/3.0f, 1e-6f));
	float prev = 0.0f;
	for(int i = 0; i <= 100; i++){
		float m = 1.0f + i*0.1f;
		float y = sdrShoulder(0.3f, m);
		CHECK(y >= prev && y <= 1.0f);
		prev = y;
	}
	CHECK(sdrShoulder(0.3f, 1000.0f) < 1.0f);
	CHECK(SDR_SHOULDER_KNEE == 1.0f);
}

int
main(void)
{
	TestShoulderUnity();
	TestShoulderAbove();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all output tests passed\n");
	return 0;
}
