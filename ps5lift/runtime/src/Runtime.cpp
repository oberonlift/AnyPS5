#include <ps5lift/State.h>

#include <pthread.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {

constexpr std::uint64_t GuestStackSize = 8ull << 20;
constexpr std::size_t HostStackSize = 512ull << 20;
constexpr std::uint64_t TlsSize = 64ull << 10;
constexpr std::uint32_t ConsoleMxcsr = 0x9fc0;
constexpr std::uint32_t MxcsrDenormalsAreZero = 1u << 6;
constexpr std::uint32_t MxcsrFlushToZero = 1u << 15;

[[noreturn]] void Fail(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Fail(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fputs("ps5lift: ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
    std::fflush(stdout);
    std::_Exit(70);
}

std::uint64_t PageSize() {
    return static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
}

void MapSegments(const Ps5LiftImage& image) {
    if (image.SegmentCount == 0) Fail("the image has no segments");
    const auto page = PageSize();
    auto begin = ~std::uint64_t{0};
    std::uint64_t end = 0;
    for (std::uint64_t i = 0; i < image.SegmentCount; ++i) {
        begin = std::min(begin, image.Segments[i].GuestAddress & ~(page - 1));
        end = std::max(end, (image.Segments[i].GuestAddress + image.Segments[i].MemorySize + page - 1) & ~(page - 1));
    }
    auto* mapped = mmap(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped != reinterpret_cast<void*>(begin)) Fail("cannot map the guest image at 0x%" PRIx64 "-0x%" PRIx64 ": %s", begin, end, std::strerror(errno));
    for (std::uint64_t i = 0; i < image.SegmentCount; ++i) std::memcpy(reinterpret_cast<void*>(image.Segments[i].GuestAddress), image.Segments[i].Data, image.Segments[i].FileSize);
}

void* MapAnonymous(const std::uint64_t size) {
    auto* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) Fail("cannot allocate %" PRIu64 " bytes: %s", size, std::strerror(errno));
    return mapped;
}

Ps5LiftFunction Lookup(const std::uint64_t target) {
    const auto* begin = ps5lift_image.Functions;
    const auto* end = begin + ps5lift_image.FunctionCount;
    const auto* found = std::lower_bound(begin, end, target, [](const Ps5LiftFunctionEntry& entry, const std::uint64_t address) { return entry.GuestAddress < address; });
    if (found == end || found->GuestAddress != target) return nullptr;
    return found->Host;
}

void* RunGuest(void*) {
    Ps5LiftState state{};
    auto* stack = static_cast<std::uint8_t*>(MapAnonymous(GuestStackSize));
    auto* tls = static_cast<std::uint8_t*>(MapAnonymous(TlsSize));
    const auto threadPointer = reinterpret_cast<std::uint64_t>(tls + TlsSize / 2);
    std::memcpy(reinterpret_cast<void*>(threadPointer), &threadPointer, sizeof(threadPointer));
    if (ps5lift_image.TlsMemorySize != 0) {
        const auto alignment = ps5lift_image.TlsAlignment;
        const auto offset = (ps5lift_image.TlsMemorySize + alignment - 1) / alignment * alignment;
        if (offset > TlsSize / 2) Fail("the static TLS block of %" PRIu64 " bytes does not fit", ps5lift_image.TlsMemorySize);
        std::memcpy(reinterpret_cast<void*>(threadPointer - offset), reinterpret_cast<const void*>(ps5lift_image.TlsAddress), ps5lift_image.TlsFileSize);
    }
    state.FsBase = threadPointer;
    state.Mxcsr = ConsoleMxcsr;
    ps5lift_apply_mxcsr(&state);

    auto* top = stack + GuestStackSize;
    static constexpr char ProgramName[] = "eboot.bin";
    top -= sizeof(ProgramName);
    std::memcpy(top, ProgramName, sizeof(ProgramName));
    const auto argument = reinterpret_cast<std::uint64_t>(top);
    const std::uint64_t entryStack[] = {1, argument, 0, 0, 0, 0};
    auto rsp = (reinterpret_cast<std::uint64_t>(top) - sizeof(entryStack)) & ~std::uint64_t{15};
    std::memcpy(reinterpret_cast<void*>(rsp), entryStack, sizeof(entryStack));
    state.Gpr[PS5LIFT_RSP] = rsp;

    const auto entry = Lookup(ps5lift_image.Entry);
    if (entry == nullptr) Fail("the entry point 0x%" PRIx64 " was not lifted", ps5lift_image.Entry);
    entry(&state);
    std::fflush(stdout);
    std::_Exit(static_cast<int>(state.Gpr[PS5LIFT_RAX] & 0xff));
}

}

extern "C" void ps5lift_apply_mxcsr(Ps5LiftState* state) {
    const auto mxcsr = state->Mxcsr;
#if defined(__aarch64__)
    constexpr std::uint64_t AlternateHandling = 1u << 1;
    constexpr std::uint64_t FlushInputs = 1u << 0;
    constexpr std::uint64_t FlushToZero = 1u << 24;
    constexpr std::uint64_t RoundingShift = 22;
    constexpr unsigned long HwcapAfp = 1ul << 20;
    static const bool alternate = (getauxval(AT_HWCAP2) & HwcapAfp) != 0;
    static constexpr std::uint64_t Rounding[4] = {0, 2, 1, 3};
    std::uint64_t fpcr = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~(FlushInputs | AlternateHandling | FlushToZero | (std::uint64_t{3} << RoundingShift));
    fpcr |= Rounding[(mxcsr >> 13) & 3] << RoundingShift;
    if (alternate) {
        fpcr |= AlternateHandling;
        if (mxcsr & MxcsrDenormalsAreZero) fpcr |= FlushInputs;
        if (mxcsr & MxcsrFlushToZero) fpcr |= FlushToZero;
    } else if (mxcsr & (MxcsrDenormalsAreZero | MxcsrFlushToZero)) {
        fpcr |= FlushToZero;
    }
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__)
    _mm_setcsr(mxcsr & ~0x3fu);
#endif
}

extern "C" void ps5lift_dispatch(Ps5LiftState* state, const std::uint64_t target) {
    const auto function = Lookup(target);
    if (function == nullptr) Fail("indirect branch to 0x%" PRIx64 ", which was not lifted", target);
    function(state);
}

extern "C" void ps5lift_unsupported(Ps5LiftState*, const std::uint64_t address, const char* mnemonic) {
    Fail("reached unsupported instruction %s at 0x%" PRIx64, mnemonic, address);
}

extern "C" void ps5lift_trap(Ps5LiftState*, const std::uint64_t address) {
    Fail("guest trap at 0x%" PRIx64, address);
}

extern "C" void ps5lift_string_op(Ps5LiftState* state, const std::uint32_t operation, const std::uint32_t width, const std::uint32_t repeat) {
    auto& rcx = state->Gpr[PS5LIFT_RCX];
    auto& rsi = state->Gpr[PS5LIFT_RSI];
    auto& rdi = state->Gpr[PS5LIFT_RDI];
    const std::int64_t step = state->Flags[PS5LIFT_DF] ? -static_cast<std::int64_t>(width) : static_cast<std::int64_t>(width);
    auto count = repeat ? rcx : 1;
    while (count != 0) {
        if (operation == 0) std::memmove(reinterpret_cast<void*>(rdi), reinterpret_cast<const void*>(rsi), width);
        else std::memcpy(reinterpret_cast<void*>(rdi), &state->Gpr[PS5LIFT_RAX], width);
        rdi += static_cast<std::uint64_t>(step);
        if (operation == 0) rsi += static_cast<std::uint64_t>(step);
        --count;
    }
    if (repeat) rcx = 0;
}

int main() {
    MapSegments(ps5lift_image);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, HostStackSize);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, RunGuest, nullptr) != 0) Fail("cannot start the guest thread");
    pthread_join(thread, nullptr);
    return 0;
}
