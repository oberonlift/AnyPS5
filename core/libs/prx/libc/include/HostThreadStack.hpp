#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTTHREADSTACK_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTTHREADSTACK_HPP

#include <cstddef>
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

struct HostThreadStack {
    std::uintptr_t low = 0;
    std::uintptr_t high = 0;
};

inline HostThreadStack CurrentHostThreadStack() {
#ifdef _WIN32
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return {static_cast<std::uintptr_t>(low), static_cast<std::uintptr_t>(high)};
#else
    thread_local const HostThreadStack stack = [] {
        HostThreadStack bounds;
#ifdef __APPLE__
        const pthread_t self = pthread_self();
        bounds.high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
        bounds.low = bounds.high - pthread_get_stacksize_np(self);
#else
        pthread_attr_t attributes;
        if (pthread_getattr_np(pthread_self(), &attributes) != 0) return bounds;
        void* base = nullptr;
        std::size_t size = 0;
        if (pthread_attr_getstack(&attributes, &base, &size) == 0 && base != nullptr) {
            bounds.low = reinterpret_cast<std::uintptr_t>(base);
            bounds.high = bounds.low + size;
        }
        pthread_attr_destroy(&attributes);
#endif
        return bounds;
    }();
    return stack;
#endif
}

#endif
