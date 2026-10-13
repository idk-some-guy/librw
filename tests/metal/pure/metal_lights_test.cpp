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
TestSpotFlag(void)
{
	CHECK(LT_SPOT == 1u);
	LTLight l = {};
	l.ids.y = LT_SPOT;
	CHECK((l.ids.y & LT_SPOT) != 0);
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

static void
TestEmissiveLuma(void)
{
	CHECK(ltEmissivePeak(0.5f, 4.0f) == 1.0f);
	CHECK(ltEmissivePeak(0.95f, 4.0f) == 4.0f);
	CHECK(ltEmissivePeak(0.0f, 4.0f) == 1.0f);
	CHECK(ltEmissivePeak(0.1f, 1.0f) == 1.0f);
	CHECK(ltEmissivePeak(0.95f, 1.0f) == 1.0f);
	CHECK(Near(ltEmissivePeak(0.75f, 4.0f), 2.5f, 1e-5f));
	CHECK(ltEmissivePeak(0.6f, 4.0f) == 1.0f);
	CHECK(ltEmissiveGate(1.0f, 0.0f, 0.0f, 4.0f) == 4.0f);
	CHECK(ltEmissiveGate(0.0f, 0.0f, 1.0f, 4.0f) == 4.0f);
	CHECK(ltEmissiveGate(1.0f, 0.2f, 0.6f, 4.0f) == 4.0f);
	CHECK(ltEmissiveGate(0.2f, 0.1f, 0.1f, 4.0f) == 1.0f);
	CHECK(ltEmissiveGate(0.5f, 0.5f, 0.5f, 4.0f) == 1.0f);
	CHECK(ltEmissiveGate(1.0f, 0.0f, 0.0f, 1.0f) == 1.0f);
	CHECK(sizeof(LTDraw) == 16);
}

static void
TestPickShadowed(void)
{
	int out[LT_MAX_SHADOWED];
	LTCand a[4] = {};
	a[0].score = 5.0f; a[0].id = 10;
	a[1].score = 1.0f; a[1].id = 11; a[1].flags = LT_CAND_PINNED;
	a[2].score = 3.0f; a[2].id = 12;
	CHECK(ltPickShadowed(a, 3, 1, out) == 1);
	CHECK(out[0] == 1);
	CHECK(ltPickShadowed(a, 3, 2, out) == 2);
	CHECK(out[0] == 1 && out[1] == 0);
	CHECK(ltPickShadowed(a, 3, 0, out) == 0);
	CHECK(ltPickShadowed(a, 0, 4, out) == 0);
	CHECK(ltPickShadowed(a, 3, 9, out) == 3);
	CHECK(out[0] == 1 && out[1] == 0 && out[2] == 2);

	LTCand g[3] = {};
	g[0].score = 4.0f; g[0].group = 7;
	g[1].score = 4.0f; g[1].group = 7;
	g[2].score = 2.0f;
	CHECK(ltPickShadowed(g, 3, 2, out) == 2);
	CHECK(out[0] == 0 && out[1] == 2);
	CHECK(ltPickShadowed(g, 3, 8, out) == 2);

	LTCand pg[3] = {};
	pg[0].flags = LT_CAND_PINNED; pg[0].group = 3; pg[0].score = 1.0f;
	pg[1].flags = LT_CAND_PINNED; pg[1].group = 3; pg[1].score = 1.0f;
	pg[2].score = 9.0f; pg[2].group = 3;
	CHECK(ltPickShadowed(pg, 3, 3, out) == 1);
	CHECK(out[0] == 0);

	LTCand t[2] = {};
	t[0].score = 10.0f;
	t[1].score = 6.0f; t[1].held = 3;
	CHECK(ltPickShadowed(t, 2, 1, out) == 1);
	CHECK(out[0] == 1);
	t[1].score = 4.0f;
	CHECK(ltPickShadowed(t, 2, 1, out) == 1);
	CHECK(out[0] == 0);
	t[1].score = 5.0f;
	CHECK(ltPickShadowed(t, 2, 1, out) == 1);
	CHECK(out[0] == 1);
	t[1].held = 0;
	t[1].score = 6.0f;
	CHECK(ltPickShadowed(t, 2, 1, out) == 1);
	CHECK(out[0] == 0);
	LTCand many[12] = {};
	int wide[LT_MAX_SHADOWED + 4];
	for(int i = 0; i < 12; i++)
		many[i].score = 1.0f + (float)i;
	CHECK(ltPickShadowed(many, 12, 12, wide) == LT_MAX_SHADOWED);
	CHECK(wide[0] == 11 && wide[LT_MAX_SHADOWED - 1] == 4);
	LTCand z[2] = {};
	z[0].score = 0.0f;
	z[1].score = NAN;
	CHECK(ltPickShadowed(z, 2, 4, out) == 0);
}

static void
TestHoldNext(void)
{
	CHECK(ltHoldNext(0, true, 30) == 30);
	CHECK(ltHoldNext(12, true, 30) == 30);
	CHECK(ltHoldNext(30, false, 30) == 29);
	CHECK(ltHoldNext(1, false, 30) == 0);
	CHECK(ltHoldNext(0, false, 30) == 0);
	CHECK(ltHoldNext(5, true, 0) == 0);
}

static void
TestRayWanted(void)
{
	LTVec3 light = {0.0f, 0.0f, 10.0f}, down = {0.0f, 0.0f, -1.0f};
	LTVec3 under = {0.0f, 0.0f, 0.0f}, side = {10.0f, 0.0f, 0.0f};
	LTVec3 far = {0.0f, 0.0f, -5.0f}, wide = {40.0f, 0.0f, 0.0f};
	float cosOuter = cosf(0.5f);
	CHECK(ltRayWanted(under, light, 20.0f, down, cosOuter, true));
	CHECK(!ltRayWanted(far, light, 14.0f, down, cosOuter, true));
	CHECK(ltRayWanted(far, light, 15.5f, down, cosOuter, true));
	CHECK(!ltRayWanted(side, light, 20.0f, down, cosOuter, true));
	CHECK(ltRayWanted(side, light, 20.0f, down, cosOuter, false));
	CHECK(!ltRayWanted(wide, light, 20.0f, down, -2.0f, false));
	CHECK(!ltRayWanted(under, light, 0.0f, down, -2.0f, false));
	CHECK(!ltRayWanted(under, light, NAN, down, -2.0f, false));
	CHECK(ltRayWanted(light, light, 5.0f, down, cosOuter, true));
	LTVec3 up = {0.0f, 0.0f, 1.0f};
	CHECK(!ltRayWanted(under, light, 20.0f, up, cosOuter, true));
	LTVec3 edgeIn = {10.0f*tanf(0.45f), 0.0f, 0.0f}, edgeOut = {10.0f*tanf(0.55f), 0.0f, 0.0f};
	CHECK(ltRayWanted(edgeIn, light, 30.0f, down, cosOuter, true));
	CHECK(!ltRayWanted(edgeOut, light, 30.0f, down, cosOuter, true));
}

static void
TestRayLength(void)
{
	CHECK(Near(ltRayLength(10.0f, 0.1f), 9.9f, 1e-6f));
	CHECK(ltRayLength(0.05f, 0.1f) == 0.0f);
	CHECK(ltRayLength(0.0f, 0.1f) == 0.0f);
	CHECK(Near(ltRayLength(10.0f, 0.0f), 10.0f, 1e-6f));
}

static void
TestDisc(void)
{
	LTVec3 d = {0.0f, 0.6f, 0.8f};
	LTVec3 r = ltDiscToward(d, 0.0f, 10.0f, 0.7f, 0.3f);
	CHECK(r.x == d.x && r.y == d.y && r.z == d.z);
	r = ltDiscToward(d, 0.5f, 0.0f, 0.7f, 0.3f);
	CHECK(r.x == d.x && r.y == d.y && r.z == d.z);
	r = ltDiscToward(d, 0.5f, 10.0f, 0.0f, 0.3f);
	CHECK(Near(r.x, d.x, 1e-6f) && Near(r.y, d.y, 1e-6f) && Near(r.z, d.z, 1e-6f));
	float maxAngle = atanf(0.5f/10.0f);
	for(int i = 0; i < 16; i++){
		float u1 = (float)i/15.0f, u2 = (float)((i*7)%16)/15.0f;
		r = ltDiscToward(d, 0.5f, 10.0f, u1, u2);
		float len = sqrtf(r.x*r.x + r.y*r.y + r.z*r.z);
		CHECK(Near(len, 1.0f, 1e-5f));
		float c = r.x*d.x + r.y*d.y + r.z*d.z;
		CHECK(acosf(fminf(c, 1.0f)) <= maxAngle + 1e-4f);
	}
	r = ltDiscToward(d, 0.5f, 10.0f, 1.0f, 0.3f);
	float c = r.x*d.x + r.y*d.y + r.z*d.z;
	CHECK(Near(acosf(fminf(c, 1.0f)), maxAngle, 1e-3f));
	LTVec3 a = ltDiscToward(d, 0.5f, 10.0f, 1.0f, 0.0f), b = ltDiscToward(d, 0.5f, 10.0f, 1.0f, 0.5f);
	CHECK(!Near(a.x, b.x, 1e-3f) || !Near(a.y, b.y, 1e-3f));
	LTVec3 ax = {1.0f, 0.0f, 0.0f};
	r = ltDiscToward(ax, 0.5f, 10.0f, 1.0f, 0.25f);
	CHECK(Near(sqrtf(r.x*r.x + r.y*r.y + r.z*r.z), 1.0f, 1e-5f));
}

int
main(void)
{
	TestFalloff();
	TestSpot();
	TestSpotFlag();
	TestSpecular();
	TestSlice();
	TestCluster();
	TestSphereBox();
	TestConeSphere();
	TestScore();
	TestBudget();
	TestEmissiveLuma();
	TestPickShadowed();
	TestHoldNext();
	TestRayWanted();
	TestRayLength();
	TestDisc();
	if(failures == 0)
		printf("all lights tests passed\n");
	return failures != 0;
}
