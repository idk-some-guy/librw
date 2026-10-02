#include <cstdio>
#include <cmath>
#include <cstring>
#include "metalnormals.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestStrip(void)
{
	uint16_t s[] = { 0, 1, 2, 3 };
	uint32_t out[18];
	CHECK(stripToList(s, 4, out) == 6);
	CHECK(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 2 && out[4] == 1 && out[5] == 3);
	uint16_t joined[] = { 0, 1, 2, 2, 5, 5, 6, 7 };
	uint32_t n = stripToList(joined, 8, out);
	CHECK(n == 6);
	CHECK(out[3] == 6 && out[4] == 5 && out[5] == 7);
	CHECK(stripToList(s, 2, out) == 0);
	uint16_t list[] = { 0, 1, 2, 3, 3, 4 };
	CHECK(listToList(list, 6, out) == 3 && out[2] == 2);
}

struct V { float x, y, z; float u, v; };

static void
TestQuadNormals(void)
{
	V v[4] = { { -1, 1, 10, 0, 0 }, { 1, 1, 10, 0, 0 }, { 1, -1, 10, 0, 0 }, { -1, -1, 10, 0, 0 } };
	uint32_t list[6] = { 0, 1, 2, 0, 2, 3 };
	float n[12];
	deriveNormals((const uint8_t*)v, sizeof(V), 0, 4, list, 6, n);
	for(int i = 0; i < 4; i++)
		CHECK(Near(n[i*3+0], 0.0f, 1e-6f) && Near(n[i*3+1], 0.0f, 1e-6f) && Near(n[i*3+2], -1.0f, 1e-6f));
}

static void
TestRidge(void)
{
	float p[6][3] = { { -1, -1, 10 }, { -1, 1, 10 }, { 0, -1, 9 }, { 0, 1, 9 }, { 1, -1, 10 }, { 1, 1, 10 } };
	uint32_t list[12] = { 0, 1, 3, 0, 3, 2, 2, 3, 4, 3, 5, 4 };
	float n[18];
	deriveNormals((const uint8_t*)p, 12, 0, 6, list, 12, n);
	CHECK(Near(n[2*3+0], 0.0f, 1e-6f) && Near(n[2*3+2], -1.0f, 1e-6f));
	CHECK(Near(fabsf(n[0*3+0]), 0.70710678f, 1e-5f) && Near(n[0*3+2], -0.70710678f, 1e-5f));
	CHECK(n[0*3+0] == -n[4*3+0]);
}

static void
TestAreaWeight(void)
{
	float p[5][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, -3, 0 } };
	uint32_t list[6] = { 0, 1, 2, 0, 3, 4 };
	float n[15];
	deriveNormals((const uint8_t*)p, 12, 0, 5, list, 6, n);
	float l = sqrtf(0.5f*0.5f + 1.5f*1.5f);
	CHECK(Near(n[0*3+2], 0.5f/l, 1e-5f) && Near(n[0*3+0], 1.5f/l, 1e-5f));
	uint32_t none[3] = { 0, 1, 2 };
	float m[15];
	for(int i = 0; i < 15; i++)
		m[i] = 7.0f;
	deriveNormals((const uint8_t*)p, 12, 0, 5, none, 3, m);
	CHECK(m[3*3+0] == 0.0f && m[3*3+1] == 0.0f && m[3*3+2] == 0.0f);
}

static void
TestOutOfRange(void)
{
	float p[5][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, -3, 0 } };
	uint32_t list[6] = { 0, 1, 2, 0, 3, 4 };
	float n[15];
	for(int i = 0; i < 15; i++)
		n[i] = 7.0f;
	deriveNormals((const uint8_t*)p, 12, 0, 4, list, 6, n);
	CHECK(Near(n[0*3+2], 1.0f, 1e-6f) && n[0*3+0] == 0.0f);
	CHECK(n[4*3+0] == 7.0f && n[4*3+1] == 7.0f && n[4*3+2] == 7.0f);
}

int
main(void)
{
	TestStrip();
	TestQuadNormals();
	TestRidge();
	TestAreaWeight();
	TestOutOfRange();
	if(failures == 0)
		printf("all normals tests passed\n");
	return failures != 0;
}
