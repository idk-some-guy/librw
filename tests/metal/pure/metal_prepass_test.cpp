#include <cstdio>
#include <cmath>
#include "metalprepass.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
RoundTrip(float x, float y, float z)
{
	float l = sqrtf(x*x + y*y + z*z);
	PPVec2 e = ppOctEncode(x, y, z);
	PPVec3 d = ppOctDecode(e.x, e.y);
	CHECK(e.x >= -1.0f && e.x <= 1.0f && e.y >= -1.0f && e.y <= 1.0f);
	CHECK(Near(d.x, x/l, 1e-5f) && Near(d.y, y/l, 1e-5f) && Near(d.z, z/l, 1e-5f));
}

static void
TestOctahedral(void)
{
	RoundTrip(0.0f, 0.0f, 1.0f);
	RoundTrip(0.0f, 0.0f, -1.0f);
	RoundTrip(1.0f, 0.0f, 0.0f);
	RoundTrip(0.0f, -1.0f, 0.0f);
	RoundTrip(0.3f, -0.4f, -0.866f);
	RoundTrip(-0.577f, 0.577f, 0.577f);
	RoundTrip(-0.2f, -0.1f, -0.97f);
	PPVec2 zero = ppOctEncode(0.0f, 0.0f, 0.0f);
	CHECK(zero.x == 0.0f && zero.y == 0.0f);
}

static void
TestMotion(void)
{
	PPVec2 m = ppMotion(0.2f, -0.3f, 1.0f, 0.2f, -0.3f, 1.0f);
	CHECK(m.x == 0.0f && m.y == 0.0f);
	m = ppMotion(0.0f, 0.0f, 1.0f, 0.2f, 0.0f, 1.0f);
	CHECK(Near(m.x, 0.1f, 1e-7f) && m.y == 0.0f);
	m = ppMotion(0.0f, 0.0f, 2.0f, 0.0f, 0.4f, 2.0f);
	CHECK(m.x == 0.0f && Near(m.y, -0.1f, 1e-7f));
	m = ppMotion(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
	CHECK(m.x == 0.0f && m.y == 0.0f);
	m = ppMotion(0.0f, 0.0f, 1.0f, 1.0f, 1.0f, -1.0f);
	CHECK(m.x == 0.0f && m.y == 0.0f);
}

static void
TestEligible(void)
{
	PrepassDraw d = { false, true, true, true, true };
	CHECK(prepassEligible(d));
	d.blend = true;
	CHECK(!prepassEligible(d));
	d.blend = false; d.ztest = false;
	CHECK(!prepassEligible(d));
	d.ztest = true; d.zwrite = false;
	CHECK(!prepassEligible(d));
	d.zwrite = true; d.sceneDepth = false;
	CHECK(!prepassEligible(d));
	d.sceneDepth = true; d.instanced = false;
	CHECK(!prepassEligible(d));
}

static void
TestKey(void)
{
	int a, b;
	float w[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 };
	float w2[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7.5f, 1 };
	float bone[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	float bone2[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0.25f, 1 };
	CHECK(prepassKey(&a, w, nullptr) == prepassKey(&a, w, nullptr));
	CHECK(prepassKey(&a, w, nullptr) != prepassKey(&b, w, nullptr));
	CHECK(prepassKey(&a, w, nullptr) != prepassKey(&a, w2, nullptr));
	CHECK(prepassKey(&a, w, nullptr) != prepassKey(&a, w, bone));
	CHECK(prepassKey(&a, w, bone) != prepassKey(&a, w, bone2));
}

int
main(void)
{
	TestOctahedral();
	TestMotion();
	TestEligible();
	TestKey();
	if(failures == 0)
		printf("all prepass tests passed\n");
	return failures != 0;
}
