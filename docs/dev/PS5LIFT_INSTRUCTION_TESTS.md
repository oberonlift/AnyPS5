# ps5lift instruction tests

Design for a test suite that proves ps5lift's translation of every x86-64 instruction a PS5 program can execute, against a real x86-64 CPU.

## Why

ps5lift translates guest code one instruction at a time, so a lifted program is only as correct as each instruction's translation. The tests today are whole C programs (`tests/fixtures`), built at `-O0` to `-O3` and lifted with and without LLVM's optimization. They show that the common paths work, but they cannot show that a given form is right for a given input: that `vpsignw` handles a zero element, or that `cvttps2dq` converts a NaN to the integer indefinite value. A translation and a hand-written test of it share their author's reading of the manual, so a misreading passes both. A lifted video decoder that produces corrupt output with every lifter version so far is most likely such a case.

The lifter also has no translation for part of the instruction set. A lifted program that reaches one of those instructions stops (`ps5lift_unsupported`). Lifted titles reach a few such Zen 2 instructions today, and every new title can bring others.

## Goal

1. Every encoding form of every instruction in the PS5 set (below) has test cases whose expected results come from executing them on x86-64 hardware.
2. Every case passes in both lift modes (optimized and `--unoptimized-lift`) and both floating-point modes (`--float exact` and `--float fast`, each judged by its own contract under "Floating-point modes") on every host where ps5lift's tests run lifted code: the GB10, the `ubuntu-24.04-arm` runner of ps5lift's CI, x86-64 Linux lifting for x86-64, and arm64 macOS once its lifted tests run. A translation that uses a host feature some of them lack keeps a path for the others, and each path runs on one of them. There are no skipped cases, no expected failures and no tolerances other than those listed under "Tolerances" and in fast mode's contract.
3. The work includes fixing at its cause every mismatch the cases find in existing translations and writing every missing translation. The project is done when the suite passes completely, and from then on a failing case blocks the change that causes it.
4. Instructions outside the PS5 set stop the program the way the console stops it, and the lift report counts them apart from instructions the lifter is missing, which reach zero.

## Non-goals

- Interactions between instructions that single cases cannot show: memory ordering between threads (`--memory tso`), self-modifying code, and the flags and registers that the lifter deliberately drops across calls and returns (see "Machine model" in [the design](PS5LIFT_DESIGN.md)). The fixtures keep covering those.
- Performance. Translations are judged by their results only.
- Title compatibility and platform ports.

## The PS5 instruction set

The PS5's CPU is an AMD Zen 2 (family 17h, model 47h), as [Cpuid.cpp](../../ps5lift/runtime/src/Cpuid.cpp) reports it. The set is taken from Zydis's instruction database (`datafiles/instructions.json` in zydis-db, at the revision that generated the tables of the Zydis submodule ps5lift builds with): every definition that is valid in 64-bit mode and whose extension is one of BASE, X87, MMX, SSE, SSE2, SSE3, SSSE3, SSE4, SSE4A, AVX, AVX2, AVX2GATHER, AVXAES, FMA, F16C, AES, PCLMULQDQ, SHA, BMI1, BMI2, LZCNT, ADOX_ADCX, MOVBE, RDRAND, RDSEED, CLFSH, CLFLUSHOPT, CLWB, XSAVE, XSAVEOPT, XSAVEC, XSAVES, RDPID, RDTSCP, PAUSE, LONGMODE or AMD3DNOW_PREFETCH, except the groups under "Instructions outside the set": definitions the database marks as CPL 0 (its `cpl` field), the I/O and IOPL-dependent instructions, those that depend on the console's segment tables, and those whose effect is the console kernel's. At the current database that leaves about 2,900 definitions under about 900 mnemonics (the generator prints the exact counts for the revision it reads); a definition that allows several operand sizes counts as one form per size.

The set follows Zen 2's behaviour where Zydis's labels do not:

- The lifter and the generator decode with Zydis's AMD branch mode (`ZYDIS_DECODER_MODE_AMD_BRANCHES`), so a 66-prefixed near branch is the 16-bit branch Zen 2 executes, not the 64-bit one Intel does.
- Encodings in the hint space that Zen 2 executes as NOPs or prefetches belong to the set with that behaviour, whatever extension Zydis gives them: `endbr32` and `endbr64`, `bnd`-prefixed branches (an ordinary branch), `cldemote`, `prefetchit0` and `prefetchit1` (NOPs), and the `0F 0D` prefetch encodings, including the one Zydis names `prefetchwt1`. This short list is the only exception to classifying by extension.
- The APX REX2 rows that the database files under in-set extensions are not in the set: their prefix is invalid on Zen 2. Instructions in the set that fault by definition, such as `ud2`, `ud0`, `ud1` and `rsm`, keep their cases: the expected result is the fault.

Zen 2 also implements CLZERO, MCOMMIT, RDPRU, MONITORX/MWAITX and FSGSBASE, but the PS5 CPUID that ps5lift reports leaves them out, so a program that checks CPUID does not use them. They are treated as instructions outside the set (an invalid-opcode stop) until a console measurement says otherwise.

The generator reads the database, not a list in the source, so a newer Zydis brings its definitions with it.

## Test cases

A case is one instruction instance with an initial machine state.

- **Instance:** the bytes Zydis's encoder produces for one form with chosen operands. Register operands are chosen to cover the special cases of the encoding rather than every combination: the low and high registers (`rax`, `r15`), `rsp` and `rbp` where an encoding treats them specially, the high byte registers (`ah` to `bh`) and the REX forms that exclude them, the same register as source and destination, and `xmm8` to `xmm15` and `ymm` forms. Memory operands cover each addressing form: base only, base and index with each scale, displacement sizes, RIP-relative, and `rsp`/`rbp`/`r12`/`r13` as base.
- **Initial state:** the sixteen GPRs, RFLAGS (CF, PF, AF, ZF, SF, OF and DF; TF and AC clear), MXCSR, the x87 control word, status word, tag word and the eight registers, `ymm0` to `ymm15`, and a memory block that holds every memory operand, with the stack inside it.
- **Inputs:** seeded random values plus edge values for each operand type. Integers: 0, 1, -1, the minimum and maximum of each width, values either side of a sign or carry boundary, and shift and rotate counts of 0, 1, width - 1, width, width + 1 and at least 255. Floating point (each width, packed and scalar): positive and negative zero, the smallest and largest denormal and normal, infinities, quiet and signaling NaNs with payloads, and values on either side of a rounding boundary. MXCSR: each rounding mode, FTZ and DAZ alone and together, and each exception unmasked; an input that runs with only one of FTZ and DAZ set also runs with both, which fast mode's comparison needs. x87: each precision and rounding control. Memory: aligned and misaligned operands, and operands that end at the block's end.

The seed fixes every case, so every run tests the same cases.

### Capturing the result

Each case runs inside guest code that the generator writes around the instance: it loads the initial state, executes the instance, and stores the whole final state into a result block in guest memory before it returns. That includes the flags, through `lahf` and `seto` rather than `pushfq` (and DF through the direction a string instruction moves), so that both sides capture them with the same instructions and the comparison does not depend on what the lifter treats as live at a return. The same guest program then produces both sides of the comparison: run natively on x86-64 it records the expected results, and lifted on aarch64 it produces the results under test. The loading and storing code uses only instructions whose own cases pass, starting from plain moves.

## Expected results

The expected results are recorded by running the guest program natively on an Intel N100 running Linux. Its CPU (Gracemont) has every extension in the set except SSE4A, and Linux matches the platform the lifted side runs on. The recordings are committed to the repository, compressed; since the seed regenerates every input, only results are stored. Recording again is a manual step, needed when the set, the inputs or Zydis change; the recorder refuses a CPU that lacks an extension it is asked to record.

### Undefined and vendor-specific results

- Flags that the manuals leave undefined for a form are not compared. The database marks them (`u` in `affected_flags`).
- Destination values that Intel leaves undefined but AMD documents (for example `bsf` and `bsr` with a zero source, which leave the destination unchanged on AMD) are expected as AMD documents them, since Zen 2 is the target; each such case has its expectation written from AMD's manual instead of recorded.
- Where Intel and AMD both document a behaviour and they differ, AMD's is expected, written from AMD's manual instead of recorded. Known forms: the x87 pointer fields `fxsave`, `fnstenv`, `fnsave` and `xsave` write (AMD writes FIP, FDP and FOP only when an unmasked exception is pending), `sysenter` (an invalid opcode in long mode on AMD), 66-prefixed near branches, and MXCSR's misaligned-SSE bit (MM, bit 17), which the PS5 CPUID's MisAlignSse bit allows in `ldmxcsr` and `fxrstor` and Intel rejects. The generator keeps this list; a form joins it when its AMD documentation differs from the recording.
- Results that are implementation-specific on both vendors, such as which NaN `dpps` returns when several products are NaN, are compared after the same canonicalization `sse41.c` uses, and listed in [TechnicalDebt.md](TechnicalDebt.md#ps5lift) as unverified on Zen 2.

### Instructions without an Intel reference

SSE4A (`extrq`, `insertq`, `movntsd`, `movntss`) exists only on AMD. Its expectations are written from AMD's manual (APM volume 4), not recorded.

### Tolerances

- The x87 transcendental instructions (`fsin`, `fcos`, `fsincos`, `fptan`, `fpatan`, `f2xm1`, `fyl2x`, `fyl2xp1`) differ between processors, and AMD documents no error bound for them. They are compared against the true function rounded to the precision control, within one unit in the last place, for operands inside each instruction's documented range; an operand outside it (for example `fsin` of a value of magnitude 2^63 or more, which sets C2 and leaves the operand) is compared exactly. They are listed in TechnicalDebt.md as unverified on Zen 2.
- `rcpps`, `rcpss`, `rsqrtps`, `rsqrtss` and their VEX forms return approximations that differ between vendors. They are compared within the relative error both vendors document (1.5 * 2^-12), with exact results for zeros, infinities and NaNs, and listed in TechnicalDebt.md as unverified on Zen 2.
- `rdrand` and `rdseed` are compared by their flags and the fact that they write, since their values cannot repeat.

These are the only cases not compared exactly under `--float exact`; `--float fast` adds the differences its contract lists.

### Instructions whose results the console defines

`cpuid` and `xgetbv` are compared against the values [Cpuid.cpp](../../ps5lift/runtime/src/Cpuid.cpp) reports for the PS5, not the recorder's own. `rdtsc`, `rdtscp` and `rdpid` are compared by the properties the console guarantees (the counter does not go backwards; the processor ID is valid).

## Floating-point modes

ps5lift lifts SSE and AVX floating point in one of two modes, chosen the way a compiler's fast-math is: `--float exact` (the default) and `--float fast` ([floating point](PS5LIFT_DESIGN.md#floating-point) in the design). Every case runs in both, and each mode is judged by its own contract. x87 is computed with SoftFloat in both modes, so the option does not change it.

`--float exact` promises x86's results in every MXCSR state on every host: NaN results as x86 picks them, MXCSR's exception flags as x86 sets them (DE included, and UE with tininess judged after rounding), FTZ and DAZ each on its own, and an unmasked SIMD exception raised at its instruction before the destination is written, compared like any other fault. It does not require FEAT_AFP: that extension's alternate handling (`FPCR.AH`) stays clear for the reason the design gives, so exact mode computes in code what the host's floating point does differently. What exact mode misses of this today (the host's exception flags, FTZ and DAZ as one switch, no SIMD exceptions; [TechnicalDebt.md](TechnicalDebt.md#ps5lift)) is a mismatch like any other, fixed in stage 3.

`--float fast` gives up exactness in a fixed list of ways, and fast-mode results are compared with these differences allowed and no others:

- A NaN result may be any NaN, with the host's payload and sign.
- Under DAZ, `min` and `max` may return a denormal operand where x86 returns a signed zero.
- MXCSR's exception flags are the host's and are not compared.
- FTZ and DAZ act together: a case with only one of them set is compared with the recording of the same inputs with both set.
- A flushed result is judged tiny before rounding, so a result that x86 rounds to the smallest normal may be a zero of the same sign.
- SIMD exceptions are not raised. An instruction that unmasks one (`ldmxcsr`, `vldmxcsr`, `fxrstor` or an `xrstor` form) stops with a message that names it and says the program needs `--float exact`, and its cases expect that stop.

A program that keeps the console's MXCSR (`0x9fc0`: FTZ and DAZ set, every exception masked) meets neither of the last two.

## Running the cases lifted

The generated guest program is lifted with ps5lift like any fixture and linked with the runtime and a driver. ctest runs it on aarch64 Linux in both lift modes and both floating-point modes, with one test per extension so that a failure names its area; the same tests run on any host that runs lifted code. A mismatch prints the instruction as Zydis formats it, the case's inputs, and the expected and actual values of every field that differs.

## Faults

Some inputs fault on the hardware: a divide by zero or an overflowing quotient, a misaligned operand of an instruction that requires alignment, an access past the memory block. A case that faults is compared by the exception (divide error, general protection, page fault, invalid opcode, SIMD floating-point, alignment check), the address of the faulting instruction, and for a page fault the data address. Registers and memory at the fault are not compared: lifted code does not deliver guest exceptions to the guest's handlers yet (TechnicalDebt.md), so nothing can observe them. When that delivery exists, the partial state the architecture defines (a `rep` string instruction's counts and pointers, a gather's mask) joins the comparison. The recorder translates Linux's signals and `si_addr` into those fields, and the driver observes a fault without ending the run.

## Instructions outside the set

Every other instruction gets cases too, which check that the lifted program stops as the console would:

- **Not on the PS5 CPU:** extensions Zen 2 lacks (AVX-512 in all its forms, XOP, FMA4, TBM, 3DNow! except its prefetches, VAES, VPCLMULQDQ, GFNI, AMX, PadLock and the rest) and the Zen 2 extras the PS5 CPUID leaves out. Zydis gives each decoded instruction's extension, so the lifter classifies them by extension, with the hint-space list above as the only exception. On the console they raise an invalid-opcode exception.
- **Privileged or I/O:** definitions the database marks as CPL 0 (21 mnemonics, among them `hlt`, `lgdt`, `wrmsr`, `swapgs`, `xsaves`, and `mov` to or from a control or debug register), which Zydis reports as privileged, and the I/O and IOPL-dependent instructions (`in`, `out`, `ins*`, `outs*`, `cli`, `sti`, and `rdpmc`, which no measurement shows the console enabling for user code), which a PS5 program is never allowed to use. On the console they raise a general-protection exception.
- **Dependent on the console's segment tables:** `mov` to a segment register, `lar`, `lsl`, `verr`, `verw`, `sgdt`, `sidt`, `sldt`, `smsw`, `str`, far `ret`, far `call`, far `jmp`, `iret` in each size, and `lfs`, `lgs`, `lss`. Their results depend on descriptor tables that cannot be measured without a PS5, so they stop with a message that names the instruction and why. They appear only where discovery takes data for code.
- **Dependent on the console's kernel:** `syscall`, `sysenter`, `int n` and `int1`. What they do is the kernel's, not the CPU's (Linux, where the recordings are made, answers `int 0x80` as a system call), so they stop with a named message too. Retail code reaches the kernel through libkernel, which AnyPS5 replaces.

The lift report counts these three groups and the instructions the lifter is missing separately, and the runtime's stop names the group.

## Stages

1. The generator, the recorder, the driver, and a report command (`tools/ps5lift.py instructions`) that runs every case and prints each form's result, with the first recording. The first run's mismatches and missing instructions are the work list.
2. The classification of instructions outside the set, in the lifter and its report.
3. A fix at its cause for every mismatch in an existing translation, including what exact mode misses of its floating-point contract.
4. A translation for every missing instruction, each landing with its cases passing.

ctest registers an extension's test once every case in it passes, so ctest never carries a failing or skipped case and main's CI stays green throughout; the report command shows the whole suite at any point. The project is done when every extension is registered.

## Decided

- The suite covers the whole set and passes completely; nothing is skipped or expected to fail.
- Expected results come from x86-64 hardware: an Intel N100 running Linux records them. A Zen 2 machine is not required.
- Instructions the console cannot execute are covered by cases that check the console's stop.
- Instructions whose results depend on the console's segment tables or its kernel stop with a named message.
- The cases run in both lift modes on aarch64 Linux.
- Floating-point exactness is a lift option, as fast-math is for a compiler, and ps5lift does not require FEAT_AFP. The suite proves `--float exact` and `--float fast` each against its own contract.

## Calls made in writing this design

These were settled without a decision from the project's owner and can be revisited:

- A case that faults is compared by its exception and addresses, not its registers and memory, until lifted code delivers guest exceptions.
- Approximation instructions (`rcp*`, `rsqrt*`) and x87 transcendentals are compared within a tolerance, the only exceptions to exact comparison besides `rdrand` and `rdseed`.
- AMD's documented behaviour wins over the Intel recording where the vendors differ.
- Decoding follows Zen 2: AMD branch mode, and the hint-space list.
- Faults on misaligned operands are reproduced: a check before each access that requires alignment.
- Zen 2 instructions the PS5 CPUID leaves out stop as invalid opcodes.
- `syscall` and `int n` stop with a named message, as do the instructions that depend on segment tables.
- ctest registers an extension once it passes completely; a report command shows the rest.
- Fast mode's contract is the list under "Floating-point modes": today's fast mode, with the floating-point environment as the host gives it, and a named stop where a program unmasks a SIMD exception instead of running on without it.
- A translation may use a host feature that some test hosts lack if it keeps a path for the others and each path runs on a test host.

## Left to the builder

- The generator's language and where it lives under `ps5lift/tools` or `ps5lift/tests`, and how it finds the zydis-db revision that matches the Zydis submodule.
- The number of cases per form beyond the edge values, and the size of the committed recordings, which should stay small enough for a public repository.
- The format of the guest program (one per extension, or one in parts) and of the recordings.
- How the driver observes a fault without ending the run.
- How expectations written from the manuals are kept next to the recorded ones.
- How exact mode computes the floating-point environment, and whether a part of FEAT_AFP that works with `FPCR.AH` clear helps on the hosts that have it.
