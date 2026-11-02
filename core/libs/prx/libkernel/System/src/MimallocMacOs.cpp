#ifdef __APPLE__

#include <crt_externs.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "prx/libc/include/GuestArena.hpp"

namespace {

std::filesystem::path Allocator() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&Allocator), &info) == 0 || info.dli_fname == nullptr) return {};
    return std::filesystem::path(info.dli_fname).parent_path() / "libmimalloc.dylib";
}

std::string Executable() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> executable(size);
    if (_NSGetExecutablePath(executable.data(), &size) != 0) return {};
    return executable.data();
}

#ifdef ANYPS5_TRANSLATED_GUEST
[[noreturn]] void Fail(const std::string& reason) {
    std::fprintf(stderr, "[memory] a translated guest needs mimalloc above the guest's first terabyte: %s\n", reason.c_str());
    std::abort();
}

[[gnu::constructor]] void RestartWithMimalloc() {
    const bool restarted = std::getenv("APS5_MIMALLOC_RESTARTED") != nullptr;
    unsetenv("APS5_MIMALLOC_RESTARTED");
    if (dlsym(RTLD_DEFAULT, "mi_version") != nullptr) return;
    if (restarted) Fail("dyld did not load libmimalloc.dylib after the restart");
    const auto allocator = Allocator();
    std::error_code error;
    if (allocator.empty() || !std::filesystem::is_regular_file(allocator, error)) Fail("no libmimalloc.dylib at " + allocator.string());
    const auto executable = Executable();
    if (executable.empty()) Fail("cannot find the executable to restart");
    const char* inserted = std::getenv("DYLD_INSERT_LIBRARIES");
    const auto libraries = inserted == nullptr || *inserted == 0 ? allocator.string() : allocator.string() + ":" + inserted;
    setenv("DYLD_INSERT_LIBRARIES", libraries.c_str(), 1);
    setenv("APS5_MIMALLOC_RESTARTED", "1", 1);
    execv(executable.c_str(), *_NSGetArgv());
    Fail(std::string("execv failed: ") + std::strerror(errno));
}
#else
[[gnu::constructor]] void RestartWithMimalloc() {
    const bool restarted = std::getenv("APS5_MIMALLOC_RESTARTED") != nullptr;
    unsetenv("APS5_MIMALLOC_RESTARTED");
    if (std::getenv("APS5_MIMALLOC") == nullptr || dlsym(RTLD_DEFAULT, "mi_version") != nullptr) return;
    if (restarted) {
        std::fprintf(stderr, "[memory] APS5_MIMALLOC: dyld did not load libmimalloc.dylib, the title keeps the system allocator\n");
        return;
    }
    const auto allocator = Allocator();
    std::error_code error;
    if (allocator.empty() || !std::filesystem::is_regular_file(allocator, error)) return;
    const auto executable = Executable();
    if (executable.empty()) return;
    const char* inserted = std::getenv("DYLD_INSERT_LIBRARIES");
    const std::string previous = inserted == nullptr ? std::string() : inserted;
    const auto libraries = previous.empty() ? allocator.string() : allocator.string() + ":" + previous;
    setenv("DYLD_INSERT_LIBRARIES", libraries.c_str(), 1);
    setenv("APS5_MIMALLOC_RESTARTED", "1", 1);
    execv(executable.c_str(), *_NSGetArgv());
    std::perror("[memory] APS5_MIMALLOC: execv");
    unsetenv("APS5_MIMALLOC_RESTARTED");
    if (inserted == nullptr) unsetenv("DYLD_INSERT_LIBRARIES");
    else setenv("DYLD_INSERT_LIBRARIES", previous.c_str(), 1);
}
#endif

}

#endif
