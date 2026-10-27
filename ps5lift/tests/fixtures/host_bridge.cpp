#include <ps5lift/State.h>

#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void ps5lift_import_host__write(Ps5LiftState* state) {
    const auto* text = reinterpret_cast<const char*>(state->Gpr[PS5LIFT_RDI]);
    const auto length = static_cast<std::size_t>(state->Gpr[PS5LIFT_RSI]);
    state->Gpr[PS5LIFT_RAX] = static_cast<std::uint64_t>(write(1, text, length));
}

extern "C" void ps5lift_import_host__exit(Ps5LiftState* state) {
    std::_Exit(static_cast<int>(state->Gpr[PS5LIFT_RDI]));
}

extern "C" void ps5lift_import_host__ticks(Ps5LiftState* state) {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    state->Gpr[PS5LIFT_RAX] = static_cast<std::uint64_t>(time.tv_sec) * 1000000000u + static_cast<std::uint64_t>(time.tv_nsec);
}

extern "C" void ps5lift_import_host__thread__create(Ps5LiftState* state) {
    state->Gpr[PS5LIFT_RAX] = ps5lift_thread_create(state, state->Gpr[PS5LIFT_RDI], state->Gpr[PS5LIFT_RSI]);
}

extern "C" void ps5lift_import_host__thread__join(Ps5LiftState* state) {
    state->Gpr[PS5LIFT_RAX] = ps5lift_thread_join(state->Gpr[PS5LIFT_RDI]);
}
