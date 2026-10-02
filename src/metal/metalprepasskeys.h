#ifndef RW_METAL_METALPREPASSKEYS_H
#define RW_METAL_METALPREPASSKEYS_H

#include <stdint.h>
#include <vector>

struct PrepassKeySet
{
	struct Slot
	{
		uint64_t key;
		uint32_t gen;
		uint32_t used;
	};

	std::vector<Slot> slots;
	std::vector<uint32_t> filled;
	uint32_t gen = 1;
	uint32_t mask = 0;
	uint32_t limit = 0;

	void reserve(uint32_t keys)
	{
		uint32_t n = 1;
		while(n < keys*2)
			n <<= 1;
		this->slots.assign(n, Slot{ 0, 0, 0 });
		this->filled.clear();
		this->filled.reserve(keys);
		this->mask = n - 1;
		this->limit = keys;
		this->gen = 1;
	}
	uint32_t capacity(void) const { return this->limit; }
	uint32_t size(void) const { return (uint32_t)this->filled.size(); }
	const void *slotData(void) const { return this->slots.data(); }

	void clear(void)
	{
		this->filled.clear();
		if(++this->gen == 0){
			for(Slot &s : this->slots)
				s.gen = 0;
			this->gen = 1;
		}
	}

	Slot *find(uint64_t key, bool *found)
	{
		uint64_t h = key ^ (key >> 29);
		h *= 0xbf58476d1ce4e5b9ull;
		for(uint32_t i = (uint32_t)(h >> 32) & this->mask;; i = (i + 1) & this->mask){
			Slot &s = this->slots[i];
			if(s.gen != this->gen){
				*found = false;
				return &s;
			}
			if(s.key == key){
				*found = true;
				return &s;
			}
		}
	}

	bool insert(uint64_t key)
	{
		if(this->slots.empty())
			return false;
		bool found;
		Slot *s = find(key, &found);
		if(found)
			return true;
		if(this->filled.size() >= this->limit)
			return false;
		s->key = key;
		s->gen = this->gen;
		s->used = 0;
		this->filled.push_back((uint32_t)(s - this->slots.data()));
		return true;
	}

	bool markUsed(uint64_t key)
	{
		if(this->slots.empty())
			return false;
		bool found;
		Slot *s = find(key, &found);
		if(found)
			s->used = 1;
		return found;
	}

	uint32_t unused(void) const
	{
		uint32_t n = 0;
		for(uint32_t i : this->filled)
			if(!this->slots[i].used)
				n++;
		return n;
	}
};

template<typename T>
struct PrevMap
{
	struct Slot
	{
		const void *key;
		uint32_t gen;
		uint32_t index;
	};

	std::vector<Slot> slots;
	std::vector<T> values;
	uint32_t gen = 1;
	uint32_t mask = 0;
	uint32_t count = 0;

	void reserve(uint32_t n)
	{
		uint32_t s = 1;
		while(s < n*2)
			s <<= 1;
		this->slots.assign(s, Slot{ nullptr, 0, 0 });
		this->values.assign(n, T());
		this->mask = s - 1;
		this->count = 0;
		this->gen = 1;
	}
	uint32_t capacity(void) const { return (uint32_t)this->values.size(); }
	uint32_t size(void) const { return this->count; }
	const void *slotData(void) const { return this->slots.data(); }

	void clear(void)
	{
		this->count = 0;
		if(++this->gen == 0){
			for(Slot &s : this->slots)
				s.gen = 0;
			this->gen = 1;
		}
	}

	Slot *slot(const void *key, bool *found)
	{
		uint64_t h = (uint64_t)(uintptr_t)key;
		h ^= h >> 29;
		h *= 0xbf58476d1ce4e5b9ull;
		for(uint32_t i = (uint32_t)(h >> 32) & this->mask;; i = (i + 1) & this->mask){
			Slot &s = this->slots[i];
			if(s.gen != this->gen){
				*found = false;
				return &s;
			}
			if(s.key == key){
				*found = true;
				return &s;
			}
		}
	}

	const T *find(const void *key)
	{
		if(this->slots.empty())
			return nullptr;
		bool found;
		Slot *s = slot(key, &found);
		return found ? &this->values[s->index] : nullptr;
	}

	void forget(const void *key)
	{
		if(this->slots.empty())
			return;
		bool found;
		Slot *s = slot(key, &found);
		if(found)
			s->key = &this->gen;
	}

	T *put(const void *key)
	{
		if(this->slots.empty())
			return nullptr;
		bool found;
		Slot *s = slot(key, &found);
		if(found)
			return &this->values[s->index];
		if(this->count >= this->values.size())
			return nullptr;
		s->key = key;
		s->gen = this->gen;
		s->index = this->count++;
		return &this->values[s->index];
	}
};

#endif
