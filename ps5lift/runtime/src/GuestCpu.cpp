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
constexpr std::uint32_t SegmentExecute = 1;
constexpr std::uint32_t SegmentWrite = 2;

}

extern "C" void GuestCpuUnwindStart_nid_no_patch(std::uint64_t* registers) {
    const auto* state = ps5lift_current_state();
    for (int i = 0; i < 16; ++i) registers[i] = state->Gpr[DwarfRegisters[i]];
    std::memcpy(&registers[DwarfRip], reinterpret_cast<const void*>(registers[DwarfRsp]), sizeof(registers[DwarfRip]));
    registers[DwarfRsp] += 8;
}

extern "C" bool GuestCpuUnwindTables_nid_no_patch(const std::uint64_t pc, const void** header, std::uint64_t* text, std::uint64_t* data) {
    if (ps5lift_image.EhFrameHeader == 0) return false;
    bool contains = false;
    for (std::uint64_t i = 0; i < ps5lift_image.SegmentCount; ++i) {
        const auto& segment = ps5lift_image.Segments[i];
        if (pc >= segment.GuestAddress && pc - segment.GuestAddress < segment.MemorySize) contains = true;
        if (segment.Protection & SegmentExecute) *text = segment.GuestAddress;
        if (segment.Protection & SegmentWrite) *data = segment.GuestAddress;
    }
    if (!contains) return false;
    *header = reinterpret_cast<const void*>(ps5lift_image.EhFrameHeader);
    return true;
}

extern "C" const char* GuestCpuImportName_nid_no_patch(const std::uint64_t address) {
    for (std::uint64_t i = 0; i < ps5lift_image.ImportCount; ++i) {
        if (ps5lift_image.Imports[i].GuestAddress == address) return ps5lift_image.Imports[i].Name;
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
