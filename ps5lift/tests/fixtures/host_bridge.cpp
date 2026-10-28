#include <ps5lift/Bridge.hpp>
#include <ps5lift/State.h>

#include <pthread.h>

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
    std::fflush(nullptr);
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

using namespace Ps5Lift::Bridge;

extern "C" void ps5lift_import_host__sort(Ps5LiftState* state) {
    auto* compare = Callback<int(const void*, const void*)>(Integer<std::uint64_t>(state, 3));
    std::qsort(Integer<void*>(state, 0), Integer<unsigned long>(state, 1), Integer<unsigned long>(state, 2), compare);
}

extern "C" void ps5lift_import_host__apply(Ps5LiftState* state) {
    auto* function = Callback<double(double, long)>(Integer<std::uint64_t>(state, 0));
    const auto count = Integer<long>(state, 1);
    Return(state, function(function(Floating<double>(state, 0), count), count + 1));
}

extern "C" void ps5lift_import_host__on__thread(Ps5LiftState* state) {
    struct Call {
        long (*Function)(long);
        long Argument;
        long Result;
    } call{Callback<long(long)>(Integer<std::uint64_t>(state, 0)), Integer<long>(state, 1), 0};
    pthread_t thread;
    pthread_create(&thread, nullptr, [](void* argument) -> void* {
        auto* call = static_cast<Call*>(argument);
        call->Result = call->Function(call->Argument);
        return nullptr;
    }, &call);
    pthread_join(thread, nullptr);
    Return(state, call.Result);
}

extern "C" int GuestCpuIterateImages_nid_no_patch(int (*callback)(void*, std::size_t, void*), void* data);

extern "C" void ps5lift_import_dl__iterate__phdr(Ps5LiftState* state) {
    Return(state, GuestCpuIterateImages_nid_no_patch(Callback<int(void*, std::size_t, void*)>(Integer<std::uint64_t>(state, 0)), Integer<void*>(state, 1)));
}

extern "C" void ps5lift_import_host__call__eight(Ps5LiftState* state) {
    auto* function = Callback<long(long, long, long, long, long, long, long, long)>(Integer<std::uint64_t>(state, 0));
    Return(state, function(1, 2, 3, 4, 5, 6, 7, 8) + function(10, 20, 30, 40, 50, 60, 70, 80));
}

extern "C" void ps5lift_import_host__extended(Ps5LiftState* state) {
    const ExtendedScope scope(state);
    long sum = Stack<long>(state, 0);
    for (int index = 0; index < 6; ++index) sum += Integer<long>(state, index);
    ReturnExtended(state, sum != 28 ? 0.0L : Extended(state, 2) / Extended(state, 4));
}
