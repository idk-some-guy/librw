#include <cstdio>
#include <cmath>
#include <cstdint>
#include "metalmodes.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

enum { U = HOSTCAND_USABLE, N = HOSTCAND_NATIVE };

static const HostModeCandidate sample[] = {
	{ 1920, 1080, 60, U },
	{ 1920, 1080, 120, U },
	{ 2560, 1440, 144, U|N },
	{ 1280, 720, 60, U },
	{ 3840, 2160, 60, U },
	{ 1600, 900, 60, 0 },
	{ 1280, 800, 60, U },
	{ 1280, 720, 60, U },
	{ 1920, 1200, 60, U },
	{ 2560, 1600, 60, U },
	{ 0, 0, 60, U },
	{ 1366, 768, 60, U },
	{ 1280, 1024, 60, U },
};
static const int32_t numSample = sizeof(sample)/sizeof(sample[0]);
static const HostMode current = { 2560, 1440, 0, 144, 5 };

static bool
Is(const HostMode &m, int32_t w, int32_t h, int32_t refresh, uint32_t flags)
{
	return m.width == w && m.height == h && m.depth == 32 && m.refresh == refresh && m.flags == flags;
}

static int32_t
Build(HostMode *out)
{
	return hostBuildModeList(sample, numSample, 2560, 1440, current, out, numSample + 2);
}

static void
TestWindowedFirst(void)
{
	HostMode out[16];
	CHECK(Build(out) == 8);
	CHECK(Is(out[0], 2560, 1440, 144, 0));
	HostMode neg = { 800, 600, 24, -1, 1 };
	CHECK(hostBuildModeList(sample, 0, 0, 0, neg, out, 4) == 1);
	CHECK(Is(out[0], 800, 600, 0, 0));
}

static void
TestUsableOnly(void)
{
	HostMode out[16];
	int32_t n = Build(out);
	for(int32_t i = 1; i < n; i++)
		CHECK(!(out[i].width == 1600 && out[i].height == 900));
}

static void
TestAtOrBelowNative(void)
{
	HostMode out[16];
	int32_t n = Build(out);
	for(int32_t i = 1; i < n; i++){
		CHECK(out[i].width <= 2560 && out[i].height <= 1440);
		CHECK(out[i].width > 0 && out[i].height > 0);
	}
	CHECK(n == 8 && Is(out[7], 2560, 1440, 144, HOSTMODE_EXCLUSIVE));
}

static void
TestDuplicates(void)
{
	HostMode out[16];
	int32_t n = Build(out);
	int count1080 = 0, count720 = 0;
	for(int32_t i = 1; i < n; i++){
		if(out[i].width == 1920 && out[i].height == 1080){
			count1080++;
			CHECK(out[i].refresh == 120);
		}
		if(out[i].width == 1280 && out[i].height == 720)
			count720++;
	}
	CHECK(count1080 == 1);
	CHECK(count720 == 1);
}

static void
TestOrder(void)
{
	HostMode out[16];
	CHECK(Build(out) == 8);
	CHECK(Is(out[1], 1280, 720, 60, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[2], 1280, 800, 60, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[3], 1280, 1024, 60, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[4], 1366, 768, 60, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[5], 1920, 1080, 120, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[6], 1920, 1200, 60, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[7], 2560, 1440, 144, HOSTMODE_EXCLUSIVE));
}

static void
TestNativeAppended(void)
{
	static const HostModeCandidate c[] = { { 1920, 1080, 60, U }, { 1280, 720, 60, U } };
	HostMode out[4];
	CHECK(hostBuildModeList(c, 2, 2560, 1440, current, out, 4) == 4);
	CHECK(Is(out[1], 1280, 720, 60, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[2], 1920, 1080, 60, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[3], 2560, 1440, 0, HOSTMODE_EXCLUSIVE));
}

static void
TestNativeUnknown(void)
{
	static const HostModeCandidate c[] = { { 3840, 2160, 60, U }, { 1280, 720, 50, U } };
	HostMode out[4];
	CHECK(hostBuildModeList(c, 2, 0, 0, current, out, 4) == 3);
	CHECK(Is(out[1], 1280, 720, 50, HOSTMODE_EXCLUSIVE));
	CHECK(Is(out[2], 3840, 2160, 60, HOSTMODE_EXCLUSIVE));
}

static void
TestCapacity(void)
{
	HostMode out[5];
	out[3].width = -7;
	out[4].width = -7;
	CHECK(hostBuildModeList(sample, numSample, 2560, 1440, current, out, 3) == 3);
	CHECK(out[3].width == -7);
	CHECK(out[4].width == -7);
	CHECK(hostBuildModeList(sample, numSample, 2560, 1440, current, out, 0) == 0);
}

static void
TestNativeSize(void)
{
	int32_t w = -1, h = -1;
	CHECK(hostNativeSize(sample, numSample, &w, &h));
	CHECK(w == 2560 && h == 1440);
	static const HostModeCandidate c[] = {
		{ 1920, 1080, 60, U }, { 2560, 1600, 60, 0 }, { 1920, 1200, 60, U }, { 0, 0, 0, N },
	};
	CHECK(hostNativeSize(c, 4, &w, &h));
	CHECK(w == 1920 && h == 1200);
	static const HostModeCandidate tie[] = { { 1600, 1200, 60, U }, { 1920, 1000, 60, U } };
	CHECK(hostNativeSize(tie, 2, &w, &h));
	CHECK(w == 1920 && h == 1000);
	CHECK(!hostNativeSize(c, 0, &w, &h));
	CHECK(w == 0 && h == 0);
}

static void
TestFindMode(void)
{
	HostMode out[16];
	int32_t n = Build(out);
	CHECK(hostFindMode(out, n, 1920, 1080) == 5);
	CHECK(hostFindMode(out, n, 1280, 720) == 1);
	CHECK(hostFindMode(out, n, 2560, 1440) == 7);
	CHECK(hostFindMode(out, n, 1600, 900) == 7);
	CHECK(hostFindMode(out, n, 3840, 2160) == 7);
	CHECK(hostFindMode(out, 1, 1280, 720) == 0);
}

static void
TestRefreshHz(void)
{
	CHECK(hostRefreshHz(60.0) == 60);
	CHECK(hostRefreshHz(59.94) == 60);
	CHECK(hostRefreshHz(119.88) == 120);
	CHECK(hostRefreshHz(29.97) == 30);
	CHECK(hostRefreshHz(23.976) == 24);
	CHECK(hostRefreshHz(0.0) == 0);
	CHECK(hostRefreshHz(-60.0) == 0);
	CHECK(hostRefreshHz(NAN) == 0);
}

int
main(void)
{
	TestWindowedFirst();
	TestUsableOnly();
	TestAtOrBelowNative();
	TestDuplicates();
	TestOrder();
	TestNativeAppended();
	TestNativeUnknown();
	TestCapacity();
	TestNativeSize();
	TestFindMode();
	TestRefreshHz();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all mode tests passed\n");
	return 0;
}
