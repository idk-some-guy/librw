#include <assert.h>
#include "metalpass.h"

namespace rw {
namespace metal {

static const PassTarget noTarget = { nullptr, nullptr };

static bool
hasTarget(const PassTarget &t)
{
	return t.color != nullptr;
}

static bool
usesRaster(const PassTarget &t, const void *raster)
{
	return t.color == raster || t.depth == raster;
}

static bool
sharesRaster(const PassTarget &a, const PassTarget &b)
{
	return (a.color && usesRaster(b, a.color)) || (a.depth && usesRaster(b, a.depth));
}

PassManager::PassManager(void)
{
	this->current = noTarget;
	this->openTarget = noTarget;
	this->open = false;
	this->hasPending = false;
	this->pendingTarget = noTarget;
	this->pending = PassClear();
	this->numActions = 0;
}

void
PassManager::reset(void)
{
	this->numActions = 0;
}

void
PassManager::push(PassActionType type, const PassTarget &target, const PassClear *clear)
{
	assert(this->numActions < MAXACTIONS);
	PassAction *a = &this->actions[this->numActions++];
	a->type = type;
	a->target = target;
	a->clear = clear ? *clear : PassClear();
}

void
PassManager::end(void)
{
	if(!this->open)
		return;
	push(PASSACTION_END, this->openTarget, nullptr);
	this->open = false;
	this->openTarget = noTarget;
}

static bool
pendingAppliesTo(const PassTarget &pending, const PassTarget &target)
{
	if(pending.color == nullptr)
		return pending.depth == target.depth;
	return samePassTarget(pending, target);
}

void
PassManager::openOn(const PassTarget &target)
{
	bool applies = this->hasPending && pendingAppliesTo(this->pendingTarget, target);
	if(this->hasPending && !applies && sharesRaster(this->pendingTarget, target))
		resolvePending();
	if(this->open && samePassTarget(this->openTarget, target) && !applies)
		return;
	end();
	if(applies){
		push(PASSACTION_BEGIN, target, &this->pending);
		this->hasPending = false;
	}else
		push(PASSACTION_BEGIN, target, nullptr);
	this->open = true;
	this->openTarget = target;
}

void
PassManager::resolvePending(void)
{
	if(!this->hasPending)
		return;
	if(!hasTarget(this->pendingTarget)){
		if(this->pendingTarget.depth && (this->pending.flags & (PASSCLEAR_DEPTH|PASSCLEAR_STENCIL))){
			end();
			push(PASSACTION_BEGIN, this->pendingTarget, &this->pending);
			push(PASSACTION_END, this->pendingTarget, nullptr);
		}
		this->hasPending = false;
		return;
	}
	openOn(this->pendingTarget);
	end();
}

void
PassManager::beginUpdate(const PassTarget &target)
{
	reset();
	this->current = target;
	if(this->open && !samePassTarget(this->openTarget, target))
		end();
}

void
PassManager::clear(const PassTarget &target, const PassClear &clear)
{
	reset();
	if(!hasTarget(target))
		return;
	this->current = target;
	if(clear.flags == 0)
		return;

	if(clear.subRect || (this->open && samePassTarget(this->openTarget, target))){
		openOn(target);
		push(PASSACTION_CLEARQUAD, target, &clear);
		return;
	}

	if(this->hasPending && !samePassTarget(this->pendingTarget, target)){
		if(pendingAppliesTo(this->pendingTarget, target))
			this->pendingTarget = target;
		else
			resolvePending();
	}

	if(!this->hasPending){
		this->hasPending = true;
		this->pendingTarget = target;
		this->pending = PassClear();
	}
	if(clear.flags & PASSCLEAR_COLOR)
		for(int i = 0; i < 4; i++)
			this->pending.color[i] = clear.color[i];
	if(clear.flags & PASSCLEAR_DEPTH)
		this->pending.depth = clear.depth;
	if(clear.flags & PASSCLEAR_STENCIL)
		this->pending.stencil = clear.stencil;
	this->pending.flags |= clear.flags;
}

void
PassManager::clearOffscreen(const PassTarget &target, const PassClear &clear)
{
	PassTarget current = this->current;
	this->clear(target, clear);
	this->current = current;
}

bool
PassManager::draw(void)
{
	reset();
	if(!hasTarget(this->current))
		return false;
	openOn(this->current);
	return true;
}

void
PassManager::flush(void)
{
	reset();
	end();
	resolvePending();
}

void
PassManager::show(void)
{
	flush();
}

void
PassManager::forget(const void *raster)
{
	reset();
	if(raster == nullptr)
		return;
	if(this->open && usesRaster(this->openTarget, raster))
		end();
	if(this->hasPending && usesRaster(this->pendingTarget, raster)){
		if(this->pendingTarget.color == raster){
			this->pendingTarget.color = nullptr;
			this->pending.flags &= ~PASSCLEAR_COLOR;
		}
		if(this->pendingTarget.depth == raster){
			this->pendingTarget.depth = nullptr;
			this->pending.flags &= ~(PASSCLEAR_DEPTH|PASSCLEAR_STENCIL);
		}
		if(this->pending.flags == 0 ||
		   (this->pendingTarget.color == nullptr && this->pendingTarget.depth == nullptr))
			this->hasPending = false;
	}
	if(usesRaster(this->current, raster))
		this->current = noTarget;
}

void
PassManager::resolve(const void *raster)
{
	reset();
	if(raster == nullptr)
		return;
	if(this->hasPending && usesRaster(this->pendingTarget, raster))
		resolvePending();
	if(this->open && usesRaster(this->openTarget, raster))
		end();
}

}
}
