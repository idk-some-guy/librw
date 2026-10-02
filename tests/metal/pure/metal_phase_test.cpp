#include <cstdio>
#include <initializer_list>
#include "metalphase.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static int mainC, mainD, sceneC, sceneD, other;

static FrameRoute
Route(int phase)
{
	FrameRoute r = { phase, &mainC, &mainD, &sceneC, &sceneD };
	return r;
}

static void
TestRoute(void)
{
	PassTarget main = { &mainC, &mainD };
	PassTarget colourOnly = { &mainC, nullptr };
	PassTarget otherT = { &other, &mainD };
	PassTarget depthOnly = { nullptr, &mainD };
	PassTarget t = routeTarget(Route(PHASE_SCENE), main);
	CHECK(t.color == &sceneC && t.depth == &sceneD);
	t = routeTarget(Route(PHASE_SCENE), colourOnly);
	CHECK(t.color == &sceneC && t.depth == nullptr);
	t = routeTarget(Route(PHASE_SCENE), otherT);
	CHECK(t.color == &other && t.depth == &mainD);
	t = routeTarget(Route(PHASE_SCENE), depthOnly);
	CHECK(t.color == nullptr && t.depth == &mainD);
	for(int p : { (int)PHASE_CLASSIC, (int)PHASE_POST, (int)PHASE_SHOWN }){
		t = routeTarget(Route(p), main);
		CHECK(t.color == &mainC && t.depth == &mainD);
	}
}

static void
TestFormats(void)
{
	CHECK(routeColorFormat(Route(PHASE_SCENE), &sceneC) == COLORFMT_SCENE);
	CHECK(routeColorFormat(Route(PHASE_SCENE), &mainC) == COLORFMT_RGBA8);
	CHECK(routeColorFormat(Route(PHASE_POST), &mainC) == COLORFMT_OVERLAY);
	CHECK(routeColorFormat(Route(PHASE_SHOWN), &mainC) == COLORFMT_OVERLAY);
	CHECK(routeColorFormat(Route(PHASE_CLASSIC), &mainC) == COLORFMT_RGBA8);
	CHECK(routeColorFormat(Route(PHASE_POST), &other) == COLORFMT_RGBA8);
	CHECK(routeColorFormat(Route(PHASE_SCENE), nullptr) == COLORFMT_RGBA8);
}

static void
TestPhases(void)
{
	CHECK(phaseOnBeginScene(PHASE_CLASSIC, true) == PHASE_SCENE);
	CHECK(phaseOnBeginScene(PHASE_SHOWN, true) == PHASE_SCENE);
	CHECK(phaseOnBeginScene(PHASE_SHOWN, false) == PHASE_CLASSIC);
	CHECK(phaseOnEndScene(PHASE_SCENE) == PHASE_POST);
	CHECK(phaseOnEndScene(PHASE_CLASSIC) == PHASE_CLASSIC);
	CHECK(phaseOnEndScene(PHASE_POST) == PHASE_POST);
	CHECK(phaseOnShow(PHASE_SCENE) == PHASE_SHOWN);
	CHECK(phaseOnShow(PHASE_POST) == PHASE_SHOWN);
	CHECK(phaseOnShow(PHASE_SHOWN) == PHASE_SHOWN);
	CHECK(phaseOnShow(PHASE_CLASSIC) == PHASE_CLASSIC);
	CHECK(phaseOnShow(PHASE_PREPASS) == PHASE_SHOWN);
	CHECK(phaseOnMainUpdate(PHASE_SHOWN) == PHASE_CLASSIC);
	CHECK(phaseOnMainUpdate(PHASE_POST) == PHASE_POST);
	CHECK(phaseOnMainUpdate(PHASE_SCENE) == PHASE_SCENE);
	CHECK(phaseOnMainUpdate(PHASE_CLASSIC) == PHASE_CLASSIC);
}

static void
TestScaleRect(void)
{
	HostRect r = { 10, 2, 16, 6 };
	HostRect s = scaleRect(r, 640, 480, 640, 480);
	CHECK(s.x == 10 && s.y == 2 && s.width == 16 && s.height == 6);
	s = scaleRect(r, 640, 480, 320, 240);
	CHECK(s.x == 5 && s.y == 1 && s.width == 8 && s.height == 3);
	HostRect a = { 0, 0, 213, 480 }, b = { 213, 0, 427, 480 };
	HostRect sa = scaleRect(a, 640, 480, 427, 320), sb = scaleRect(b, 640, 480, 427, 320);
	CHECK(sa.x == 0 && sa.x + sa.width == sb.x && sb.x + sb.width == 427);
	HostRect odd = { 1, 1, 1, 1 };
	s = scaleRect(odd, 2, 2, 3, 3);
	CHECK(s.x == 2 && s.y == 2 && s.width == 1 && s.height == 1);
	HostRect full = { 0, 0, 3840, 2160 };
	s = scaleRect(full, 3840, 2160, 2259, 1271);
	CHECK(s.x == 0 && s.y == 0 && s.width == 2259 && s.height == 1271);
	CHECK(scaleEdge(5, 0, 100) == 5);
}

int
main(void)
{
	TestRoute();
	TestFormats();
	TestPhases();
	TestScaleRect();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all phase tests passed\n");
	return 0;
}
