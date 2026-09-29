#ifndef RW_METAL_METALPASS_H
#define RW_METAL_METALPASS_H

#include <stdint.h>

namespace rw {
namespace metal {

enum
{
	PASSCLEAR_COLOR   = 1,
	PASSCLEAR_DEPTH   = 2,
	PASSCLEAR_STENCIL = 4,
};

struct PassTarget
{
	const void *color;
	const void *depth;
};

inline bool
samePassTarget(const PassTarget &a, const PassTarget &b)
{
	return a.color == b.color && a.depth == b.depth;
}

struct PassClear
{
	uint32_t flags;
	float color[4];
	float depth;
	uint32_t stencil;
	bool subRect;
	int32_t x, y, w, h;
};

enum PassActionType
{
	PASSACTION_END,
	PASSACTION_BEGIN,
	PASSACTION_CLEARQUAD,
};

struct PassAction
{
	PassActionType type;
	PassTarget target;
	PassClear clear;
};

struct PassManager
{
	enum { MAXACTIONS = 8 };

	PassTarget current;
	PassTarget openTarget;
	bool open;
	bool hasPending;
	PassTarget pendingTarget;
	PassClear pending;

	int numActions;
	PassAction actions[MAXACTIONS];

	PassManager(void);
	bool isOpen(void) const { return this->open; }

	void beginUpdate(const PassTarget &target);
	void clear(const PassTarget &target, const PassClear &clear);
	bool draw(void);
	void flush(void);
	void show(void);
	void forget(const void *raster);

private:
	void reset(void);
	void push(PassActionType type, const PassTarget &target, const PassClear *clear);
	void end(void);
	void openOn(const PassTarget &target);
	void resolvePending(void);
};

}
}

#endif
