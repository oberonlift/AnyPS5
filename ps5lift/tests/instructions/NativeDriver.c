#define _GNU_SOURCE

#include "records.h"

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

enum {
    HostMxcsr = 0x1f80,
    WordSize = 8,
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

typedef struct Fault {
    uint8_t Outcome;
    uint16_t Detail;
    uint64_t Address;
    uint64_t Data;
} Fault;

typedef struct Program {
    uint32_t Count;
    uint8_t* Input;
    uint8_t* Output;
    uint8_t* Block;
    uint8_t* Guard;
    const uint8_t* Entries;
} Program;

typedef struct Cursor {
    const uint8_t* Data;
    size_t Size;
    size_t Position;
} Cursor;

typedef struct Case {
    Ps5LiftInstructionInputHead Head;
    const uint8_t* Slot;
    const uint8_t* Patches;
    uint16_t PatchCount;
} Case;

static const int handledSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
static sigjmp_buf caseJump;
static volatile sig_atomic_t inCase;
static Fault fault;
static uint8_t signalStack[SignalStackSize];
static uint8_t initialBlock[PS5LIFT_INSTRUCTION_BLOCK_SIZE];
static uint8_t resultBuffer[sizeof(Ps5LiftInstructionResultHead)
                            + (PS5LIFT_INSTRUCTION_SLOT_SIZE + PS5LIFT_INSTRUCTION_BLOCK_SIZE) / WordSize
                                  * sizeof(Ps5LiftInstructionWord)
                            + sizeof(Ps5LiftInstructionExpectedCount)];

__attribute__((format(printf, 1, 2))) static bool Fail(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("ps5lift instruction driver: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    return false;
}

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

static const char* Variable(const char* name)
{
    const char* value = getenv(name);
    if (value == NULL) Fail("%s is not set", name);
    return value;
}

static bool ReadPosition(const char* text, uint32_t limit, uint32_t* position)
{
    char* end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || errno != 0 || *end != '\0' || value > limit) {
        return Fail("%s is \"%s\", not a case position from 0 to %u", PS5LIFT_INSTRUCTION_FIRST_VARIABLE, text, limit);
    }
    *position = (uint32_t)value;
    return true;
}

static bool ReadTable(const void* table, Program* program)
{
    Ps5LiftInstructionTableHeader header;
    memcpy(&header, table, sizeof(header));
    if (memcmp(header.Magic, PS5LIFT_INSTRUCTION_TABLE_MAGIC, sizeof(header.Magic)) != 0) {
        return Fail("the guest program's table does not start with %s", PS5LIFT_INSTRUCTION_TABLE_MAGIC);
    }
    if (header.Guard != header.Block + PS5LIFT_INSTRUCTION_BLOCK_SIZE) {
        return Fail("the guard at %#llx does not follow the block at %#llx", (unsigned long long)header.Guard,
                    (unsigned long long)header.Block);
    }
    *program = (Program){
        .Count = header.Count,
        .Input = (uint8_t*)(uintptr_t)header.Input,
        .Output = (uint8_t*)(uintptr_t)header.Output,
        .Block = (uint8_t*)(uintptr_t)header.Block,
        .Guard = (uint8_t*)(uintptr_t)header.Guard,
        .Entries = (const uint8_t*)table + sizeof(header),
    };
    return true;
}

static CaseFunction Function(const Program* program, uint32_t instance)
{
    Ps5LiftInstructionTableEntry entry;
    memcpy(&entry, program->Entries + (size_t)instance * sizeof(entry), sizeof(entry));
    return (CaseFunction)(uintptr_t)entry.Function;
}

static const uint8_t* Take(Cursor* cursor, size_t size)
{
    if (size > cursor->Size - cursor->Position) return NULL;
    const uint8_t* data = cursor->Data + cursor->Position;
    cursor->Position += size;
    return data;
}

static bool MapInputs(const char* path, Cursor* cursor, Ps5LiftInstructionFileHeader* header)
{
    const int file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) return Fail("cannot open %s: %s", path, strerror(errno));
    struct stat status;
    if (fstat(file, &status) != 0) {
        close(file);
        return Fail("cannot read the size of %s: %s", path, strerror(errno));
    }
    void* data = mmap(NULL, (size_t)status.st_size, PROT_READ, MAP_PRIVATE, file, 0);
    close(file);
    if (data == MAP_FAILED) return Fail("cannot map %s: %s", path, strerror(errno));
    *cursor = (Cursor){.Data = data, .Size = (size_t)status.st_size};
    const uint8_t* bytes = Take(cursor, sizeof(*header));
    if (bytes == NULL) return Fail("%s is shorter than its header", path);
    memcpy(header, bytes, sizeof(*header));
    if (memcmp(header->Magic, PS5LIFT_INSTRUCTION_INPUT_MAGIC, sizeof(header->Magic)) != 0) {
        return Fail("%s does not start with %s", path, PS5LIFT_INSTRUCTION_INPUT_MAGIC);
    }
    return true;
}

static bool ReadCase(Cursor* cursor, uint32_t position, Case* item)
{
    const uint8_t* head = Take(cursor, sizeof(item->Head));
    const uint8_t* slot = head == NULL ? NULL : Take(cursor, PS5LIFT_INSTRUCTION_SLOT_SIZE);
    const uint8_t* count = slot == NULL ? NULL : Take(cursor, sizeof(Ps5LiftInstructionPatchCount));
    if (count == NULL) return Fail("the inputs end inside the record at position %u", position);
    Ps5LiftInstructionPatchCount patches;
    memcpy(&item->Head, head, sizeof(item->Head));
    memcpy(&patches, count, sizeof(patches));
    item->Slot = slot;
    item->PatchCount = patches.Count;
    item->Patches = cursor->Data + cursor->Position;
    for (uint16_t i = 0; i < patches.Count; ++i) {
        const uint8_t* bytes = Take(cursor, sizeof(Ps5LiftInstructionPatchHead));
        if (bytes == NULL) return Fail("the inputs end inside case %u's patches", item->Head.CaseIndex);
        Ps5LiftInstructionPatchHead patch;
        memcpy(&patch, bytes, sizeof(patch));
        if (patch.Offset + patch.Length > PS5LIFT_INSTRUCTION_BLOCK_SIZE) {
            return Fail("case %u patches %u bytes at block offset %u, past the block's end", item->Head.CaseIndex,
                        patch.Length, patch.Offset);
        }
        if (Take(cursor, patch.Length) == NULL) return Fail("the inputs end inside case %u's patches", item->Head.CaseIndex);
    }
    return true;
}

static void PrepareCase(const Program* program, const Case* item)
{
    Ps5LiftFillBlock(program->Block, item->Head.FillSeed);
    const uint8_t* patch = item->Patches;
    for (uint16_t i = 0; i < item->PatchCount; ++i) {
        Ps5LiftInstructionPatchHead head;
        memcpy(&head, patch, sizeof(head));
        memcpy(program->Block + head.Offset, patch + sizeof(head), head.Length);
        patch += sizeof(head) + head.Length;
    }
    memcpy(initialBlock, program->Block, sizeof(initialBlock));
    memcpy(program->Input, item->Slot, PS5LIFT_INSTRUCTION_SLOT_SIZE);
    memcpy(program->Output, item->Slot, PS5LIFT_INSTRUCTION_SLOT_SIZE);
}

static Fault RunCase(CaseFunction function)
{
    if (sigsetjmp(caseJump, 1) != 0) {
        inCase = 0;
        RestoreHostFloatingPoint();
        return fault;
    }
    inCase = 1;
    function();
    inCase = 0;
    RestoreHostFloatingPoint();
    return (Fault){.Outcome = PS5LIFT_INSTRUCTION_OUTCOME_COMPLETED};
}

static size_t AppendChangedWords(uint8_t* out, const uint8_t* before, const uint8_t* after, size_t size,
                                 uint16_t* count)
{
    size_t used = 0;
    for (size_t offset = 0; offset < size; offset += WordSize) {
        uint64_t previous;
        uint64_t current;
        memcpy(&previous, before + offset, sizeof(previous));
        memcpy(&current, after + offset, sizeof(current));
        if (previous == current) continue;
        const Ps5LiftInstructionWord word = {.Offset = (uint16_t)offset, .Value = current};
        memcpy(out + used, &word, sizeof(word));
        used += sizeof(word);
        ++*count;
    }
    return used;
}

static bool WriteAll(int file, const uint8_t* data, size_t size)
{
    while (size > 0) {
        const ssize_t written = write(file, data, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            return Fail("cannot write a result: %s", strerror(errno));
        }
        data += written;
        size -= (size_t)written;
    }
    return true;
}

static bool WriteResult(int file, const Program* program, const Case* item, Fault outcome)
{
    uint16_t slotWords = 0;
    uint16_t memoryWords = 0;
    size_t used = sizeof(Ps5LiftInstructionResultHead);
    if (outcome.Outcome == PS5LIFT_INSTRUCTION_OUTCOME_COMPLETED) {
        Ps5LiftRestoreUnrecordedSlotBytes(program->Output, item->Slot);
        used += AppendChangedWords(resultBuffer + used, item->Slot, program->Output, PS5LIFT_INSTRUCTION_SLOT_SIZE,
                                   &slotWords);
        used += AppendChangedWords(resultBuffer + used, initialBlock, program->Block, PS5LIFT_INSTRUCTION_BLOCK_SIZE,
                                   &memoryWords);
    }
    if (item->Head.ConsoleDefined) {
        const Ps5LiftInstructionExpectedCount none = {.Count = 0};
        memcpy(resultBuffer + used, &none, sizeof(none));
        used += sizeof(none);
    }
    const Ps5LiftInstructionResultHead head = {
        .CaseIndex = item->Head.CaseIndex,
        .Outcome = outcome.Outcome,
        .ConsoleDefined = item->Head.ConsoleDefined,
        .Detail = outcome.Detail,
        .SlotWords = slotWords,
        .MemoryWords = memoryWords,
        .Address = outcome.Address,
        .Data = outcome.Data,
    };
    memcpy(resultBuffer, &head, sizeof(head));
    return WriteAll(file, resultBuffer, used);
}

static bool RunCases(const void* table)
{
    const char* inputsPath = Variable(PS5LIFT_INSTRUCTION_INPUTS_VARIABLE);
    const char* resultsPath = Variable(PS5LIFT_INSTRUCTION_RESULTS_VARIABLE);
    const char* firstText = Variable(PS5LIFT_INSTRUCTION_FIRST_VARIABLE);
    if (inputsPath == NULL || resultsPath == NULL || firstText == NULL) return false;
    Program program;
    Cursor cursor;
    Ps5LiftInstructionFileHeader header;
    uint32_t first;
    if (!ReadTable(table, &program) || !MapInputs(inputsPath, &cursor, &header)
        || !ReadPosition(firstText, header.Count, &first)) {
        return false;
    }
    const int results = open(resultsPath, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (results < 0) return Fail("cannot open %s: %s", resultsPath, strerror(errno));
    if (mprotect(program.Guard, PS5LIFT_INSTRUCTION_GUARD_SIZE, PROT_NONE) != 0) {
        return Fail("cannot protect the guard: %s", strerror(errno));
    }
    if (!InstallHandlers()) return false;
    for (uint32_t position = 0; position < header.Count; ++position) {
        Case item;
        if (!ReadCase(&cursor, position, &item)) return false;
        if (position < first) continue;
        if (item.Head.Instance >= program.Count) {
            return Fail("case %u runs instance %u, and the program has %u", item.Head.CaseIndex, item.Head.Instance,
                        program.Count);
        }
        PrepareCase(&program, &item);
        const Fault outcome = RunCase(Function(&program, item.Head.Instance));
        if (!WriteResult(results, &program, &item, outcome)) return false;
    }
    if (cursor.Position != cursor.Size) return Fail("%zu bytes follow the last case", cursor.Size - cursor.Position);
    return true;
}

int host_run_instructions(const void* table)
{
    return RunCases(table) ? 0 : PS5LIFT_INSTRUCTION_DRIVER_FAILED;
}

void host_exit(int code)
{
    exit(code);
}
