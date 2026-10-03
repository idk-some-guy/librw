#include <cstdio>
#include <cmath>
#include <utility>
#include "metalprepass.h"
#include "metalprepasskeys.h"

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
	CHECK(m.x > 1.0f && m.y > 1.0f);
	m = ppMotion(0.0f, 0.0f, 1.0f, 1.0f, 1.0f, -1.0f);
	CHECK(m.x > 1.0f && m.y > 1.0f);
	m = ppMotion(0.5f, -0.5f, 1.0f, 0.5f, -0.5f, 0.0f);
	CHECK(m.x > 1.0f && m.y > 1.0f);
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

static void
TestKeySet(void)
{
	PrepassKeySet s;
	s.reserve(4);
	CHECK(s.capacity() == 4);
	CHECK(!s.markUsed(7));
	CHECK(s.insert(7) && s.insert(0) && s.insert(7));
	CHECK(s.size() == 2 && s.unused() == 2);
	CHECK(s.markUsed(7) && s.unused() == 1);
	CHECK(!s.markUsed(8));
	CHECK(s.insert(1ull << 40) && s.insert(9));
	CHECK(!s.insert(10));
	CHECK(s.size() == 4 && !s.markUsed(10));
	CHECK(s.markUsed(0) && s.markUsed(1ull << 40) && s.markUsed(9) && s.unused() == 0);
	const void *slots = s.slotData();
	s.clear();
	CHECK(s.size() == 0 && s.unused() == 0 && !s.markUsed(7) && !s.markUsed(0));
	for(uint64_t k = 100; k < 104; k++)
		CHECK(s.insert(k*0x9e3779b97f4a7c15ull));
	CHECK(s.unused() == 4 && s.slotData() == slots);
}

static void
TestPrevMap(void)
{
	int a, b, c;
	PrevMap<float> m;
	CHECK(m.capacity() == 0 && m.put(&a) == nullptr && m.find(&a) == nullptr);
	m.reserve(2);
	CHECK(m.capacity() == 2 && m.find(&a) == nullptr);
	float *pa = m.put(&a);
	CHECK(pa != nullptr);
	*pa = 1.5f;
	CHECK(m.put(&a) == pa && m.find(&a) && *m.find(&a) == 1.5f);
	float *pb = m.put(&b);
	CHECK(pb != nullptr && pb != pa);
	*pb = 2.5f;
	CHECK(m.put(&c) == nullptr && m.find(&c) == nullptr && m.size() == 2);
	CHECK(*m.find(&a) == 1.5f && *m.find(&b) == 2.5f);
	PrevMap<float> n;
	n.reserve(2);
	const void *slots = m.slotData();
	std::swap(m, n);
	CHECK(n.find(&a) && *n.find(&a) == 1.5f && m.find(&a) == nullptr && n.slotData() == slots);
	n.clear();
	CHECK(n.size() == 0 && n.find(&a) == nullptr && n.find(&b) == nullptr && n.slotData() == slots);
	CHECK(n.put(&c) != nullptr && n.put(&a) != nullptr && n.put(&b) == nullptr);
	int keys[8];
	PrevMap<int> f;
	f.forget(&keys[0]);
	f.reserve(8);
	for(int i = 0; i < 8; i++)
		*f.put(&keys[i]) = i;
	for(int i = 0; i < 8; i += 2)
		f.forget(&keys[i]);
	f.forget(&a);
	bool kept = true;
	for(int i = 0; i < 8; i++)
		kept = kept && (i % 2 == 0 ? f.find(&keys[i]) == nullptr : f.find(&keys[i]) && *f.find(&keys[i]) == i);
	CHECK(kept && f.size() == 8 && f.put(&keys[0]) == nullptr);
	f.clear();
	CHECK(f.put(&keys[0]) != nullptr && f.find(&keys[0]) != nullptr);
}

static PPVec3
Unit(PPVec3 v)
{
	float l = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
	PPVec3 r = { v.x/l, v.y/l, v.z/l };
	return r;
}

static float
Dot(PPVec3 a, PPVec3 b)
{
	return a.x*b.x + a.y*b.y + a.z*b.z;
}

static void
TestFacing(void)
{
	PPVec3 v = { 0.0f, -1.0f, 0.0f };
	PPVec3 rim = Unit({ 0.98f, 0.199f, 0.0f });
	PPVec3 r = ppFacingNormal(rim, v);
	CHECK(Near(Dot(r, v), -Dot(rim, v), 1e-6f) && Dot(r, v) > 0.0f);
	CHECK(Near(r.x, rim.x, 1e-6f) && r.z == 0.0f && Near(Dot(r, r), 1.0f, 1e-6f));
	PPVec3 grazing = Unit({ 1.0f, 0.29f, 0.0f });
	r = ppFacingNormal(grazing, v);
	CHECK(Dot(grazing, v) > PP_FACING_REFLECT && r.x > 0.9f && Dot(r, v) > 0.0f);
	PPVec3 plane = Unit({ 0.3f, 0.9f, 0.1f });
	r = ppFacingNormal(plane, v);
	CHECK(r.x == -plane.x && r.y == -plane.y && r.z == -plane.z);
	PPVec3 below = Unit({ 1.0f, 0.32f, 0.0f });
	r = ppFacingNormal(below, v);
	CHECK(Dot(below, v) < PP_FACING_REFLECT && r.x == -below.x && r.y == -below.y);
	PPVec3 front = Unit({ 0.2f, -0.9f, 0.3f }), head = { 0.0f, -1.0f, 0.0f }, side = { 1.0f, 0.0f, 0.0f };
	r = ppFacingNormal(front, v);
	CHECK(r.x == front.x && r.y == front.y && r.z == front.z);
	r = ppFacingNormal(head, v);
	CHECK(r.x == head.x && r.y == head.y && r.z == head.z);
	r = ppFacingNormal(side, v);
	CHECK(r.x == side.x && r.y == side.y && r.z == side.z);
	PPVec3 bad = { NAN, 0.0f, 1.0f }, inf = { INFINITY, 0.0f, 0.0f };
	r = ppFacingNormal(bad, v);
	CHECK(r.x == v.x && r.y == v.y && r.z == v.z);
	r = ppFacingNormal(inf, v);
	CHECK(r.x == v.x && r.y == v.y && r.z == v.z);
	CHECK(PP_FACING_REFLECT < 0.0f && PP_FACING_REFLECT > -1.0f);
}

int
main(void)
{
	TestOctahedral();
	TestMotion();
	TestEligible();
	TestKey();
	TestKeySet();
	TestPrevMap();
	TestFacing();
	if(failures == 0)
		printf("all prepass tests passed\n");
	return failures != 0;
}
