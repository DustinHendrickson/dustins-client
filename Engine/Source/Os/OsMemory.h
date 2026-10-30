#ifndef ENGINE_SOURCE_OS_OSMEMORY_H
#define ENGINE_SOURCE_OS_OSMEMORY_H

#include <cstddef>

#if defined(WOWDECOMP_ARCH_64)
void* OsAllocNear(const void* anchor, size_t size);
void OsFreeNear(void* ptr);
#if defined(__APPLE__)
// Memory that OsAllocNear can later find room near (freed with OsFreeNear).
void* OsAllocNearBase(size_t size);
#endif
#endif

#endif
