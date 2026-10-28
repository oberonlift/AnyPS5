#include <ps5lift/Bridge.hpp>
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

extern "C" {
__attribute__((weak)) bool GuestArenaContains_nid_postfix(const void* pointer, std::size_t bytes);
__attribute__((weak)) void GuestArenaCommit_nid_postfix(void* pointer, std::size_t bytes, std::uint32_t protection, std::size_t granule);
}

namespace {

constexpr std::uint32_t ArenaReadWrite = 0x04;
constexpr std::uint64_t GuestStackSize = 8ull << 20;
constexpr std::size_t HostStackSize = 512ull << 20;
constexpr std::uint64_t TlsSize = 64ull << 10;
constexpr std::uint32_t ConsoleMxcsr = 0x9fc0;
constexpr std::uint16_t ConsoleX87Control = 0x037f;

std::uint64_t PageSize() {
    return static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
}

void BindDataImports(const Ps5LiftImage& image) {
    for (std::uint64_t i = 0; i < image.DataImportCount; ++i) {
        const auto& import = image.DataImports[i];
        const auto value = reinterpret_cast<std::uint64_t>(*import.Address) + static_cast<std::uint64_t>(import.Addend);
        std::memcpy(reinterpret_cast<void*>(import.Slot), &value, sizeof(value));
    }
}

void MapSegments(const Ps5LiftImage& image) {
    if (image.SegmentCount == 0) ps5lift_fail("the image has no segments");
    const auto page = PageSize();
    auto begin = ~std::uint64_t{0};
    std::uint64_t end = 0;
    for (std::uint64_t i = 0; i < image.SegmentCount; ++i) {
        begin = std::min(begin, image.Segments[i].GuestAddress & ~(page - 1));
        end = std::max(end, (image.Segments[i].GuestAddress + image.Segments[i].MemorySize + page - 1) & ~(page - 1));
    }
    auto* const start = reinterpret_cast<void*>(begin);
    if (GuestArenaContains_nid_postfix != nullptr && GuestArenaContains_nid_postfix(start, end - begin)) {
        GuestArenaCommit_nid_postfix(start, end - begin, ArenaReadWrite, end - begin);
        for (std::uint64_t i = 0; i < image.SegmentCount; ++i) std::memcpy(reinterpret_cast<void*>(image.Segments[i].GuestAddress), image.Segments[i].Data, image.Segments[i].FileSize);
        BindDataImports(image);
        return;
    }
    auto* mapped = mmap(start, end - begin, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped != start) ps5lift_fail("cannot map the guest image at 0x%" PRIx64 "-0x%" PRIx64 ": %s", begin, end, std::strerror(errno));
    for (std::uint64_t i = 0; i < image.SegmentCount; ++i) std::memcpy(reinterpret_cast<void*>(image.Segments[i].GuestAddress), image.Segments[i].Data, image.Segments[i].FileSize);
    BindDataImports(image);
}

void* MapAnonymous(const std::uint64_t size) {
    auto* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) ps5lift_fail("cannot allocate %" PRIu64 " bytes: %s", size, std::strerror(errno));
    return mapped;
}

Ps5LiftFunction Lookup(const std::uint64_t target) {
    const auto* begin = ps5lift_program.Functions;
    const auto* end = begin + ps5lift_program.FunctionCount;
    const auto* found = std::lower_bound(begin, end, target, [](const Ps5LiftFunctionEntry& entry, const std::uint64_t address) { return entry.GuestAddress < address; });
    if (found == end || found->GuestAddress != target) return nullptr;
    return found->Host;
}

thread_local Ps5LiftState* CurrentState = nullptr;

struct GuestThread {
    std::uint64_t Entry = 0;
    std::uint64_t Argument = 0;
    std::uint32_t Mxcsr = ConsoleMxcsr;
    bool Main = false;
    std::uint64_t Result = 0;
    std::uint8_t* Stack = nullptr;
    std::uint8_t* Tls = nullptr;
    pthread_t Handle{};
};

std::uint64_t PrepareThread(GuestThread& thread, Ps5LiftState& state) {
    thread.Stack = static_cast<std::uint8_t*>(MapAnonymous(GuestStackSize));
    thread.Tls = static_cast<std::uint8_t*>(MapAnonymous(TlsSize));
    const auto threadPointer = reinterpret_cast<std::uint64_t>(thread.Tls + TlsSize / 2);
    std::memcpy(reinterpret_cast<void*>(threadPointer), &threadPointer, sizeof(threadPointer));
    for (std::uint64_t i = 0; i < ps5lift_program.ImageCount; ++i) {
        const auto& image = ps5lift_program.Images[i];
        if (image.TlsMemorySize == 0) continue;
        if (image.TlsOffset > TlsSize / 2) ps5lift_fail("the static TLS of %s does not fit", image.Path);
        std::memcpy(reinterpret_cast<void*>(threadPointer - image.TlsOffset), reinterpret_cast<const void*>(image.TlsAddress), image.TlsFileSize);
    }
    state.FsBase = threadPointer;
    state.Mxcsr = thread.Mxcsr & ~0x3fu;
    state.X87Control = ConsoleX87Control;
    ps5lift_apply_mxcsr(&state);
    return reinterpret_cast<std::uint64_t>(thread.Stack + GuestStackSize);
}

void Initialize(const std::uint64_t argc, const std::uint64_t argv, const std::uint64_t environment) {
    for (std::uint64_t i = 0; i < ps5lift_program.InitializationCount; ++i) {
        const auto& image = ps5lift_program.Images[ps5lift_program.Initialization[i]];
        if (image.Init != 0) Ps5Lift::Bridge::CallGuest<void>(image.Init, argc, argv, environment);
        for (std::uint64_t index = 0; index < image.InitCount; ++index) {
            std::uint64_t initializer = 0;
            std::memcpy(&initializer, reinterpret_cast<const void*>(image.InitArray + 8 * index), sizeof(initializer));
            if (initializer != 0 && initializer != ~std::uint64_t{0}) Ps5Lift::Bridge::CallGuest<void>(initializer, argc, argv, environment);
        }
    }
}

void* RunThread(void* argument) {
    auto& thread = *static_cast<GuestThread*>(argument);
    Ps5LiftState state{};
    const auto top = PrepareThread(thread, state);
    CurrentState = &state;
    const auto function = Lookup(thread.Entry);
    if (function == nullptr) ps5lift_fail("the thread entry 0x%" PRIx64 " was not lifted", thread.Entry);

    if (thread.Main) {
        static constexpr char ProgramName[] = "eboot.bin";
        const auto name = top - sizeof(ProgramName);
        std::memcpy(reinterpret_cast<void*>(name), ProgramName, sizeof(ProgramName));
        const std::uint64_t entryStack[] = {1, name, 0, 0, 0, 0};
        const auto rsp = (name - sizeof(entryStack)) & ~std::uint64_t{15};
        std::memcpy(reinterpret_cast<void*>(rsp), entryStack, sizeof(entryStack));
        state.Gpr[PS5LIFT_RSP] = rsp;
        Initialize(entryStack[0], rsp + 8, rsp + 24);
        function(&state);
        if (state.Unwinding) ps5lift_fail("nothing on the main thread's guest stack resumes at 0x%" PRIx64, state.ResumeRip);
        std::fflush(stdout);
        std::_Exit(static_cast<int>(state.Gpr[PS5LIFT_RAX] & 0xff));
    }

    constexpr std::uint64_t ReturnAddress = 0;
    const auto rsp = (top & ~std::uint64_t{15}) - 8;
    std::memcpy(reinterpret_cast<void*>(rsp), &ReturnAddress, sizeof(ReturnAddress));
    state.Gpr[PS5LIFT_RSP] = rsp;
    state.Gpr[PS5LIFT_RDI] = thread.Argument;
    function(&state);
    if (state.Unwinding) ps5lift_fail("nothing on a guest thread's stack resumes at 0x%" PRIx64, state.ResumeRip);
    thread.Result = state.Gpr[PS5LIFT_RAX];
    return nullptr;
}

void StartThread(GuestThread& thread) {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, HostStackSize);
    const auto error = pthread_create(&thread.Handle, &attributes, RunThread, &thread);
    pthread_attr_destroy(&attributes);
    if (error != 0) ps5lift_fail("cannot start a guest thread: %s", std::strerror(error));
}

}

namespace {

constexpr std::uint32_t MxcsrExceptions = 0x3f;

#if defined(__aarch64__)
constexpr std::uint64_t FpsrBits[6] = {1u << 0, 1u << 7, 1u << 1, 1u << 2, 1u << 3, 1u << 4};
#endif

std::uint32_t HostExceptions() {
#if defined(__aarch64__)
    std::uint64_t fpsr = 0;
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    std::uint32_t flags = 0;
    for (unsigned i = 0; i < 6; ++i) {
        if (fpsr & FpsrBits[i]) flags |= 1u << i;
    }
    return flags;
#elif defined(__x86_64__)
    return _mm_getcsr() & MxcsrExceptions;
#else
    return 0;
#endif
}

void SetHostExceptions(const std::uint32_t flags) {
#if defined(__aarch64__)
    std::uint64_t fpsr = 0;
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
    for (unsigned i = 0; i < 6; ++i) {
        if (flags & (1u << i)) fpsr |= FpsrBits[i];
        else fpsr &= ~FpsrBits[i];
    }
    __asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
#elif defined(__x86_64__)
    _mm_setcsr((_mm_getcsr() & ~MxcsrExceptions) | (flags & MxcsrExceptions));
#endif
}

}

extern "C" void ps5lift_apply_mxcsr(Ps5LiftState* state) {
    const auto mxcsr = state->Mxcsr;
#if defined(__aarch64__)
    constexpr std::uint32_t MxcsrDenormalsAreZero = 1u << 6;
    constexpr std::uint32_t MxcsrFlushToZero = 1u << 15;
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
    _mm_setcsr((mxcsr & ~MxcsrExceptions) | 0x1f80u);
#endif
    SetHostExceptions(mxcsr & MxcsrExceptions);
}

extern "C" std::uint64_t ps5lift_read_tsc(void) {
#if defined(__aarch64__)
    std::uint64_t ticks;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#else
    return __rdtsc();
#endif
}

extern "C" std::uint32_t ps5lift_read_mxcsr(Ps5LiftState* state) {
    return (state->Mxcsr & ~MxcsrExceptions) | HostExceptions();
}

extern "C" void ps5lift_dispatch(Ps5LiftState* state, const std::uint64_t target) {
    const auto function = Lookup(target);
    if (function == nullptr) ps5lift_fail("indirect branch to 0x%" PRIx64 ", which was not lifted", target);
    function(state);
}

extern "C" void ps5lift_fail(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fputs("ps5lift: ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
    std::fflush(stdout);
    std::_Exit(70);
}

extern "C" void ps5lift_import_____tls__get__addr(Ps5LiftState* state) {
    std::uint64_t index[2];
    std::memcpy(index, reinterpret_cast<const void*>(state->Gpr[PS5LIFT_RDI]), sizeof(index));
    if (index[0] == 0 || index[0] > ps5lift_program.ImageCount || ps5lift_program.Images[index[0] - 1].TlsMemorySize == 0)
        ps5lift_fail("__tls_get_addr of module %" PRIu64 ", which has no TLS", index[0]);
    state->Gpr[PS5LIFT_RAX] = state->FsBase - ps5lift_program.Images[index[0] - 1].TlsOffset + index[1];
}

extern "C" void ps5lift_unsupported(Ps5LiftState*, const std::uint64_t address, const char* mnemonic) {
    ps5lift_fail("reached unsupported instruction %s at 0x%" PRIx64, mnemonic, address);
}

extern "C" void ps5lift_trap(Ps5LiftState*, const std::uint64_t address) {
    ps5lift_fail("guest trap at 0x%" PRIx64, address);
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

extern "C" std::uint64_t ps5lift_thread_create(Ps5LiftState* parent, const std::uint64_t entry, const std::uint64_t argument) {
    auto* thread = new GuestThread;
    thread->Entry = entry;
    thread->Argument = argument;
    thread->Mxcsr = ps5lift_read_mxcsr(parent);
    StartThread(*thread);
    return reinterpret_cast<std::uint64_t>(thread);
}

extern "C" Ps5LiftState* ps5lift_current_state(void) {
    if (CurrentState != nullptr) return CurrentState;
    struct ForeignThread {
        GuestThread Thread;
        Ps5LiftState State{};
        ~ForeignThread() {
            munmap(Thread.Stack, GuestStackSize);
            munmap(Thread.Tls, TlsSize);
        }
    };
    thread_local ForeignThread foreign;
    const auto top = PrepareThread(foreign.Thread, foreign.State);
    foreign.State.Gpr[PS5LIFT_RSP] = top & ~std::uint64_t{15};
    CurrentState = &foreign.State;
    return CurrentState;
}

extern "C" std::uint64_t ps5lift_thread_join(const std::uint64_t handle) {
    auto* thread = reinterpret_cast<GuestThread*>(handle);
    const auto error = pthread_join(thread->Handle, nullptr);
    if (error != 0) ps5lift_fail("cannot join a guest thread: %s", std::strerror(error));
    const auto result = thread->Result;
    munmap(thread->Stack, GuestStackSize);
    munmap(thread->Tls, TlsSize);
    delete thread;
    return result;
}

extern "C" void ps5lift_run_main(void) {
    for (std::uint64_t i = 0; i < ps5lift_program.ImageCount; ++i) {
        MapSegments(ps5lift_program.Images[i]);
    }
    GuestThread thread;
    thread.Entry = ps5lift_program.Entry;
    thread.Main = true;
    StartThread(thread);
    pthread_join(thread.Handle, nullptr);
}
