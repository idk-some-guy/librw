#include <cstdio>
#include <cmath>
#include "metalsettings.h"
#include "metalshadow.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestQuality(void)
{
	using namespace rw::metal;
	SHQuality off = shQuality(SHADOWQUALITY_OFF), low = shQuality(SHADOWQUALITY_LOW);
	SHQuality med = shQuality(SHADOWQUALITY_MEDIUM), high = shQuality(SHADOWQUALITY_HIGH);
	CHECK(off.raysPerPixel == 0);
	CHECK(low.raysPerPixel == 1 && low.halfRes == 1 && low.maxHistory == 16.0f);
	CHECK(med.raysPerPixel == 1 && med.halfRes == 0 && med.maxHistory == 12.0f);
	CHECK(high.raysPerPixel == 2 && high.halfRes == 0 && high.maxHistory == 8.0f);
	CHECK(shQuality(99).raysPerPixel == 0 && shQuality(-1).raysPerPixel == 0);
}

static void
TestHash(void)
{
	float sum = 0.0f;
	int n = 0;
	for(unsigned y = 0; y < 64; y++)
		for(unsigned x = 0; x < 64; x++){
			float h = shHash(x, y, 7, 0);
			CHECK(h >= 0.0f && h < 1.0f);
			sum += h;
			n++;
		}
	CHECK(Near(sum/n, 0.5f, 0.02f));
	CHECK(shHash(3, 4, 5, 0) != shHash(3, 4, 6, 0) && shHash(3, 4, 5, 0) != shHash(3, 4, 5, 1));
}

static void
TestDisc(void)
{
	SHVec3 sun = { 0.0f, 0.6f, 0.8f };
	float radius = 0.02f, maxAngle = 0.0f, sumX = 0.0f;
	int inner = 0;
	for(int i = 0; i < 400; i++){
		SHVec3 d = shDiscDirection(sun, radius, shHash(i, 0, 0, 0), shHash(i, 0, 0, 1));
		float l = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
		CHECK(Near(l, 1.0f, 1e-5f));
		float c = d.x*sun.x + d.y*sun.y + d.z*sun.z;
		float a = acosf(fminf(c, 1.0f));
		if(a > maxAngle)
			maxAngle = a;
		if(a < radius*0.5f)
			inner++;
		sumX += d.x;
	}
	CHECK(maxAngle <= radius*1.01f && maxAngle > radius*0.8f);
	CHECK(fabsf(sumX/400) < radius*0.2f);
	CHECK(inner >= 70 && inner <= 140);
	SHVec3 up = { 0.0f, 0.0f, 1.0f };
	SHVec3 d = shDiscDirection(up, 0.0f, 0.3f, 0.7f);
	CHECK(Near(d.z, 1.0f, 1e-6f));
}

static void
TestAccumulate(void)
{
	SHAccum a = shAccumulate(0.3f, 0.0f, 1.0f, 12.0f);
	CHECK(a.value == 1.0f && a.length == 1.0f);
	a = shAccumulate(1.0f, 1.0f, 0.0f, 12.0f);
	CHECK(Near(a.value, 0.5f, 1e-6f) && a.length == 2.0f);
	a = shAccumulate(0.5f, 12.0f, 1.0f, 12.0f);
	CHECK(Near(a.value, 0.5f + 0.5f/12.0f, 1e-6f) && a.length == 12.0f);
	CHECK(!shDisoccluded(20.0f, 20.5f) && shDisoccluded(20.0f, 23.0f));
	CHECK(!shDisoccluded(5.0f, 5.3f) && shDisoccluded(5.0f, 5.4f));
	CHECK(!shDisoccluded(20.0f, 21.13f) && shDisoccluded(21.13f, 20.0f));
	CHECK(shSunUp(0.01f) && !shSunUp(0.0f) && !shSunUp(-0.059f));
	CHECK(Near(shBias(0.0f), 0.02f, 1e-7f) && Near(shBias(100.0f), 0.17f, 1e-6f));
	CHECK(Near(shLinearDepth(0.0f, 1.0f, 101.0f), 1.0f, 1e-4f) && Near(shLinearDepth(1.0f, 1.0f, 101.0f), 101.0f, 1e-2f));
}

static void
TestClampedAccumulate(void)
{
	CHECK(shClampHistory(0.2f, 0.5f, 0.9f) == 0.5f && shClampHistory(0.95f, 0.5f, 0.9f) == 0.9f);
	CHECK(shClampHistory(0.7f, 0.5f, 0.9f) == 0.7f);
	SHAccum a = shAccumulateClamped(0.0f, 8.0f, 1.0f, 1.0f, 1.0f, 12.0f);
	CHECK(a.value == 1.0f && a.length == 1.0f);
	a = shAccumulateClamped(1.0f, 8.0f, 0.0f, 0.0f, 0.0f, 12.0f);
	CHECK(a.value == 0.0f && a.length == 1.0f);
	a = shAccumulateClamped(0.3f, 8.0f, 1.0f, 0.0f, 1.0f, 12.0f);
	SHAccum b = shAccumulate(0.3f, 8.0f, 1.0f, 12.0f);
	CHECK(a.value == b.value && a.length == b.length && a.length == 9.0f);
	a = shAccumulateClamped(0.4f, 8.0f, 0.8f, 0.6f, 1.0f, 12.0f);
	CHECK(Near(a.value, 0.6f + 0.2f/9.0f, 1e-6f) && a.length == 9.0f);
	a = shAccumulateClamped(0.25f, 8.0f, 1.0f, 0.75f, 1.0f, 12.0f);
	CHECK(a.length == 9.0f);
	a = shAccumulateClamped(0.2f, 8.0f, 1.0f, 0.75f, 1.0f, 12.0f);
	CHECK(a.value == 1.0f && a.length == 1.0f);
	a = shAccumulateClamped(0.5f, 12.0f, 0.5f, 0.0f, 1.0f, 12.0f);
	CHECK(a.length == 12.0f);
}

static void
TestGates(void)
{
	using namespace rw::metal;
	CHECK(shSourceOn(SHADOWQUALITY_LOW, true, true) && shSourceOn(SHADOWQUALITY_MEDIUM, true, true) && shSourceOn(SHADOWQUALITY_HIGH, true, true));
	CHECK(!shSourceOn(SHADOWQUALITY_OFF, true, true) && !shSourceOn(99, true, true) && !shSourceOn(-1, true, true));
	CHECK(!shSourceOn(SHADOWQUALITY_MEDIUM, false, true));
	CHECK(!shSourceOn(SHADOWQUALITY_MEDIUM, true, false));
	CHECK(shPassRuns(SHADOWQUALITY_MEDIUM, true, true, true, 0.5f, 1.0f, 1.0f));
	CHECK(shPassRuns(SHADOWQUALITY_LOW, true, true, true, 0.01f, 0.2f, 1.0f));
	CHECK(!shPassRuns(SHADOWQUALITY_OFF, true, true, true, 0.5f, 1.0f, 1.0f));
	CHECK(!shPassRuns(SHADOWQUALITY_MEDIUM, false, true, true, 0.5f, 1.0f, 1.0f));
	CHECK(!shPassRuns(SHADOWQUALITY_MEDIUM, true, false, true, 0.5f, 1.0f, 1.0f));
	CHECK(!shPassRuns(SHADOWQUALITY_MEDIUM, true, true, false, 0.5f, 1.0f, 1.0f));
	CHECK(!shPassRuns(SHADOWQUALITY_MEDIUM, true, true, true, 0.0f, 1.0f, 1.0f) && !shPassRuns(SHADOWQUALITY_MEDIUM, true, true, true, -0.5f, 1.0f, 1.0f));
	CHECK(!shPassRuns(SHADOWQUALITY_MEDIUM, true, true, true, 0.5f, 0.0f, 1.0f));
	CHECK(!shPassRuns(SHADOWQUALITY_MEDIUM, true, true, true, 0.5f, 1.0f, 0.0f));
}

static void
TestOrigin(void)
{
	SHVec3 p = { 10.0f, 20.0f, 3.0f }, v = { 0.0f, -1.0f, 0.0f };
	SHVec3 o = shRayOrigin(p, v, 40.0f);
	CHECK(o.x == p.x && o.z == p.z && Near(o.y - p.y, -shBias(40.0f), 1e-6f));
	SHVec3 flat = { 0.0f, 0.0f, 0.0f }, grazing = shNormalize({ 0.0f, -0.9887f, 0.15f });
	float ds[3] = { 10.0f, 40.0f, 150.0f };
	for(int i = 0; i < 3; i++)
		CHECK(shRayOrigin(flat, grazing, ds[i]).z > 0.0f);
}

static void
TestSunMoved(void)
{
	float r1 = 1.0f*3.14159265f/180.0f, r3 = 3.0f*3.14159265f/180.0f;
	SHVec3 a = { 0.7071f, 0.0f, 0.7071f };
	float r19 = 1.9f*3.14159265f/180.0f, r21 = 2.1f*3.14159265f/180.0f;
	SHVec3 b1 = { cosf(0.7854f + r1), 0.0f, sinf(0.7854f + r1) }, b3 = { cosf(0.7854f + r3), 0.0f, sinf(0.7854f + r3) };
	SHVec3 b19 = { cosf(0.7854f + r19), 0.0f, sinf(0.7854f + r19) }, b21 = { cosf(0.7854f + r21), 0.0f, sinf(0.7854f + r21) };
	SHVec3 back = { -0.7071f, 0.0f, -0.7071f }, longer = { 1.4142f, 0.0f, 1.4142f };
	CHECK(!shSunMoved(a, a));
	CHECK(!shSunMoved(a, longer));
	CHECK(!shSunMoved(a, b1));
	CHECK(!shSunMoved(a, b19));
	CHECK(shSunMoved(a, b21));
	CHECK(shSunMoved(a, b3));
	CHECK(shSunMoved(a, back));
}

static void
TestHistoryMatches(void)
{
	SHVec3 up = { 0.0f, 0.0f, 1.0f }, rear = { 0.0f, -1.0f, 0.0f };
	SHVec3 g = { 0.0f, 6.0f, 0.0f };
	CHECK(shDisoccluded(6.42f, 6.0f));
	CHECK(shHistoryMatches(g, g, up, 6.42f, 6.0f));
	SHVec3 far0 = { 0.0f, 200.0f, 0.0f }, far1 = { 0.0f, 210.0f, 0.0f };
	CHECK(shHistoryMatches(far1, far0, up, 210.0f, 200.0f));
	SHVec3 ground = { 0.0f, 10.0f, 0.0f }, car = { 0.0f, 8.0f, 1.2f };
	CHECK(!shHistoryMatches(car, ground, up, 7.5f, 10.0f));
	SHVec3 now = { 0.0f, 6.0f, 1.0f }, before = { 0.0f, 6.42f, 1.0f };
	CHECK(shHistoryMatches(before, now, rear, 6.0f, 6.0f));
	CHECK(!shHistoryMatches(before, now, rear, 6.6f, 6.0f));
}

static void
TestCap(void)
{
	float d2[6] = { 50.0f, 4.0f, 900.0f, 1.0f, 25.0f, 400.0f };
	uint32_t order[6];
	uint32_t n = shCapByDistance(d2, 6, 3, order);
	CHECK(n == 3 && order[0] == 3 && order[1] == 1 && order[2] == 4);
	CHECK(shCapByDistance(d2, 6, 10, order) == 6 && order[5] == 2);
	CHECK(shCapByDistance(d2, 0, 3, order) == 0);
	float tied[5] = { 5.0f, 1.0f, 5.0f, 1.0f, 5.0f };
	CHECK(shCapByDistance(tied, 5, 3, order) == 3 && order[0] == 1 && order[1] == 3 && order[2] == 0);
}

int
main(void)
{
	TestQuality();
	TestHash();
	TestDisc();
	TestAccumulate();
	TestClampedAccumulate();
	TestGates();
	TestOrigin();
	TestSunMoved();
	TestHistoryMatches();
	TestCap();
	if(failures == 0)
		printf("all shadow tests passed\n");
	return failures != 0;
}
