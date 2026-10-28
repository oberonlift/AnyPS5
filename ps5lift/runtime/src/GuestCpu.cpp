#include <ps5lift/State.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
