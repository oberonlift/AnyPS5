# Design

## Goal

Run PS5 titles natively on aarch64 Linux (first target: DGX Spark, GB10 with Cortex-X925/A725 and a Blackwell GPU) by recompiling the guest's x86-64 code ahead of time. AnyPS5 already reimplements the system libraries, AGC and the shader recompiler as C++. Those build natively for aarch64, so after recompilation nothing in the process is x86-64.

PS5 titles suit static recompilation: retail games cannot generate code at run time, every executable and module ships with the title, executables carry relocations and `.eh_frame`, and imports are named by NID.

## Pipeline

```
eboot.bin + sce_module/*.prx
  -> AnyPS5 relinker SELF unwrapping, SCE dynamic tables and modules converted to a Linux x86-64 ELF whose imports are named by NID
  -> GuestImage      AnyPS5 ElfReader: segments, PT_TLS, the dynamic segment's symbols and relocations, applied statically; eh_frame function starts
  -> Discovery       Zydis recursive descent from every known start; call targets become new starts until a fixpoint; jump tables
  -> Lifter          one LLVM function per guest function, void guest_<address>(Ps5LiftState*), in parts of about 50000 instructions
  -> Codegen         each part on a thread of its own: LLVM O2, then an object for the target triple and CPU; one archive of all parts
  -> link            with the runtime, the import bridges and the native AnyPS5 libraries
```

## Images

A program can be several ELF images: the executable and the modules it names in `DT_NEEDED` as `$ORIGIN/<path>`, which is how the AnyPS5 relinker names the `.guest.prx` modules it converts from a title's `sce_module` directory. Any other `DT_NEEDED` entry is a native library, reached through bridges. `GuestProgram::Load` ([Program.cpp](../lifter/src/Program.cpp)) reads the executable at its link addresses and the modules breadth-first, each position-independent module at the next 16 MiB boundary after the previous image and its import addresses, and relocates them only once all are read: an undefined symbol binds to the first image in that order that exports it, as `ld.so` would bind it, and only a symbol no image exports becomes an import. A relocation against a symbol the image defines binds to its own definition.

Discovery runs per image, from its entry point, `DT_INIT`, relocation targets, symbols and `.eh_frame`. The functions of each image go into parts of their own, and one more part holds `ps5lift_program`: a table per image (segments, TLS, imports, data imports, `.eh_frame_hdr`, `DT_INIT` and `DT_INIT_ARRAY`), the order to initialize the modules in, which is a depth-first walk of their dependencies, and one sorted function table over every image for dispatch. Before the executable's entry point the runtime calls each module's `DT_INIT` and `DT_INIT_ARRAY` entries with `argc`, `argv` and `envp` on the main thread. `tests/fixtures/modules.c` runs an executable whose module's constructor calls an import, and which calls a module function directly and through a pointer and reads and writes the module's data.

## Machine model

`Ps5LiftState` ([State.h](../runtime/include/ps5lift/State.h)) holds the sixteen GPRs, the flags as bytes, the FS and GS bases, MXCSR and sixteen 256-bit vector registers. Each lifted function caches the registers its instructions name (explicitly or implicitly) in allocas: it loads them on entry, stores them back before every call, return, dispatch and runtime helper, and reloads them after. Registers a function never names stay in the state, where its callees find them. LLVM promotes the allocas to SSA values, so registers live in host registers within a function and flags nobody reads are deleted.

The status flags do not cross calls or returns, as the SysV ABI says; only DF does. Compiled code never reads flags its caller or callee left behind, and dropping them removes the flag and parity computations that would otherwise precede every call. `--preserve-flags` keeps them for hand-written assembly that does rely on them.

Guest memory is identity mapped: the runtime maps the image at its link addresses, so a guest pointer is a host pointer and a memory operand becomes one load or store. Native libraries can read guest structures directly; x86-64 SysV and AArch64 LP64 lay out ordinary C types the same way (`long double` and `va_list` differ).

The guest stack is real guest memory. `call` pushes the return address, the callee's `ret` pops it, and the host call returns; guest code that reads its return address or walks its stack sees what it would see on the console.

## Control flow

- Direct calls are host calls to `guest_<target>`.
- Direct jumps inside a function are branches; jumps or fall-through to another function's start are tail calls.
- Indirect calls and jumps go through `ps5lift_dispatch`, which looks up the target in the sorted function table. A miss stops with the address, which names a function discovery has to find.
- Jump tables (absolute `jmp [table + index*8]`, `mov reg, [table + index*8]; jmp reg`, and clang's PIC `lea; movsxd; add; jmp`) are recovered and lifted as an LLVM `switch` that falls back to dispatch. The bound comes from the guarding `cmp` or `sub` with `ja`/`jae`, on the index or on a register it is a 32- or 64-bit `mov` copy of. A table without a recognised guard ends at the next address that code refers to as data, which a linear sweep of the executable segments collects before discovery, or at its first entry outside the function. `ps5lift` reports how many tables it sized that way.
- Every undefined symbol a relocation names gets a synthetic address past the image, and its slots are filled with it. Each such address is a lifted function that calls `ps5lift_import_<name>(Ps5LiftState*)` and pops the return address; `_` in the name becomes `__`, `+` `_p`, `-` `_m` and `#` `_h`. A `call` or `jmp` through a slot that holds an import is a direct call to it, and anything else that loads the slot reaches it through dispatch, so PLT stubs and PS5's `call [rip+slot]` both work. For PS5 titles the name is the NID, and the bridge calls the AnyPS5 implementation with arguments taken from rdi, rsi, rdx, rcx, r8, r9, xmm0-7 and the stack.
- Guest code can also leave a frame without `ret`. To unwind, the runtime puts the target's registers into the state, records the guest address to resume at and the stack pointer it resumes with, and sets `Unwinding`. Lifted code checks that flag after every guest call: a function with no resume point returns at once, without writing its cached registers back, and a function with resume points takes over when its entry stack pointer is at or above the recorded one, which only the target frame and frames outside it satisfy and the target is reached first. It clears the flag, reloads its registers and branches to the resume point. `longjmp` is the first user: discovery makes the return address of every call to a `setjmp`-family import (by name or NID) a resume point, and the runtime's `setjmp` and `longjmp` ([Jump.cpp](../runtime/src/Jump.cpp)) use AnyPS5's 96-byte `jmp_buf`, including its signal mask when AnyPS5's libc is linked in. The check costs a load and a branch that is not taken after each call; frames do not need unwind tables or landing pads.
- Guest C++ exceptions use the same mechanism. Every landing pad in a function's LSDA is a resume point, found through the image's `.eh_frame_hdr`. The guest's `__cxa_throw` reaches AnyPS5's exception runtime through its bridge, and AnyPS5's DWARF unwinder, built for a lifted guest, starts from the guest frame that called the import, finds frames in the guest's unwind tables, resolves the personality routine from its import name and calls AnyPS5's own, and installs the landing pad's registers into the state. It then throws a host `GuestUnwind` exception, which every generated bridge catches. That leaves the native frames between the bridge and the unwinder, and the bridge returns into the lifted frames with the flag set.
- An undefined data symbol (an `STT_OBJECT` import such as `_Stdout` or a `__cxxabiv1` type_info vtable) gets no synthetic address. The lifter records each slot that refers to it, and the runtime fills the slot with the address of `ps5lift_data_<NID>`, which the bridge generator defines for each variable AnyPS5 exports. `--import-nids` makes the lifter use the NID of a plain symbol name, so a guest built for Linux against libstdc++ reaches AnyPS5's exports.
- Relocation targets inside executable segments seed discovery, which is how function pointers in stripped images are found.
- The runtime enters the image the way Linux does, with `argc`, `argv`, `envp` and `auxv` at `rsp`, so the relinker's entry stub runs unchanged. It copies `PT_TLS` below the thread pointer (x86-64 variant II) and stores the thread pointer at `fs:0`.

## x87

The eight x87 registers, the tag bits, the control word and the status word (with TOP in bits 11-13) live in `Ps5LiftState`. Each x87 instruction becomes a call to `ps5lift_x87` ([X87.cpp](../runtime/src/X87.cpp)) with an operation, an operand format and an address or register index; it needs no register flush, because it only touches the x87 state and guest memory. The arithmetic is Berkeley SoftFloat's 80-bit extended format with its 8086 specialization, which follows x86's NaN rules, built with thread-local state so each guest thread has its own rounding mode. On top of SoftFloat the runtime reproduces what the x87 adds: rounding and precision control from the control word (precision applies only to `fadd`, `fsub`, `fmul`, `fdiv` and `fsqrt`), the denormal-operand flag, C1 as the round-up indicator, stack overflow and underflow with the indefinite NaN, integer stores that write the integer indefinite value, C0/C2/C3 left unchanged by instructions that do not define them, and the exact low quotient bits of `fprem` and `fprem1`. `tests/fixtures/x87.c` checks arithmetic, comparisons, conversions, `fsqrt`, `frndint`, `fscale`, `fprem`, `fprem1`, `fxam`, the constants and the exception flags under every rounding mode and three precisions against native results.

MMX registers are the low 64 bits of the physical x87 registers in the state, as on the hardware. The lifter reads and writes them there directly; an instruction with an MMX operand sets TOP to 0 and marks every register valid, a write sets bits 64-79 of its register to ones, and `emms` marks them all empty. MMX arithmetic goes through the same vector code as SSE, with a 64-bit operand as one lane. `tests/fixtures/mmx.c` checks every MMX instruction EVO Player uses and that an x87 store sees an MMX value; Rosetta, which supplied the other expected values, does not alias the two and stores the indefinite value there.

### Guest long double

The guest's `long double` is the x87 80-bit format in a 16-byte slot. AnyPS5's libc computes with the host's `long double`, which on aarch64 is IEEE binary128: it holds every 80-bit value exactly and has the same exponent range. The conversion happens where a value crosses into or out of guest memory. Bridges read `long double` arguments from their 16-byte stack slots and push `long double` results onto the guest's x87 stack, and the libc reads and writes guest memory through `GuestLongDouble::Load` and `Store` (va_list arguments, `scanf("%Lf")`, `modfl`, `_LDtest`, and the code that takes 80-bit values apart). Both directions go through SoftFloat in the runtime. A result is rounded to 80 bits under the guest's x87 rounding control, and while a bridge with a `long double` argument or result runs, the host rounding mode follows that control too, so a single operation rounds the same as on the console in the directed modes. The host's default NaN becomes the x87 indefinite value, and guest NaNs reach the host with their signalling bit intact. `strtold` parses with round-to-odd in binary128 so that the final rounding to 80 bits is correct. On an x86-64 host none of this converts anything, because the formats are the same.

## Variadic imports

A guest `printf` passes its arguments the SysV way, and an aarch64 host cannot express that call with its own `va_list`. Every variadic AnyPS5 export has a `v` sibling that takes a `VaList*`, a pointer to the SysV `va_list` structure, so the bridge for a variadic export builds that structure and calls the sibling: `VariadicArguments` in [Bridge.hpp](../runtime/include/ps5lift/Bridge.hpp) copies rdi to r9 and xmm0 to xmm7 into a register save area, sets `gp_offset` and `fp_offset` past the named arguments and points the overflow area at the guest stack. Guest code that builds its own `va_list` and calls `vsnprintf` passes it through unchanged.

In a lifted build AnyPS5's libc reads every `VaList` with its portable formatter (`FormatArguments`, the code its Windows build uses) instead of handing it to the host's `vprintf`, and scans with its in-process scanner. Host code that calls the libc's own `...` functions, such as the libc itself or upstream's tests, wraps its `va_list` in a `VaList` tagged with `gp_offset = 0xffffffff` (`APS5_VA_BEGIN` in [GuestVaList.hpp](../3rdparty/anyps5/core/libs/prx/libc/include/GuestVaList.hpp)), and `FormatArguments` reads that one with `va_arg`. The lifted libc keeps the host's C++ runtime and personality routine instead of replacing them with AnyPS5's guest unwinder, so its own `throw` and `catch` work on aarch64. When the libc is linked in, its guest arena has reserved the PS5 user address range, and the runtime commits the guest image inside it.

The generator follows the rule AnyPS5's NID patcher applies to a built library: every `extern "C"` function or variable a library defines is an export, named by the NID of its name without `_nid_postfix` or `_nid_disambigN`, unless it ends in `_nid_no_patch`; `_nid_no_patch_cut` and `APS5_EXPORT` give an explicit NID. Bridges for several libraries go into one file, and a NID that more than one exports (libkernel re-exports most of libc) is bridged to the first library named. The program links the libraries as the shared objects AnyPS5's build makes before NID patching, libc first, so the dynamic linker binds the few functions two of them define (`exit`, `rename`) to libc's. A variadic export without a `v` sibling, such as `open` or `fcntl`, receives the guest's remaining integer argument registers as extra arguments, which is where an aarch64 or x86-64 host's `va_arg` reads them.

libkernel built for a lifted guest reads the timestamp counter through the runtime (`cntvct_el0` on aarch64), as the lifted `rdtsc` instruction does. A guest thread's entry point arrives in `scePthreadCreate` as a callback thunk, so the new host thread runs it on a guest state of its own. A handler that `sceKernelRaiseException` delivers runs on the interrupted thread below its guest stack pointer, sees the guest registers as of that thread's last call and leaves them unchanged.

libSceFiber built for a lifted guest gives every fiber a host stack of its own (the context size plus 16 MiB, reserved but not committed) and switches host stacks with its own assembly, an aarch64 version beside the x86 one. Around each switch it saves the guest's callee-saved registers, stack pointer, MXCSR and x87 control word through `GuestCpuSaveFiber` and restores them through `GuestCpuRestoreFiber` once the fiber that switched away runs again, on whichever thread that is. A fiber starts with its guest stack pointer at the top of its guest context and calls its entry, a callback thunk, on the running thread's guest state. `tests/fixtures/fiber.c` switches two fibers back and forth and returns to the thread.

`tests/fixtures/libc.c` is a guest that calls `printf`, `snprintf`, `vsnprintf` with its own `va_list`, `sscanf`, `strtold`, `strtod`, `modfl`, `fabsl` and `puts` by NID. It is lifted and linked with the generated bridges, the libc built in lifted mode and the runtime, and its output is compared with `libc.expected`, which matches glibc except where AnyPS5 prints `long double` digits the way the console does. `tools/run_libc_tests.py` builds upstream's libc tests against the same libc.

## Callbacks

Native code cannot call a guest address. A bridge that receives a function pointer passes the native code a thunk instead: `Callback<R(Args...)>(guestAddress)` in [Bridge.hpp](../runtime/include/ps5lift/Bridge.hpp) returns one of 256 compile-time thunks per signature, the same one every time for the same guest address, so pointer comparisons keep working. A thunk places its arguments in the SysV registers and on the guest stack, pushes a return address and calls the lifted target through dispatch on the calling thread's guest state.

The runtime keeps each guest thread's state in a thread-local. A native thread that calls a thunk without one, such as a thread a native library started, gets a guest stack, a TLS block and a state on its first call, which is how a native `pthread_create` or a worker pool can run guest code.

## Memory ordering

x86 is TSO; the GB10 cores have no TSO mode. With `--memory tso` every guest load except an `rsp`-based one is an acquire load and every store a release store. On a CPU with FEAT_LRCPC (`neoverse-v2`, the GB10) acquire loads become `LDAPR`, which is cheaper than `LDAR`. Locked instructions, `xchg` with memory, `xadd` and `cmpxchg` are sequentially consistent LLVM atomics, lowered to LSE. `mfence` is a full fence.

qemu 8.2 computes the address of `STLUR` and `LDAPUR` with a negative offset wrongly; the test driver turns off `rcpc-immo` when it runs aarch64 code under qemu, and runs it with `-one-insn-per-tb` (see [known gaps](DEBT.md)). Real hardware is not affected by the first.

Guest memory accesses and register-state accesses carry distinct TBAA tags. The state is host memory that guest code cannot address, so LLVM may keep registers in host registers across guest loads and stores and drop redundant state traffic.

## Arithmetic

Flags are computed exactly for add, adc, sub, sbb, cmp, logic, inc, dec, neg, shifts and rotates (including the count-zero case), the multiplies and the BMI1/BMI2 instructions. Shifts are computed in 128 bits so no LLVM shift is ever poison. AF is computed for arithmetic and cleared for logic.

## Floating point

`--float exact` (the default) reproduces SSE and AVX results bit for bit, including NaNs, which LLVM otherwise leaves unspecified:

- A NaN result is the first NaN source, quieted: the first operand for two-operand arithmetic, and for FMA the left multiplicand, the right multiplicand, then the addend, as measured on an Intel host. Invalid operations without a NaN source give the negative default NaN (`0xffc00000`, `0xfff8000000000000`).
- `min` and `max` return the second operand when either is NaN or both are zero, and under DAZ return a denormal operand as a signed zero.
- Conversions to integers return the integer indefinite value (`0x80000000`, `0x8000000000000000`) for NaN and out-of-range inputs; float, double and half conversions carry NaN payloads as x86 does; `vcvtps2ph` with an immediate rounding mode is computed in integer arithmetic so it ignores the dynamic mode.
- Scalar instructions merge into the destination (legacy) or the first source (VEX) and zero bits 128-255 for VEX.
- `ldmxcsr` stores the guest MXCSR and calls the runtime, which sets the host rounding mode and flushing: on aarch64 with FEAT_AFP (the GB10), DAZ maps to `FPCR.FIZ` and FTZ to `FPCR.FZ` with `FPCR.AH`; without it either bit sets `FPCR.FZ`. Guests start with the console's MXCSR, `0x9fc0`.

`--float fast` drops the NaN and DAZ handling and leaves NaN payloads to the host.

## Roadmap

1. **Fixtures (done).** Integer code, calls, recursion, function pointers, jump tables, 128-bit products, division, atomics and Zen 2 BMI code lift and match the native output at -O0 to -O3, for aarch64 and x86-64.
2. **PS5 executables (in progress).** Done: PS5 ELFs go through AnyPS5's relinker and lift with NID imports, static relocations, relocation-target discovery and static TLS; AnyPS5's PS5-style `import` and `tls` fixtures run on x86-64 and aarch64. Remaining: generate the import bridges from AnyPS5's exports by NID, seed discovery from the init and fini arrays, TLS relocations, and lift the bundled `sce_module` files the relinker converts.
3. **Native AnyPS5 libraries on aarch64 (in progress).** `tools/generate_bridges.py` generates the import bridges from a library's export prototypes with libclang: 1421 of libc's 1454 function exports, including `long double` and variadic ones. AnyPS5's libc builds for aarch64 in lifted mode, passes seven of upstream's libc test programs there, and serves a lifted guest's `printf`, `sscanf` and `strtold` through the generated bridges. libkernel serves guest threads, mutexes and timers, guest `setjmp`, `longjmp` and C++ exceptions work, and libSceFiber switches fibers. AnyPS5's own CMake build with `ANYPS5_GUEST_LIFTED` builds all of its libraries for aarch64, libSceAgcDriver and its shader recompiler included. Remaining: guests that use libraries beyond libc, libkernel and libSceFiber, starting with libSceVideoOut and libSceAgcDriver.
4. **SSE, AVX2, FMA and F16C (done).** Integer and floating-point vector instructions, scalar SSE math, FMA3 and F16C, checked against native results by `tests/fixtures/vector.c` and `tests/fixtures/float.c` under three MXCSR settings. Remaining: x87 for libc's `long double`, `dpps`, gathers and masked moves.
5. **Exceptions.** Landing pads as dispatch entries so guest `.eh_frame` unwinding can resume inside lifted code.
6. **Breakout demo, then one title.** Followed by performance work: relaxing ordering where provably private, keeping vector registers in SSA, and recovering prototypes so hot functions skip the state.
