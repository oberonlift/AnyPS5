#define _GNU_SOURCE

#include "DriverCore.h"

#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>

enum {
    HostMxcsr = 0x1f80,
    BreakpointLength = 1,
    SignalStackSize = 1 << 20,
};

enum {
    TrapDivideError = 0,
    TrapBreakpoint = 3,
    TrapInvalidOpcode = 6,
    TrapGeneralProtection = 13,
    TrapPageFault = 14,
    TrapX87 = 16,
    TrapAlignmentCheck = 17,
    TrapSimd = 19,
};

typedef void (*CaseFunction)(void);

static const int handledSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
static sigjmp_buf caseJump;
static volatile sig_atomic_t inCase;
static Fault fault;
static uint8_t signalStack[SignalStackSize];

static void RestoreHostFloatingPoint(void)
{
    const uint32_t mxcsr = HostMxcsr;
    __asm__ volatile("fninit\n\tldmxcsr %0" : : "m"(mxcsr) : "memory");
}

static Fault Classify(int signalNumber, const siginfo_t* info, const ucontext_t* context)
{
    const greg_t* registers = context->uc_mcontext.gregs;
    Fault result = {.Address = (uint64_t)registers[REG_RIP]};
    switch (registers[REG_TRAPNO]) {
    case TrapDivideError: result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_DIVIDE_ERROR; break;
    case TrapBreakpoint:
        result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_BREAKPOINT;
        result.Address -= BreakpointLength;
        break;
    case TrapInvalidOpcode: result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_INVALID_OPCODE; break;
    case TrapGeneralProtection: result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_GENERAL_PROTECTION; break;
    case TrapPageFault:
        result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_PAGE_FAULT;
        result.Data = (uint64_t)(uintptr_t)info->si_addr;
        break;
    case TrapX87: result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_X87_FLOATING_POINT; break;
    case TrapAlignmentCheck: result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_ALIGNMENT_CHECK; break;
    case TrapSimd: result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_SIMD_FLOATING_POINT; break;
    default:
        result.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_HOST_SIGNAL;
        result.Detail = (uint16_t)signalNumber;
        break;
    }
    return result;
}

static void OnSignal(int signalNumber, siginfo_t* info, void* context)
{
    if (!inCase) {
        struct sigaction defaultAction = {.sa_handler = SIG_DFL};
        sigaction(signalNumber, &defaultAction, NULL);
        raise(signalNumber);
        return;
    }
    fault = Classify(signalNumber, info, context);
    siglongjmp(caseJump, 1);
}

static bool InstallHandlers(void)
{
    const stack_t alternate = {.ss_sp = signalStack, .ss_size = sizeof(signalStack)};
    if (sigaltstack(&alternate, NULL) != 0) return Fail("sigaltstack: %s", strerror(errno));
    struct sigaction action = {.sa_sigaction = OnSignal, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(handledSignals) / sizeof(handledSignals[0]); ++i) {
        sigaddset(&action.sa_mask, handledSignals[i]);
    }
    for (size_t i = 0; i < sizeof(handledSignals) / sizeof(handledSignals[0]); ++i) {
        if (sigaction(handledSignals[i], &action, NULL) != 0) {
            return Fail("sigaction(%s): %s", strsignal(handledSignals[i]), strerror(errno));
        }
    }
    return true;
}

static Fault RunCase(const Ps5LiftInstructionTableEntry* entry)
{
    if (sigsetjmp(caseJump, 1) != 0) {
        inCase = 0;
        RestoreHostFloatingPoint();
        return fault;
    }
    inCase = 1;
    ((CaseFunction)(uintptr_t)entry->Function)();
    inCase = 0;
    RestoreHostFloatingPoint();
    return (Fault){.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_COMPLETED};
}

static uint8_t NoConsoleValues(const Case* item, const Ps5LiftInstructionTableEntry* entry, uint64_t* values)
{
    (void)item;
    (void)entry;
    (void)values;
    return 0;
}

int host_run_instructions(const void* table)
{
    static const DriverHooks hooks = {
        .InstallHandlers = InstallHandlers,
        .RunCase = RunCase,
        .ConsoleValues = NoConsoleValues,
    };
    return RunCases(table, &hooks) ? 0 : PS5LIFT_INSTRUCTION_DRIVER_FAILED;
}

void host_exit(int code)
{
    exit(code);
}
