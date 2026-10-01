#include <cstdio>
#include <cmath>
#include "metalbloom.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static void
TestKaris(void)
{
	CHECK(bloomKarisWeight(0.0f, 0.0f, 0.0f) == 1.0f);
	CHECK(fabsf(bloomKarisWeight(1.0f, 1.0f, 1.0f) - 0.5f) < 1e-6f);
	CHECK(fabsf(bloomKarisWeight(100.0f, 0.0f, 0.0f) - 1.0f/22.26f) < 1e-6f);
	float prev = 2.0f;
	for(int i = 0; i <= 64; i++){
		float x = i*0.5f;
		float w = bloomKarisWeight(x, x, x);
		CHECK(w < prev && w > 0.0f);
		CHECK(x*w < 1.0f);
		prev = w;
	}
}

static void
TestRadius(void)
{
	CHECK(bloomRadius(0.0f) == 0.5f);
	CHECK(bloomRadius(1.0f) == 1.0f);
	CHECK(bloomRadius(3.0f) == 2.0f);
	CHECK(bloomRadius(-1.0f) == 0.5f);
	CHECK(bloomRadius(9.0f) == 2.0f);
	CHECK(bloomRadius(2.0f) > bloomRadius(1.0f));
}

static void
TestLevels(void)
{
	static const int spans[BLOOM_LEVELS] = { 960, 480, 240, 120, 60, 30 };
	for(int i = 0; i < BLOOM_LEVELS; i++)
		CHECK(bloomLevelSpan(1920, i) == spans[i]);
	CHECK(bloomLevelSpan(1271, 0) == 635);
	CHECK(bloomLevelSpan(40, 5) == 1);
	CHECK(bloomLevelSpan(1, 0) == 1);
	CHECK(BLOOM_LEVELS == 6);
}

int
main(void)
{
	TestKaris();
	TestRadius();
	TestLevels();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all bloom tests passed\n");
	return 0;
}
