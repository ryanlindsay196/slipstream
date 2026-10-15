// memory.h - fixed-budget arenas and a heap tripwire.
#pragma once

#include "core/base.h"

namespace ss {

struct Arena {
    const char* name      = "";
    u8*         base      = nullptr;
    size_t      capacity  = 0;
    size_t      used      = 0;
    size_t      highWater = 0;

    void  Init(const char* arenaName, void* memory, size_t size);
    void* Alloc(size_t size, size_t align = 16);  // asserts with a budget report on overflow

    template <class T>
    T* Push(size_t count = 1) {
        static_assert(std::is_trivially_copyable_v<T>, "Arena::Push expects POD-style types");
        T* p = (T*)Alloc(sizeof(T) * count, alignof(T) < 16 ? 16 : alignof(T));
        memset((void*)p, 0, sizeof(T) * count);
        return p;
    }

    size_t Mark() const { return used; }
    void   Rewind(size_t mark) { used = mark; }
    void   Reset() { used = 0; }
};

struct ScratchScope {
    Arena& arena;
    size_t mark;
    explicit ScratchScope(Arena& a) : arena(a), mark(a.Mark()) {}
    ~ScratchScope() { arena.Rewind(mark); }
    ScratchScope(const ScratchScope&)            = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
};

namespace heapguard {
void Lock();  // from here on, any operator new is a budget violation
void Unlock();
u64  AllocsWhileLocked();
}  // namespace heapguard

}  // namespace ss
