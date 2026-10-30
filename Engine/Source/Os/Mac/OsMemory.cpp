// macOS implementation of Os/Win64/OsMemory.cpp: memory within +-1.5 GB of an address (for code that must be
// reachable with 32-bit displacements). mmap takes the address as a hint, so candidates are tried outward from the
// anchor in 64 KB steps, keeping the first mapping that lands in range. munmap needs the size, which OsFreeNear isn't
// given, so allocations are remembered.
//
// Android builds this file too. There a hinted mmap whose address is taken is moved anywhere (Linux), and the heap's
// reservations can surround the anchor, so the search alone finds nothing: a mapping where the kernel puts it and an
// ordinary allocation are tried first (both usually land near the model, which came from the same places), then the
// search, asking the kernel not to move a candidate (MAP_FIXED_NOREPLACE). Android's heap also tags its pointers in
// the top byte (0xb4...), which the processor ignores on loads and stores: distances are measured, and the search
// started, without it.
//
// On Apple silicon the search finds nothing either: ordinary allocations sit in a narrow gap between two 12 GB
// reservations (the commpage and the GPU carveout), so a model loaded there could never get its .anim buffers (stealth
// walk and stand, many emotes) and played its fallback instead. Model data is put in one reserved range of the near
// distance instead (OsAllocNearBase), and buffers for it come from the same range: any two addresses in it are near.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include "Engine/Source/Os/OsMemory.h"

#if defined(__ANDROID__)
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

namespace {

const uintptr_t kNearRange = 0x60000000;
const uintptr_t kGranularity = 0x10000;

std::mutex s_lock;
std::map<void*, size_t>* s_allocations;

#if defined(__APPLE__)
const uintptr_t kArenaSize = kNearRange;
const uintptr_t kArenaPage = 0x4000;

// 0 until first used; UINTPTR_MAX if the range couldn't be reserved.
uintptr_t s_arenaBase;
// Free spans of the range, by start address.
std::map<uintptr_t, uintptr_t>* s_arenaFree;
#endif

}

// An address without the heap's tag in its top byte (Android); elsewhere, as it is.
static uintptr_t Untagged(const void* ptr) {
    uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
#if defined(__ANDROID__) && defined(__aarch64__)
    address &= (uintptr_t(1) << 56) - 1;
#endif
    return address;
}

static bool IsNear(uintptr_t center, const void* ptr, uintptr_t bytes) {
    uintptr_t address = Untagged(ptr);
    uintptr_t distance = address > center ? address - center : center - address;
    return distance + bytes <= kNearRange;
}

static void Remember(void* ptr, size_t bytes) {
    std::lock_guard<std::mutex> lock(s_lock);

    if (!s_allocations) {
        s_allocations = new std::map<void*, size_t>();
    }

    (*s_allocations)[ptr] = bytes;
}

#if defined(__APPLE__)
static bool InArena(const void* ptr) {
    uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    return s_arenaBase && s_arenaBase != UINTPTR_MAX && address >= s_arenaBase && address < s_arenaBase + kArenaSize;
}

// Pages from the reserved range, mapped readable and writable (zeroed). Called with s_lock held.
static void* ArenaAlloc(size_t size) {
    if (!s_arenaBase) {
        void* range = mmap(nullptr, kArenaSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);

        if (range == MAP_FAILED) {
            s_arenaBase = UINTPTR_MAX;
            return nullptr;
        }

        s_arenaBase = reinterpret_cast<uintptr_t>(range);
        s_arenaFree = new std::map<uintptr_t, uintptr_t>();
        (*s_arenaFree)[s_arenaBase] = kArenaSize;
    }

    if (s_arenaBase == UINTPTR_MAX) {
        return nullptr;
    }

    uintptr_t bytes = (size + kArenaPage - 1) & ~(kArenaPage - 1);

    for (auto it = s_arenaFree->begin(); it != s_arenaFree->end(); ++it) {
        if (it->second < bytes) {
            continue;
        }

        uintptr_t start = it->first;
        uintptr_t left = it->second - bytes;
        s_arenaFree->erase(it);

        if (left) {
            (*s_arenaFree)[start + bytes] = left;
        }

        void* ptr = mmap(reinterpret_cast<void*>(start), bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);

        if (ptr == MAP_FAILED) {
            (*s_arenaFree)[start] = bytes;
            return nullptr;
        }

        if (!s_allocations) {
            s_allocations = new std::map<void*, size_t>();
        }

        (*s_allocations)[ptr] = bytes;
        return ptr;
    }

    return nullptr;
}

// Returns the pages to the system, keeping the range reserved. Called with s_lock held.
static void ArenaFree(void* ptr, uintptr_t bytes) {
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    mmap(ptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);

    auto next = s_arenaFree->lower_bound(start);

    if (next != s_arenaFree->end() && start + bytes == next->first) {
        bytes += next->second;
        next = s_arenaFree->erase(next);
    }

    if (next != s_arenaFree->begin()) {
        auto previous = std::prev(next);

        if (previous->first + previous->second == start) {
            previous->second += bytes;
            return;
        }
    }

    (*s_arenaFree)[start] = bytes;
}

void* OsAllocNearBase(size_t size) {
    {
        std::lock_guard<std::mutex> lock(s_lock);
        void* ptr = ArenaAlloc(size);

        if (ptr) {
            return ptr;
        }
    }

    // The range is full: an ordinary allocation, whose buffers may then not find room near it.
    void* allocated = aligned_alloc(16, (size + 15) & ~static_cast<size_t>(15));

    if (allocated) {
        Remember(allocated, 0);
    }

    return allocated;
}
#endif

void* OsAllocNear(const void* anchor, size_t size) {
#if defined(__APPLE__)
    {
        std::lock_guard<std::mutex> lock(s_lock);

        if (InArena(anchor)) {
            void* ptr = ArenaAlloc(size);

            if (ptr) {
                return ptr;
            }
        }
    }
#endif

    uintptr_t center = Untagged(anchor) & ~(kGranularity - 1);
    uintptr_t bytes = (size + kGranularity - 1) & ~(kGranularity - 1);

#if defined(__ANDROID__)
    // Where the kernel puts a new mapping.
    void* mapped = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    if (mapped != MAP_FAILED) {
        if (IsNear(center, mapped, bytes)) {
            Remember(mapped, bytes);
            return mapped;
        }

        munmap(mapped, bytes);
    }

    // An ordinary allocation (freed with free: remembered with no size).
    void* allocated = aligned_alloc(16, (size + 15) & ~static_cast<size_t>(15));

    if (allocated) {
        if (IsNear(center, allocated, size)) {
            Remember(allocated, 0);
            return allocated;
        }

        free(allocated);
    }

    const int flags = MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE;
#else
    const int flags = MAP_PRIVATE | MAP_ANON;
#endif

    for (uintptr_t offset = kGranularity; offset < kNearRange; offset += kGranularity) {
        uintptr_t candidates[2] = { center + offset, center > offset ? center - offset : 0 };

        for (uintptr_t candidate : candidates) {
            if (!candidate) {
                continue;
            }

            void* ptr = mmap(reinterpret_cast<void*>(candidate), bytes, PROT_READ | PROT_WRITE, flags, -1, 0);

            if (ptr == MAP_FAILED) {
                continue;
            }

            if (IsNear(center, ptr, bytes)) {
                Remember(ptr, bytes);
                return ptr;
            }

            munmap(ptr, bytes);
        }
    }

    return nullptr;
}

void OsFreeNear(void* ptr) {
    if (!ptr) {
        return;
    }

    size_t bytes = 0;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(s_lock);

        if (s_allocations) {
            auto it = s_allocations->find(ptr);

            if (it != s_allocations->end()) {
                bytes = it->second;
                found = true;
                s_allocations->erase(it);
            }
        }

#if defined(__APPLE__)
        if (found && InArena(ptr)) {
            ArenaFree(ptr, bytes);
            return;
        }
#endif
    }

    // A mapping has its size; an ordinary allocation has none.
    if (bytes) {
        munmap(ptr, bytes);
    } else if (found) {
        free(ptr);
    }
}
