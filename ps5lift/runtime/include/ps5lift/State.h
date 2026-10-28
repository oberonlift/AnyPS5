#ifndef PS5LIFT_STATE_H
#define PS5LIFT_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum Ps5LiftGpr {
    PS5LIFT_RAX,
    PS5LIFT_RCX,
    PS5LIFT_RDX,
    PS5LIFT_RBX,
    PS5LIFT_RSP,
    PS5LIFT_RBP,
    PS5LIFT_RSI,
    PS5LIFT_RDI,
    PS5LIFT_R8,
    PS5LIFT_R9,
    PS5LIFT_R10,
    PS5LIFT_R11,
    PS5LIFT_R12,
    PS5LIFT_R13,
    PS5LIFT_R14,
    PS5LIFT_R15,
    PS5LIFT_GPR_COUNT
};

enum Ps5LiftFlag {
    PS5LIFT_CF,
    PS5LIFT_PF,
    PS5LIFT_AF,
    PS5LIFT_ZF,
    PS5LIFT_SF,
    PS5LIFT_OF,
    PS5LIFT_DF,
    PS5LIFT_FLAG_COUNT
};

typedef struct Ps5LiftState {
    uint64_t Gpr[PS5LIFT_GPR_COUNT];
    uint64_t Rip;
    uint64_t FsBase;
    uint64_t GsBase;
    uint8_t Flags[PS5LIFT_FLAG_COUNT];
    uint8_t Padding[1];
    uint32_t Mxcsr;
    uint16_t X87Control;
    uint16_t X87Status;
    uint8_t Vector[16][32];
} Ps5LiftState;

typedef void (*Ps5LiftFunction)(Ps5LiftState*);

typedef struct Ps5LiftFunctionEntry {
    uint64_t GuestAddress;
    Ps5LiftFunction Host;
} Ps5LiftFunctionEntry;

typedef struct Ps5LiftSegment {
    uint64_t GuestAddress;
    uint64_t MemorySize;
    uint64_t FileSize;
    const uint8_t* Data;
    uint32_t Protection;
} Ps5LiftSegment;

typedef struct Ps5LiftImage {
    const Ps5LiftSegment* Segments;
    uint64_t SegmentCount;
    const Ps5LiftFunctionEntry* Functions;
    uint64_t FunctionCount;
    uint64_t Entry;
    uint64_t TlsAddress;
    uint64_t TlsFileSize;
    uint64_t TlsMemorySize;
    uint64_t TlsAlignment;
} Ps5LiftImage;

extern const Ps5LiftImage ps5lift_image;

void ps5lift_dispatch(Ps5LiftState* state, uint64_t target);
void ps5lift_apply_mxcsr(Ps5LiftState* state);
uint32_t ps5lift_read_mxcsr(Ps5LiftState* state);
uint64_t ps5lift_thread_create(Ps5LiftState* parent, uint64_t entry, uint64_t argument);
uint64_t ps5lift_thread_join(uint64_t handle);
Ps5LiftState* ps5lift_current_state(void);
void ps5lift_unsupported(Ps5LiftState* state, uint64_t address, const char* mnemonic);
void ps5lift_trap(Ps5LiftState* state, uint64_t address);
void ps5lift_string_op(Ps5LiftState* state, uint32_t operation, uint32_t width, uint32_t repeat);

#ifdef __cplusplus
}
#endif

#endif
