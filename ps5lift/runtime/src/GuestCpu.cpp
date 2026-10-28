#include <ps5lift/Bridge.hpp>
#include <ps5lift/State.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" std::uint32_t GuestCpuMxcsr_nid_no_patch(void) {
    return ps5lift_read_mxcsr(ps5lift_current_state());
}

extern "C" void GuestCpuSetMxcsr_nid_no_patch(const std::uint32_t value) {
    auto* state = ps5lift_current_state();
    state->Mxcsr = value;
    ps5lift_apply_mxcsr(state);
}

extern "C" std::uint16_t GuestCpuX87Control_nid_no_patch(void) {
    return ps5lift_current_state()->X87Control;
}

extern "C" void GuestCpuSetX87Control_nid_no_patch(const std::uint16_t value) {
    ps5lift_current_state()->X87Control = value;
}

extern "C" std::uint16_t GuestCpuX87Status_nid_no_patch(void) {
    return ps5lift_current_state()->X87Status;
}

extern "C" void GuestCpuSetX87Status_nid_no_patch(const std::uint16_t value) {
    ps5lift_current_state()->X87Status = value;
}

extern "C" void GuestCpuX87Wait_nid_no_patch(void) {
    const auto* state = ps5lift_current_state();
    const auto pending = state->X87Status & ~state->X87Control & 0x3fu;
    if (pending == 0) return;
    std::fprintf(stderr, "ps5lift: fwait with unmasked x87 exceptions 0x%x pending; the console raises SIGFPE\n", pending);
    std::fflush(stdout);
    std::_Exit(70);
}

extern "C" std::uint32_t GuestCpuMxcsrMask_nid_no_patch(void) {
    return 0xffffu;
}

extern "C" void GuestCpuLoadExtended_nid_no_patch(const void* guest, long double* host) {
    ps5lift_extended_to_host(guest, host);
}

extern "C" void GuestCpuStoreExtended_nid_no_patch(const long double* host, void* guest) {
    ps5lift_extended_from_host(ps5lift_current_state(), host, guest);
}

namespace {

constexpr int DwarfRegisters[16] = {PS5LIFT_RAX, PS5LIFT_RDX, PS5LIFT_RCX, PS5LIFT_RBX, PS5LIFT_RSI, PS5LIFT_RDI, PS5LIFT_RBP, PS5LIFT_RSP,
                                    PS5LIFT_R8, PS5LIFT_R9, PS5LIFT_R10, PS5LIFT_R11, PS5LIFT_R12, PS5LIFT_R13, PS5LIFT_R14, PS5LIFT_R15};
constexpr int DwarfRsp = 7;
constexpr int DwarfRip = 16;

// The layout of glibc's and FreeBSD's dl_phdr_info, which AnyPS5's libraries hand to dl_iterate_phdr callbacks.
struct LoadedImage {
    std::uint64_t Address;
    const char* Name;
    const void* ProgramHeaders;
    std::uint16_t ProgramHeaderCount;
    unsigned long long Loads;
    unsigned long long Unloads;
    std::size_t TlsModule;
    void* TlsData;
};

}

extern "C" void GuestCpuUnwindStart_nid_no_patch(std::uint64_t* registers) {
    const auto* state = ps5lift_current_state();
    for (int i = 0; i < 16; ++i) registers[i] = state->Gpr[DwarfRegisters[i]];
    std::memcpy(&registers[DwarfRip], reinterpret_cast<const void*>(registers[DwarfRsp]), sizeof(registers[DwarfRip]));
    registers[DwarfRsp] += 8;
}

extern "C" int GuestCpuIterateImages_nid_no_patch(int (*callback)(LoadedImage*, std::size_t, void*), void* data) {
    const auto* state = ps5lift_current_state();
    for (std::uint64_t index = 0; index < ps5lift_program.ImageCount; ++index) {
        const auto& image = ps5lift_program.Images[index];
        LoadedImage loaded{image.LoadBias, image.Name, reinterpret_cast<const void*>(image.ProgramHeaders), static_cast<std::uint16_t>(image.ProgramHeaderCount), ps5lift_program.ImageCount, 0, image.TlsModule,
            image.TlsModule == 0 ? nullptr : reinterpret_cast<void*>(state->FsBase - image.TlsOffset)};
        if (const int result = callback(&loaded, sizeof(loaded), data); result != 0) return result;
    }
    return 0;
}

extern "C" const char* GuestCpuImportName_nid_no_patch(const std::uint64_t address) {
    for (std::uint64_t index = 0; index < ps5lift_program.ImageCount; ++index) {
        const auto& image = ps5lift_program.Images[index];
        for (std::uint64_t i = 0; i < image.ImportCount; ++i) {
            if (image.Imports[i].GuestAddress == address) return image.Imports[i].Name;
        }
    }
    return nullptr;
}

extern "C" [[noreturn]] void GuestCpuUnwindResume_nid_no_patch(const std::uint64_t* registers) {
    auto* state = ps5lift_current_state();
    for (int i = 0; i < 16; ++i) state->Gpr[DwarfRegisters[i]] = registers[i];
    state->ResumeRip = registers[DwarfRip];
    state->ResumeRsp = registers[DwarfRsp];
    state->Unwinding = 1;
    throw Ps5Lift::Bridge::GuestUnwind{};
}

extern "C" std::uint64_t GuestCpuReadTsc_nid_no_patch(void) {
    return ps5lift_read_tsc();
}

extern "C" void GuestCpuCallInterrupted_nid_no_patch(const std::uint64_t target, const std::uint64_t first, const std::uint64_t second) {
    constexpr std::uint64_t InterruptedStackGap = 64ull << 10;
    auto* state = ps5lift_current_state();
    const Ps5LiftState saved = *state;
    state->Gpr[PS5LIFT_RSP] = (saved.Gpr[PS5LIFT_RSP] - InterruptedStackGap) & ~std::uint64_t{15};
    state->Unwinding = 0;
    Ps5Lift::Bridge::CallGuest<void>(target, first, second);
    *state = saved;
}

extern "C" void GuestCpuInterruptedContext_nid_no_patch(std::uint64_t* registers, std::uint64_t* flags) {
    static constexpr unsigned FlagBits[PS5LIFT_FLAG_COUNT] = {0, 2, 4, 6, 7, 11, 10};
    const auto* state = ps5lift_current_state();
    for (int i = 0; i < 16; ++i) registers[i] = state->Gpr[DwarfRegisters[i]];
    registers[DwarfRip] = state->Rip;
    std::uint64_t value = 0x202;
    for (int i = 0; i < PS5LIFT_FLAG_COUNT; ++i) value |= static_cast<std::uint64_t>(state->Flags[i] & 1) << FlagBits[i];
    *flags = value;
}

namespace {

constexpr int FiberRegisters[] = {PS5LIFT_RBX, PS5LIFT_RBP, PS5LIFT_R12, PS5LIFT_R13, PS5LIFT_R14, PS5LIFT_R15, PS5LIFT_RSP};

}

extern "C" void GuestCpuSaveFiber_nid_no_patch(std::uint64_t* context) {
    auto* state = ps5lift_current_state();
    for (int i = 0; i < 7; ++i) context[i] = state->Gpr[FiberRegisters[i]];
    context[7] = static_cast<std::uint64_t>(ps5lift_read_mxcsr(state)) | static_cast<std::uint64_t>(state->X87Control) << 32;
}

extern "C" void GuestCpuRestoreFiber_nid_no_patch(const std::uint64_t* context) {
    auto* state = ps5lift_current_state();
    for (int i = 0; i < 7; ++i) state->Gpr[FiberRegisters[i]] = context[i];
    state->Mxcsr = static_cast<std::uint32_t>(context[7]);
    state->X87Control = static_cast<std::uint16_t>(context[7] >> 32);
    state->Unwinding = 0;
    ps5lift_apply_mxcsr(state);
}

extern "C" void GuestCpuStartFiber_nid_no_patch(const std::uint64_t stackTop) {
    auto* state = ps5lift_current_state();
    if (stackTop != 0) state->Gpr[PS5LIFT_RSP] = stackTop & ~std::uint64_t{15};
    state->Unwinding = 0;
}
