# Optimizations

Performance work for agents to pick up. Nothing here is implemented. Take the work items in order: each says where the code is, what has to stay true, and how to check it. After an item lands, re-measure and update the baseline in this file.

Rules for every item:

- Correctness comes first. `ctest` passes with both memory models and every fixture, and EVO Player still reaches its home screen.
- Lifted code runs only on Linux, so timings come from the GB10. The Mac can lift and read the generated code.
- Lifting a title takes about 11 minutes and 13 GB on the GB10, so iterate on EVO Player or the probe below, and lift a title once for the final number.

## Baseline

Measured on 2026-10-30. The probe was lifted at 265ee08; the benchmark used the GB10 checkout's `ps5lift` of that day.

### Benchmark

`tools/bench.py` on the GB10, scale 16, best of three runs. `tso/native` compares with the native build without vectorization.

| Kernel | native | native, no vectors | lifted, relaxed | lifted, TSO | tso/relaxed | tso/native |
|---|---|---|---|---|---|---|
| stream | 24.5 ms | 38.2 ms | 28.2 ms | 41.5 ms | 1.47× | 1.08× |
| chase | 54.2 ms | 52.6 ms | 64.9 ms | 66.9 ms | 1.03× | 1.27× |
| copy | 4.9 ms | 4.0 ms | 5.9 ms | 7.2 ms | 1.21× | 1.81× |
| hash | 10.5 ms | 11.3 ms | 10.4 ms | 11.7 ms | 1.13× | 1.03× |
| sort | 7.6 ms | 7.6 ms | 8.5 ms | 21.8 ms | 2.57× | 2.87× |

Without ordering, lifted code is within about 20% of native. Ordering costs little where loads wait on each other (`chase`) and the most where stores dominate (`sort`). The kernels have no indirect calls, no frame pointers and no floating point, so items 1, 2 and 4 below need kernels of their own before they can be timed.

To run it: `python3 -B tools/bench.py --ps5lift <build>/lifter/ps5lift --clang /usr/lib/llvm-18/bin/clang --clangxx /usr/lib/llvm-18/bin/clang++`. It builds the runtime into `runtime-<triple>` beside the `ps5lift` build directory; in a checkout another agent is using, copy the binary to `/tmp/<dir>/build/lifter/` first so that build stays untouched.

### Probe

Small kernels, built for x86-64 and lifted for `neoverse-v2`, then read as aarch64 assembly:

- **Summing an array of `long`.** x86 takes 4 instructions per element. With `--memory tso` the lifted loop takes 6: `cbnz` on the alignment test, `ldapr`, the add, and the loop. LLVM hoisted the test's `and` because an 8-byte stride keeps the alignment. With `--memory relaxed` LLVM unrolls the loop four times with `ldp`, at 9 instructions per four elements. The ordered loop is not unrolled.
- **A dot product over `__m128`.** With `--float exact` an iteration takes 17 instructions: two loads, the `fmla`, the loop, and 12 for NaN handling (`fcmeq`, `bif`/`bit` and `orr` per source, plus a copy of the accumulator they need). With `--float fast` LLVM unrolls it twice, at 7 instructions for two iterations.
- **A loop calling a small function directly.** The callee was in the same part, so LLVM inlined it. The loop then vectorized, and the return-address store and the `Unwinding` check left the loop.
- **The same loop calling through a pointer.** Around `ps5lift_dispatch` each iteration stores 11 values (the registers the function uses, DF, `rip`, the return address) and loads 10 (the registers, DF, `Unwinding`), on top of the lookup itself.

To reproduce on the Mac or the GB10:

1. Write the kernels as `noinline` functions, with `#pragma clang loop vectorize(disable) unroll(disable)` on the scalar loops, and a `_start` that calls them and spins.
2. Build them with `clang --target=x86_64-linux-gnu -O2 -march=znver2 -fpie -nostdlib -static -ffreestanding -fuse-ld=lld -Wl,--image-base=<GUEST_BASE from tools/run_fixture.py>`.
3. Lift with `ps5lift probe.elf -o probe.a --jobs 1`, adding `--memory relaxed` or `--float fast` to compare.
4. Read the code with `llvm-ar x probe.a && llvm-objdump -d --no-show-raw-insn part*.o`.

### Instruction mix

Static counts from a linear sweep of each relinked executable's code segments with GNU `objdump`: how often an instruction appears, not how often it runs. The function counts are `ps5lift`'s.

| | EVO Player | Unreal title A | Unreal title B |
|---|---|---|---|
| functions | 59,046 | 645,447 | 630,588 |
| direct call sites | 69,551 | 2,201,899 | 1,761,446 |
| indirect call sites | 56,856 | 297,390 | 189,577 |
| `mov %rsp,%rbp` | 28,679 | 463,422 | 418,572 |
| memory operands, scalar through `rbp` | 20.3% | 23.9% | 19.3% |
| memory operands, scalar through `rsp` | 14.5% | 2.2% | 2.5% |
| memory operands, scalar RIP-relative | under 2.1% | 5.4% | 7.5% |

Both titles keep frame pointers and address their locals through `rbp`, so the TSO exemption for `rsp` covers little of their stack traffic.

How the counts were made: the relinked executables have no section headers, so `llvm-objdump -d` finds nothing. Instead, cut each `PT_LOAD` with flags `R E` out of `work/output/app.elf` and run `x86_64-linux-gnu-objdump -D -b binary -m i386:x86-64 --adjust-vma=<vaddr>` on it. Then classify each memory operand by its base register, skipping `lea`, `nop` and `prefetch`; operands whose instruction names an `xmm` or `ymm` register count as vector.

For an ordering change, a better before-and-after check than timing is the number of ordered instructions in the lifted code: `llvm-objdump -d --no-show-raw-insn <archive> | grep -c -E '\b(ldapr|ldapur|ldar|stlr|stlur)\b'`.

### `FPCR.AH` on the GB10

The runtime sets `FPCR.AH` on hosts with FEAT_AFP (the function that reads `HwcapAfp` in [Runtime.cpp](../runtime/src/Runtime.cpp)). The GB10 has the feature on all 20 cores, and so does the Apple M4. A test on the GB10 ran each operation with the bit off and on and compared the results with x86's rules as [the design](DESIGN.md#floating-point) states them:

| Operation | AH=0 | AH=1 | x86 |
|---|---|---|---|
| add(QNaN 1, SNaN 2) | `7fc00002` | `7fc00001` | `7fc00001` |
| add(SNaN 1, QNaN 2) | `7fc00001` | `7fc00001` | `7fc00001` |
| add(QNaN 1, QNaN 2) | `7fc00001` | `7fc00001` | `7fc00001` |
| vector add(QNaN 1, SNaN 2) | `7fc00002` | `7fc00001` | `7fc00001` |
| mul(0, ∞) | `7fc00000` | `ffc00000` | `ffc00000` |
| max(QNaN 1, 1.0) | `7fc00001` | `3f800000` | `3f800000` |
| max(1.0, QNaN 1) | `7fc00001` | `7fc00001` | `7fc00001` |
| max(+0, −0) | `00000000` | `80000000` | `80000000` |
| min(−0, +0) | `80000000` | `00000000` | `00000000` |
| fma(QNaN 1, QNaN 2, QNaN 3) | `7fc00003` | `7fc00001` | `7fc00001` |
| fma(1.0, QNaN 2, QNaN 3) | `7fc00003` | `7fc00002` | `7fc00002` |
| fma(QNaN 1, 1.0, QNaN 3) | `7fc00003` | `7fc00001` | `7fc00001` |
| fma(QNaN 1, SNaN 2, 1.0) | `7fc00002` | `7fc00001` | `7fc00001` |
| fma(1.0, 1.0, SNaN 1) | `7fc00001` | `7fc00001` | `7fc00001` |
| fma(0, ∞, QNaN 3) | `7fc00000` | `7fc00003` | `7fc00003` |

`fma(a, b, c)` is `a * b + c`: x86's `vfmadd`, and aarch64's `fmadd` with `a` and `b` as the multiplicands. With the bit on, all fifteen cases match x86; with it off, eleven differ.

The test was a C program for the host. It set the bit with `mrs`/`msr fpcr`, ran each operation through inline assembly (`fadd`, `fmul`, `fmax`, `fmin`, `fmadd`, and a vector `fadd` on `.4s`) so the compiler could not fold it, and printed the result bits. It did not cover denormal inputs to `fmax` and `fmin` with `FPCR.FIZ` set.

## Work items

### 1. Hash the dispatch lookup

- **Code:** `Ps5Lift::LookupFunction` in [Runtime.cpp](../runtime/src/Runtime.cpp) binary-searches `ps5lift_program.Functions`, a sorted array of `Ps5LiftFunctionEntry {GuestAddress, Host}`. It has four callers:
  - `ps5lift_resolve`, which lifted indirect tail jumps call;
  - `ps5lift_dispatch`, for indirect calls and the callback thunks in [Bridge.hpp](../runtime/include/ps5lift/Bridge.hpp);
  - the thread entry in `RunThread`;
  - the fault handler in [NativeCall.cpp](../runtime/src/NativeCall.cpp).
- **Cost now:** about 20 dependent loads over a 10 MB table for a title, on every virtual call.
- **Change:**
  - Build an open-addressing table keyed by guest address, a power of two at least twice the function count.
  - Build it before the first guest thread starts, in `ps5lift_run_main` before `StartThread`, or as a function-local static.
  - Leave it read-only afterwards, so threads read it without locking.
  - Keep `LookupFunction`'s contract: null for an address that is not a function start.
  - Building the table at lift time in `TableLifter` instead would move the work out of start-up, at about 20 MB per title.
- **Measure:** first add a kernel to `tests/fixtures/bench.c` that makes indirect calls through a table of many functions, and record its baseline.
- **Check:** `ctest`. The `basic`, `callbacks` and `modules` fixtures call through function pointers and so go through dispatch.

### 2. Leave frame-pointer accesses unordered under TSO

- **Code:** `NeedsOrdering` in [Lifter.cpp](../lifter/src/Lifter.cpp) makes an access plain only when `operand.mem.base == ZYDIS_REGISTER_RSP`. `Push`, `Pop` and `CallGuest` already pass `ordered = false`.
- **Change:** also make an access plain when its base is `rbp` and the function keeps a frame pointer.
  - A function keeps one when its entry block starts with `push %rbp; mov %rsp,%rbp` and nothing else writes `rbp` except the restoring `pop %rbp` or `leave`.
  - Compute this once per function. `FindUsedRegisters` already walks every instruction, and discovery has `ReachingDefinitions`.
  - A function that uses `rbp` as a general register fails the test and keeps ordering.
- **What must stay true:** a frame is private to its thread. The `rsp` rule already assumes this. State the new rule in DESIGN.md's memory-ordering section.
- **Later:** registers that hold the address of a local, such as `lea -0x40(%rbp),%rax`, need dataflow and can follow.
- **Check:**
  - `ctest`.
  - The ordered-instruction count of EVO Player's or a title's lifted code should drop by about the `rbp` share in the table above.
  - For a timing, build the benchmark guest with `-fno-omit-frame-pointer` as well.

### 3. Leave loads from read-only segments unordered

- **Code:** `NeedsOrdering` in Lifter.cpp.
- **Change:** an access through `rip` without an index or segment override has a known address, `_instruction->Next() + disp`. If the whole access lies in a segment without `PF_W` (`GuestSegment::Flags` in [GuestImage.hpp](../lifter/include/lifter/GuestImage.hpp)), no other thread can write it, so the load can be plain.
- **Gain:** many of these are float constants loaded with `movss` or `movsd`. Today they take an `ldapr` into a general register and a move to the vector unit.
- **What must stay true:** the guest does not make its read-only segments writable at run time. Write that assumption down where the rule goes.
- **Check:** `ctest`, and the ordered-instruction count.

### 4. Let the hardware follow x86's NaN rules

- **Code:** in Lifter.cpp, `NanResult`, `Quiet`, `Arithmetic` (min and max), `FusedMultiplyAdd` and `DenormalsAreZero`. In Runtime.cpp, the FPCR setup and `RequireHostFeatures`.
- **Cost now:** 8 to 12 instructions per floating-point operation under `--float exact`.
- **Why the handling cannot simply go:** with `FPCR.AH` set, the GB10 returns x86's results for operands in the guest's order, and an x86-64 target does so by nature. But LLVM treats NaN payloads as unspecified and rewrites operations in ways that change them:
  - InstCombine orders the operands of commutative operations by complexity, and casts rank below other instructions. Lifted vector code is full of bitcasts, so `fadd`, `fmul` and the multiplicands of `fma` do get swapped. That changes the result when both operands are NaN.
  - It folds `fsub x, 0.0` to `x`, which leaves a signalling NaN unquieted.
  - It turns `fsub -0.0, x` into `fneg x`, which can flip a NaN's sign.
  - Code generation may commute operands as well.
- **Options, cheapest first:**
  - (a) Guard only the guest's first operand, as `select(isnan(first), quiet(first), r)`. That's about 3 instructions. It covers swapped operands and identity folds whose constant is the second operand, but not the `fneg` case.
  - (b) Also quiet a NaN result, for about 6 instructions.
  - (c) Hide the operations from the optimizer until code generation: constrained intrinsics, or calls to a pseudo-function replaced just before codegen. This is exact, but costs optimizations.

  Pick one only after a probe with two NaN operands in each order, at `-O0` to `-O3`.
- **Min and max:**
  - With `AH` set, `fmax` and `fmin` already match x86, so they could use the target intrinsics if LLVM does not treat those as commutative.
  - Their DAZ handling, which loads MXCSR from the state for every operand, can go only if `FPCR.FIZ` flushes their inputs. The test above did not check that.
- **Hosts without FEAT_AFP:** either `RequireHostFeatures` demands the feature, as it demands AES and PMULL, or the lifter keeps today's lowering behind an option. The lifter cannot see the run-time host.
- **Check:**
  - `ctest`, especially `tests/fixtures/float.c`. It feeds quiet and signalling NaNs and compares the results with an Intel CPU's at `-O0` to `-O3` (`float-O*.expected`).
  - The `vector` and `avx` fixtures.
  - The dot-product probe's instruction count.

## Later

- **Handle unaligned ordered accesses after they fault.**
  - Today `Load`, `Store` and `SplitOnAlignment` in Lifter.cpp test alignment before every ordered access.
  - The GB10 (FEAT_LSE2) faults only on ordered accesses that cross a 16-byte boundary, which compiled code rarely makes.
  - A `SIGBUS` handler beside the `SIGSEGV` one in NativeCall.cpp would decode `ldapr`, `ldapur`, `ldar`, `stlr` and `stlur` (2, 4 and 8 bytes), do the access plainly with a `dmb`, write the destination register and step past the instruction.
  - That removes an `and` and a branch per access where LLVM cannot hoist the `and`. It also removes the second path through loop bodies, which is likely what stops LLVM from unrolling ordered loops.
  - macOS reports alignment faults as `SIGBUS` too.
- **Pass fewer registers across calls.**
  - Today `Flush`, `Reload` and `CallGuest` in Lifter.cpp store every register the function uses before a call and reload them all after.
  - The SysV ABI preserves no vector register. So store only `ymm0`–`ymm7` (the arguments) and reload only `ymm0`–`ymm1` (the results), and reload no caller-saved general register other than `rax`, `rdx` and `rsp`.
  - Keep storing the callee-saved registers, because the unwinder restores a landing pad's registers from the values its callees pushed.
  - Hand-written assembly with its own conventions breaks this, and FFmpeg has some, so gate it behind a switch like `--preserve-flags`.
  - Handlers that `sceKernelRaiseException` delivers read the registers as of the last call, so they would see stale vector registers.
- **The [roadmap](DESIGN.md#roadmap)'s items:**
  - order only memory that another thread can reach;
  - recover prototypes so hot functions skip the state;
  - inline across parts. The probe's direct-call loop shows what inlining gains.

## Before starting

Profile, since the instruction mix is static. Run `perf record -g` on the lifted EVO Player under `xvfb-run` on the GB10, and on a title once one runs far enough. Check `LookupFunction`'s share of the time, then use `perf annotate` on the hottest lifted functions to see how much goes to `ldapr`/`stlr` and to NaN handling.
