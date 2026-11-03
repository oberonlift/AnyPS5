#include "prx/libSceAmpr/include/Api.hpp"
#include "prx/libkernel/include/Api.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static std::uint64_t HostAddress(std::uint64_t guest) {
    return reinterpret_cast<std::uint64_t>(GuestArena::GuestToHost(guest));
}

static void RequireMappedAt(std::uint64_t start) {
    constexpr std::uint64_t page = 0x4000;
    constexpr std::int32_t cpuReadWrite = 0x03;
    std::int64_t pool = -1;
    Require(sceAmprAmmGiveDirectMemory(0, static_cast<std::int64_t>(sceKernelGetDirectMemorySize()), page, page, 1, &pool) == 0, "AMM took no direct memory");
    alignas(8) std::array<std::uint8_t, 256> memory{};
    Apr::CommandBufferObject buffer{};
    Require(sceAmprCommandBufferConstructor(&buffer) == 0 && sceAmprCommandBufferSetBuffer(&buffer, memory.data(), memory.size()) == 0, "the AMM command buffer was not set up");
    Require(sceAmprAmmCommandBufferMap(&buffer, start, page, 0, cpuReadWrite) == 0, "the AMM map was not recorded");
    std::uint32_t id = 0;
    Require(sceAmprAmmSubmitCommandBuffer3(memory.data(), sceAmprCommandBufferGetCurrentOffset(&buffer), 0, &id) == 0 && sceAmprAmmWaitCommandBufferCompletion(id) == 0, "the AMM map did not complete");
    auto* mapped = reinterpret_cast<volatile std::uint64_t*>(HostAddress(start));
    *mapped = 0x5a5a5a5a;
    Require(*mapped == 0x5a5a5a5a, "the AMM page is not where the reported range says");
}

int main() {
    try {
        std::uintptr_t arenaBase = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
        Require(arenaBase != 0 && arenaBytes != 0, "the guest arena is unavailable");
        const std::uint64_t arenaEnd = arenaBase + arenaBytes;
        constexpr std::uint64_t occupied = 4ull << 20;
        GuestArena::GuestArenaMarkUsed_nid_postfix(reinterpret_cast<void*>(arenaEnd - occupied), occupied);

        std::uint64_t start = 0;
        std::uint64_t end = 0;
        std::uint64_t multimapStart = 0;
        std::uint64_t multimapEnd = 0;
        Require(sceAmprAmmGetVirtualAddressRanges(&start, &end, &multimapStart, &multimapEnd) == 0, "AMM ranges were not reported");
        Require(end - start == 256ull << 30 && end == multimapStart && multimapEnd - multimapStart == 32ull << 30, "AMM ranges are not a 256 GiB range and an adjacent 32 GiB multimap range");
        Require(HostAddress(start) >= arenaBase && HostAddress(multimapEnd) <= arenaEnd - occupied, "AMM ranges are not guest addresses of the free top of the guest arena");
        Require(start % (64ull << 30) == 0, "the AMM range does not start on a 64 GiB boundary");
        Require(GuestArena::GuestArenaHighestFree_nid_no_patch(multimapEnd - start, 64ull << 30) < HostAddress(start), "AMM ranges are not at the top of the free guest arena");
        RequireMappedAt(start);
        std::puts("AMM ranges placed below the used top of the guest arena");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
