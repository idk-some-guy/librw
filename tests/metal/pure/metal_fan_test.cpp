#include <cstdio>
#include <cstdint>
#include <cmath>
#include "metalfan.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

struct Point
{
	float x, y;
};

static float
SignedArea(const Point &a, const Point &b, const Point &c)
{
	return (b.x - a.x)*(c.y - a.y) - (c.x - a.x)*(b.y - a.y);
}

static void
Polygon(Point *pts, int n)
{
	for(int i = 0; i < n; i++){
		float a = 6.2831853f*i/n;
		pts[i].x = cosf(a);
		pts[i].y = sinf(a);
	}
}

static void
TestFanOfN(int n)
{
	uint16_t idx[400] = {};
	Point pts[128];
	Polygon(pts, n);
	int32_t count = fanToList(idx, n);
	CHECK(count == 3*(n-2));
	CHECK(count == drawElementCount(IMPRIM_TRIFAN, n));
	CHECK(primitiveCount(IMPRIM_TRIFAN, n) == n-2);
	for(int t = 0; t < n-2; t++){
		CHECK(idx[t*3] == 0);
		CHECK(idx[t*3+1] == t+1);
		CHECK(idx[t*3+2] == t+2);
		float fanArea = SignedArea(pts[0], pts[t+1], pts[t+2]);
		float listArea = SignedArea(pts[idx[t*3]], pts[idx[t*3+1]], pts[idx[t*3+2]]);
		CHECK(fanArea > 0.0f && listArea > 0.0f);
	}
}

static void
TestFans(void)
{
	TestFanOfN(3);
	TestFanOfN(4);
	TestFanOfN(5);
	TestFanOfN(100);
}

static void
TestFanWideIndices(void)
{
	static uint32_t idx[3*70000];
	int32_t count = fanToList(idx, 70000);
	CHECK(count == 3*69998);
	CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2);
	CHECK(idx[count-3] == 0 && idx[count-2] == 69998 && idx[count-1] == 69999);
}

static void
TestIndexedFans(void)
{
	static const uint16_t quad[] = { 7, 3, 9, 1 };
	uint16_t idx[16] = {};
	int32_t count = indexedFanToList(idx, quad, 4);
	CHECK(count == 6);
	CHECK(idx[0] == 7 && idx[1] == 3 && idx[2] == 9);
	CHECK(idx[3] == 7 && idx[4] == 9 && idx[5] == 1);

	static const uint16_t five[] = { 4, 0, 1, 2, 3 };
	count = indexedFanToList(idx, five, 5);
	CHECK(count == 9);
	static const uint16_t want[] = { 4, 0, 1, 4, 1, 2, 4, 2, 3 };
	for(int i = 0; i < 9; i++)
		CHECK(idx[i] == want[i]);

	Point pts[5];
	Polygon(pts, 5);
	static const uint16_t rotated[] = { 2, 3, 4, 0, 1 };
	count = indexedFanToList(idx, rotated, 5);
	CHECK(count == 9);
	for(int t = 0; t < 3; t++){
		float fanArea = SignedArea(pts[rotated[0]], pts[rotated[t+1]], pts[rotated[t+2]]);
		float listArea = SignedArea(pts[idx[t*3]], pts[idx[t*3+1]], pts[idx[t*3+2]]);
		CHECK(fanArea > 0.0f && listArea > 0.0f);
	}
}

static void
TestDegenerateFans(void)
{
	uint16_t idx[8] = { 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA };
	static const uint16_t two[] = { 5, 6 };
	for(int n = -1; n < 3; n++){
		CHECK(fanToList(idx, n) == 0);
		CHECK(drawElementCount(IMPRIM_TRIFAN, n) == 0);
		CHECK(primitiveCount(IMPRIM_TRIFAN, n) == 0);
	}
	CHECK(indexedFanToList(idx, two, 2) == 0);
	CHECK(indexedFanToList(idx, two, 0) == 0);
	for(int i = 0; i < 8; i++)
		CHECK(idx[i] == 0xAAAA);
}

static void
TestCountsPerPrimitive(void)
{
	CHECK(drawElementCount(IMPRIM_LINELIST, 4) == 4);
	CHECK(drawElementCount(IMPRIM_LINELIST, 5) == 4);
	CHECK(drawElementCount(IMPRIM_LINELIST, 1) == 0);
	CHECK(primitiveCount(IMPRIM_LINELIST, 5) == 2);

	CHECK(drawElementCount(IMPRIM_POLYLINE, 5) == 5);
	CHECK(drawElementCount(IMPRIM_POLYLINE, 1) == 0);
	CHECK(primitiveCount(IMPRIM_POLYLINE, 5) == 4);

	CHECK(drawElementCount(IMPRIM_TRILIST, 6) == 6);
	CHECK(drawElementCount(IMPRIM_TRILIST, 8) == 6);
	CHECK(drawElementCount(IMPRIM_TRILIST, 2) == 0);
	CHECK(primitiveCount(IMPRIM_TRILIST, 8) == 2);

	CHECK(drawElementCount(IMPRIM_TRISTRIP, 5) == 5);
	CHECK(drawElementCount(IMPRIM_TRISTRIP, 2) == 0);
	CHECK(primitiveCount(IMPRIM_TRISTRIP, 5) == 3);

	CHECK(drawElementCount(IMPRIM_TRIFAN, 6) == 12);
	CHECK(primitiveCount(IMPRIM_TRIFAN, 6) == 4);

	CHECK(drawElementCount(IMPRIM_POINTLIST, 3) == 3);
	CHECK(drawElementCount(IMPRIM_POINTLIST, 0) == 0);
	CHECK(primitiveCount(IMPRIM_POINTLIST, 3) == 3);

	CHECK(drawElementCount(IMPRIM_NONE, 3) == 3);
	CHECK(primitiveCount(IMPRIM_NONE, 3) == 3);

	CHECK(drawElementCount(7, 3) == 0);
	CHECK(drawElementCount(-1, 3) == 0);
	CHECK(primitiveCount(7, 3) == 0);
	CHECK(drawElementCount(IMPRIM_TRILIST, -3) == 0);
	CHECK(drawElementCount(IMPRIM_POINTLIST, -3) == 0);
}

int
main(void)
{
	TestFans();
	TestFanWideIndices();
	TestIndexedFans();
	TestDegenerateFans();
	TestCountsPerPrimitive();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all fan tests passed\n");
	return 0;
}
