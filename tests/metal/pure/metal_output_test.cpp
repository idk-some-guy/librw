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

static void
TestLinear(void)
{
	CHECK(srgbToLinear(0.0f) == 0.0f);
	CHECK(Near(srgbToLinear(1.0f), 1.0f, 1e-6f));
	CHECK(Near(srgbToLinear(0.5f), 0.214041f, 1e-5f));
	CHECK(Near(srgbToLinear(128/255.0f), 0.215861f, 1e-5f));
	CHECK(Near(srgbToLinear(0.04045f), 0.0031308f, 1e-6f));
	CHECK(Near(srgbToLinear(0.0405f), srgbToLinear(0.04045f), 1e-5f));
}

static void
TestPeak(void)
{
	CHECK(edrPeak(1000.0f, 4.0f) == 4.0f);
	CHECK(edrPeak(1000.0f, 16.0f) == 10.0f);
	CHECK(edrPeak(400.0f, 16.0f) == 4.0f);
	CHECK(edrPeak(4000.0f, 2.5f) == 2.5f);
	CHECK(edrPeak(1000.0f, 1.0f) == 1.0f);
	CHECK(edrPeak(50.0f, 16.0f) == 1.0f);
	CHECK(edrPeak(1000.0f, NAN) == 1.0f);
	CHECK(EDR_REFERENCE_WHITE_NITS == 100.0f);
}

static void
TestEdrChannel(void)
{
	for(int i = 0; i <= 255; i++){
		float x = i/255.0f;
		CHECK(edrChannel(x, 4.0f) == srgbToLinear(x));
	}
	CHECK(Near(edrChannel(1.0f, 4.0f), 1.0f, 1e-6f));
	float h = 1e-3f;
	CHECK(Near((edrChannel(1.0f + h, 4.0f) - 1.0f)/h, EDR_SLOPE, 0.01f));
	CHECK(Near((1.0f - edrChannel(1.0f - h, 4.0f))/h, EDR_SLOPE, 0.01f));
	float prev = 1.0f;
	for(int i = 1; i <= 200; i++){
		float y = edrChannel(1.0f + i*0.25f, 4.0f);
		CHECK(y > prev && y < 4.0f);
		prev = y;
	}
	CHECK(edrChannel(1000.0f, 4.0f) > 3.95f);
	CHECK(Near(edrChannel(8.0f, 4.0f), 1.0f + 3.0f*7.0f*EDR_SLOPE/(3.0f + 7.0f*EDR_SLOPE), 1e-5f));
	CHECK(edrChannel(5.0f, 1.0f) == 1.0f);
	CHECK(edrChannel(0.5f, 1.0f) == srgbToLinear(0.5f));
	CHECK(edrChannel(1.2f, 0.5f) == 1.0f);
	CHECK(edrChannel(-0.5f, 4.0f) == 0.0f);
	CHECK(edrChannel(INFINITY, 4.0f) == 4.0f);
}

static void
TestWanted(void)
{
	CHECK(edrWanted(1, 0, 4.0f));
	CHECK(edrWanted(2, 0, 1.5f));
	CHECK(!edrWanted(0, 0, 4.0f));
	CHECK(!edrWanted(1, 1, 4.0f));
	CHECK(!edrWanted(1, 0, 1.0f));
}

static void
TestEdrBlend(void)
{
	const float pairs[][2] = { { 2.0f, 2.0f }, { 0.0f, 2.0f }, { 0.5f, 2.0f }, { 8.0f, 8.0f }, { 1.0f, 8.0f }, { 0.3f, 0.5f } };
	for(int i = 0; i < 6; i++){
		float x = pairs[i][0], m = pairs[i][1];
		float sdr = srgbToLinear(sdrShoulder(x, m));
		CHECK(edrBlend(x, m, 1.0f) == sdr);
		CHECK(edrBlend(x, m, 0.5f) == sdr);
		CHECK(Near(edrBlend(x, m, 1.0001f), sdr, 1e-3f));
		CHECK(edrBlend(x, m, 1.25f) == edrChannel(x, 1.25f));
		CHECK(edrBlend(x, m, 2.0f) == edrChannel(x, 2.0f));
		CHECK(edrBlend(x, m, 4.0f) == edrChannel(x, 4.0f));
		bool up = false, down = false;
		float prev = edrBlend(x, m, 1.0f);
		for(int j = 1; j <= 100; j++){
			float y = edrBlend(x, m, 1.0f + j*0.005f);
			up = up || y > prev + 1e-6f;
			down = down || y < prev - 1e-6f;
			prev = y;
		}
		CHECK(!(up && down));
	}
	float mid = edrBlend(0.0f, 2.0f, 1.05f);
	CHECK(mid > 0.0f && mid < srgbToLinear(sdrShoulder(0.0f, 2.0f)));
}

int
main(void)
{
	TestShoulderUnity();
	TestShoulderAbove();
	TestLinear();
	TestPeak();
	TestEdrChannel();
	TestWanted();
	TestEdrBlend();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all output tests passed\n");
	return 0;
}
