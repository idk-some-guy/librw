#include <cstdio>
#include <cstdint>
#include "metaldrawable.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Rect(HostRect r, int32_t x, int32_t y, int32_t w, int32_t h)
{
	return r.x == x && r.y == y && r.width == w && r.height == h;
}

static void
TestWindowed(void)
{
	int32_t w, h;
	hostWindowDrawable(1280.0f, 720.0f, 2.0f, &w, &h);
	CHECK(w == 2560 && h == 1440);
	hostWindowDrawable(1280.0f, 720.0f, 1.0f, &w, &h);
	CHECK(w == 1280 && h == 720);
	hostWindowDrawable(1512.0f, 982.0f, 2.0f, &w, &h);
	CHECK(w == 3024 && h == 1964);
	hostWindowDrawable(640.5f, 480.25f, 2.0f, &w, &h);
	CHECK(w == 1281 && h == 961);
}

static void
TestWindowedUnknown(void)
{
	int32_t w, h;
	hostWindowDrawable(1280.0f, 720.0f, 0.0f, &w, &h);
	CHECK(w == 1280 && h == 720);
	hostWindowDrawable(1280.0f, 720.0f, -2.0f, &w, &h);
	CHECK(w == 1280 && h == 720);
	hostWindowDrawable(0.0f, 720.0f, 2.0f, &w, &h);
	CHECK(w == 0 && h == 1440);
	hostWindowDrawable(0.2f, 0.2f, 1.0f, &w, &h);
	CHECK(w == 1 && h == 1);
}

static void
TestRenderSize(void)
{
	int32_t w, h;
	hostRenderSize(1920, 1080, 3024, 1964, &w, &h);
	CHECK(w == 1920 && h == 1080);
	hostRenderSize(3024, 1964, 3024, 1964, &w, &h);
	CHECK(w == 3024 && h == 1964);
	hostRenderSize(3840, 2160, 3024, 1964, &w, &h);
	CHECK(w == 3024 && h == 1964);
	hostRenderSize(3024, 1965, 3024, 1964, &w, &h);
	CHECK(w == 3024 && h == 1964);
	hostRenderSize(3840, 1080, 3024, 1964, &w, &h);
	CHECK(w == 3024 && h == 1964);
	hostRenderSize(0, 0, 3024, 1964, &w, &h);
	CHECK(w == 3024 && h == 1964);
	hostRenderSize(-1, 600, 3024, 1964, &w, &h);
	CHECK(w == 3024 && h == 1964);
	hostRenderSize(1280, 720, 0, 0, &w, &h);
	CHECK(w == 1280 && h == 720);
	hostRenderSize(-5, 720, 0, 0, &w, &h);
	CHECK(w == 0 && h == 0);
}

static void
TestFit(void)
{
	CHECK(Rect(hostFitRect(1920, 1080, 3024, 1964), 0, 131, 3024, 1701));
	CHECK(Rect(hostFitRect(1280, 720, 1000, 1000), 0, 218, 1000, 563));
	CHECK(Rect(hostFitRect(1024, 768, 2560, 1440), 320, 0, 1920, 1440));
	CHECK(Rect(hostFitRect(1000, 700, 1920, 1080), 188, 0, 1543, 1080));
	CHECK(Rect(hostFitRect(2560, 1440, 2560, 1440), 0, 0, 2560, 1440));
	CHECK(Rect(hostFitRect(1280, 720, 2560, 1440), 0, 0, 2560, 1440));
}

static void
TestFitDegenerate(void)
{
	CHECK(Rect(hostFitRect(0, 720, 2560, 1440), 0, 0, 2560, 1440));
	CHECK(Rect(hostFitRect(1280, 720, 0, 1440), 0, 0, 0, 1440));
	CHECK(Rect(hostFitRect(1280, 720, -4, -4), 0, 0, 0, 0));
}

static void
TestMap(void)
{
	HostRect fit = { 320, 0, 1920, 1440 };
	float x, y;
	hostMapToRender(320.0f, 0.0f, fit, 1024, 768, &x, &y);
	CHECK(x == 0.0f && y == 0.0f);
	hostMapToRender(1280.0f, 720.0f, fit, 1024, 768, &x, &y);
	CHECK(x == 512.0f && y == 384.0f);
	hostMapToRender(2240.0f, 1440.0f, fit, 1024, 768, &x, &y);
	CHECK(x == 1024.0f && y == 768.0f);
	hostMapToRender(100.0f, 720.0f, fit, 1024, 768, &x, &y);
	CHECK(x == 0.0f && y == 384.0f);
	hostMapToRender(2500.0f, 1500.0f, fit, 1024, 768, &x, &y);
	CHECK(x == 1024.0f && y == 768.0f);
	HostRect none = { 0, 0, 0, 0 };
	hostMapToRender(50.0f, 50.0f, none, 1024, 768, &x, &y);
	CHECK(x == 0.0f && y == 0.0f);
}

int
main(void)
{
	TestWindowed();
	TestWindowedUnknown();
	TestRenderSize();
	TestFit();
	TestFitDegenerate();
	TestMap();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all drawable tests passed\n");
	return 0;
}
