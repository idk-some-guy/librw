#include <cstdio>
#include "metalpass.h"

using namespace rw::metal;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static int colorA, depthA, colorB, depthB;
static const PassTarget targetA = { &colorA, &depthA };
static const PassTarget targetB = { &colorB, &depthB };

struct Log
{
	PassAction actions[64];
	int num;
};

static void
Record(Log *log, const PassManager &pm)
{
	for(int i = 0; i < pm.numActions; i++)
		log->actions[log->num++] = pm.actions[i];
}

static int
Count(const Log &log, PassActionType type)
{
	int n = 0;
	for(int i = 0; i < log.num; i++)
		if(log.actions[i].type == type)
			n++;
	return n;
}

static const PassAction*
Nth(const Log &log, PassActionType type, int nth)
{
	for(int i = 0; i < log.num; i++)
		if(log.actions[i].type == type && nth-- == 0)
			return &log.actions[i];
	return nullptr;
}

static PassClear
FullClear(uint32_t flags, float r, float g, float b, float a)
{
	PassClear c = {};
	c.flags = flags;
	c.color[0] = r;
	c.color[1] = g;
	c.color[2] = b;
	c.color[3] = a;
	c.depth = 1.0f;
	c.stencil = 0;
	return c;
}

static PassClear
SubClear(uint32_t flags, int x, int y, int w, int h)
{
	PassClear c = FullClear(flags, 0.0f, 0.0f, 0.0f, 1.0f);
	c.subRect = true;
	c.x = x;
	c.y = y;
	c.w = w;
	c.h = h;
	return c;
}

static void
TestClearBeginDrawIsOneClearingPass(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 0.25f, 0.5f, 0.75f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.show(); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 1);
	CHECK(Count(log, PASSACTION_END) == 1);
	CHECK(Count(log, PASSACTION_CLEARQUAD) == 0);
	const PassAction *b = Nth(log, PASSACTION_BEGIN, 0);
	CHECK(b && samePassTarget(b->target, targetA));
	CHECK(b && b->clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH));
	CHECK(b && b->clear.color[0] == 0.25f && b->clear.color[2] == 0.75f);
	CHECK(log.num > 0 && log.actions[log.num-1].type == PASSACTION_END);
	CHECK(!pm.isOpen());
}

static void
TestSwitchingTargetsResumesWithLoad(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 3);
	CHECK(Count(log, PASSACTION_END) == 2);
	const PassAction *first = Nth(log, PASSACTION_BEGIN, 0);
	const PassAction *second = Nth(log, PASSACTION_BEGIN, 1);
	const PassAction *third = Nth(log, PASSACTION_BEGIN, 2);
	CHECK(first && first->clear.flags == PASSCLEAR_COLOR);
	CHECK(second && samePassTarget(second->target, targetB) && second->clear.flags == 0);
	CHECK(third && samePassTarget(third->target, targetA) && third->clear.flags == 0);
	CHECK(pm.isOpen() && samePassTarget(pm.openTarget, targetA));
}

static void
TestBeginSameTargetKeepsEncoder(void)
{
	PassManager pm;
	pm.beginUpdate(targetA);
	pm.draw();
	pm.beginUpdate(targetA);
	CHECK(pm.numActions == 0);
	pm.draw();
	CHECK(pm.numActions == 0);
}

static void
TestClearWhilePassOpenDrawsQuad(void)
{
	PassManager pm;
	Log log = {};
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 0.0f, 1.0f, 0.0f, 1.0f)); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 1);
	CHECK(Count(log, PASSACTION_END) == 0);
	CHECK(Count(log, PASSACTION_CLEARQUAD) == 1);
	const PassAction *q = Nth(log, PASSACTION_CLEARQUAD, 0);
	CHECK(q && !q->clear.subRect && q->clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH));
	CHECK(q && q->clear.color[1] == 1.0f);
	CHECK(pm.isOpen());
}

static void
TestSubRectClearDrawsQuad(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, SubClear(PASSCLEAR_COLOR, 10, 20, 30, 40)); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 1);
	CHECK(Count(log, PASSACTION_CLEARQUAD) == 1);
	const PassAction *b = Nth(log, PASSACTION_BEGIN, 0);
	CHECK(b && b->clear.flags == 0);
	const PassAction *q = Nth(log, PASSACTION_CLEARQUAD, 0);
	CHECK(q && q->clear.subRect);
	CHECK(q && q->clear.x == 10 && q->clear.y == 20 && q->clear.w == 30 && q->clear.h == 40);
	CHECK(log.num == 2 && log.actions[0].type == PASSACTION_BEGIN);
}

static void
TestSubRectClearKeepsPendingFullClear(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_DEPTH, 0.0f, 0.0f, 0.0f, 0.0f)); Record(&log, pm);
	pm.clear(targetA, SubClear(PASSCLEAR_COLOR, 0, 0, 8, 8)); Record(&log, pm);

	const PassAction *b = Nth(log, PASSACTION_BEGIN, 0);
	CHECK(Count(log, PASSACTION_BEGIN) == 1);
	CHECK(b && b->clear.flags == PASSCLEAR_DEPTH);
	CHECK(Count(log, PASSACTION_CLEARQUAD) == 1);
}

static void
TestShowWithPendingClearClearsTarget(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 0.0f, 0.0f, 1.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.show(); Record(&log, pm);

	CHECK(log.num == 2);
	CHECK(log.num == 2 && log.actions[0].type == PASSACTION_BEGIN);
	CHECK(log.num == 2 && log.actions[1].type == PASSACTION_END);
	CHECK(log.actions[0].clear.flags == PASSCLEAR_COLOR && log.actions[0].clear.color[2] == 1.0f);
	CHECK(!pm.isOpen());

	pm.show();
	CHECK(pm.numActions == 0);
}

static void
TestLaterClearWins(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 0.0f, 1.0f, 0.0f, 0.5f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 1);
	CHECK(Count(log, PASSACTION_CLEARQUAD) == 0);
	const PassAction *b = Nth(log, PASSACTION_BEGIN, 0);
	CHECK(b && b->clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH));
	CHECK(b && b->clear.color[0] == 0.0f && b->clear.color[1] == 1.0f && b->clear.color[3] == 0.5f);
}

static void
TestFlushEndsPassAndResolvesPendingClear(void)
{
	PassManager pm;
	Log log = {};
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f)); Record(&log, pm);
	CHECK(pm.isOpen() && samePassTarget(pm.openTarget, targetB));
	pm.flush(); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 2);
	CHECK(Count(log, PASSACTION_END) == 2);
	const PassAction *second = Nth(log, PASSACTION_BEGIN, 1);
	CHECK(second && samePassTarget(second->target, targetA) && second->clear.flags == PASSCLEAR_COLOR);
	CHECK(!pm.isOpen());
}

static void
TestClearOnOtherTargetResolvesEarlierPending(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.clear(targetB, FullClear(PASSCLEAR_COLOR, 0.0f, 1.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 2);
	const PassAction *first = Nth(log, PASSACTION_BEGIN, 0);
	const PassAction *second = Nth(log, PASSACTION_BEGIN, 1);
	CHECK(first && samePassTarget(first->target, targetA) && first->clear.color[0] == 1.0f);
	CHECK(second && samePassTarget(second->target, targetB) && second->clear.color[1] == 1.0f);
}

static void
TestDrawWithoutTargetDoesNothing(void)
{
	PassManager pm;
	CHECK(!pm.draw());
	CHECK(pm.numActions == 0);
	pm.beginUpdate(targetA);
	CHECK(pm.draw());
}

static void
TestForgetDropsDestroyedTarget(void)
{
	PassManager pm;
	pm.clear(targetB, FullClear(PASSCLEAR_COLOR, 0.0f, 0.0f, 0.0f, 1.0f));
	pm.beginUpdate(targetA);
	pm.draw();
	pm.forget(&depthA);
	CHECK(pm.numActions == 1 && pm.actions[0].type == PASSACTION_END);
	CHECK(!pm.isOpen());
	CHECK(!pm.draw());
	pm.forget(&colorB);
	pm.show();
	CHECK(pm.numActions == 0);
}

static const PassTarget targetSharedColor = { &colorA, &depthB };
static const PassTarget targetSharedDepth = { &colorB, &depthA };

static void
CheckPendingAResolvedBefore(const Log &log, const PassTarget &next, bool thenClearQuad)
{
	int want = thenClearQuad ? 4 : 3;
	CHECK(log.num == want);
	if(log.num != want)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN);
	CHECK(samePassTarget(log.actions[0].target, targetA));
	CHECK(log.actions[0].clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH));
	CHECK(log.actions[0].clear.color[0] == 1.0f);
	CHECK(log.actions[1].type == PASSACTION_END);
	CHECK(samePassTarget(log.actions[1].target, targetA));
	CHECK(log.actions[2].type == PASSACTION_BEGIN);
	CHECK(samePassTarget(log.actions[2].target, next));
	CHECK(log.actions[2].clear.flags == 0);
	if(thenClearQuad){
		CHECK(log.actions[3].type == PASSACTION_CLEARQUAD);
		CHECK(samePassTarget(log.actions[3].target, next));
		CHECK(log.actions[3].clear.subRect);
	}
}

static void
TestPendingClearResolvesBeforeDrawOnSharedColor(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetSharedColor); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	CheckPendingAResolvedBefore(log, targetSharedColor, false);
	pm.show(); Record(&log, pm);
	CHECK(Count(log, PASSACTION_BEGIN) == 2);
	CHECK(Count(log, PASSACTION_END) == 2);
}

static void
TestPendingClearResolvesBeforeDrawOnSharedDepth(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetSharedDepth); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	CheckPendingAResolvedBefore(log, targetSharedDepth, false);
	pm.show(); Record(&log, pm);
	CHECK(Count(log, PASSACTION_BEGIN) == 2);
}

static void
TestPendingClearResolvesBeforeSubRectClearOnSharedColor(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.clear(targetSharedColor, SubClear(PASSCLEAR_COLOR, 0, 0, 4, 4)); Record(&log, pm);
	CheckPendingAResolvedBefore(log, targetSharedColor, true);
}

static void
TestPendingClearResolvesBeforeDrawOnOpenPassSharingRaster(void)
{
	PassManager pm;
	Log log = {};
	pm.beginUpdate(targetSharedColor);
	pm.draw();
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetSharedColor); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 4);
	CHECK(log.num == 4 && log.actions[0].type == PASSACTION_END);
	CHECK(log.num == 4 && log.actions[1].type == PASSACTION_BEGIN && samePassTarget(log.actions[1].target, targetA));
	CHECK(log.num == 4 && log.actions[1].clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH));
	CHECK(log.num == 4 && log.actions[2].type == PASSACTION_END);
	CHECK(log.num == 4 && log.actions[3].type == PASSACTION_BEGIN && samePassTarget(log.actions[3].target, targetSharedColor));
	CHECK(log.num == 4 && log.actions[3].clear.flags == 0);
}

static void
TestPendingClearSurvivesUnrelatedPassAndLoadsOnce(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.show(); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 2);
	CHECK(Count(log, PASSACTION_END) == 2);
	const PassAction *first = Nth(log, PASSACTION_BEGIN, 0);
	const PassAction *second = Nth(log, PASSACTION_BEGIN, 1);
	CHECK(first && samePassTarget(first->target, targetB) && first->clear.flags == 0);
	CHECK(second && samePassTarget(second->target, targetA));
	CHECK(second && second->clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH) && second->clear.color[0] == 1.0f);
}

static void
TestClearOnSharedTargetResolvesEarlierPendingFirst(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.clear(targetSharedColor, FullClear(PASSCLEAR_COLOR, 0.0f, 1.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.show(); Record(&log, pm);

	CHECK(Count(log, PASSACTION_BEGIN) == 2);
	CHECK(Count(log, PASSACTION_END) == 2);
	const PassAction *first = Nth(log, PASSACTION_BEGIN, 0);
	const PassAction *second = Nth(log, PASSACTION_BEGIN, 1);
	CHECK(first && samePassTarget(first->target, targetA));
	CHECK(first && first->clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH) && first->clear.color[0] == 1.0f);
	CHECK(second && samePassTarget(second->target, targetSharedColor));
	CHECK(second && second->clear.flags == PASSCLEAR_COLOR && second->clear.color[1] == 1.0f);
}

static void
TestClearWithNoFlagsSetsTargetOnly(void)
{
	PassManager pm;
	pm.clear(targetA, FullClear(0, 1.0f, 0.0f, 0.0f, 1.0f));
	CHECK(pm.numActions == 0);
	CHECK(!pm.hasPending);
	CHECK(samePassTarget(pm.current, targetA));
	pm.clear(targetA, SubClear(0, 0, 0, 4, 4));
	CHECK(pm.numActions == 0);
	CHECK(!pm.isOpen());
	pm.show();
	CHECK(pm.numActions == 0);
}

static const PassTarget colorAOnly = { &colorA, nullptr };
static const PassTarget colorBOnly = { &colorB, nullptr };
static const PassTarget depthAOnly = { nullptr, &depthA };

static void
TestForgetDepthKeepsColourClearForColourPass(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH|PASSCLEAR_STENCIL, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.forget(&depthA); Record(&log, pm);
	CHECK(log.num == 0);
	CHECK(pm.hasPending);
	pm.beginUpdate(colorAOnly); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 1);
	CHECK(log.num == 1 && log.actions[0].type == PASSACTION_BEGIN);
	CHECK(log.num == 1 && samePassTarget(log.actions[0].target, colorAOnly));
	CHECK(log.num == 1 && log.actions[0].clear.flags == PASSCLEAR_COLOR);
	CHECK(log.num == 1 && log.actions[0].clear.color[0] == 1.0f);
}

static void
TestForgetDepthKeepsColourClearForPassWithNewDepth(void)
{
	PassManager pm;
	Log log = {};
	const PassTarget colorANewDepth = { &colorA, &depthB };
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.forget(&depthA); Record(&log, pm);
	pm.beginUpdate(colorANewDepth); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 3);
	if(log.num != 3)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN && samePassTarget(log.actions[0].target, colorAOnly));
	CHECK(log.actions[0].clear.flags == PASSCLEAR_COLOR && log.actions[0].clear.color[0] == 1.0f);
	CHECK(log.actions[1].type == PASSACTION_END);
	CHECK(log.actions[2].type == PASSACTION_BEGIN && samePassTarget(log.actions[2].target, colorANewDepth));
	CHECK(log.actions[2].clear.flags == 0);
}

static void
TestForgetColourKeepsDepthClearForNextPassOnDepth(void)
{
	PassManager pm;
	Log log = {};
	PassClear c = FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH|PASSCLEAR_STENCIL, 1.0f, 0.0f, 0.0f, 1.0f);
	c.depth = 0.5f;
	c.stencil = 7;
	pm.clear(targetA, c); Record(&log, pm);
	pm.forget(&colorA); Record(&log, pm);
	CHECK(log.num == 0);
	CHECK(pm.hasPending);
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetSharedDepth); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 3);
	if(log.num != 3)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN && samePassTarget(log.actions[0].target, targetB));
	CHECK(log.actions[0].clear.flags == 0);
	CHECK(log.actions[1].type == PASSACTION_END);
	CHECK(log.actions[2].type == PASSACTION_BEGIN && samePassTarget(log.actions[2].target, targetSharedDepth));
	CHECK(log.actions[2].clear.flags == (PASSCLEAR_DEPTH|PASSCLEAR_STENCIL));
	CHECK(log.actions[2].clear.depth == 0.5f && log.actions[2].clear.stencil == 7);
	CHECK(!pm.hasPending);
}

static void
TestForgetColourDepthClearOnOpenPassRestartsIt(void)
{
	PassManager pm;
	Log log = {};
	pm.beginUpdate(targetSharedDepth);
	pm.draw();
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	CHECK(pm.isOpen() && pm.hasPending);
	pm.forget(&colorA); Record(&log, pm);
	pm.beginUpdate(targetSharedDepth); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 2);
	if(log.num != 2)
		return;
	CHECK(log.actions[0].type == PASSACTION_END && samePassTarget(log.actions[0].target, targetSharedDepth));
	CHECK(log.actions[1].type == PASSACTION_BEGIN && samePassTarget(log.actions[1].target, targetSharedDepth));
	CHECK(log.actions[1].clear.flags == PASSCLEAR_DEPTH);
}

static void
TestForgetColourRunsDepthClearWhenForcedWithoutPass(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.forget(&colorA); Record(&log, pm);
	pm.show(); Record(&log, pm);
	CHECK(!pm.hasPending);
	pm.beginUpdate(targetSharedDepth); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 3);
	if(log.num != 3)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN && samePassTarget(log.actions[0].target, depthAOnly));
	CHECK(log.actions[0].clear.flags == PASSCLEAR_DEPTH);
	CHECK(log.actions[1].type == PASSACTION_END && samePassTarget(log.actions[1].target, depthAOnly));
	CHECK(log.actions[2].type == PASSACTION_BEGIN && log.actions[2].clear.flags == 0);
	CHECK(samePassTarget(log.actions[2].target, targetSharedDepth));
}

static void
TestDepthOnlyPendingSurvivesOffscreenClear(void)
{
	PassManager pm;
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f));
	pm.forget(&colorA);
	pm.clearOffscreen(colorBOnly, FullClear(PASSCLEAR_COLOR, 0.0f, 0.0f, 0.0f, 0.0f));

	CHECK(pm.numActions == 2);
	if(pm.numActions != 2)
		return;
	CHECK(pm.actions[0].type == PASSACTION_BEGIN && samePassTarget(pm.actions[0].target, depthAOnly));
	CHECK(pm.actions[0].clear.flags == PASSCLEAR_DEPTH && pm.actions[0].clear.depth == 1.0f);
	CHECK(pm.actions[1].type == PASSACTION_END && samePassTarget(pm.actions[1].target, depthAOnly));

	pm.flush();
	CHECK(pm.numActions == 2);
	if(pm.numActions != 2)
		return;
	CHECK(pm.actions[0].type == PASSACTION_BEGIN && samePassTarget(pm.actions[0].target, colorBOnly));
	CHECK(pm.actions[0].clear.flags == PASSCLEAR_COLOR);
	CHECK(pm.actions[1].type == PASSACTION_END && samePassTarget(pm.actions[1].target, colorBOnly));
}

static void
TestDepthOnlyPendingFlush(void)
{
	PassManager pm;
	pm.clear(targetA, FullClear(PASSCLEAR_DEPTH|PASSCLEAR_STENCIL, 0.0f, 0.0f, 0.0f, 0.0f));
	pm.forget(&colorA);
	pm.flush();

	CHECK(pm.numActions == 2);
	if(pm.numActions != 2)
		return;
	CHECK(pm.actions[0].type == PASSACTION_BEGIN && samePassTarget(pm.actions[0].target, depthAOnly));
	CHECK(pm.actions[0].clear.flags == (PASSCLEAR_DEPTH|PASSCLEAR_STENCIL));
	CHECK(pm.actions[1].type == PASSACTION_END && samePassTarget(pm.actions[1].target, depthAOnly));
	CHECK(!pm.hasPending);
}

static void
TestEmptyDepthOnlyPendingDropped(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.forget(&colorA); Record(&log, pm);
	pm.flush(); Record(&log, pm);
	CHECK(Count(log, PASSACTION_BEGIN) == 0);
	CHECK(!pm.hasPending);

	PassManager pm2;
	pm2.hasPending = true;
	pm2.pendingTarget = depthAOnly;
	pm2.pending.flags = PASSCLEAR_COLOR;
	pm2.flush();
	CHECK(pm2.numActions == 0);
	CHECK(!pm2.hasPending);
}

static void
TestForgetColourThenColourClearOnSameDepthKeepsDepthClear(void)
{
	PassManager pm;
	Log log = {};
	PassClear c = FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH|PASSCLEAR_STENCIL, 1.0f, 0.0f, 0.0f, 1.0f);
	c.depth = 0.5f;
	c.stencil = 7;
	pm.clear(targetA, c); Record(&log, pm);
	pm.forget(&colorA); Record(&log, pm);
	PassClear c2 = FullClear(PASSCLEAR_COLOR, 0.0f, 1.0f, 0.0f, 1.0f);
	c2.depth = 0.25f;
	c2.stencil = 3;
	pm.clear(targetSharedDepth, c2); Record(&log, pm);
	CHECK(log.num == 0);
	CHECK(pm.hasPending);
	pm.draw(); Record(&log, pm);
	pm.flush(); Record(&log, pm);

	CHECK(log.num == 2);
	if(log.num != 2)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN && samePassTarget(log.actions[0].target, targetSharedDepth));
	CHECK(log.actions[0].clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH|PASSCLEAR_STENCIL));
	CHECK(log.actions[0].clear.color[0] == 0.0f && log.actions[0].clear.color[1] == 1.0f &&
	      log.actions[0].clear.color[2] == 0.0f && log.actions[0].clear.color[3] == 1.0f);
	CHECK(log.actions[0].clear.depth == 0.5f && log.actions[0].clear.stencil == 7);
	CHECK(log.actions[1].type == PASSACTION_END && samePassTarget(log.actions[1].target, targetSharedDepth));
	CHECK(!pm.hasPending);
}

static void
TestForgetColourWithoutDepthDropsClear(void)
{
	PassManager pm;
	pm.clear(colorAOnly, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f));
	pm.forget(&colorA);
	CHECK(!pm.hasPending);
	CHECK(pm.numActions == 0);
}

static void
TestForgetDepthDropsDepthOnlyClear(void)
{
	PassManager pm;
	pm.clear(targetA, FullClear(PASSCLEAR_DEPTH|PASSCLEAR_STENCIL, 0.0f, 0.0f, 0.0f, 0.0f));
	pm.forget(&depthA);
	CHECK(!pm.hasPending);
	pm.beginUpdate(colorAOnly);
	pm.draw();
	CHECK(pm.numActions == 1 && pm.actions[0].clear.flags == 0);
}

static void
TestResumeAfterPassOnSharedDepthLoads(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetSharedDepth); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.show(); Record(&log, pm);

	CHECK(log.num == 6);
	if(log.num != 6)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN && samePassTarget(log.actions[0].target, targetA));
	CHECK(log.actions[0].clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH));
	CHECK(log.actions[1].type == PASSACTION_END && samePassTarget(log.actions[1].target, targetA));
	CHECK(log.actions[2].type == PASSACTION_BEGIN && samePassTarget(log.actions[2].target, targetSharedDepth));
	CHECK(log.actions[2].clear.flags == 0);
	CHECK(log.actions[3].type == PASSACTION_END && samePassTarget(log.actions[3].target, targetSharedDepth));
	CHECK(log.actions[4].type == PASSACTION_BEGIN && samePassTarget(log.actions[4].target, targetA));
	CHECK(log.actions[4].clear.flags == 0);
	CHECK(log.actions[5].type == PASSACTION_END && samePassTarget(log.actions[5].target, targetA));
}

static void
TestResolveRunsPendingClearOnSampledRaster(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.resolve(&colorA); Record(&log, pm);
	CHECK(!pm.isOpen());
	CHECK(!pm.hasPending);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 5);
	if(log.num != 5)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN && samePassTarget(log.actions[0].target, targetB));
	CHECK(log.actions[1].type == PASSACTION_END && samePassTarget(log.actions[1].target, targetB));
	CHECK(log.actions[2].type == PASSACTION_BEGIN && samePassTarget(log.actions[2].target, targetA));
	CHECK(log.actions[2].clear.flags == (PASSCLEAR_COLOR|PASSCLEAR_DEPTH));
	CHECK(log.actions[3].type == PASSACTION_END && samePassTarget(log.actions[3].target, targetA));
	CHECK(log.actions[4].type == PASSACTION_BEGIN && samePassTarget(log.actions[4].target, targetB));
	CHECK(log.actions[4].clear.flags == 0);
}

static void
TestOffscreenClearKeepsCurrentAndOpenPass(void)
{
	static int colorC;
	static const PassTarget targetC = { &colorC, nullptr };
	PassManager pm;
	Log log = {};
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.clearOffscreen(targetC, FullClear(PASSCLEAR_COLOR, 0.0f, 0.0f, 0.0f, 0.0f)); Record(&log, pm);
	CHECK(samePassTarget(pm.current, targetA));
	CHECK(pm.isOpen() && samePassTarget(pm.openTarget, targetA));
	CHECK(pm.hasPending && samePassTarget(pm.pendingTarget, targetC));
	pm.draw(); Record(&log, pm);
	CHECK(pm.hasPending);
	pm.resolve(&colorC); Record(&log, pm);
	CHECK(!pm.hasPending);
	pm.resolve(&colorC); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	CHECK(log.num == 5);
	if(log.num != 5)
		return;
	CHECK(log.actions[0].type == PASSACTION_BEGIN && samePassTarget(log.actions[0].target, targetA));
	CHECK(log.actions[1].type == PASSACTION_END && samePassTarget(log.actions[1].target, targetA));
	CHECK(log.actions[2].type == PASSACTION_BEGIN && samePassTarget(log.actions[2].target, targetC));
	CHECK(log.actions[2].clear.flags == PASSCLEAR_COLOR && log.actions[2].clear.color[3] == 0.0f);
	CHECK(log.actions[3].type == PASSACTION_END && samePassTarget(log.actions[3].target, targetC));
	CHECK(log.actions[4].type == PASSACTION_BEGIN && samePassTarget(log.actions[4].target, targetA));
	CHECK(log.actions[4].clear.flags == 0);
}

static void
TestOffscreenClearIsConsumedByFirstPassOnTarget(void)
{
	static int colorC;
	static const PassTarget targetC = { &colorC, nullptr };
	PassManager pm;
	Log log = {};
	pm.clearOffscreen(targetC, FullClear(PASSCLEAR_COLOR, 0.0f, 0.0f, 0.0f, 0.0f)); Record(&log, pm);
	CHECK(pm.current.color == nullptr);
	pm.beginUpdate(targetC); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.clear(targetB, FullClear(PASSCLEAR_COLOR, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.flush(); Record(&log, pm);

	int clears = 0;
	for(int i = 0; i < log.num; i++)
		if(log.actions[i].type == PASSACTION_BEGIN && samePassTarget(log.actions[i].target, targetC) &&
		   log.actions[i].clear.flags != 0)
			clears++;
	CHECK(clears == 1);
	CHECK(log.num >= 1 && log.actions[0].type == PASSACTION_BEGIN &&
	      samePassTarget(log.actions[0].target, targetC) && log.actions[0].clear.flags == PASSCLEAR_COLOR);
}

static void
TestResolveEndsPassRenderingIntoRaster(void)
{
	PassManager pm;
	pm.beginUpdate(targetA);
	pm.draw();
	pm.resolve(&colorA);
	CHECK(pm.numActions == 1 && pm.actions[0].type == PASSACTION_END);
	CHECK(!pm.isOpen());
}

static void
TestResolveLeavesUnrelatedWorkAlone(void)
{
	PassManager pm;
	pm.clear(targetB, FullClear(PASSCLEAR_COLOR, 1.0f, 0.0f, 0.0f, 1.0f));
	pm.beginUpdate(targetB);
	pm.draw();
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 1.0f, 0.0f, 0.0f, 1.0f));
	pm.beginUpdate(targetB);
	pm.resolve(&colorB);
	CHECK(pm.numActions == 1 && pm.actions[0].type == PASSACTION_END);
	CHECK(pm.hasPending && samePassTarget(pm.pendingTarget, targetA));
	pm.resolve(nullptr);
	CHECK(pm.numActions == 0);
}

static int colorS1, depthS1, colorR1, depthR1, colorS2, depthS2, colorR2, depthR2;
static const PassTarget targetS1 = { &colorS1, &depthS1 };
static const PassTarget targetR1 = { &colorR1, &depthR1 };
static const PassTarget targetS2 = { &colorS2, &depthS2 };
static const PassTarget targetR2 = { &colorR2, &depthR2 };

struct Expected
{
	PassActionType type;
	PassTarget target;
	uint32_t flags;
};

static void
CheckSequence(const char *name, const Log &log, const Expected *want, int num)
{
	if(log.num != num){
		printf("FAIL %s: %d actions, expected %d\n", name, log.num, num);
		failures++;
	}
	for(int i = 0; i < log.num && i < num; i++){
		const PassAction &a = log.actions[i];
		if(a.type != want[i].type || !samePassTarget(a.target, want[i].target) ||
		   (a.type == PASSACTION_BEGIN && a.clear.flags != want[i].flags)){
			printf("FAIL %s action %d: type %d flags %u\n", name, i, (int)a.type, (unsigned)a.clear.flags);
			failures++;
		}
	}
}

static void
TestShadowSequenceTwoCasters(void)
{
	PassManager pm;
	Log log = {};
	const PassTarget *shadow[2] = { &targetS1, &targetS2 };
	const PassTarget *resample[2] = { &targetR1, &targetR2 };
	int *shadowColor[2] = { &colorS1, &colorS2 };
	for(int k = 0; k < 2; k++){
		pm.clear(*shadow[k], FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 1.0f, 1.0f, 0.0f)); Record(&log, pm);
		pm.beginUpdate(*shadow[k]); Record(&log, pm);
		pm.draw(); Record(&log, pm);
		pm.draw(); Record(&log, pm);
		pm.beginUpdate(*resample[k]); Record(&log, pm);
		pm.resolve(shadowColor[k]); Record(&log, pm);
		pm.draw(); Record(&log, pm);
		pm.beginUpdate(*resample[k]); Record(&log, pm);
		pm.draw(); Record(&log, pm);
	}
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH|PASSCLEAR_STENCIL, 0.5f, 0.5f, 0.5f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.show(); Record(&log, pm);

	const Expected want[] = {
		{ PASSACTION_BEGIN, targetS1, PASSCLEAR_COLOR|PASSCLEAR_DEPTH },
		{ PASSACTION_END, targetS1, 0 },
		{ PASSACTION_BEGIN, targetR1, 0 },
		{ PASSACTION_END, targetR1, 0 },
		{ PASSACTION_BEGIN, targetS2, PASSCLEAR_COLOR|PASSCLEAR_DEPTH },
		{ PASSACTION_END, targetS2, 0 },
		{ PASSACTION_BEGIN, targetR2, 0 },
		{ PASSACTION_END, targetR2, 0 },
		{ PASSACTION_BEGIN, targetA, PASSCLEAR_COLOR|PASSCLEAR_DEPTH|PASSCLEAR_STENCIL },
		{ PASSACTION_END, targetA, 0 },
	};
	CheckSequence(__func__, log, want, 10);
	CHECK(Count(log, PASSACTION_CLEARQUAD) == 0);
	CHECK(!pm.isOpen());
}

static void
TestEnvMapSwitchResumesScene(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 0.0f, 0.0f, 1.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.clear(targetB, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 0.5f, 0.5f, 1.0f, 1.0f));
	CHECK(pm.numActions == 0);
	CHECK(pm.isOpen() && samePassTarget(pm.openTarget, targetA));
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	const Expected want[] = {
		{ PASSACTION_BEGIN, targetA, PASSCLEAR_COLOR|PASSCLEAR_DEPTH },
		{ PASSACTION_END, targetA, 0 },
		{ PASSACTION_BEGIN, targetB, PASSCLEAR_COLOR|PASSCLEAR_DEPTH },
		{ PASSACTION_END, targetB, 0 },
		{ PASSACTION_BEGIN, targetA, 0 },
	};
	CheckSequence(__func__, log, want, 5);
	CHECK(pm.isOpen() && samePassTarget(pm.openTarget, targetA));
}

static void
TestSecondaryClearBeforeFirstSceneDraw(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR|PASSCLEAR_DEPTH, 1.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.clear(targetB, FullClear(PASSCLEAR_COLOR, 0.0f, 1.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetB); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);

	const Expected want[] = {
		{ PASSACTION_BEGIN, targetA, PASSCLEAR_COLOR|PASSCLEAR_DEPTH },
		{ PASSACTION_END, targetA, 0 },
		{ PASSACTION_BEGIN, targetB, PASSCLEAR_COLOR },
		{ PASSACTION_END, targetB, 0 },
		{ PASSACTION_BEGIN, targetA, 0 },
	};
	CheckSequence(__func__, log, want, 5);
	CHECK(log.num == 5 && log.actions[0].clear.color[0] == 1.0f);
	CHECK(log.num == 5 && log.actions[2].clear.color[1] == 1.0f && log.actions[2].clear.color[0] == 0.0f);
}

static void
TestBlurPingPongLoadsEachTime(void)
{
	PassManager pm;
	Log log = {};
	pm.clear(targetA, FullClear(PASSCLEAR_COLOR, 0.0f, 0.0f, 0.0f, 1.0f)); Record(&log, pm);
	pm.beginUpdate(targetA); Record(&log, pm);
	pm.draw(); Record(&log, pm);
	for(int i = 0; i < 2; i++){
		pm.beginUpdate(targetB); Record(&log, pm);
		pm.draw(); Record(&log, pm);
		pm.beginUpdate(targetA); Record(&log, pm);
		pm.draw(); Record(&log, pm);
	}

	const Expected want[] = {
		{ PASSACTION_BEGIN, targetA, PASSCLEAR_COLOR },
		{ PASSACTION_END, targetA, 0 },
		{ PASSACTION_BEGIN, targetB, 0 },
		{ PASSACTION_END, targetB, 0 },
		{ PASSACTION_BEGIN, targetA, 0 },
		{ PASSACTION_END, targetA, 0 },
		{ PASSACTION_BEGIN, targetB, 0 },
		{ PASSACTION_END, targetB, 0 },
		{ PASSACTION_BEGIN, targetA, 0 },
	};
	CheckSequence(__func__, log, want, 9);
	CHECK(Count(log, PASSACTION_BEGIN) == 5);
}

int
main(void)
{
	TestClearBeginDrawIsOneClearingPass();
	TestSwitchingTargetsResumesWithLoad();
	TestBeginSameTargetKeepsEncoder();
	TestClearWhilePassOpenDrawsQuad();
	TestSubRectClearDrawsQuad();
	TestSubRectClearKeepsPendingFullClear();
	TestShowWithPendingClearClearsTarget();
	TestLaterClearWins();
	TestFlushEndsPassAndResolvesPendingClear();
	TestClearOnOtherTargetResolvesEarlierPending();
	TestDrawWithoutTargetDoesNothing();
	TestForgetDropsDestroyedTarget();
	TestPendingClearResolvesBeforeDrawOnSharedColor();
	TestPendingClearResolvesBeforeDrawOnSharedDepth();
	TestPendingClearResolvesBeforeSubRectClearOnSharedColor();
	TestPendingClearResolvesBeforeDrawOnOpenPassSharingRaster();
	TestPendingClearSurvivesUnrelatedPassAndLoadsOnce();
	TestClearOnSharedTargetResolvesEarlierPendingFirst();
	TestClearWithNoFlagsSetsTargetOnly();
	TestForgetDepthKeepsColourClearForColourPass();
	TestForgetDepthKeepsColourClearForPassWithNewDepth();
	TestForgetColourKeepsDepthClearForNextPassOnDepth();
	TestForgetColourDepthClearOnOpenPassRestartsIt();
	TestForgetColourRunsDepthClearWhenForcedWithoutPass();
	TestDepthOnlyPendingSurvivesOffscreenClear();
	TestDepthOnlyPendingFlush();
	TestEmptyDepthOnlyPendingDropped();
	TestForgetColourThenColourClearOnSameDepthKeepsDepthClear();
	TestForgetColourWithoutDepthDropsClear();
	TestForgetDepthDropsDepthOnlyClear();
	TestResumeAfterPassOnSharedDepthLoads();
	TestResolveRunsPendingClearOnSampledRaster();
	TestResolveEndsPassRenderingIntoRaster();
	TestResolveLeavesUnrelatedWorkAlone();
	TestOffscreenClearKeepsCurrentAndOpenPass();
	TestOffscreenClearIsConsumedByFirstPassOnTarget();
	TestShadowSequenceTwoCasters();
	TestEnvMapSwitchResumesScene();
	TestSecondaryClearBeforeFirstSceneDraw();
	TestBlurPingPongLoadsEachTime();
	if(failures == 0)
		printf("all tests passed\n");
	return failures == 0 ? 0 : 1;
}
