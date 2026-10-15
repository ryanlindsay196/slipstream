#include "core/memory.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdlib>

namespace ss {

void Arena::Init(const char* arenaName, void* memory, size_t size) {
    name      = arenaName;
    base      = (u8*)memory;
    capacity  = size;
    used      = 0;
    highWater = 0;
}

void* Arena::Alloc(size_t size, size_t align) {
    SS_ASSERT(align && (align & (align - 1)) == 0, "alignment must be a power of two");
    size_t start = (used + (align - 1)) & ~(align - 1);
    if (start + size > capacity) {
        plat::Log("[memory] arena '%s' over budget: requested %zu bytes, used %zu / %zu\n", name, size, used,
                  capacity);
        SS_ASSERT(false, "arena budget exceeded");
    }
    used = start + size;
    if (used > highWater) highWater = used;
    return base + start;
}

namespace heapguard {
static std::atomic<bool> g_locked{false};
static std::atomic<u64>  g_lockedAllocs{0};

void Lock() { g_locked = true; }
void Unlock() { g_locked = false; }
u64  AllocsWhileLocked() { return g_lockedAllocs; }

static void* Allocate(size_t size) {
    if (g_locked) ++g_lockedAllocs;
    void* p = malloc(size ? size : 1);
    SS_ASSERT(p, "out of memory");
    return p;
}
}  // namespace heapguard

}  // namespace ss

void* operator new(size_t size) { return ss::heapguard::Allocate(size); }
void* operator new[](size_t size) { return ss::heapguard::Allocate(size); }
void  operator delete(void* p) noexcept { free(p); }
void  operator delete[](void* p) noexcept { free(p); }
void  operator delete(void* p, size_t) noexcept { free(p); }
void  operator delete[](void* p, size_t) noexcept { free(p); }
