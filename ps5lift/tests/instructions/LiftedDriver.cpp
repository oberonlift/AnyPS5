#include "DriverCore.h"

#include <ps5lift/Bridge.hpp>
#include <ps5lift/State.h>

#include <setjmp.h>
#include <signal.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::size_t SignalStackSize = 1 << 20;
constexpr std::uint8_t CpuidOpcode[] = {0x0f, 0xa2};
constexpr std::uint8_t XgetbvOpcode[] = {0x0f, 0x01, 0xd0};
constexpr int CpuidResults[] = {PS5LIFT_RAX, PS5LIFT_RBX, PS5LIFT_RCX, PS5LIFT_RDX};

sigjmp_buf caseJump;
volatile sig_atomic_t inCase;
Fault fault;
std::uint64_t caseInstance;
struct sigaction previousSegv;
struct sigaction previousBus;
std::uint8_t signalStack[SignalStackSize];

bool StopOutcome(const std::uint32_t kind, std::uint8_t* outcome) {
    switch (kind) {
    case PS5LIFT_STOP_DIVIDE_ERROR: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_DIVIDE_ERROR; return true;
    case PS5LIFT_STOP_BREAKPOINT: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_BREAKPOINT; return true;
    case PS5LIFT_STOP_INVALID_OPCODE: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_INVALID_OPCODE; return true;
    case PS5LIFT_STOP_GENERAL_PROTECTION: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_GENERAL_PROTECTION; return true;
    case PS5LIFT_STOP_CONSOLE_KERNEL: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_CONSOLE_KERNEL; return true;
    case PS5LIFT_STOP_MISSING_TRANSLATION: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_MISSING_TRANSLATION; return true;
    case PS5LIFT_STOP_SEGMENT_TABLES: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_SEGMENT_TABLES; return true;
    case PS5LIFT_STOP_NEEDS_EXACT_FLOAT: *outcome = PS5LIFT_INSTRUCTION_OUTCOME_NEEDS_EXACT_FLOAT; return true;
    default: return false;
    }
}

void OnStop(Ps5LiftState* state, const std::uint32_t kind, const std::uint64_t address, const std::uint64_t detail, const char* mnemonic) {
    if (!inCase) {
        ps5lift_set_stop_handler(nullptr);
        ps5lift_stop(state, kind, address, detail, mnemonic);
    }
    std::uint8_t outcome = 0;
    if (!StopOutcome(kind, &outcome) || detail > UINT16_MAX) {
        Fail("the runtime stopped %s at 0x%" PRIx64 " with kind %" PRIu32 " and detail 0x%" PRIx64 ", which no result holds", mnemonic, address, kind, detail);
        std::_Exit(PS5LIFT_INSTRUCTION_DRIVER_FAILED);
    }
    fault = Fault{outcome, static_cast<std::uint16_t>(detail), address, 0};
    siglongjmp(caseJump, 1);
}

void ChainSignal(const int signal, siginfo_t* info, void* context) {
    const auto& previous = signal == SIGBUS ? previousBus : previousSegv;
    if ((previous.sa_flags & SA_SIGINFO) != 0) {
        previous.sa_sigaction(signal, info, context);
        return;
    }
    if (previous.sa_handler == SIG_DFL || previous.sa_handler == SIG_IGN) {
        struct sigaction defaultAction {};
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);
        sigaction(signal, &defaultAction, nullptr);
        return;
    }
    previous.sa_handler(signal);
}

void OnSignal(const int signal, siginfo_t* info, void* context) {
    if (!inCase) {
        ChainSignal(signal, info, context);
        return;
    }
    const auto data = reinterpret_cast<std::uint64_t>(info->si_addr);
    if (signal == SIGSEGV) {
        fault = Fault{PS5LIFT_INSTRUCTION_OUTCOME_PAGE_FAULT, 0, caseInstance, data};
    } else {
        fault = Fault{PS5LIFT_INSTRUCTION_OUTCOME_HOST_SIGNAL, static_cast<std::uint16_t>(signal), caseInstance, data};
    }
    siglongjmp(caseJump, 1);
}

bool InstallHandlers() {
    stack_t alternate{};
    alternate.ss_sp = signalStack;
    alternate.ss_size = sizeof(signalStack);
    if (sigaltstack(&alternate, nullptr) != 0) return Fail("sigaltstack: %s", std::strerror(errno));
    struct sigaction action {};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGSEGV);
    sigaddset(&action.sa_mask, SIGBUS);
    if (sigaction(SIGSEGV, &action, &previousSegv) != 0) return Fail("sigaction(SIGSEGV): %s", std::strerror(errno));
    if (sigaction(SIGBUS, &action, &previousBus) != 0) return Fail("sigaction(SIGBUS): %s", std::strerror(errno));
    ps5lift_set_stop_handler(OnStop);
    return true;
}

Fault RunCase(const Ps5LiftInstructionTableEntry* entry) {
    auto* const state = ps5lift_current_state();
    const auto rsp = state->Gpr[PS5LIFT_RSP];
    caseInstance = entry->Instance;
    if (sigsetjmp(caseJump, 1) != 0) {
        inCase = 0;
        state->Gpr[PS5LIFT_RSP] = rsp;
        state->Unwinding = 0;
        return fault;
    }
    inCase = 1;
    Ps5Lift::Bridge::CallGuest<void>(entry->Function);
    inCase = 0;
    return Fault{PS5LIFT_INSTRUCTION_OUTCOME_COMPLETED, 0, 0, 0};
}

template <std::size_t Size>
bool EndsWith(const Ps5LiftInstructionTableEntry* entry, const std::uint8_t (&opcode)[Size]) {
    const auto* end = reinterpret_cast<const std::uint8_t*>(entry->Instance + entry->Length);
    return entry->Length >= Size && std::memcmp(end - Size, opcode, Size) == 0;
}

std::uint64_t InputRegister(const Case* item, const int number) {
    std::uint64_t value;
    std::memcpy(&value, item->Slot + PS5LIFT_INSTRUCTION_GPR + PS5LIFT_INSTRUCTION_GPR_STRIDE * number, sizeof(value));
    return value;
}

bool ConsoleValues(const Case* item, const Ps5LiftInstructionTableEntry* entry, std::uint64_t* values, std::uint8_t* count) {
    if (EndsWith(entry, CpuidOpcode)) {
        Ps5LiftState query{};
        query.Gpr[PS5LIFT_RAX] = InputRegister(item, PS5LIFT_RAX);
        query.Gpr[PS5LIFT_RCX] = InputRegister(item, PS5LIFT_RCX);
        ps5lift_cpuid(&query, entry->Instance);
        for (const auto result : CpuidResults) values[(*count)++] = query.Gpr[result];
        return true;
    }
    if (EndsWith(entry, XgetbvOpcode)) {
        values[(*count)++] = ps5lift_xgetbv(entry->Instance, static_cast<std::uint32_t>(InputRegister(item, PS5LIFT_RCX)));
        return true;
    }
    return Fail("case %u is console-defined, but the instance at 0x%" PRIx64 " is neither cpuid nor xgetbv", item->Head.CaseIndex, entry->Instance);
}

}

extern "C" void ps5lift_import_host__run__instructions(Ps5LiftState* state) {
    static constexpr DriverHooks hooks{InstallHandlers, RunCase, ConsoleValues};
    const auto* table = reinterpret_cast<const void*>(state->Gpr[PS5LIFT_RDI]);
    state->Gpr[PS5LIFT_RAX] = RunCases(table, &hooks) ? 0 : PS5LIFT_INSTRUCTION_DRIVER_FAILED;
}

extern "C" void ps5lift_import_host__exit(Ps5LiftState* state) {
    std::fflush(nullptr);
    std::_Exit(static_cast<int>(state->Gpr[PS5LIFT_RDI]));
}
