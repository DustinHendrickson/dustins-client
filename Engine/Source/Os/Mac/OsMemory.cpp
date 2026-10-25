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

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include "Engine/Source/Os/OsMemory.h"

#if defined(__ANDROID__)
#include <cstdlib>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

namespace {

const uintptr_t kNearRange = 0x60000000;
const uintptr_t kGranularity = 0x10000;

std::mutex s_lock;
std::map<void*, size_t>* s_allocations;

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

void* OsAllocNear(const void* anchor, size_t size) {
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
    }

    // A mapping has its size; an ordinary allocation (Android) has none.
    if (bytes) {
        munmap(ptr, bytes);
    }
#if defined(__ANDROID__)
    else if (found) {
        free(ptr);
    }
#endif
}
