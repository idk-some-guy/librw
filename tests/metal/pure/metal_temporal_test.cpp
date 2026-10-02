#include <cstdio>
#include <cmath>
#include "metaltemporal.h"

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Near(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static void
TestHalton(void)
{
	const float base2[8] = { 0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f };
	const float base3[8] = { 1.0f/3, 2.0f/3, 1.0f/9, 4.0f/9, 7.0f/9, 2.0f/9, 5.0f/9, 8.0f/9 };
	for(int i = 0; i < 8; i++){
		CHECK(Near(tmHalton(i+1, 2), base2[i], 1e-6f));
		CHECK(Near(tmHalton(i+1, 3), base3[i], 1e-6f));
	}
	CHECK(tmHalton(0, 2) == 0.0f);
}

static void
TestJitter(void)
{
	float sx = 0.0f, sy = 0.0f;
	for(unsigned f = 0; f < TM_JITTER_PHASES; f++){
		TMJitter j = tmJitter(f);
		TMJitter k = tmJitter(f + TM_JITTER_PHASES);
		CHECK(j.x >= -0.5f && j.x < 0.5f && j.y >= -0.5f && j.y < 0.5f);
		CHECK(j.x == k.x && j.y == k.y);
		sx += j.x;
		sy += j.y;
	}
	CHECK(fabsf(sx/TM_JITTER_PHASES) < 0.1f && fabsf(sy/TM_JITTER_PHASES) < 0.1f);
	TMJitter a = tmJitter(0), b = tmJitter(1);
	CHECK(a.x != b.x || a.y != b.y);
}

static void
Project(const float p[16], float x, float y, float z, int w, int h, float *px, float *py)
{
	float cx = p[0]*x + p[4]*y + p[8]*z + p[12];
	float cy = p[1]*x + p[5]*y + p[9]*z + p[13];
	float cw = p[3]*x + p[7]*y + p[11]*z + p[15];
	*px = (cx/cw*0.5f + 0.5f)*w;
	*py = (0.5f - cy/cw*0.5f)*h;
}

static void
TestJitterProjection(void)
{
	float n = 1.0f, f = 101.0f;
	float base[16] = { 1.0f, 0, 0, 0, 0, 1.0f/0.75f, 0, 0, 0, 0, (f+n)/(f-n), 1.0f, 0, 0, -2.0f*n*f/(f-n), 0 };
	float jit[16];
	for(int i = 0; i < 16; i++)
		jit[i] = base[i];
	tmJitterProjection(jit, 0.25f, -0.375f, 640, 480);
	float x0, y0, x1, y1;
	Project(base, 1.5f, -2.0f, 10.0f, 640, 480, &x0, &y0);
	Project(jit, 1.5f, -2.0f, 10.0f, 640, 480, &x1, &y1);
	CHECK(Near(x1 - x0, 0.25f, 1e-3f));
	CHECK(Near(y1 - y0, -0.375f, 1e-3f));
	Project(jit, -3.0f, 4.0f, 60.0f, 640, 480, &x1, &y1);
	Project(base, -3.0f, 4.0f, 60.0f, 640, 480, &x0, &y0);
	CHECK(Near(x1 - x0, 0.25f, 1e-3f) && Near(y1 - y0, -0.375f, 1e-3f));
	CHECK(jit[10] == base[10] && jit[14] == base[14] && jit[11] == base[11]);
}

static void
TestUnjitter(void)
{
	float n = 1.0f, f = 101.0f;
	float base[16] = { 1.0f, 0, 0, 0, 0, 1.0f/0.75f, 0, 0, 0, 0, (f+n)/(f-n), 1.0f, 0, 0, -2.0f*n*f/(f-n), 0 };
	float jit[16];
	const float jx[2] = { 0.25f, -0.4375f }, jy[2] = { -0.375f, 0.3889f };
	for(int k = 0; k < 2; k++){
		for(int i = 0; i < 16; i++)
			jit[i] = base[i];
		tmJitterProjection(jit, jx[k], jy[k], 640, 480);
		float x0, y0, x1, y1;
		Project(base, 1.5f, -2.0f, 10.0f, 640, 480, &x0, &y0);
		Project(jit, 1.5f, -2.0f, 10.0f, 640, 480, &x1, &y1);
		TMUv uv = tmUnjitterUv(x0 - 0.5f, y0 - 0.5f, jx[k], jy[k], 640, 480);
		CHECK(Near(uv.u*640.0f, x1, 1e-3f) && Near(uv.v*480.0f, y1, 1e-3f));
	}
	TMUv still = tmUnjitterUv(10.0f, 20.0f, 0.0f, 0.0f, 640, 480);
	CHECK(Near(still.u, 10.5f/640.0f, 1e-7f) && Near(still.v, 20.5f/480.0f, 1e-7f));
}

static void
TestColour(void)
{
	TMColor c = { 0.2f, 0.6f, 0.9f };
	TMColor r = tmFromYCoCg(tmToYCoCg(c));
	CHECK(Near(r.r, c.r, 1e-6f) && Near(r.g, c.g, 1e-6f) && Near(r.b, c.b, 1e-6f));
	TMColor hot = { 5.0f, 0.5f, 0.5f };
	TMColor back = tmExpand(tmCompress(hot));
	CHECK(Near(back.r, 5.0f, 1e-4f) && Near(back.g, 0.5f, 1e-5f) && Near(back.b, 0.5f, 1e-5f));
	TMColor small = tmCompress(hot);
	CHECK(small.r < 5.0f && small.r > 0.0f);
	TMColor mn = { 0.0f, -0.1f, -0.1f }, mx = { 1.0f, 0.1f, 0.1f };
	TMColor inside = { 0.5f, 0.0f, 0.05f };
	TMColor kept = tmClipAabb(inside, mn, mx);
	CHECK(kept.r == inside.r && kept.g == inside.g && kept.b == inside.b);
	TMColor outside = { 2.0f, 0.0f, 0.0f };
	TMColor clipped = tmClipAabb(outside, mn, mx);
	CHECK(Near(clipped.r, 1.0f, 1e-5f) && Near(clipped.g, 0.0f, 1e-6f));
	TMColor m = tmMix(mn, mx, 0.25f);
	CHECK(Near(m.r, 0.25f, 1e-6f) && Near(m.g, -0.05f, 1e-6f));
}

static void
TestSaturatedHues(void)
{
	TMColor red = { 100.0f, 0.0f, 0.0f }, blue = { 0.0f, 0.0f, 100.0f };
	TMColor a = tmToYCoCg(tmCompress(red)), b = tmToYCoCg(tmCompress(blue));
	TMColor mn = tmMinC(a, b), mx = tmMaxC(a, b);
	TMColor corners[2] = { { mx.r, mx.g, mn.b }, { mn.r, mn.g, mn.b } };
	for(int i = 0; i < 2; i++){
		TMColor h = tmFromYCoCg(tmClipAabb(corners[i], mn, mx));
		TMColor cur = tmCompress(red);
		for(int k = 0; k < 3; k++){
			TMColor r = tmExpand(tmMix(cur, h, k*0.45f));
			CHECK(r.r >= 0.0f && r.g >= 0.0f && r.b >= 0.0f);
			CHECK(tmLuma(r) <= 64.0f);
		}
	}
	TMColor over = { 5.0f, 0.0f, 2.0f };
	TMColor e = tmExpand(over);
	CHECK(tmLuma(e) <= 64.0f && e.r > 0.0f);
}

static void
TestHistory(void)
{
	CHECK(tmHistoryValid(0.0f, 1.0f) && tmHistoryValid(0.5f, 0.5f));
	CHECK(!tmHistoryValid(-0.001f, 0.5f) && !tmHistoryValid(0.5f, 1.001f));
	CHECK(tmHistoryWeight(0.0f, false) == 0.0f);
	CHECK(Near(tmHistoryWeight(0.0f, true), 1.0f - TM_HISTORY_ALPHA, 1e-7f));
	CHECK(tmHistoryWeight(1.0f, true) == 0.0f);
	CHECK(Near(tmHistoryWeight(0.5f, true), 0.5f*(1.0f - TM_HISTORY_ALPHA), 1e-7f));
	CHECK(tmHistoryWeight(2.0f, true) == 0.0f);
	CHECK(!tmCameraCut(3.0f, 4.0f, 0.0f) && tmCameraCut(0.0f, 0.0f, 10.5f));
}

int
main(void)
{
	TestHalton();
	TestJitter();
	TestJitterProjection();
	TestUnjitter();
	TestColour();
	TestSaturatedHues();
	TestHistory();
	if(failures == 0)
		printf("all temporal tests passed\n");
	return failures != 0;
}
