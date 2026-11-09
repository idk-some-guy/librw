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

static void
TestHoldFrames(void)
{
	static_assert(LT_HOLD_FRAMES >= 2 && LT_HOLD_FRAMES <= 16, "a few frames");
	int held = ltHoldNext(0, true, LT_HOLD_FRAMES), frames = 0;
	while(held > 0){
		held = ltHoldNext(held, false, LT_HOLD_FRAMES);
		frames++;
	}
	CHECK(frames == LT_HOLD_FRAMES);
}

static void
TestInView(void)
{
	float n = 1.0f, f = 101.0f, w = 1.0f, h = 0.75f, m[16] = {};
	m[0] = n/w;
	m[5] = n/h;
	m[10] = -(f + n)/(f - n);
	m[11] = -1.0f;
	m[14] = -2.0f*f*n/(f - n);
	LTVec3 ahead = {0.0f, 0.0f, -10.0f}, behind = {0.0f, 0.0f, 10.0f}, left = {-30.0f, 0.0f, -10.0f}, beyond = {0.0f, 0.0f, -150.0f};
	CHECK(ltInView(m, ahead, 0.5f));
	CHECK(!ltInView(m, behind, 0.5f));
	CHECK(!ltInView(m, left, 5.0f));
	CHECK(ltInView(m, left, 25.0f));
	CHECK(!ltInView(m, beyond, 5.0f));
	LTVec3 edge = {12.0f, 0.0f, -10.0f};
	CHECK(!ltInView(m, edge, 0.1f));
	CHECK(ltInView(m, edge, 2.0f));
	LTVec3 above = {0.0f, 9.0f, -10.0f};
	CHECK(!ltInView(m, above, 0.5f));
	CHECK(ltInView(m, above, 3.0f));
	CHECK(ltInView(m, LTVec3{0.0f, 0.0f, -0.5f}, 1.0f));
}

static void
TestSlotReset(void)
{
	CHECK(!ltSlotReset(true, 7u, 7u));
	CHECK(ltSlotReset(true, 7u, 8u));
	CHECK(ltSlotReset(false, 7u, 7u));
	CHECK(!ltSlotReset(true, 0u, 0u));
	CHECK(ltSlotReset(false, 0u, 0u));
}

static void
TestSlotLookup(void)
{
	LTCand c[5] = { { 1.0f, 10u, 0u, 0u, 0 }, { 1.0f, 11u, 0u, 7u, 0 }, { 1.0f, 12u, 0u, 7u, 0 }, { 1.0f, 13u, 0u, 0u, 0 }, { 1.0f, 14u, 0u, 9u, 0 } };
	int picked[2] = { 1, 3 };
	CHECK(ltSlotFor(c, 5, picked, 2, 1) == 0);
	CHECK(ltSlotFor(c, 5, picked, 2, 2) == 0);
	CHECK(ltSlotFor(c, 5, picked, 2, 3) == 1);
	CHECK(ltSlotFor(c, 5, picked, 2, 0) == -1);
	CHECK(ltSlotFor(c, 5, picked, 2, 4) == -1);
	CHECK(ltSlotFor(c, 5, picked, 0, 1) == -1);
	int sparse[3] = { -1, 1, -1 };
	CHECK(ltSlotFor(c, 5, sparse, 3, 1) == 1);
	CHECK(ltSlotFor(c, 5, sparse, 3, 2) == 1);
	CHECK(ltSlotFor(c, 5, sparse, 3, 0) == -1);
	CHECK(ltSlotFor(c, 5, sparse, 3, 4) == -1);
}

static void
TestGroupSignature(void)
{
	LTCand c[5] = { { 1.0f, 21u, 0u, 5u, 0 }, { 1.0f, 20u, 0u, 5u, 0 }, { 1.0f, 22u, 0u, 0u, 0 }, { 1.0f, 30u, 0u, 6u, 0 }, { 1.0f, 19u, 0u, 5u, 0 } };
	CHECK(ltGroupSignature(c, 5, 2) == 0u);
	CHECK(ltGroupSignature(c, 5, 0) == ltGroupSignature(c, 5, 1));
	CHECK(ltGroupSignature(c, 5, 0) == ltGroupSignature(c, 5, 4));
	CHECK(ltGroupSignature(c, 5, 0) != 0u);
	CHECK(ltGroupSignature(c, 5, 3) != 0u && ltGroupSignature(c, 5, 3) != ltGroupSignature(c, 5, 0));
	LTCand order[3] = { c[4], c[0], c[1] };
	CHECK(ltGroupSignature(order, 3, 0) == ltGroupSignature(c, 5, 0));
	CHECK(ltGroupSignature(c, 2, 0) != ltGroupSignature(c, 5, 0));
	LTCand leave[2] = { c[4], c[1] };
	CHECK(ltGroupSignature(leave, 2, 0) != ltGroupSignature(order, 3, 0));
	LTCand swap[3] = { c[4], c[0], { 1.0f, 23u, 0u, 5u, 0 } };
	CHECK(ltGroupSignature(swap, 3, 0) != ltGroupSignature(order, 3, 0));
	CHECK(ltGroupIdentity(leave, 2, 0) == ltGroupIdentity(order, 3, 0) && ltGroupSignature(leave, 2, 0) != ltGroupSignature(order, 3, 0));
}

static void
TestIdentify(void)
{
	LTCand base[3] = { { 1.0f, 5u, LT_CAND_PINNED, 9u, 0 }, { 1.0f, 9u, LT_CAND_PINNED, 9u, 0 }, { 1.0f, 40u, 0u, 0u, 0 } };
	LTCand a[3] = { base[0], base[1], base[2] }, swap[3] = { base[0], { 1.0f, 12u, LT_CAND_PINNED, 9u, 0 }, base[2] };
	unsigned int ia[3], sa[3], is[3], ss[3];
	ltIdentify(a, 3, ia, sa);
	ltIdentify(swap, 3, is, ss);
	CHECK(ia[0] == 5u && ia[1] == 5u && ia[2] == 40u && is[0] == 5u && is[1] == 5u);
	CHECK(a[0].id == 5u && a[1].id == 5u && a[2].id == 40u);
	CHECK(sa[0] == sa[1] && sa[0] != 0u && sa[2] == 0u);
	CHECK(ss[1] != sa[1] && ss[0] != sa[0]);
	LTCand lone[1] = { base[0] };
	unsigned int il[1], sl[1];
	ltIdentify(lone, 1, il, sl);
	CHECK(sl[0] != 0u && sl[0] != sa[0]);

	LTCand dark[3] = { base[0], base[1], base[2] };
	dark[1].flags |= LT_CAND_EMPTY;
	unsigned int id[3], sd[3];
	ltIdentify(dark, 3, id, sd);
	CHECK(id[0] == 5u && id[1] == 5u);
	CHECK(sd[0] != sa[0] && sd[0] == sl[0]);
	CHECK(sd[1] == sa[1]);

	LTCand picked[2] = { base[0], base[1] };
	picked[0].flags |= LT_CAND_EMPTY;
	unsigned int ip[2], sp[2];
	ltIdentify(picked, 2, ip, sp);
	CHECK(sp[0] == sa[0] && sp[1] != sp[0]);
}

static void
TestHoleMask(void)
{
	int s[4] = { 2, -1, 0, -1 };
	CHECK(ltHoleMask(s, 4) == 10u);
	CHECK(ltHoleMask(s, 3) == 2u);
	CHECK(ltHoleMask(s, 0) == 0u);
	LTCand c[3] = { { 0.0f, 31u, LT_CAND_PINNED | LT_CAND_EMPTY, 0u, 0 }, { 2.0f, 32u, 0u, 0u, 0 }, { 0.0f, 33u, 0u, 0u, 0 } };
	int picked[LT_MAX_SHADOWED], slots[LT_MAX_SHADOWED];
	int count = ltPickShadowed(c, 3, 2, picked);
	int span = ltOrderSlots(slots, 2, picked, count, c, nullptr, 0, 0u);
	CHECK(count == 2 && span == 2 && slots[0] >= 0 && slots[1] >= 0);
	CHECK(ltHoleMask(slots, span) == 0u);
	unsigned int ids[2] = { 31u, 32u };
	int back[1] = { 1 };
	span = ltOrderSlots(slots, 2, back, 1, c, ids, 2, 0u);
	CHECK(span == 2 && slots[0] == -1 && slots[1] == 1 && ltHoleMask(slots, span) == 1u);
}

static void
TestRollCandidates(void)
{
	LTRoll r = {};
	ltRollCandidates(&r, 10);
	r.cur = 4;
	r.fresh = true;
	ltRollCandidates(&r, 11);
	CHECK(r.prev == 4 && r.cur == 0 && !r.fresh);
	ltRollCandidates(&r, 11);
	CHECK(r.prev == 4);
	ltRollCandidates(&r, 12);
	CHECK(r.prev == 4);
	ltRollCandidates(&r, 13);
	CHECK(r.prev == 4);
	ltRollCandidates(&r, 16);
	CHECK(r.prev == 0);

	LTRoll g = {};
	ltRollCandidates(&g, 20);
	g.cur = 3;
	g.fresh = true;
	ltRollCandidates(&g, 24);
	CHECK(g.prev == 0);

	LTRoll s = {};
	s.frame = 30;
	s.prev = 5;
	s.scene = true;
	ltRollCandidates(&s, 31);
	CHECK(s.prev == 0 && !s.scene);

	LTRoll gap = {};
	gap.frame = 40;
	gap.cur = 2;
	gap.fresh = true;
	ltRollCandidates(&gap, 41);
	CHECK(gap.prev == 2);
	ltRollCandidates(&gap, 45);
	CHECK(gap.prev == 0);
}

static void
TestRunContinues(void)
{
	CHECK(!ltRunContinues(false, 0, 5));
	CHECK(ltRunContinues(true, 5, 6));
	CHECK(ltRunContinues(true, 5, 5));
	CHECK(!ltRunContinues(true, 5, 7));
	CHECK(!ltRunContinues(true, 5, 4));
}

static void
TestEntrySlot(void)
{
	CHECK(ltEntrySlot(1, 2u, true) == 1);
	CHECK(ltEntrySlot(0, 1u, true) == 0);
	CHECK(ltEntrySlot(-1, 2u, true) == -1);
	CHECK(ltEntrySlot(2, 2u, true) == -1);
	CHECK(ltEntrySlot(1, 2u, false) == -1);
	CHECK(ltEntrySlot(0, 0u, true) == -1);
	CHECK(ltLightTextureBound(true, 1u));
	CHECK(!ltLightTextureBound(false, 1u));
	CHECK(!ltLightTextureBound(true, 0u));
}

static void
TestOrderSlots(void)
{
	LTCand c[6] = { { 1.0f, 10u, 0u, 0u, 0 }, { 1.0f, 11u, 0u, 0u, 0 }, { 1.0f, 12u, 0u, 0u, 0 }, { 1.0f, 13u, 0u, 0u, 0 }, { 1.0f, 14u, 0u, 0u, 0 }, { 1.0f, 15u, 0u, 0u, 0 } };
	int s[LT_MAX_SHADOWED];
	int a[2] = { 1, 0 };
	unsigned int prev[2] = { 10u, 11u };
	CHECK(ltOrderSlots(s, 2, a, 2, c, prev, 2, 0u) == 2);
	CHECK(s[0] == 0 && s[1] == 1);
	int b[3] = { 2, 0, 1 };
	unsigned int prevB[3] = { 10u, 11u, 12u };
	CHECK(ltOrderSlots(s, 3, b, 3, c, prevB, 3, 0u) == 3);
	CHECK(s[0] == 0 && s[1] == 1 && s[2] == 2);
	int d[3] = { 3, 1, 0 };
	CHECK(ltOrderSlots(s, 3, d, 3, c, prevB, 3, 0u) == 3);
	CHECK(s[1] == 1 && s[0] == 0 && s[2] == 3);
	int e[2] = { 4, 5 };
	CHECK(ltOrderSlots(s, 2, e, 2, c, prev, 2, 0u) == 2);
	CHECK(s[0] == 4 && s[1] == 5);
	int f[2] = { 5, 1 };
	CHECK(ltOrderSlots(s, 2, f, 2, c, prev, 2, 0u) == 2);
	CHECK(s[1] == 1 && s[0] == 5);
	int g[2] = { 0, 4 };
	unsigned int wide[4] = { 13u, 10u, 11u, 12u };
	CHECK(ltOrderSlots(s, 4, g, 2, c, wide, 4, 0u) == 2);
	CHECK(s[1] == 0 && s[0] == 4 && s[2] == -1 && s[3] == -1);
	int over[2] = { 0, 4 };
	unsigned int tall[4] = { 13u, 12u, 11u, 10u };
	CHECK(ltOrderSlots(s, 2, over, 2, c, tall, 4, 0u) == 2);
	CHECK(s[0] == 0 && s[1] == 4);
	int h[2] = { 1, 0 };
	CHECK(ltOrderSlots(s, 2, h, 2, c, nullptr, 0, 0u) == 2);
	CHECK(s[0] == 1 && s[1] == 0);
	int zero[1] = { 7 };
	CHECK(ltOrderSlots(s, 2, zero, 0, c, prev, 2, 0u) == 0);
	CHECK(s[0] == -1 && s[1] == -1);

	int mid[2] = { 0, 2 };
	CHECK(ltOrderSlots(s, 3, mid, 2, c, prevB, 3, 0u) == 3);
	CHECK(s[0] == 0 && s[1] == -1 && s[2] == 2);
	int top[1] = { 2 };
	CHECK(ltOrderSlots(s, 3, top, 1, c, prevB, 3, 0u) == 3);
	CHECK(s[0] == -1 && s[1] == -1 && s[2] == 2);
	unsigned int holed[3] = { 10u, LT_NO_ID, 12u };
	int fill[3] = { 0, 3, 2 };
	CHECK(ltOrderSlots(s, 3, fill, 3, c, holed, 3, 2u) == 3);
	CHECK(s[0] == 0 && s[1] == 3 && s[2] == 2);
	int two[2] = { 0, 3 };
	CHECK(ltOrderSlots(s, 3, two, 2, c, holed, 3, 2u) == 2);
	CHECK(s[0] == 0 && s[1] == 3 && s[2] == -1);
	int mix[3] = { 1, 4, 5 };
	CHECK(ltOrderSlots(s, 4, mix, 3, c, prevB, 3, 0u) == 3);
	CHECK(s[0] == 4 && s[1] == 1 && s[2] == 5 && s[3] == -1);
	int hole[2] = { 4, 0 };
	unsigned int gone[3] = { LT_NO_ID, 11u, 12u };
	CHECK(ltOrderSlots(s, 3, hole, 2, c, gone, 3, 1u) == 2);
	CHECK(s[0] == 4 && s[1] == 0 && s[2] == -1);
	int back[2] = { 3, 2 };
	unsigned int lowGone[3] = { LT_NO_ID, LT_NO_ID, 12u };
	CHECK(ltOrderSlots(s, 3, back, 2, c, lowGone, 3, 3u) == 3);
	CHECK(s[0] == 3 && s[1] == -1 && s[2] == 2);

	LTCand edge[2] = { { 1.0f, 10u, 0u, 0u, 0 }, { 1.0f, LT_NO_ID, 0u, 0u, 0 } };
	int liveBoth[2] = { 1, 0 };
	unsigned int liveIds[2] = { 10u, LT_NO_ID };
	CHECK(ltOrderSlots(s, 2, liveBoth, 2, edge, liveIds, 2, 0u) == 2);
	CHECK(s[0] == 0 && s[1] == 1);
	int lone[1] = { 1 };
	unsigned int holeAbove[2] = { 10u, LT_NO_ID };
	CHECK(ltOrderSlots(s, 2, lone, 1, edge, holeAbove, 2, 2u) == 1);
	CHECK(s[0] == 1 && s[1] == -1);
}

static void
TestGroupIdentity(void)
{
	LTCand c[5] = { { 1.0f, 21u, 0u, 5u, 0 }, { 1.0f, 20u, 0u, 5u, 0 }, { 1.0f, 22u, 0u, 0u, 0 }, { 1.0f, 30u, 0u, 6u, 0 }, { 1.0f, 19u, 0u, 5u, 0 } };
	CHECK(ltGroupIdentity(c, 5, 0) == 19u);
	CHECK(ltGroupIdentity(c, 5, 1) == 19u);
	CHECK(ltGroupIdentity(c, 5, 4) == 19u);
	CHECK(ltGroupIdentity(c, 5, 2) == 22u);
	CHECK(ltGroupIdentity(c, 5, 3) == 30u);
	CHECK(ltGroupIdentity(c, 2, 0) == 20u);
	LTCand swapped[2] = { c[1], c[0] };
	CHECK(ltGroupIdentity(swapped, 2, 0) == ltGroupIdentity(c, 2, 0));
	CHECK(ltGroupIdentity(swapped, 2, 1) == ltGroupIdentity(c, 2, 1));
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
	TestHoldFrames();
	TestInView();
	TestSlotReset();
	TestSlotLookup();
	TestOrderSlots();
	TestGroupIdentity();
	TestGroupSignature();
	TestIdentify();
	TestHoleMask();
	TestRollCandidates();
	TestRunContinues();
	TestEntrySlot();
	if(failures == 0)
		printf("all lights tests passed\n");
	return failures != 0;
}
