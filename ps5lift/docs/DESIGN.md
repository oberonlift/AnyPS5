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
  -> Lifter          one LLVM function per guest function, void guest_<address>(Ps5LiftState*)
  -> Codegen         LLVM O2, then an object file for the target triple and CPU
  -> link            with the runtime, the import bridges and the native AnyPS5 libraries
```

## Machine model

`Ps5LiftState` ([State.h](../runtime/include/ps5lift/State.h)) holds the sixteen GPRs, the flags as bytes, the FS and GS bases, MXCSR and sixteen 256-bit vector registers. Each lifted function loads the GPRs, flags and vector registers into allocas on entry and stores them back before every call, return, dispatch and runtime helper, and reloads them after. LLVM promotes the allocas to SSA values, so registers live in host registers within a function and flags nobody reads are deleted.

Guest memory is identity mapped: the runtime maps the image at its link addresses, so a guest pointer is a host pointer and a memory operand becomes one load or store. Native libraries can read guest structures directly; x86-64 SysV and AArch64 LP64 lay out ordinary C types the same way (`long double` and `va_list` differ).

The guest stack is real guest memory. `call` pushes the return address, the callee's `ret` pops it, and the host call returns; guest code that reads its return address or walks its stack sees what it would see on the console.

## Control flow

- Direct calls are host calls to `guest_<target>`.
- Direct jumps inside a function are branches; jumps or fall-through to another function's start are tail calls.
- Indirect calls and jumps go through `ps5lift_dispatch`, which looks up the target in the sorted function table. A miss stops with the address, which names a function discovery has to find.
- Jump tables (absolute `jmp [table + index*8]`, `mov reg, [table + index*8]; jmp reg`, and clang's PIC `lea; movsxd; add; jmp`) are recovered and lifted as an LLVM `switch` that falls back to dispatch. The bound comes from the guarding `cmp` or `sub` with `ja`/`jae`; without one, entries are read while they point inside the function.
- Every undefined symbol a relocation names gets a synthetic address past the image, and its slots are filled with it. Each such address is a lifted function that calls `ps5lift_import_<name>(Ps5LiftState*)` and pops the return address; `_` in the name becomes `__`, `+` `_p`, `-` `_m` and `#` `_h`. A `call` or `jmp` through a slot that holds an import is a direct call to it, and anything else that loads the slot reaches it through dispatch, so PLT stubs and PS5's `call [rip+slot]` both work. For PS5 titles the name is the NID, and the bridge calls the AnyPS5 implementation with arguments taken from rdi, rsi, rdx, rcx, r8, r9, xmm0-7 and the stack.
- Relocation targets inside executable segments seed discovery, which is how function pointers in stripped images are found.
- The runtime enters the image the way Linux does, with `argc`, `argv`, `envp` and `auxv` at `rsp`, so the relinker's entry stub runs unchanged. It copies `PT_TLS` below the thread pointer (x86-64 variant II) and stores the thread pointer at `fs:0`.

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
3. **Native AnyPS5 libraries on aarch64.** Build the libraries for aarch64 and port their x86-specific parts (`UserContext`, `Fiber`, `RaiseException`, `Backtrace`, `HostCpu`, `FloatingPoint`, `AtomicBuiltins`, about 28 files) to read and write `Ps5LiftState`. Reverse bridges let native code call guest callbacks: thread entries, `atexit`, comparators, fibers and JobManager jobs.
4. **SSE, AVX2, FMA and F16C (done).** Integer and floating-point vector instructions, scalar SSE math, FMA3 and F16C, checked against native results by `tests/fixtures/vector.c` and `tests/fixtures/float.c` under three MXCSR settings. Remaining: x87 for libc's `long double`, `dpps`, gathers and masked moves.
5. **Exceptions.** Landing pads as dispatch entries so guest `.eh_frame` unwinding can resume inside lifted code.
6. **Breakout demo, then one title.** Followed by performance work: relaxing ordering where provably private, keeping vector registers in SSA, and recovering prototypes so hot functions skip the state.
