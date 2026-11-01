#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMAPNOREPLACE_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTMAPNOREPLACE_HPP

#include <cerrno>
#include <cstddef>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdexcept>
#include <string>
#endif

inline void* MapAnonymousNoReplace(void* address, std::size_t bytes, int protection) {
#ifdef __APPLE__
    mach_vm_address_t placed = reinterpret_cast<mach_vm_address_t>(address);
    const kern_return_t result = mach_vm_allocate(mach_task_self(), &placed, bytes, VM_FLAGS_FIXED);
    if (result == KERN_NO_SPACE || result == KERN_INVALID_ADDRESS || result == KERN_RESOURCE_SHORTAGE) {
        errno = result == KERN_NO_SPACE ? EEXIST : ENOMEM;
        return MAP_FAILED;
    }
    if (result != KERN_SUCCESS) throw std::runtime_error(std::string("allocate a fixed anonymous mapping: ") + mach_error_string(result));
    if (::mprotect(address, bytes, protection) != 0) {
        const int error = errno;
        mach_vm_deallocate(mach_task_self(), placed, bytes);
        errno = error;
        return MAP_FAILED;
    }
    return address;
#else
    return ::mmap(address, bytes, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
}

#endif
