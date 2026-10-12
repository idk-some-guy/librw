#include <cstdio>
#include <cmath>
#include "metallights.h"

static int failures = 0;

static_assert(sizeof(LTLight) == 80, "the light record");
static_assert(sizeof(LTFrame) == 192, "the frame block");
static_assert(alignof(LTLight) == 16 && alignof(LTFrame) == 16, "the record alignment");
static_assert(LT_PER_CLUSTER*sizeof(uint16_t) == 64, "the cluster record");

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestFalloff(void)
{
	CHECK(Near(ltFalloff(0.0f, 10.0f), 1.0f/0.01f, 1e-3f));
	float d2 = 25.0f, q = d2/100.0f, w = 1.0f - q*q;
	CHECK(Near(ltFalloff(d2, 10.0f), w*w/(d2 + 0.01f), 1e-6f));
	CHECK(ltFalloff(100.0f, 10.0f) == 0.0f);
	CHECK(ltFalloff(121.0f, 10.0f) == 0.0f);
	CHECK(ltFalloff(4.0f, 0.0f) == 0.0f);
	CHECK(ltFalloff(4.0f, NAN) == 0.0f);
	float prev = ltFalloff(0.0f, 10.0f);
	bool mono = true;
	for(int i = 1; i <= 20; i++){
		float d = i*0.5f, v = ltFalloff(d*d, 10.0f);
		if(v > prev)
			mono = false;
		prev = v;
	}
	CHECK(mono);
}

static void
TestSpot(void)
{
	CHECK(Near(ltSpot(0.95f, 0.8f, 0.9f), 1.0f, 1e-6f));
	CHECK(Near(ltSpot(0.7f, 0.8f, 0.9f), 0.0f, 1e-6f));
	CHECK(Near(ltSpot(0.85f, 0.8f, 0.9f), 0.5f, 1e-6f));
	CHECK(ltSpot(-1.0f, -2.0f, 0.9f) > 0.0f);
	CHECK(ltSpot(1.0f, -2.0f, 1.0f) == 1.0f);
	CHECK(ltSpot(0.3f, -2.0f, -1.0f) == 1.0f);
}

static void
TestSpecular(void)
{
	CHECK(ltSpecular(1.0f, 0.0f) == 0.0f);
	CHECK(Near(ltSpecular(0.5f, 1.0f), powf(0.5f, 128.0f), 1e-30f));
	CHECK(Near(ltSpecular(0.9f, 0.5f), 0.5f*powf(0.9f, 68.0f), 1e-6f));
	CHECK(ltSpecular(-0.5f, 1.0f) == 0.0f);
}

static void
TestSlice(void)
{
	CHECK(ltSlice(1.0f, 1.0f, 1000.0f) == 0);
	CHECK(ltSlice(1000.0f, 1.0f, 1000.0f) == 23);
	CHECK(ltSlice(0.5f, 1.0f, 1000.0f) == 0);
	CHECK(ltSlice(5000.0f, 1.0f, 1000.0f) == 23);
	bool trip = true;
	for(unsigned k = 0; k < LT_CLUSTER_Z; k++)
		if(ltSlice(ltSliceNear(k, 1.0f, 1000.0f), 1.0f, 1000.0f) != k)
			trip = false;
	CHECK(trip);
	bool trip2 = true;
	for(unsigned k = 0; k < LT_CLUSTER_Z; k++)
		if(ltSlice(ltSliceNear(k, 0.1f, 300.0f), 0.1f, 300.0f) != k)
			trip2 = false;
	CHECK(trip2);
	CHECK(Near(ltSliceNear(0, 1.0f, 1000.0f), 1.0f, 1e-6f));
	CHECK(ltSliceNear(12, 1.0f, 1000.0f) < 40.0f);
}

static void
TestCluster(void)
{
	float w = 1600.0f, h = 900.0f;
	CHECK(ltCluster(0.0f, 0.0f, 1.0f, w, h, 1.0f, 1000.0f) == 0);
	CHECK(ltCluster(w - 0.5f, 0.0f, 1.0f, w, h, 1.0f, 1000.0f) == 15);
	CHECK(ltCluster(0.0f, h - 0.5f, 1.0f, w, h, 1.0f, 1000.0f) == 16*8);
	CHECK(ltCluster(w - 0.5f, h - 0.5f, 1000.0f, w, h, 1.0f, 1000.0f) == 15 + 16*(8 + 9*23));
	CHECK(ltCluster(w*0.5f, h*0.5f, 1.0f, w, h, 1.0f, 1000.0f) == 8 + 16*4);
	CHECK(ltCluster(-5.0f, h + 5.0f, 1.0f, w, h, 1.0f, 1000.0f) == 16*8);
	CHECK(ltCluster(100.0f, 100.0f, 1000.0f, w, h, 1.0f, 1000.0f) == 1 + 16*(1 + 9*23));
}

static void
TestSphereBox(void)
{
	LTVec3 lo = { 0, 0, 0 }, hi = { 1, 1, 1 };
	LTVec3 in = { 0.5f, 0.5f, 0.5f };
	CHECK(ltSphereHitsBox(in, 0.1f, lo, hi));
	LTVec3 face = { 2.0f, 0.5f, 0.5f };
	CHECK(ltSphereHitsBox(face, 1.0f, lo, hi));
	CHECK(!ltSphereHitsBox(face, 0.99f, lo, hi));
	LTVec3 corner = { 2.0f, 2.0f, 2.0f };
	float d = sqrtf(3.0f);
	CHECK(!ltSphereHitsBox(corner, d - 0.01f, lo, hi));
	CHECK(ltSphereHitsBox(corner, d + 0.01f, lo, hi));
}

static void
TestConeSphere(void)
{
	LTVec3 apex = { 0, 0, 0 }, dir = { 0, 0, 1 };
	float cosOuter = cosf(0.5f);
	LTVec3 axis = { 0, 0, 10 };
	CHECK(ltConeHitsSphere(apex, dir, cosOuter, 20.0f, axis, 1.0f));
	LTVec3 wide = { 10, 0, 1 };
	CHECK(!ltConeHitsSphere(apex, dir, cosOuter, 20.0f, wide, 1.0f));
	LTVec3 behind = { 0, 0, -10 };
	CHECK(!ltConeHitsSphere(apex, dir, cosOuter, 20.0f, behind, 1.0f));
	LTVec3 edge = { 10.0f*tanf(0.5f) + 0.5f, 0, 10 };
	CHECK(ltConeHitsSphere(apex, dir, cosOuter, 20.0f, edge, 1.0f));
	LTVec3 far = { 0, 0, 30 };
	CHECK(!ltConeHitsSphere(apex, dir, cosOuter, 20.0f, far, 1.0f));
}

static void
TestScore(void)
{
	CHECK(Near(ltScore(8.0f, 4.0f), 2.0f, 1e-6f));
	CHECK(Near(ltScore(8.0f, 0.25f), 8.0f, 1e-6f));
}

static void
TestBudget(void)
{
	CHECK(ltBudget(-1, 0) == 0);
	CHECK(ltBudget(-1, 1) == 0);
	CHECK(ltBudget(-1, 2) == 2);
	CHECK(ltBudget(-1, 3) == 4);
	CHECK(ltBudget(9, 3) == 8);
	CHECK(ltBudget(3, 0) == 0);
	CHECK(ltBudget(3, 2) == 3);
	CHECK(ltBudget(-5, 3) == 4);
	CHECK(ltBudget(0, 3) == 0);
}

int
main(void)
{
	TestFalloff();
	TestSpot();
	TestSpecular();
	TestSlice();
	TestCluster();
	TestSphereBox();
	TestConeSphere();
	TestScore();
	TestBudget();
	if(failures == 0)
		printf("all lights tests passed\n");
	return failures != 0;
}
