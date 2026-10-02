#include <cstdio>
#include <cmath>
#include "metaldering.h"
#include "metalsettings.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static void
TestClamp(void)
{
	CHECK(deringClamp(0.7f, 0.5f, 5.0f) == 0.7f);
	CHECK(deringClamp(0.5f, 0.5f, 5.0f) == 0.5f);
	CHECK(deringClamp(5.0f, 0.5f, 5.0f) == 5.0f);
	CHECK(deringClamp(0.44f, 0.5f, 5.0f) == 0.5f);
	CHECK(deringClamp(-1.0f, 0.5f, 5.0f) == 0.5f);
	CHECK(deringClamp(6.39f, 0.5f, 5.0f) == 5.0f);
	CHECK(deringClamp(INFINITY, 0.5f, 5.0f) == 5.0f);
	CHECK(deringClamp(-INFINITY, 0.5f, 5.0f) == 0.5f);
	CHECK(deringClamp(NAN, 0.5f, 5.0f) == 0.5f);
	CHECK(deringClamp(0.3f, 0.3f, 0.3f) == 0.3f);
	CHECK(deringClamp(0.9f, 0.3f, 0.3f) == 0.3f);
}

static void
TestTexel(void)
{
	for(int x = 0; x < 640; x++)
		CHECK(deringTexel(x, 640, 640) == x);
	for(int x = 0; x < 640; x++)
		CHECK(deringTexel(x, 640, 320) == x/2);
	CHECK(deringTexel(0, 640, 427) == 0);
	CHECK(deringTexel(639, 640, 427) == 426);
	CHECK(deringTexel(1, 640, 427) == 1);
	CHECK(deringTexel(2, 640, 427) == 1);
	CHECK(deringTexel(3, 640, 427) == 2);
	int prev = 0;
	for(int x = 0; x < 3840; x++){
		int t = deringTexel(x, 3840, 1280);
		CHECK(t >= prev && t <= prev + 1 && t < 1280);
		prev = t;
	}
	CHECK(deringTexel(5, 1, 1) == 0 && deringTexel(-3, 640, 427) == 0);
}

static void
TestFactor(int32_t factor)
{
	static const int spans[3] = { 640, 2560, 3840 };
	for(int s = 0; s < 3; s++){
		int out = spans[s], in = internalSpan(out, factor);
		int prev = 0;
		for(int x = 0; x < out; x++){
			int t = deringTexel(x, out, in);
			CHECK(t == (int)floor((x + 0.5)*in/out));
			CHECK(t >= prev && t <= prev + 1);
			prev = t;
		}
		CHECK(deringTexel(0, out, in) == 0 && prev == in - 1);
	}
}

static void
TestBalanced(void)
{
	CHECK(internalSpan(640, UPSCALE_QUALITY) == 427 && internalSpan(640, UPSCALE_BALANCED) == 376);
	CHECK(deringTexel(1, 640, 376) == 0);
	CHECK(deringTexel(2, 640, 376) == 1);
	CHECK(deringTexel(3, 640, 376) == 2);
	CHECK(deringTexel(320, 640, 376) == 188);
	CHECK(deringTexel(639, 640, 376) == 375);
	TestFactor(UPSCALE_QUALITY);
	TestFactor(UPSCALE_BALANCED);
}

int
main(void)
{
	TestClamp();
	TestTexel();
	TestBalanced();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all dering tests passed\n");
	return 0;
}
