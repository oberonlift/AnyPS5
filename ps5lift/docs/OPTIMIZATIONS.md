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

Without ordering, lifted code is within about 20% of native. Ordering costs little where loads wait on each other (`chase`) and the most where stores dominate (`sort`). The kernels have no indirect calls, no frame pointers and no floating point, so items 1 and 2 below need kernels of their own before they can be timed. Vector loads and stores wider than 64 bits are not ordered yet ([known gaps](DEBT.md)), so the TSO column understates what full TSO costs; ordering them will add to it.

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

The runtime set `FPCR.AH` on hosts with FEAT_AFP until it turned out that the bit also stops `FNEG` and `FABS` from changing a NaN, which breaks the constants LLVM 23 builds with `FNEG` ([floating point](DESIGN.md#floating-point)); it leaves the bit clear now, so letting the hardware follow x86's NaN rules is not an option. The GB10 has the feature on all 20 cores, and so does the Apple M4. A test on the GB10 ran each operation with the bit off and on and compared the results with x86's rules as [the design](DESIGN.md#floating-point) states them:

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

### 2. Leave a frame unordered only when its address does not escape

This item fixes a correctness hole in today's rule, then extends the rule to `rbp`.

- **The hole:** `NeedsOrdering` in [Lifter.cpp](../lifter/src/Lifter.cpp) makes an access plain whenever `operand.mem.base == ZYDIS_REGISTER_RSP`, and `Push`, `Pop` and `CallGuest` pass `ordered = false`. That assumes a frame is private to its thread, which is false once a function hands out the address of a local. A job counter or completion flag on the waiting thread's stack is the usual case: workers update it through a pointer, and the owner reads it through `rsp`. x86 compiles `std::atomic` loads and release stores to a plain `mov`, so the owner's accesses lift unordered:
  - its read of the flag is not ordered before its reads of what the workers wrote, so it can read their results before the flag says they are done;
  - `pause` lifts to nothing, so a spin loop on a plain load contains nothing that writes memory, and LLVM may hoist the load out of the loop and spin forever.
- **Change:** decide once per function whether its frame is private, and leave its frame accesses plain only when it is: through `rsp`, and through `rbp` when the function keeps a frame pointer. A function whose frame escapes keeps every `rsp` and `rbp` access ordered.
  - A frame escapes when an instruction copies an address derived from `rsp` or `rbp` into memory or into a register other than `rsp` and the frame pointer. Examples are `lea 0x10(%rsp),%rdi`, `mov %rsp,%rax` and `push %rsp`. Stack adjustments (`sub`, `add` and `and` on `rsp`), `lea -0x10(%rbp),%rsp`, the prologue's `mov %rsp,%rbp`, and `leave` do not make it escape.
  - Variadic functions take the address of their stack arguments with `lea` to build a `va_list`, so they escape, which is correct.
  - `push`, `pop`, `call` and `ret` stay plain in every function. They touch saved registers and return addresses, which no pointer a function hands out may reach.
  - A function keeps a frame pointer when its entry block starts with `push %rbp; mov %rsp,%rbp` and nothing else writes `rbp` except the restoring `pop %rbp` or `leave`. A function that uses `rbp` as a general register keeps its `rbp` accesses ordered.
  - Compute both facts in one pass over the function. `FindUsedRegisters` already walks every instruction, and discovery has `ReachingDefinitions`.
- **Gain:** escaping functions' `rsp` accesses become ordered, which costs a little. In private frames the `rbp` accesses, 19–24% of the titles' memory operands, become plain.
- **What must stay true:** a frame whose address no instruction hands out is private to its thread. Replace the `rsp` sentence in DESIGN.md's memory-ordering section with this rule.
- **Test first:** add a case to `tests/fixtures/threads.c`.
  - A function passes the addresses of a local flag and a local data word to a new thread. That thread writes the data and then sets the flag with `__atomic_store_n(..., __ATOMIC_RELEASE)`.
  - The function spins on the flag with `__atomic_load_n(..., __ATOMIC_ACQUIRE)` and then prints the data.
  - Check with objdump that the guest addresses both locals through `rsp` or `rbp`.
  - With today's rule, -O2 is expected to hang or print stale data. Confirm that the test fails before the change and passes after it.
- **Later:** a register that holds the address of a local, as after `lea -0x40(%rbp),%rax`, can go plain only where dataflow proves the address never leaves the function: it is not stored to memory, passed to a call or returned, and not copied to a register that is. A mistake in that proof reopens the hole above.
- **Check:**
  - `ctest`, including the new case.
  - The ordered-instruction count of EVO Player's or a title's lifted code should drop by about the `rbp` share in the table above, less the frames that escape.
  - For a timing, build the benchmark guest with `-fno-omit-frame-pointer` as well.

### 3. Leave loads from read-only segments unordered

- **Code:** `NeedsOrdering` in Lifter.cpp.
- **Change:** an access through `rip` without an index or segment override has a known address, `_instruction->Next() + disp`. If the whole access lies in a segment without `PF_W` (`GuestSegment::Flags` in [GuestImage.hpp](../lifter/include/lifter/GuestImage.hpp)), no other thread can write it, so the load can be plain.
- **Gain:** many of these are float constants loaded with `movss` or `movsd`. Today they take an `ldapr` into a general register and a move to the vector unit.
- **What must stay true:** the values in those segments never change, so reordering loads of them cannot be observed. That holds while the guest neither makes its read-only segments writable nor maps their pages a second time. Write that assumption down where the rule goes.
- **Make it checked:** in lifted builds, AnyPS5's `sceKernelMprotect` and POSIX `mprotect` ([DirectMemory](../3rdparty/anyps5/core/libs/prx/libkernel/DirectMemory/)) should stop with an error when asked to make any part of a guest image's read-only segment writable. The images are found through `GuestCpuIterateImages`. A title that breaks the assumption then stops at that call instead of running with unordered loads.
- **Check:** `ctest`, and the ordered-instruction count.

## Later

- **Handle unaligned ordered accesses after they fault.**
  - Today `Load`, `Store` and `SplitOnAlignment` in Lifter.cpp test alignment before every ordered access.
  - With FEAT_LSE2, an ordered access faults only when it crosses a 16-byte boundary, which compiled code rarely does. The kernel controls that tolerance (`SCTLR_EL1.nAA`). Before relying on it, confirm it on Linux and macOS with an unaligned `ldapr` inside a 16-byte block and one across a boundary.
  - A `SIGBUS` handler beside the `SIGSEGV` one in NativeCall.cpp would decode `ldapr`, `ldapur`, `ldar`, `stlr` and `stlur` (2, 4 and 8 bytes), do the access plainly with a `dmb`, write the destination register and step past the instruction.
  - That removes an `and` and a branch per access where LLVM cannot hoist the `and`. It also removes the second path through loop bodies, which is likely what stops LLVM from unrolling ordered loops.
  - macOS reports alignment faults as `SIGBUS` too.
  - The memory model does not change, but the handler carries its own risks:
    - It has to reproduce each form exactly (width, sign and zero extension, `wzr`/`xzr`, the signed offsets of `ldapur` and `stlur`) inside a signal handler.
    - An access that also faults for another reason, such as one into an unmapped page, must still reach the `SIGSEGV` path and guest exception delivery as it does today.
  - Each crossing access then costs a signal instead of a branch, so code that crosses often, such as a loop over packed structures, becomes far slower. Count the faults in a run of EVO Player and of a title before making this the default.
- **Merge adjacent ordered accesses.**
  - Two ordered accesses of the same kind to adjacent addresses with no instruction between them, such as `mov %eax,(%rdi); mov %edx,0x4(%rdi)`, can become one access of twice the width:
    - an observer of a merged store sees both stores or neither, and TSO allows both outcomes;
    - two loads read at one instant are ordered as TSO requires.
  - Merge only when the merged access is naturally aligned. Test its alignment and, when the test fails, do the two original ordered accesses. The unaligned path of today's `Store`, a plain store after `DMB ISH`, is not enough for a merged store, since its halves could become visible in either order.
  - Nothing may sit between the two accesses: no other memory access, call or fence.
  - Another thread may read either half with a narrower access. That is the mixed-size part of the Arm memory model, so write the argument for it into DESIGN.md with the change.
  - Before implementing, count the adjacent pairs in a title's code the way the instruction-mix table was made.
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

## Ruled out

Each of these was considered for the ordering cost and rejected. Do not pick them up without new evidence.

- **Reusing a loaded or stored value across another load.**
  - Forwarding a store to a later load of the same address, or reusing a load for a repeated one, is safe only when no other load sits between them, and never across loop iterations, where it is the hoisting hang of item 2.
  - With a load between them, TSO can require the newer value. Suppose this thread stores to `a` and then loads `b`, and another thread set `b` after it saw this thread's store and overwrote `a`. Then the reload of `a` must see the other thread's value.
  - The safe case is narrow and the gain small.
- **Ordering switches per title or per function.** Other x86 translators for ARM let users weaken ordering for a program. A title that needs the ordering then fails rarely and unreproducibly, which the correctness rule at the top excludes.
- **Leaving thread-local storage unordered.** TLS is private only by convention. A thread can hand out a pointer to its own variable, just as it can for a local in its frame.
- **Apple's hardware TSO mode.**
  - Apple's cores switch to x86 ordering with an undocumented bit (`ACTLR_EL1.TSOEN`) that only the kernel writes, and macOS sets it only for Rosetta processes. The GB10's cores have no such mode.
  - Code lifted with `--memory relaxed` that runs on a thread with the bit set would be correctly ordered. The runtime starts every guest thread itself (`RunThread` in [Runtime.cpp](../runtime/src/Runtime.cpp)), so supporting the bit would be a small change if macOS ever exposed it.
  - Today the only ways to set it are a third-party kernel extension that needs System Integrity Protection lowered, a Linux virtual machine (macOS 14.4 and later) or Asahi Linux. None of these can be asked of a user.
  - The bit is undocumented, and Apple cuts Rosetta back from macOS 28, so it may not stay.
  - At most, use the kernel extension once on a developer's Mac to measure what ordering costs on the M4.

## Before starting

Profile, since the instruction mix is static. Run `perf record -g` on the lifted EVO Player under `xvfb-run` on the GB10, and on a title once one runs far enough. Check `LookupFunction`'s share of the time, then use `perf annotate` on the hottest lifted functions to see how much goes to `ldapr`/`stlr` and to NaN handling.
