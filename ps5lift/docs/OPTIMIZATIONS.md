# Optimizations

Where lifted code loses time against native code, and what to change, cheapest first. Nothing here is implemented yet; the numbers are the baseline to measure the changes against.

## Measurements

### Benchmark

`tools/bench.py` on the GB10, scale 16, best of three runs. `tso/native` compares with the native build without vectorization.

| Kernel | native | native, no vectors | lifted, relaxed | lifted, TSO | tso/relaxed | tso/native |
|---|---|---|---|---|---|---|
| stream | 24.5 ms | 38.2 ms | 28.2 ms | 41.5 ms | 1.47× | 1.08× |
| chase | 54.2 ms | 52.6 ms | 64.9 ms | 66.9 ms | 1.03× | 1.27× |
| copy | 4.9 ms | 4.0 ms | 5.9 ms | 7.2 ms | 1.21× | 1.81× |
| hash | 10.5 ms | 11.3 ms | 10.4 ms | 11.7 ms | 1.13× | 1.03× |
| sort | 7.6 ms | 7.6 ms | 8.5 ms | 21.8 ms | 2.57× | 2.87× |

Without ordering, lifted code is within about 20% of native code. TSO costs little where loads wait on each other (pointer chasing) and the most where stores dominate (heap sort).

### Probe kernels

Small loops built with `clang -O2 -march=znver2`, lifted for `neoverse-v2`, and read as aarch64 assembly:

- **Summing an array of `long`.** x86 needs 4 instructions per element. With `--memory tso` the lifted loop takes 6: the alignment test's branch before the `ldapr` (LLVM hoisted its `and`, since an 8-byte stride keeps the alignment), then the add and the loop. With `--memory relaxed` LLVM unrolls it four times with `ldp`, at 9 instructions per four elements.
- **A dot product over `__m128`.** With `--float exact` an iteration takes 17 instructions: two loads, an `fmla`, the loop, and 12 instructions of NaN handling (`fcmeq`, `bif`/`bit`, `orr` per source, and a copy of the accumulator they need). With `--float fast` LLVM unrolls it twice, at 7 instructions for two iterations.
- **A loop calling a small function directly.** The callee is in the same part, so LLVM inlined it. The loop then vectorized, and the return-address store and the `Unwinding` check moved out of it. Inlining removes all of a call's overhead.
- **A loop calling the same function through a pointer.** Around `ps5lift_dispatch` each iteration stores 11 values (the registers the function uses, DF, `rip` and the return address) and loads 10 (the registers, DF and `Unwinding`), on top of the lookup itself.

### Instruction mix

The counts come from a linear sweep of each relinked executable's code segments with GNU `objdump`. They are static: how often an instruction appears, not how often it runs. Function counts are `ps5lift`'s. The two commercial titles are C++ engines.

| | EVO Player | title A | title B |
|---|---|---|---|
| functions | 59,046 | 645,447 | 630,588 |
| direct call sites | 69,551 | 2,201,899 | 1,761,446 |
| indirect call sites | 56,856 | 297,390 | 189,577 |
| `mov %rsp,%rbp` | 28,679 | 463,422 | 418,572 |
| memory operands, scalar through `rbp` | 20.3% | 23.9% | 19.3% |
| memory operands, scalar through `rsp` | 14.5% | 2.2% | 2.5% |
| memory operands, scalar RIP-relative | under 2.1% | 5.4% | 7.5% |

Both titles keep frame pointers and address their locals through `rbp`, so the TSO exemption for `rsp` covers little of their stack traffic.

### The GB10's alternate floating-point mode

The runtime sets `FPCR.AH` on hosts with FEAT_AFP ([Runtime.cpp](../runtime/src/Runtime.cpp)), which the GB10 and the Apple M4 have. A test on the GB10 compared the hardware's results, with the bit off and on, with x86's rules as [the design](DESIGN.md#floating-point) states them:

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

`fma(a, b, c)` is `a * b + c`, x86's `vfmadd` and aarch64's `fmadd` with `a` and `b` as the multiplicands. With the bit on, all fifteen cases match x86; with it off, eleven differ.

## Small changes

1. **Find indirect-call targets with a hash table.** Every indirect call, and every indirect jump without a jump table, goes through `LookupFunction`, which does a `std::lower_bound` over every function of the program. For a title with 645,447 functions that is about 20 dependent loads over a 10 MB table on each virtual call, and the deeper loads likely miss the cache. A hash table built at start-up from the sorted table finds a target in one or two probes. Only the runtime changes, and nothing about correctness does.

2. **Do not order frame-pointer accesses.** Under `--memory tso` an access through `rsp` is plain on the assumption that stack memory belongs to its thread. Accesses through `rbp` in a function that sets `rbp` from `rsp` in its prologue and writes it nowhere else reach the same memory and need the same treatment. In both titles that means a quarter to a fifth of all memory operands, which are now acquire loads or release stores with an alignment test each. Discovery already finds the instructions that set a register (`ReachingDefinitions` in [Discovery.cpp](../lifter/src/Discovery.cpp)), which is what the check needs.

3. **Do not order loads from read-only segments.** Nothing can race memory that nothing writes. A RIP-relative load whose address lies in a segment without write permission can be a plain load; the address is known when the program is lifted, so `NeedsOrdering` can decide. Scalar RIP-relative operands are 5–7% of the titles' memory operands. Some of them are float constants, which today take an `ldapr` into a general register and a move to the vector register.

4. **Let the hardware follow x86's NaN rules.** `NanResult` and the min/max lowering emit 8 to 12 instructions per floating-point operation to reproduce x86's results. With `FPCR.AH` set, the hardware produces the same results itself. The handling cannot simply go, because LLVM treats NaN payloads as unspecified: it may swap the operands of `fadd`, `fmul` or the multiplicands of `fma`, and fold `x * 1.0` to `x`, which leaves a signalling NaN unquieted. It can shrink to what operand order affects, a NaN first operand, quieted: about three instructions. Min and max could become the target's `fmax` and `fmin`, as long as LLVM does not commute those intrinsics; their DAZ handling, which loads MXCSR from the state for every operand, could go too if `FPCR.FIZ`, which the runtime sets for DAZ, flushes their inputs, which the test above did not cover. Hosts without FEAT_AFP keep the current lowering, or the runtime requires the feature as it requires AES and PMULL. `tests/fixtures/float.c`, which feeds quiet and signalling NaNs and compares the results with an Intel CPU's, checks the change.

## Medium changes

- **Catch unaligned ordered accesses in a `SIGBUS` handler.** An ordered access now tests its alignment and branches to a plain access with a fence. The GB10 (FEAT_LSE2) faults only on ordered accesses that cross a 16-byte boundary, which compiled code rarely makes. A handler that performs the faulting `ldapr` or `stlr` as a plain access with a fence, then resumes after it, would remove the test: an `and` and a branch per access where LLVM cannot hoist the `and` as it did in the summing loop, and the second path through the loop body, which is likely what keeps LLVM from unrolling that loop.
- **Pass fewer registers across calls.** A call stores every register the function uses and reloads all of them after it. The SysV ABI does not preserve vector registers across calls, so `ymm8`–`ymm15` need not be stored, and only `ymm0` and `ymm1` hold results to reload; likewise the caller-saved general registers other than `rax` and `rdx` need no reload. Callee-saved registers must still be stored, because the unwinder restores landing pads from the values their callees pushed. Hand-written assembly with its own conventions breaks this, as it would break the flags rule, and FFmpeg has some, so it needs a switch like `--preserve-flags`.

## Larger changes

These are on the [roadmap](DESIGN.md#roadmap): ordering only memory another thread can reach, recovering prototypes so hot functions skip the state, and inlining across parts, which the probe's direct-call loop shows pays off.

## First step

Profile before changing anything: `perf` on EVO Player, and on a title once one runs far enough. The instruction-mix numbers are static, and only a profile shows how much time dispatch, ordering and NaN handling take.
