#include "Runtime.hpp"

#include <ps5lift/Bridge.hpp>
#include <ps5lift/State.h>

#include <pthread.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>
#else
#include <sys/auxv.h>
#endif

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {

constexpr std::uint64_t GuestSpaceEnd = std::uint64_t{1} << 40;

#if defined(__APPLE__)
bool HostFeature(const char* name) {
    int value = 0;
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) ps5lift_fail("cannot read the host CPU feature %s: %s", name, std::strerror(errno));
    return value != 0;
}

constexpr std::uint64_t GuestImagesStart = 0xFC00000000ull + PS5LIFT_GUEST_BASE;
constexpr std::uint64_t GuestImagesEnd = GuestSpaceEnd + PS5LIFT_GUEST_BASE;
constexpr std::uint64_t ThreadAreaStart = 0x800000000ull + PS5LIFT_GUEST_BASE;
constexpr std::uint64_t ThreadAreaEnd = 0xFC0000000ull + PS5LIFT_GUEST_BASE;
constexpr std::uint64_t ThreadSlotSize = 16ull << 20;

kern_return_t Reserve(const std::uint64_t begin, const std::uint64_t end) {
    mach_vm_address_t address = begin;
    return mach_vm_map(mach_task_self(), &address, end - begin, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT);
}

void ReserveExactly(const std::uint64_t begin, const std::uint64_t end, const char* what) {
    if (const kern_return_t result = Reserve(begin, end); result != KERN_SUCCESS)
        ps5lift_fail("cannot reserve %s at 0x%" PRIx64 "-0x%" PRIx64 ": %s", what, begin, end, mach_error_string(result));
}

void ReserveFreeBelow(const std::uint64_t limit) {
    std::uint64_t cursor = 0;
    while (cursor < limit) {
        mach_vm_address_t region = cursor;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const kern_return_t result = mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (result != KERN_SUCCESS && result != KERN_INVALID_ADDRESS) ps5lift_fail("cannot list the host's mappings: %s", mach_error_string(result));
        const std::uint64_t next = result == KERN_INVALID_ADDRESS ? limit : std::min<std::uint64_t>(region, limit);
        if (next > cursor) {
            const kern_return_t reserved = Reserve(cursor, next);
            if (reserved != KERN_SUCCESS && reserved != KERN_NO_SPACE && reserved != KERN_INVALID_ADDRESS)
                ps5lift_fail("cannot reserve 0x%" PRIx64 "-0x%" PRIx64 " below the guest window: %s", cursor, next, mach_error_string(reserved));
        }
        if (result == KERN_INVALID_ADDRESS) break;
        cursor = region + size;
    }
}

struct GuestWindowReservation {
    GuestWindowReservation() {
        ReserveExactly(GuestImagesStart, GuestImagesEnd, "the guest images' range");
        ReserveExactly(ThreadAreaStart, ThreadAreaEnd, "the guest threads' memory");
        ReserveFreeBelow(GuestSpaceEnd);
    }
};

const GuestWindowReservation ReservedGuestWindow;

class ThreadArea {
public:
    static ThreadArea& Get() {
        static ThreadArea& area = *new ThreadArea;
        return area;
    }

    std::uint8_t* Take(const std::uint64_t guard, const std::uint64_t bytes) {
        if (guard + bytes > ThreadSlotSize) ps5lift_fail("guest thread memory of 0x%" PRIx64 " bytes does not fit a slot", bytes);
        std::uint64_t slot = 0;
        {
            std::lock_guard lock(_lock);
            if (!_free.empty()) {
                slot = _free.back();
                _free.pop_back();
            } else {
                if (_next + ThreadSlotSize > ThreadAreaEnd) ps5lift_fail("the guest threads' memory is full");
                slot = _next;
                _next += ThreadSlotSize;
            }
        }
        auto* memory = reinterpret_cast<std::uint8_t*>(slot + guard);
        if (mprotect(memory, bytes, PROT_READ | PROT_WRITE) != 0) ps5lift_fail("cannot commit guest thread memory at %p: %s", static_cast<void*>(memory), std::strerror(errno));
        return memory;
    }

    void Give(std::uint8_t* const memory, const std::uint64_t guard) {
        const auto slot = reinterpret_cast<std::uint64_t>(memory) - guard;
        if (mmap(reinterpret_cast<void*>(slot), ThreadSlotSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
            ps5lift_fail("cannot release guest thread memory at 0x%" PRIx64 ": %s", slot, std::strerror(errno));
        std::lock_guard lock(_lock);
        _free.push_back(slot);
    }

private:
    std::mutex _lock;
    std::vector<std::uint64_t> _free;
    std::uint64_t _next = ThreadAreaStart;
};
#endif

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
        const auto value = Ps5Lift::Bridge::GuestAddress(static_cast<const char*>(*import.Address) + import.Addend);
        std::memcpy(Ps5Lift::Bridge::HostAddress(import.Slot), &value, sizeof(value));
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
    auto* const start = Ps5Lift::Bridge::HostAddress(begin);
    static auto* const arenaContains = Ps5Lift::AnyPs5Function<bool(const void*, std::size_t)>("GuestArenaContains_nid_postfix");
    static auto* const arenaCommit = Ps5Lift::AnyPs5Function<void(void*, std::size_t, std::uint32_t, std::size_t)>("GuestArenaCommit_nid_postfix");
    if (arenaContains != nullptr && arenaContains(start, end - begin)) {
        arenaCommit(start, end - begin, ArenaReadWrite, end - begin);
        for (std::uint64_t i = 0; i < image.SegmentCount; ++i) std::memcpy(Ps5Lift::Bridge::HostAddress(image.Segments[i].GuestAddress), image.Segments[i].Data, image.Segments[i].FileSize);
        BindDataImports(image);
        return;
    }
#if defined(__APPLE__)
    mach_vm_address_t address = reinterpret_cast<std::uint64_t>(start);
    const int reserved = address >= GuestImagesStart && address + (end - begin) <= GuestImagesEnd ? VM_FLAGS_OVERWRITE : 0;
    if (const kern_return_t result = mach_vm_allocate(mach_task_self(), &address, end - begin, VM_FLAGS_FIXED | reserved); result != KERN_SUCCESS)
        ps5lift_fail("cannot map the guest image at 0x%" PRIx64 "-0x%" PRIx64 ": %s", begin, end, mach_error_string(result));
#else
    auto* mapped = mmap(start, end - begin, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped != start) ps5lift_fail("cannot map the guest image at 0x%" PRIx64 "-0x%" PRIx64 ": %s", begin, end, std::strerror(errno));
#endif
    for (std::uint64_t i = 0; i < image.SegmentCount; ++i) std::memcpy(Ps5Lift::Bridge::HostAddress(image.Segments[i].GuestAddress), image.Segments[i].Data, image.Segments[i].FileSize);
    BindDataImports(image);
}

void* MapAnonymous(const std::uint64_t size) {
    auto* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) ps5lift_fail("cannot allocate %" PRIu64 " bytes: %s", size, std::strerror(errno));
    return mapped;
}

struct ThreadMemory {
    std::uint8_t* Stack = nullptr;
    std::uint8_t* Tls = nullptr;
    std::uint8_t* InterruptStack = nullptr;
};

struct HostFloatingPoint {
#if defined(__aarch64__)
    std::uint64_t Fpcr = 0;
    std::uint64_t Fpsr = 0;
#elif defined(__x86_64__)
    std::uint32_t Mxcsr = 0;
#endif
};

HostFloatingPoint SaveHostFloatingPoint() {
    HostFloatingPoint saved;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved.Fpcr));
    __asm__ volatile("mrs %0, fpsr" : "=r"(saved.Fpsr));
#elif defined(__x86_64__)
    saved.Mxcsr = _mm_getcsr();
#endif
    return saved;
}

void RestoreHostFloatingPoint(const HostFloatingPoint& saved) {
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(saved.Fpcr));
    __asm__ volatile("msr fpsr, %0" : : "r"(saved.Fpsr));
#elif defined(__x86_64__)
    _mm_setcsr(saved.Mxcsr);
#endif
}

struct ForeignThread {
    ThreadMemory Memory;
    Ps5LiftState State{};
    std::uint64_t Entered = 0;
    HostFloatingPoint Host;
};

thread_local Ps5LiftState* CurrentState = nullptr;
thread_local std::uint64_t InterruptStackTop = 0;
thread_local ForeignThread* Foreign = nullptr;

bool GuestEnvironmentLive() {
    return Foreign == nullptr || Foreign->Entered != 0;
}

#if defined(__APPLE__)
std::uint8_t* MapGuestStack() {
    return ThreadArea::Get().Take(PageSize(), GuestStackSize);
}

void UnmapGuestStack(std::uint8_t* const stack) {
    ThreadArea::Get().Give(stack, PageSize());
}

std::uint8_t* MapTls() {
    return ThreadArea::Get().Take(0, TlsSize);
}

void UnmapTls(std::uint8_t* const tls) {
    ThreadArea::Get().Give(tls, 0);
}
#else
std::uint8_t* MapGuestStack() {
    const auto guard = PageSize();
    auto* mapping = static_cast<std::uint8_t*>(MapAnonymous(guard + GuestStackSize));
    if (mprotect(mapping, guard, PROT_NONE) != 0) ps5lift_fail("cannot protect the guard page below a guest stack: %s", std::strerror(errno));
    return mapping + guard;
}

void UnmapGuestStack(std::uint8_t* const stack) {
    const auto guard = PageSize();
    munmap(stack - guard, guard + GuestStackSize);
}

std::uint8_t* MapTls() {
    return static_cast<std::uint8_t*>(MapAnonymous(TlsSize));
}

void UnmapTls(std::uint8_t* const tls) {
    munmap(tls, TlsSize);
}
#endif

std::uint64_t StackTop(const std::uint8_t* const stack) {
    return Ps5Lift::Bridge::GuestAddress(stack + GuestStackSize);
}

ThreadMemory MapThreadMemory(const bool ownStack) {
    return {ownStack ? MapGuestStack() : nullptr, MapTls(), MapGuestStack()};
}

void UnmapThreadMemory(const ThreadMemory& memory) {
    if (memory.Stack != nullptr) UnmapGuestStack(memory.Stack);
    UnmapTls(memory.Tls);
    UnmapGuestStack(memory.InterruptStack);
}

void BindThread(Ps5LiftState* const state, const ThreadMemory& memory) {
    CurrentState = state;
    InterruptStackTop = StackTop(memory.InterruptStack);
}

void UnbindThread() {
    CurrentState = nullptr;
    InterruptStackTop = 0;
}

void PrepareState(Ps5LiftState& state, std::uint8_t* const tls, const std::uint32_t mxcsr) {
    const auto& program = Ps5Lift::Program();
    auto* const hostThreadPointer = tls + TlsSize / 2;
    const auto threadPointer = Ps5Lift::Bridge::GuestAddress(hostThreadPointer);
    std::memcpy(hostThreadPointer, &threadPointer, sizeof(threadPointer));
    for (std::uint64_t i = 0; i < program.ImageCount; ++i) {
        const auto& image = program.Images[i];
        if (image.TlsMemorySize == 0) continue;
        if (image.TlsOffset > TlsSize / 2) ps5lift_fail("the static TLS of image %llu does not fit", static_cast<unsigned long long>(i));
        std::memcpy(hostThreadPointer - image.TlsOffset, Ps5Lift::Bridge::HostAddress(image.TlsAddress), image.TlsFileSize);
    }
    state.FsBase = threadPointer;
    state.Mxcsr = mxcsr & ~0x3fu;
    state.X87Control = ConsoleX87Control;
}

void Initialize(const std::uint64_t argc, const std::uint64_t argv, const std::uint64_t environment) {
    const auto& program = Ps5Lift::Program();
    for (std::uint64_t i = 0; i < program.InitializationCount; ++i) {
        const auto& image = program.Images[program.Initialization[i]];
        if (image.Init != 0) Ps5Lift::Bridge::CallGuest<void>(image.Init, argc, argv, environment);
        for (std::uint64_t index = 0; index < image.InitCount; ++index) {
            std::uint64_t initializer = 0;
            std::memcpy(&initializer, Ps5Lift::Bridge::HostAddress(image.InitArray + 8 * index), sizeof(initializer));
            if (initializer != 0 && initializer != ~std::uint64_t{0}) Ps5Lift::Bridge::CallGuest<void>(initializer, argc, argv, environment);
        }
    }
}

void* RunMain(void* argument) {
    const auto entry = *static_cast<const std::uint64_t*>(argument);
    Ps5LiftState state{};
    const auto memory = MapThreadMemory(true);
    PrepareState(state, memory.Tls, ConsoleMxcsr);
    const auto top = StackTop(memory.Stack);
    BindThread(&state, memory);
    ps5lift_apply_mxcsr(&state);
    const auto function = Ps5Lift::LookupFunction(entry);
    if (function == nullptr) ps5lift_fail("the program entry 0x%" PRIx64 " was not lifted", entry);
    static constexpr char ProgramName[] = "eboot.bin";
    const auto name = top - sizeof(ProgramName);
    std::memcpy(Ps5Lift::Bridge::HostAddress(name), ProgramName, sizeof(ProgramName));
    const std::uint64_t entryStack[] = {1, name, 0, 0, 0, 0};
    const auto rsp = (name - sizeof(entryStack)) & ~std::uint64_t{15};
    std::memcpy(Ps5Lift::Bridge::HostAddress(rsp), entryStack, sizeof(entryStack));
    state.Gpr[PS5LIFT_RSP] = rsp;
    Initialize(entryStack[0], rsp + 8, rsp + 24);
    function(&state);
    if (state.Unwinding) ps5lift_fail_unresumed(&state, "on the main thread's guest stack");
    std::fflush(stdout);
    std::_Exit(static_cast<int>(state.Gpr[PS5LIFT_RAX] & 0xff));
}

pthread_t StartMain(std::uint64_t& entry) {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, HostStackSize);
    pthread_t handle{};
    const auto error = pthread_create(&handle, &attributes, RunMain, &entry);
    pthread_attr_destroy(&attributes);
    if (error != 0) ps5lift_fail("cannot start the main guest thread: %s", std::strerror(error));
    return handle;
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
    if (!GuestEnvironmentLive()) return;
    const auto mxcsr = state->Mxcsr;
#if defined(__aarch64__)
    constexpr std::uint32_t MxcsrDenormalsAreZero = 1u << 6;
    constexpr std::uint32_t MxcsrFlushToZero = 1u << 15;
    constexpr std::uint64_t FlushToZero = 1u << 24;
    constexpr std::uint64_t RoundingShift = 22;
    static constexpr std::uint64_t Rounding[4] = {0, 2, 1, 3};
    std::uint64_t fpcr = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~(FlushToZero | (std::uint64_t{3} << RoundingShift));
    fpcr |= Rounding[(mxcsr >> 13) & 3] << RoundingShift;
    if (mxcsr & (MxcsrDenormalsAreZero | MxcsrFlushToZero)) fpcr |= FlushToZero;
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

extern "C" void ps5lift_flush_cache_line(const std::uint64_t address) {
#if defined(__aarch64__)
    asm volatile("dc civac, %0" : : "r"(Ps5Lift::Bridge::HostAddress(address)) : "memory");
#else
    _mm_clflush(Ps5Lift::Bridge::HostAddress(address));
#endif
}

extern "C" void ps5lift_write_back_cache_line(const std::uint64_t address) {
#if defined(__aarch64__)
    asm volatile("dc cvac, %0" : : "r"(Ps5Lift::Bridge::HostAddress(address)) : "memory");
#else
    _mm_clflush(Ps5Lift::Bridge::HostAddress(address));
#endif
}

extern "C" std::uint64_t ps5lift_random(void) {
    std::uint64_t value = 0;
#if defined(__APPLE__)
    if (getentropy(&value, sizeof(value)) != 0) ps5lift_fail("rdrand: the host gave no random bytes: %s", std::strerror(errno));
#else
    if (getrandom(&value, sizeof(value), 0) != static_cast<ssize_t>(sizeof(value))) ps5lift_fail("rdrand: the host gave no random bytes: %s", std::strerror(errno));
#endif
    return value;
}

extern "C" std::uint32_t ps5lift_processor_id(void) {
#if defined(__APPLE__)
    std::size_t processor = 0;
    if (const int error = pthread_cpu_number_np(&processor); error != 0) ps5lift_fail("rdpid: the host cannot tell which processor runs this thread: %s", std::strerror(error));
#else
    const int processor = sched_getcpu();
    if (processor < 0) ps5lift_fail("rdpid: the host cannot tell which processor runs this thread: %s", std::strerror(errno));
#endif
    return static_cast<std::uint32_t>(processor);
}

extern "C" std::uint32_t ps5lift_read_mxcsr(Ps5LiftState* state) {
    if (!GuestEnvironmentLive()) return state->Mxcsr;
    return (state->Mxcsr & ~MxcsrExceptions) | HostExceptions();
}

extern "C" Ps5LiftFunction ps5lift_resolve(const std::uint64_t target) {
    const auto function = Ps5Lift::LookupFunction(target);
    if (function == nullptr) ps5lift_fail("indirect branch to 0x%" PRIx64 ", which was not lifted", target);
    return function;
}

namespace {

struct ReturnJump {
    std::uint64_t Rip = 0;
    std::uint64_t Rsp = 0;
};

thread_local ReturnJump LastReturnJump;

void ReturnByJump(Ps5LiftState*) {}

}

extern "C" Ps5LiftFunction ps5lift_resolve_jump(Ps5LiftState* state, const std::uint64_t target) {
    if (const auto function = Ps5Lift::LookupFunction(target)) return function;
    state->ResumeRip = target;
    state->ResumeRsp = state->Gpr[PS5LIFT_RSP];
    state->ReturnSlot = 0;
    state->UnwindPassed = 0;
    state->Unwinding = 1;
    LastReturnJump = {target, state->ResumeRsp};
    return ReturnByJump;
}

extern "C" void ps5lift_fail_unresumed(const Ps5LiftState* state, const char* frames) {
    if (state->ResumeRip == LastReturnJump.Rip && state->ResumeRsp == LastReturnJump.Rsp) {
        ps5lift_fail("indirect jump to 0x%" PRIx64 ", which was not lifted and where no call %s returns", state->ResumeRip, frames);
    }
    ps5lift_fail("a longjmp, exception or ret on a switched stack resumes at 0x%" PRIx64 ", where no frame %s resumes", state->ResumeRip, frames);
}

extern "C" void ps5lift_dispatch(Ps5LiftState* state, const std::uint64_t target) {
    ps5lift_resolve(target)(state);
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
    const auto& program = Ps5Lift::Program();
    std::uint64_t index[2];
    std::memcpy(index, Ps5Lift::Bridge::HostAddress(state->Gpr[PS5LIFT_RDI]), sizeof(index));
    if (index[0] == 0 || index[0] > program.ImageCount || program.Images[index[0] - 1].TlsMemorySize == 0)
        ps5lift_fail("__tls_get_addr of module %" PRIu64 ", which has no TLS", index[0]);
    state->Gpr[PS5LIFT_RAX] = state->FsBase - program.Images[index[0] - 1].TlsOffset + index[1];
}

extern "C" void ps5lift_unsupported(Ps5LiftState*, const std::uint64_t address, const char* mnemonic) {
    ps5lift_fail("reached unsupported instruction %s at 0x%" PRIx64, mnemonic, address);
}

extern "C" void ps5lift_unknown_resume(Ps5LiftState*, const std::uint64_t function, const std::uint64_t resume) {
    ps5lift_fail("a longjmp or exception resumes at 0x%" PRIx64 " in the frame of 0x%" PRIx64 ", which has no resume point there", resume, function);
}

extern "C" void ps5lift_interrupt(Ps5LiftState* state, const std::uint64_t address, const std::uint32_t vector) {
    std::uint32_t code = 0;
    std::memcpy(&code, Ps5Lift::Bridge::HostAddress(state->FsBase + 0x28), sizeof(code));
    ps5lift_fail("guest interrupt 0x%x at 0x%" PRIx64 ", with 0x%08x at %%fs:0x28", vector, address, code);
}

extern "C" void ps5lift_trap(Ps5LiftState*, const std::uint64_t address) {
    ps5lift_fail("guest trap at 0x%" PRIx64, address);
}

extern "C" void ps5lift_left_jump_table(Ps5LiftState*, const std::uint64_t address) {
    ps5lift_fail("the jump at 0x%" PRIx64 " went outside its jump table, which discovery found closed", address);
}

extern "C" void ps5lift_divide_error(Ps5LiftState*, const std::uint64_t address) {
    ps5lift_fail("divide error at 0x%" PRIx64 ": the divisor is zero or the quotient does not fit", address);
}

extern "C" void ps5lift_string_op(Ps5LiftState* state, const std::uint32_t operation, const std::uint32_t width, const std::uint32_t repeat, const std::uint64_t sourceBase) {
    auto& rcx = state->Gpr[PS5LIFT_RCX];
    auto& rsi = state->Gpr[PS5LIFT_RSI];
    auto& rdi = state->Gpr[PS5LIFT_RDI];
    const bool forward = state->Flags[PS5LIFT_DF] == 0;
    const std::int64_t step = forward ? static_cast<std::int64_t>(width) : -static_cast<std::int64_t>(width);
    const std::uint64_t count = repeat ? rcx : 1;
    const std::uint64_t bytes = count * width;
    auto* destination = static_cast<std::uint8_t*>(Ps5Lift::Bridge::HostAddress(rdi));
    switch (operation) {
    case PS5LIFT_STRING_MOVS: {
        const auto* source = static_cast<const std::uint8_t*>(Ps5Lift::Bridge::HostAddress(rsi + sourceBase));
        if (forward && (destination <= source || destination >= source + bytes)) {
            std::memmove(destination, source, bytes);
        } else {
            for (std::uint64_t i = 0; i < count; ++i) std::memmove(destination + static_cast<std::int64_t>(i) * step, source + static_cast<std::int64_t>(i) * step, width);
        }
        rsi += static_cast<std::uint64_t>(static_cast<std::int64_t>(count) * step);
        break;
    }
    case PS5LIFT_STRING_STOS:
        if (forward && width == 1) {
            std::memset(destination, static_cast<int>(state->Gpr[PS5LIFT_RAX] & 0xff), bytes);
        } else {
            for (std::uint64_t i = 0; i < count; ++i) std::memcpy(destination + static_cast<std::int64_t>(i) * step, &state->Gpr[PS5LIFT_RAX], width);
        }
        break;
    default:
        ps5lift_fail("unknown string operation %u", operation);
    }
    rdi += static_cast<std::uint64_t>(static_cast<std::int64_t>(count) * step);
    if (repeat) rcx = 0;
}

namespace {

void ReleaseForeignThread(void* pointer) {
    auto* foreign = static_cast<ForeignThread*>(pointer);
    Foreign = nullptr;
    UnbindThread();
    UnmapThreadMemory(foreign->Memory);
    delete foreign;
}

pthread_key_t ForeignThreadKey() {
    static const pthread_key_t key = [] {
        pthread_key_t created;
        if (const int error = pthread_key_create(&created, ReleaseForeignThread); error != 0) ps5lift_fail("cannot create the key that releases native threads' guest state: %s", std::strerror(error));
        return created;
    }();
    return key;
}

Ps5LiftState* StartForeignThread(const ThreadMemory& memory, const std::uint64_t stackTop) {
    auto* foreign = new ForeignThread;
    foreign->Memory = memory;
    if (const int error = pthread_setspecific(ForeignThreadKey(), foreign); error != 0) ps5lift_fail("cannot keep a native thread's guest state: %s", std::strerror(error));
    PrepareState(foreign->State, memory.Tls, ConsoleMxcsr);
    foreign->State.Gpr[PS5LIFT_RSP] = stackTop & ~std::uint64_t{15};
    Foreign = foreign;
    BindThread(&foreign->State, memory);
    return CurrentState;
}

}

Ps5LiftState* Ps5Lift::BoundState() {
    return CurrentState;
}

extern "C" Ps5LiftState* ps5lift_current_state(void) {
    if (CurrentState != nullptr) return CurrentState;
    const auto memory = MapThreadMemory(true);
    return StartForeignThread(memory, StackTop(memory.Stack));
}

extern "C" void ps5lift_use_guest_stack(const std::uint64_t stackTop) {
    if (CurrentState != nullptr) ps5lift_fail("a thread that already runs guest code cannot move to another guest stack");
    StartForeignThread(MapThreadMemory(false), stackTop);
}

extern "C" std::uint64_t ps5lift_interrupt_stack(void) {
    ps5lift_current_state();
    return InterruptStackTop;
}

extern "C" std::uint64_t ps5lift_host_stack_size(void) {
    return HostStackSize;
}

extern "C" Ps5LiftState* ps5lift_enter_guest(void) {
    auto* state = ps5lift_current_state();
    if (Foreign != nullptr && Foreign->Entered++ == 0) {
        Foreign->Host = SaveHostFloatingPoint();
        ps5lift_apply_mxcsr(state);
    }
    return state;
}

extern "C" void ps5lift_leave_guest(void) {
    if (Foreign == nullptr) return;
    if (Foreign->Entered == 1) {
        Foreign->State.Mxcsr = ps5lift_read_mxcsr(&Foreign->State);
        RestoreHostFloatingPoint(Foreign->Host);
    }
    --Foreign->Entered;
}

namespace {

void RequireHostFeatures() {
#if defined(__aarch64__)
#if defined(__APPLE__)
    const bool present = HostFeature("hw.optional.arm.FEAT_AES") && HostFeature("hw.optional.arm.FEAT_PMULL") && HostFeature("hw.optional.armv8_crc32");
#else
    constexpr unsigned long required = HWCAP_AES | HWCAP_PMULL | HWCAP_CRC32;
    const bool present = (getauxval(AT_HWCAP) & required) == required;
#endif
    if (!present) ps5lift_fail("the host CPU lacks AES, PMULL or CRC32, which lifted AES-NI, PCLMULQDQ and crc32 use");
#else
    if (!__builtin_cpu_supports("aes") || !__builtin_cpu_supports("pclmul") || !__builtin_cpu_supports("sse4.2")) ps5lift_fail("the host CPU lacks AES-NI, PCLMULQDQ or SSE4.2, which lifted code uses");
#endif
}

void RequireHighNativeHeap() {
    if (dlsym(RTLD_DEFAULT, "mi_version") == nullptr) ps5lift_fail("AnyPS5 runs without mimalloc, so native allocations can land in the guest's first terabyte");
    void* const probe = std::malloc(64);
    const auto address = reinterpret_cast<std::uintptr_t>(probe);
    std::free(probe);
    if (address < GuestSpaceEnd) ps5lift_fail("a native allocation landed at 0x%" PRIxPTR ", inside the guest's first terabyte", address);
}

}

namespace {

const Ps5LiftProgram* UsedProgram = nullptr;

class FunctionIndex {
public:
    void Build(const Ps5LiftFunctionEntry* functions, const std::uint64_t count) {
        const auto size = std::bit_ceil(std::max<std::uint64_t>(2, count * 2));
        _slots.assign(size, Ps5LiftFunctionEntry{});
        _mask = size - 1;
        _shift = 64 - std::countr_zero(size);
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto& function = functions[i];
            if (function.GuestAddress == 0) ps5lift_fail("the lifted program has a function at guest address 0");
            auto slot = Home(function.GuestAddress);
            for (; _slots[slot].GuestAddress != 0; slot = (slot + 1) & _mask) {
                if (_slots[slot].GuestAddress == function.GuestAddress) ps5lift_fail("the lifted program lists the function at 0x%" PRIx64 " twice", function.GuestAddress);
            }
            _slots[slot] = function;
        }
    }

    Ps5LiftFunction Find(const std::uint64_t address) const {
        for (auto slot = Home(address);; slot = (slot + 1) & _mask) {
            const auto& entry = _slots[slot];
            if (entry.GuestAddress == address) return entry.Host;
            if (entry.GuestAddress == 0) return nullptr;
        }
    }

private:
    std::uint64_t Home(const std::uint64_t address) const {
        return (address * 0x9e3779b97f4a7c15ull) >> _shift;
    }

    std::vector<Ps5LiftFunctionEntry> _slots;
    std::uint64_t _mask = 0;
    int _shift = 0;
};

FunctionIndex FunctionsByAddress;

}

extern "C" void ps5lift_use_program(const Ps5LiftProgram* program) {
    if (UsedProgram == program) return;
    if (program->GuestBase != PS5LIFT_GUEST_BASE)
        ps5lift_fail("the program was lifted for guest base 0x%" PRIx64 ", and this runtime uses 0x%" PRIx64, program->GuestBase, static_cast<std::uint64_t>(PS5LIFT_GUEST_BASE));
    if (UsedProgram != nullptr) ps5lift_fail("the executable handed the runtime a second lifted program");
    UsedProgram = program;
    FunctionsByAddress.Build(program->Functions, program->FunctionCount);
}

const Ps5LiftProgram& Ps5Lift::Program() {
    if (UsedProgram == nullptr) ps5lift_fail("the executable has not handed the runtime its lifted program yet");
    return *UsedProgram;
}

Ps5LiftFunction Ps5Lift::LookupFunction(const std::uint64_t address) {
    Ps5Lift::Program();
    return FunctionsByAddress.Find(address);
}

extern "C" void ps5lift_run_main(void) {
    const auto& program = Ps5Lift::Program();
    RequireHostFeatures();
    const auto translation = Ps5Lift::AnyPs5Function<std::uint64_t()>("GuestArenaTranslation_nid_no_patch");
    if (translation != nullptr && translation() != PS5LIFT_GUEST_BASE)
        ps5lift_fail("AnyPS5's libraries translate guest addresses by 0x%" PRIx64 ", and this runtime by 0x%" PRIx64, translation(), static_cast<std::uint64_t>(PS5LIFT_GUEST_BASE));
    if (translation != nullptr && PS5LIFT_GUEST_BASE != 0) RequireHighNativeHeap();
    for (std::uint64_t i = 0; i < program.ImageCount; ++i) {
        MapSegments(program.Images[i]);
    }
    Ps5Lift::InstallNativeCalls();
    auto entry = program.Entry;
#ifdef __APPLE__
    if (const auto serveMainQueue = Ps5Lift::AnyPs5Function<void(void (*)(void*), void*)>("Aps5ServeMainQueue_nid_no_patch")) {
        serveMainQueue([](void* argument) { StartMain(*static_cast<std::uint64_t*>(argument)); }, &entry);
        ps5lift_fail("AnyPS5 stopped serving the main queue");
    }
#endif
    pthread_join(StartMain(entry), nullptr);
}
