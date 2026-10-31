#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMAPNOREPLACE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMAPNOREPLACE_HPP

#include <cerrno>
#include <cstddef>
#include <sys/mman.h>
#include <system_error>

inline void* MapAnonymousNoReplace(void* address, std::size_t bytes, int protection) {
#ifdef __APPLE__
    void* const placed = ::mmap(address, bytes, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (placed == MAP_FAILED || placed == address) return placed;
    if (::munmap(placed, bytes) != 0) throw std::system_error(errno, std::generic_category(), "unmap a hinted mapping placed away from its hint");
    errno = EEXIST;
    return MAP_FAILED;
#else
    return ::mmap(address, bytes, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
}

#endif
