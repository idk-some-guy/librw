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
	CHECK(med.raysPerPixel == 1 && med.halfRes == 1 && med.maxHistory == 12.0f);
	CHECK(high.raysPerPixel == 1 && high.halfRes == 0 && high.maxHistory == 12.0f);
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
TestClampBoxNeeded(void)
{
	CHECK(!shClampBoxNeeded(0.5f, 0.5f) && !shClampBoxNeeded(0.0f, 0.0f) && !shClampBoxNeeded(1.0f, 1.0f));
	CHECK(shClampBoxNeeded(0.4f, 0.5f) && shClampBoxNeeded(0.6f, 0.5f));
	CHECK(shClampBoxNeeded(0.0f, 1.0f) && shClampBoxNeeded(1.0f, 0.0f));
	const float values[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
	int needed = 0, changed = 0;
	for(float prev : values)
		for(float cur : values)
			for(float lo : values)
				for(float hi : values){
					if(lo > cur || hi < cur)
						continue;
					SHAccum box = shAccumulateClamped(prev, 8.0f, cur, lo, hi, 12.0f), none = shAccumulateClamped(prev, 8.0f, cur, cur, cur, 12.0f);
					bool same = box.value == none.value && box.length == none.length;
					if(!shClampBoxNeeded(prev, cur))
						CHECK(same);
					needed += shClampBoxNeeded(prev, cur);
					changed += !same;
				}
	CHECK(needed > 0 && changed > 0);
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
	SHVec3 p = { 10.0f, 20.0f, 3.0f }, up = { 0.0f, 0.0f, 1.0f };
	CHECK(Near(shNormalOffset(0.0f), 0.005f, 1e-7f) && Near(shNormalOffset(100.0f), 0.025f, 1e-6f));
	SHVec3 o = shRayOrigin(p, up, 60.0f);
	CHECK(o.x == p.x && o.y == p.y && Near(o.z - p.z, 0.017f, 1e-6f));
	SHVec3 tilted = shNormalize({ 1.0f, 0.0f, -0.0875f }), inward = { 0.0f, 0.0f, -1.0f };
	float ds[3] = { 10.0f, 60.0f, 300.0f };
	for(int i = 0; i < 3; i++){
		float below = p.z - shRayOrigin(p, tilted, ds[i]).z;
		CHECK(below > 0.0f && below <= shNormalOffset(ds[i])*0.0872f + 1e-6f);
		CHECK(Near(p.z - shRayOrigin(p, inward, ds[i]).z, shNormalOffset(ds[i]), 1e-6f));
	}
	CHECK(shNormalOffset(300.0f) <= 0.0651f);
}

static void
TestFacesAway(void)
{
	float r = 0.016f;
	CHECK(shFacesAwayFromSun(1.0f, -0.5f, r));
	CHECK(shFacesAwayFromSun(1.0f, -r, r));
	CHECK(!shFacesAwayFromSun(1.0f, -0.5f*r, r) && !shFacesAwayFromSun(1.0f, 0.0f, r) && !shFacesAwayFromSun(1.0f, 0.7f, r));
	CHECK(!shFacesAwayFromSun(0.0f, -0.9f, r));
	CHECK(!shFacesAwayFromSun(1.0f, NAN, r));
}

static void
TestPhase(void)
{
	unsigned int seen[4] = { 0, 0, 0, 0 };
	for(unsigned int f = 0; f < 4; f++){
		unsigned int ox = shPhaseX(f), oy = shPhaseY(f);
		CHECK(ox <= 1u && oy <= 1u);
		seen[ox + 2*oy]++;
		CHECK(shPhaseX(f + 4) == ox && shPhaseY(f + 4) == oy);
	}
	CHECK(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && seen[3] == 1);
	CHECK(shRaySource(5u, 0u, 640u) == 10u && shRaySource(5u, 1u, 640u) == 11u && shRaySource(319u, 1u, 640u) == 639u);
	CHECK(shRaySource(320u, 1u, 641u) == 640u);
	for(unsigned int g = 0; g < 9; g++){
		unsigned int own = 0;
		for(unsigned int off = 0; off < 2; off++)
			if(shUpsampleTap(g, off, 0).weight == 1.0f){
				own++;
				SHTap t = shUpsampleTap(g, off, 0);
				CHECK(shRaySource(t.index, off, 640u) == g && t.weight == 1.0f && shUpsampleTap(g, off, 1).weight == 0.0f);
			}
		CHECK(own == 1u);
	}
	SHTap a = shUpsampleTap(11u, 1u, 0), b = shUpsampleTap(11u, 1u, 1);
	CHECK(a.index == 5u && a.weight == 1.0f && b.weight == 0.0f);
	a = shUpsampleTap(10u, 1u, 0);
	b = shUpsampleTap(10u, 1u, 1);
	CHECK(a.index == 4u && b.index == 5u && a.weight == 0.5f && b.weight == 0.5f);
	CHECK(shRaySource(a.index, 1u, 640u) == 9u && shRaySource(b.index, 1u, 640u) == 11u);
	a = shUpsampleTap(0u, 1u, 0);
	b = shUpsampleTap(0u, 1u, 1);
	CHECK(a.index == 0u && b.index == 0u && a.weight == 0.5f && b.weight == 0.5f && shRaySource(a.index, 1u, 640u) == 1u);
}

static void
TestUpsample(void)
{
	SHTap e0 = shUpsampleTap(10u, 0u, 0), e1 = shUpsampleTap(10u, 0u, 1), o0 = shUpsampleTap(11u, 0u, 0), o1 = shUpsampleTap(11u, 0u, 1);
	CHECK(e0.index == 5u && e0.weight == 1.0f && e1.weight == 0.0f);
	CHECK(o0.index == 5u && o1.index == 6u && o0.weight == 0.5f && o1.weight == 0.5f);
	CHECK(shUpsampleWeight(20.0f, 20.0f) > 9999.0f);
	CHECK(shUpsampleWeight(15.0f, 20.0f) < 4.01f && shUpsampleWeight(15.0f, 20.0f) > 3.99f);
	CHECK(shUpsampleWeight(15.0f, 20.0f)/shUpsampleWeight(20.0f, 20.0f) < 0.001f);
	CHECK(Near(shUpsampleWeight(20.2f, 20.0f), shUpsampleWeight(19.8f, 20.0f), 1e-3f));
}

static void
TestUpsampleNormal(void)
{
	CHECK(shUpsampleNormalWeight(1.0f, 1.0f, 1.0f) == 1.0f);
	CHECK(Near(shUpsampleNormalWeight(1.0f, 1.0f, 0.9f), 0.43046721f, 1e-6f));
	CHECK(shUpsampleNormalWeight(1.0f, 1.0f, 0.99f) > shUpsampleNormalWeight(1.0f, 1.0f, 0.9f));
	CHECK(shUpsampleNormalWeight(1.0f, 1.0f, 0.70710678f) < 0.07f);
	CHECK(shUpsampleNormalWeight(1.0f, 1.0f, 0.0f) == SH_NORMAL_WEIGHT_FLOOR && shUpsampleNormalWeight(1.0f, 1.0f, -1.0f) == SH_NORMAL_WEIGHT_FLOOR);
	CHECK(shUpsampleNormalWeight(1.0f, 1.0f, NAN) == SH_NORMAL_WEIGHT_FLOOR);
	CHECK(SH_NORMAL_WEIGHT_FLOOR > 0.0f && SH_NORMAL_WEIGHT_FLOOR <= 1e-4f);
	CHECK(shUpsampleNormalWeight(0.0f, 1.0f, 0.0f) == 1.0f && shUpsampleNormalWeight(1.0f, 0.0f, -1.0f) == 1.0f);
	CHECK(shClampTapAgrees(1.0f, 1.0f, 1.0f) && shClampTapAgrees(1.0f, 1.0f, 0.95f));
	CHECK(!shClampTapAgrees(1.0f, 1.0f, 0.9f) && !shClampTapAgrees(1.0f, 1.0f, 0.0f) && !shClampTapAgrees(1.0f, 1.0f, NAN));
	CHECK(shClampTapAgrees(0.0f, 1.0f, 0.0f) && shClampTapAgrees(1.0f, 0.0f, -1.0f));
	float wall = shUpsampleWeight(20.0f, 20.0f)*shUpsampleNormalWeight(1.0f, 1.0f, 0.0f), ground = shUpsampleWeight(20.05f, 20.0f);
	CHECK(wall/(wall + ground) < 0.01f);
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

static void
TestMeshCast(void)
{
	CHECK(shMeshCast(false, 255, true, true, true) == SH_CAST_ALPHA);
	CHECK(shMeshCast(true, 255, true, true, true) == SH_CAST_NONE);
	CHECK(shMeshCast(false, 254, true, true, true) == SH_CAST_NONE);
	CHECK(shMeshCast(false, 255, true, false, true) == SH_CAST_NONE);
	CHECK(shMeshCast(false, 255, true, true, false) == SH_CAST_NONE);
	CHECK(shMeshCast(false, 255, false, true, true) == SH_CAST_OPAQUE);
	CHECK(shMeshCast(false, 255, false, false, false) == SH_CAST_OPAQUE);
	CHECK(shMeshCast(true, 255, false, true, true) == SH_CAST_NONE);
	CHECK(shMeshCast(false, 128, false, true, true) == SH_CAST_NONE);
}

static void
TestAlphaRecord(void)
{
	CHECK(shRecordsAlphaRef(true, 1));
	CHECK(!shRecordsAlphaRef(false, 1));
	CHECK(!shRecordsAlphaRef(true, 0));
	CHECK(!shRecordsAlphaRef(true, 2));
}

static void
TestAlphaThreshold(void)
{
	CHECK(shAlphaThreshold(-1.0f) == 0.5f);
	CHECK(shAlphaThreshold(0.0f) == 0.5f);
	CHECK(shAlphaThreshold(3/255.0f) == 0.5f);
	CHECK(shAlphaThreshold(128/255.0f) == 128/255.0f);
	CHECK(shAlphaThreshold(200/255.0f) == 200/255.0f);
}

static void
TestAddress(void)
{
	CHECK(shAddressMode(1) == 0u && shAddressMode(2) == 1u && shAddressMode(3) == 2u && shAddressMode(4) == 2u && shAddressMode(0) == 0u);
	CHECK(Near(shAddress(-0.25f, 0u), 0.75f, 1e-6f) && Near(shAddress(1.25f, 0u), 0.25f, 1e-6f));
	CHECK(Near(shAddress(1.25f, 1u), 0.75f, 1e-6f) && Near(shAddress(-0.25f, 1u), 0.25f, 1e-6f));
	CHECK(Near(shAddress(0.3f, 1u), 0.3f, 1e-6f));
	CHECK(shAddress(-0.5f, 2u) == 0.0f && shAddress(1.5f, 2u) == 1.0f && Near(shAddress(0.3f, 2u), 0.3f, 1e-6f));
}

static void
TestAlphaIndex(void)
{
	CHECK(shAlphaIndexValid(0u, 0u, 1u));
	CHECK(!shAlphaIndexValid(0u, 1u, 1u));
	CHECK(shAlphaIndexValid(2u, 0u, 3u));
	CHECK(!shAlphaIndexValid(2u, 1u, 3u));
	CHECK(!shAlphaIndexValid(3u, 0u, 3u));
	CHECK(!shAlphaIndexValid(0u, 0u, 0u));
	CHECK(!shAlphaIndexValid(0xFFFFFFFFu, 0u, 4096u));
	CHECK(!shAlphaIndexValid(0xFFFFFFFEu, 3u, 4096u));
	CHECK(!shAlphaIndexValid(1u, 0xFFFFFFFFu, 4096u));
}

static void
TestBackFaces(void)
{
	CHECK(shCullsBackFaces(1.0f, -0.1f));
	CHECK(shCullsBackFaces(0.5f, -1e-6f));
	CHECK(!shCullsBackFaces(1.0f, 0.1f) && !shCullsBackFaces(1.0f, 0.0f));
	CHECK(!shCullsBackFaces(0.0f, -0.1f));
	CHECK(!shCullsBackFaces(1.0f, NAN));
}

static void
TestClassDark(void)
{
	CHECK(SH_CLASS_DARK == -0.6f);
	CHECK(shClassDark(-0.61f) && shClassDark(-1.0f));
	CHECK(!shClassDark(-0.6f) && !shClassDark(-0.59f) && !shClassDark(0.0f) && !shClassDark(0.5f));
	CHECK(!shClassDark(NAN));
}

static void
TestFacetSide(void)
{
	CHECK(shFacetPositive(0.5f, 0.49f, 0.48f, 0.51f, 0.52f, true, true));
	CHECK(!shFacetPositive(0.5f, 0.49f, 0.48f, 0.9f, 0.9f, true, true));
	CHECK(shFacetPositive(0.5f, 0.9f, 0.9f, 0.51f, 0.52f, true, true));
	CHECK(!shFacetPositive(0.5f, 0.49f, 0.48f, 0.52f, 0.55f, true, true));
	CHECK(shFacetPositive(0.5f, 0.9f, 0.9f, 0.6f, 0.7f, true, true));
	CHECK(shFacetPositive(0.5f, 0.51f, 0.40f, 0.55f, 0.60f, true, true));
	CHECK(shFacetPositive(0.5f, 0.49f, 0.48f, 0.9f, 0.9f, false, true));
	CHECK(!shFacetPositive(0.5f, 0.9f, 0.9f, 0.51f, 0.52f, true, false));
}

static void
TestFrontWinding(void)
{
	SHVec3 x = { 1.0f, 0.0f, 0.0f }, y = { 0.0f, 1.0f, 0.0f }, z = { 0.0f, 0.0f, 1.0f };
	SHVec3 mx = { -1.0f, 0.0f, 0.0f }, my = { 0.0f, -1.0f, 0.0f };
	SHVec3 r = { 0.6f, 0.8f, 0.0f }, u = { -0.8f, 0.6f, 0.0f }, big = { 0.0f, 0.0f, 4.0f };
	CHECK(!shFrontCounterClockwise(x, y, z));
	CHECK(shFrontCounterClockwise(mx, y, z) && shFrontCounterClockwise(x, my, z));
	CHECK(!shFrontCounterClockwise(mx, my, z));
	CHECK(!shFrontCounterClockwise(r, u, big) && shFrontCounterClockwise(u, r, big));
}

int
main(void)
{
	TestQuality();
	TestHash();
	TestDisc();
	TestAccumulate();
	TestClampedAccumulate();
	TestClampBoxNeeded();
	TestGates();
	TestOrigin();
	TestPhase();
	TestFacesAway();
	TestUpsample();
	TestUpsampleNormal();
	TestSunMoved();
	TestHistoryMatches();
	TestCap();
	TestMeshCast();
	TestAlphaRecord();
	TestAlphaThreshold();
	TestAddress();
	TestAlphaIndex();
	TestBackFaces();
	TestClassDark();
	TestFacetSide();
	TestFrontWinding();
	if(failures == 0)
		printf("all shadow tests passed\n");
	return failures != 0;
}
