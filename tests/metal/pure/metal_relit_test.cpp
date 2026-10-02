#include <cstdio>
#include <cmath>
#include <cstring>
#include "metalrelit.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestWeight(void)
{
	CHECK(relitClampWeight(NAN) == 1.0f);
	CHECK(relitClampWeight(-0.5f) == 0.0f && relitClampWeight(1.5f) == 1.0f && relitClampWeight(0.6f) == 0.6f);
	CHECK(relitPrelitScale(0.6f, true) == 0.6f && relitPrelitScale(0.6f, false) == 1.0f);
	CHECK(Near(relitLiveShare(0.6f, true), 0.4f, 1e-7f) && relitLiveShare(0.6f, false) == 1.0f);
	CHECK(relitLiveShare(1.0f, true) == 0.0f && relitPrelitScale(1.0f, true) == 1.0f);
	CHECK(relitSnapWeight(0.9999999f) == 1.0f && relitSnapWeight(nextafterf(1.0f, 0.0f)) == 1.0f);
	CHECK(relitSnapWeight(0.99999905f) == 1.0f && relitSnapWeight(1.0f) == 1.0f && relitSnapWeight(1.5f) == 1.0f);
	CHECK(relitSnapWeight(0.999998f) == 0.999998f && relitSnapWeight(0.6f) == 0.6f && relitSnapWeight(-0.5f) == 0.0f);
	CHECK(relitSnapWeight(NAN) == 1.0f);
}

static void
TestHemisphere(void)
{
	RLColor top = { 0.2f, 0.4f, 0.8f }, bottom = { 0.6f, 0.5f, 0.4f };
	RLColor up = relitHemisphere(1.0f, top, bottom);
	RLColor down = relitHemisphere(-1.0f, top, bottom);
	RLColor side = relitHemisphere(0.0f, top, bottom);
	CHECK(Near(up.r, 0.2f, 1e-6f) && Near(up.b, 0.8f, 1e-6f));
	CHECK(Near(down.r, 0.6f, 1e-6f) && Near(down.b, 0.4f, 1e-6f));
	CHECK(Near(side.g, 0.45f, 1e-6f));
	RLColor beyond = relitHemisphere(3.0f, top, bottom);
	CHECK(Near(beyond.r, 0.2f, 1e-6f));
	RLColor c = { 0.9f, 0.3f, 0.1f };
	RLColor same = relitDesaturate(c, 1.0f);
	RLColor grey = relitDesaturate(c, 0.0f);
	CHECK(Near(same.r, 0.9f, 1e-6f) && Near(same.b, 0.1f, 1e-6f));
	CHECK(Near(grey.r, grey.g, 1e-6f) && Near(grey.g, grey.b, 1e-6f) &&
		Near(grey.r, 0.299f*0.9f + 0.587f*0.3f + 0.114f*0.1f, 1e-6f));
}

static void
TestSplit(void)
{
	RLSplit s = relitSplit(0.4f, 0.0f, 0.5f);
	CHECK(Near(s.full, 0.2f, 1e-7f) && s.sun == 0.0f);
	s = relitSplit(0.4f, 0.3f, 1.0f);
	CHECK(Near(s.full, 0.7f, 1e-6f) && Near(s.sun, 0.3f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 1.0f), 0.7f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 0.0f), 0.4f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 0.5f), 0.55f, 1e-6f));
	s = relitSplit(0.9f, 0.3f, 1.0f);
	CHECK(s.full == 1.0f && Near(s.sun, 0.1f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 0.0f), 0.9f, 1e-6f));
	CHECK(Near(relitShadowed(s.full, s.sun, 2.0f), 1.0f, 1e-6f) && Near(relitShadowed(s.full, s.sun, -1.0f), 0.9f, 1e-6f));
	s = relitSplit(-0.2f, 0.1f, 1.0f);
	CHECK(s.full == 0.0f && s.sun == 0.0f);
	s = relitSplit(0.9f, 0.5f, 0.5f);
	CHECK(Near(s.full, 0.5f, 1e-6f) && Near(s.sun, 0.05f, 1e-6f));
}

static bool
Vec(RLVec3 v, float x, float y, float z, float tol)
{
	return Near(v.x, x, tol) && Near(v.y, y, tol) && Near(v.z, z, tol);
}

static void
TestFillNormal(void)
{
	RLVec3 g = { 0.0f, 0.0f, -4.0f }, up = { 0.0f, 1.0f, 0.0f };
	float c = cosf(0.6f), s = sinf(0.6f);
	RLVec3 unit = { s, 0.0f, -c };
	CHECK(Vec(relitFillNormal(unit, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	RLVec3 none = { 0.0f, 0.0f, 0.0f };
	CHECK(Vec(relitFillNormal(none, 0.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	CHECK(Vec(relitFillNormal(unit, 0.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	RLVec3 half = { 0.5f*s, 0.0f, -0.5f*c };
	CHECK(Vec(relitFillNormal(half, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	float hx = 0.5f*s, hz = -0.5f*c - 0.5f, hl = sqrtf(hx*hx + hz*hz);
	CHECK(Vec(relitFillNormal(half, 0.5f, g, up), hx/hl, 0.0f, hz/hl, 1e-6f));
	CHECK(Vec(relitFillNormal(unit, 0.5f, g, up), hx/hl, 0.0f, hz/hl, 1e-6f));
	RLVec3 fading = { 1e-4f*s, 0.0f, -1e-4f*c };
	CHECK(Vec(relitFillNormal(fading, 1e-4f, g, up), 0.0f, 0.0f, -1.0f, 2e-4f));
	RLVec3 longer = { 2.0f*s, 0.0f, -2.0f*c };
	CHECK(Vec(relitFillNormal(longer, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	CHECK(Vec(relitFillNormal(unit, 2.0f, g, up), s, 0.0f, -c, 1e-6f) && Vec(relitFillNormal(unit, -1.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	RLVec3 behind = { -s, 0.0f, c };
	CHECK(Vec(relitFillNormal(behind, 1.0f, g, up), s, 0.0f, -c, 1e-6f));
	CHECK(Vec(relitFillNormal(none, 1.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
	RLVec3 flat = { 0.0f, 0.0f, 0.0f };
	CHECK(Vec(relitFillNormal(none, 0.0f, flat, up), 0.0f, 1.0f, 0.0f, 1e-6f));
	RLVec3 tiny = { 0.0f, 0.0f, -1e-20f };
	float tl = sqrtf(0.25f*s*s + 0.25f + 0.25f*c*c);
	CHECK(Vec(relitFillNormal(half, 0.5f, tiny, up), 0.5f*s/tl, 0.5f/tl, -0.5f*c/tl, 1e-6f));
}

static void
TestFillMidpoint(void)
{
	float s = sinf(30.0f*3.14159265f/180.0f), c = cosf(30.0f*3.14159265f/180.0f);
	RLVec3 mid = { 0.5f*(s - s), 0.0f, -c };
	RLVec3 g = { 0.70710678f, 0.0f, -0.70710678f }, up = { 0.0f, 1.0f, 0.0f };
	CHECK(Vec(relitFillNormal(mid, 1.0f, g, up), 0.0f, 0.0f, -1.0f, 1e-6f));
}

static void
TestWeighted(void)
{
	const float cs[] = { 0.0f, 1e-8f, 0.1f, 140.0f/255.0f, 0.99999994f, 1.0f, 3.0f };
	for(float c : cs){
		float r = relitWeighted(c, 1.0f);
		CHECK(memcmp(&r, &c, sizeof(r)) == 0);
	}
	CHECK(relitWeighted(0.6f, 0.5f) == 0.3f && relitWeighted(0.6f, 0.0f) == 0.0f);
}

static void
TestSplitAdd(void)
{
	const float bs[] = { 0.0f, 0.3f, 0.9f, 1.0f, 1.4f }, hs[] = { 0.0f, 0.05f, 0.5f, 2.0f }, ss[] = { 0.0f, 0.05f, 0.5f, 2.0f },
		ms[] = { 0.0f, 0.5f, 1.0f };
	for(float b : bs)
		for(float h : hs)
			for(float s : ss)
				for(float m : ms){
					RLSplit a = relitSplitAdd(fminf(fmaxf(b, 0.0f), 1.0f)*m, h, s, m), r = relitSplit(b + h, s, m);
					CHECK(Near(a.full, r.full, 1e-6f) && Near(a.sun, r.sun, 1e-6f));
				}
	const float over[] = { 0.5f, 0.50000006f, 0.2f };
	for(float c : over){
		RLSplit z = relitSplitAdd(c, 0.0f, 0.0f, 0.5f);
		CHECK(memcmp(&z.full, &c, sizeof(c)) == 0 && z.sun == 0.0f);
	}
}

static void
TestSunCos(void)
{
	CHECK(relitSunCos(-0.4f, false) == 0.0f && relitSunCos(-0.4f, true) == 0.4f);
	CHECK(relitSunCos(0.7f, false) == 0.7f && relitSunCos(0.7f, true) == 0.7f);
}

static void
Cross(const double a[3], const double b[3], double out[3])
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

static void
Rotation(double deg, double x, double y, double z, float m[12])
{
	double l = sqrt(x*x + y*y + z*z), c = cos(deg*M_PI/180.0), s = sin(deg*M_PI/180.0);
	x /= l;
	y /= l;
	z /= l;
	double r[9] = {
		c + x*x*(1-c), y*x*(1-c) + z*s, z*x*(1-c) - y*s,
		x*y*(1-c) - z*s, c + y*y*(1-c), z*y*(1-c) + x*s,
		x*z*(1-c) + y*s, y*z*(1-c) - x*s, c + z*z*(1-c),
	};
	for(int i = 0; i < 9; i++)
		m[i] = (float)r[i];
	m[9] = m[10] = m[11] = 0.0f;
}

static void
TestCofactor(void)
{
	float ms[6][12] = {
		{ 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 },
		{},
		{ 2, 0, 0, 0, 0.5f, 0, 0, 0, 3, 0, 0, 0 },
		{ -1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 },
		{ 1, 0, 0, 0.7f, 1, 0, -0.3f, 0.4f, 1, 0, 0, 0 },
		{},
	};
	Rotation(30.0, 1, 2, 3, ms[1]);
	Rotation(-75.0, 0.2, -1, 0.5, ms[5]);
	ms[5][9] = 2000.0f;
	ms[5][10] = -2000.0f;
	ms[5][11] = 2000.0f;
	const float edges[4][2][3] = {
		{ { 1, 0, 0 }, { 0, 1, 0 } },
		{ { 0.3f, -2, 0.5f }, { 1.5f, 0.25f, -0.75f } },
		{ { 10, 0.01f, 3 }, { -4, 2, 0.002f } },
		{ { 0.001f, 0.002f, 0 }, { 0, 0.001f, 0.003f } },
	};
	for(int i = 0; i < 6; i++){
		const float *m = ms[i];
		float k[9];
		relitCofactor(m, m + 3, m + 6, k);
		for(int e = 0; e < 4; e++){
			const float *a = edges[e][0], *b = edges[e][1];
			double p0[3], p1[3], p2[3], ma[3], mb[3], want[3];
			for(int r = 0; r < 3; r++){
				p0[r] = m[9 + r];
				p1[r] = m[9 + r] + (double)m[r]*a[0] + (double)m[3 + r]*a[1] + (double)m[6 + r]*a[2];
				p2[r] = m[9 + r] + (double)m[r]*b[0] + (double)m[3 + r]*b[1] + (double)m[6 + r]*b[2];
				ma[r] = p1[r] - p0[r];
				mb[r] = p2[r] - p0[r];
			}
			Cross(ma, mb, want);
			float ab[3] = { a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0] };
			float got[3];
			for(int r = 0; r < 3; r++)
				got[r] = k[r]*ab[0] + k[3 + r]*ab[1] + k[6 + r]*ab[2];
			double lw = sqrt(want[0]*want[0] + want[1]*want[1] + want[2]*want[2]);
			double lg = sqrt((double)got[0]*got[0] + (double)got[1]*got[1] + (double)got[2]*got[2]);
			double tol = 1e-5*(lw > lg ? lw : lg);
			for(int r = 0; r < 3; r++)
				if(fabs(got[r] - want[r]) > tol){
					printf("  matrix %d edges %d component %d: %g, want %g\n", i, e, r, got[r], want[r]);
					failures++;
				}
		}
	}
}

int
main(void)
{
	TestWeight();
	TestHemisphere();
	TestSplit();
	TestFillNormal();
	TestFillMidpoint();
	TestWeighted();
	TestSplitAdd();
	TestSunCos();
	TestCofactor();
	if(failures == 0)
		printf("all relit tests passed\n");
	return failures != 0;
}
