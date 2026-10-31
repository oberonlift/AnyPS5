# Optimizations

Performance work for agents to pick up. Take the work items in order: each says where the code is, what has to stay true, and how to check it. After an item lands, re-measure, update the baseline in this file and move the item to [Done](#done).

Rules for every item:

- Correctness comes first. `ctest` passes with both memory models and every fixture, and EVO Player still reaches its home screen.
- Lifted code runs only on Linux, so timings come from the GB10. The Mac can lift and read the generated code.
- Lifting a title takes about 11 minutes and 13 GB on the GB10, so iterate on EVO Player or the probe below, and lift a title once for the final number.

## Baseline

The probe was lifted on 2026-10-30, at d758281d61. The benchmark was measured on 2026-10-31, after the dispatch lookup was hashed.

### Benchmark

`tools/bench.py` on the GB10, scale 16, best of five runs. `tso/native` compares with the native build without vectorization.

| Kernel | native | native, no vectors | lifted, relaxed | lifted, TSO | tso/relaxed | tso/native |
|---|---|---|---|---|---|---|
| stream | 18.3 ms | 36.7 ms | 25.4 ms | 37.8 ms | 1.49× | 1.03× |
| chase | 52.1 ms | 52.4 ms | 51.7 ms | 55.9 ms | 1.08× | 1.07× |
| copy | 3.9 ms | 4.6 ms | 4.1 ms | 6.2 ms | 1.52× | 1.34× |
| hash | 10.5 ms | 8.4 ms | 10.2 ms | 11.2 ms | 1.11× | 1.34× |
| sort | 7.6 ms | 7.6 ms | 8.5 ms | 21.7 ms | 2.55× | 2.88× |
| dispatch | 18.2 ms | 16.6 ms | 35.7 ms | 39.1 ms | 1.10× | 2.35× |

Without ordering, lifted code other than `dispatch` is within about 20% of native. Ordering costs little where loads wait on each other (`chase`) and the most where stores dominate (`sort`). `dispatch` makes 1,048,576 indirect calls through a table of 65,536 functions: one takes 34 ns lifted and 16 ns native, and besides the lookup the lifted call stores and reloads the state around `ps5lift_dispatch` (see the probe below). The kernels keep no frame pointers and use no floating point. Vector loads and stores wider than 64 bits are not ordered yet ([known gaps](DEBT.md)), so the TSO column understates what full TSO costs; ordering them will add to it.

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

Both titles keep frame pointers and address their locals through `rbp`, so the TSO exemption for `rsp` covered little of their stack traffic until private frames took in `rbp` ([Done](#done)).

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

None is open. The next candidates are under [Later](#later) and [From AnyPS5's ARM64 compiler](#from-anyps5s-arm64-compiler).

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
- **The [roadmap](DESIGN.md#roadmap)'s items:**
  - order only memory that another thread can reach;
  - recover prototypes so hot functions skip the state;
  - inline across parts. The probe's direct-call loop shows what inlining gains.
- **Leave a local's address plain while it stays in the function.**
  - A function that puts the address of a local in a register, as `lea -0x40(%rbp),%rax` does, orders its whole frame today.
  - Its accesses could stay plain where dataflow proves the address never leaves the function: it is not stored to memory, passed to a call or returned, and not copied to a register that is.
  - A mistake in that proof reopens the hole that private frames closed.

## From AnyPS5's ARM64 compiler

The draft pull request [boykopovar/AnyPS5#5021](https://github.com/boykopovar/AnyPS5/pull/5021) compiles PS5 x86-64 code to ARM64 ahead of time, as `ps5lift` does, but only for programs that come with a reviewed contract of their own. These are its optimizations that could carry over, read from its code at commit `151294f`; its [known gaps](https://github.com/yabanana/AnyPS5/blob/151294f979ab00aaff2f702314425caf61a72790/docs/dev/debt/aot.md) describe each one in full. None has been measured here, so none is a work item yet.

### Give leaf functions a native calling convention

- **Theirs:** for a leaf function (no calls, no indirect jumps, no runtime helpers, no vector registers, and no stack changes beyond an exact `push %rbp; mov %rsp,%rbp` … `pop %rbp`) that reads at most six general registers and writes at most two, they compile a native core that takes the register values as arguments and returns its results. The guest still sees its stack written as on the console: the return address and the saved `rbp`. The state-based entry stays for callers through dispatch.
- **For us:** a direct call inside a part is already inlined when the callee is small (see the probe), so the gain is on calls between parts. A part holds about 50,000 instructions and a title over 600,000 functions, so most calls cross parts. This is a narrow first step toward the roadmap's "recover prototypes so hot functions skip the state", and an alternative to inlining across parts.
- **Change:** emit the core as a hidden function `guest_<address>_core` that takes and returns `i64` values, and make `guest_<address>` a wrapper that loads the arguments from the state, calls the core and stores the results. A direct caller in another part calls the core with its cached registers.
- **What must stay true:** the return address and saved `rbp` still go into guest memory, since unwinders and stack walkers read them. The input set must be exact: a register the leaf reads but the core does not receive is a wrong value, not a missing one.
- **Measure first:** how many direct calls between parts in EVO Player and a title reach a leaf that qualifies.

### Run unordered until the process shares memory

- **Theirs:** with "memory epochs", every function is compiled twice, unordered and ordered. The program starts in the unordered version and switches for good when its thread crosses a boundary that publishes guest memory: creating a thread, a shared or file-backed mapping, registering a signal handler, or calling a library function not known to keep guest memory private. A frame suspended in the unordered version continues in the ordered version when its call returns.
- **For us:**
  - While no other thread exists, nothing can observe the order of a thread's accesses, so unordered code is correct. This is not the per-title switch [ruled out](#ruled-out) below: it weakens nothing another thread could see.
  - AnyPS5's own threads read guest memory too (AGC command buffers, audio, VideoOut), so every import that may start or wake one must publish. Treat every import as publishing except a list known to touch only the calling thread.
  - The switch can reuse the resume points. `CallGuest` already loads `Unwinding` after every guest call; a "shared" flag beside it lets one load test both. The ordered twin then needs an entry for each return address of the unordered one, as for a landing pad. The switch happens only on a guest thread inside a call, never from another thread in the middle of a function.
- **Cost:** lifted code, lift time and lift memory double, and a title already takes 11 minutes and 13 GB.
- **Measure first:** how much of a run happens before the first publishing call. PS5 titles start threads early, so the gain may be start-up alone. Count instructions or time until the first `scePthreadCreate` in EVO Player and in a title.

### Store and reload less around imports

- **Theirs:** eleven allocation, memory, string and pthread functions are called directly in the native ABI once a run-time check confirms the import resolves to the expected implementation, and only the registers the call writes are written back ([guest_provider_calls.py](https://github.com/yabanana/AnyPS5/blob/151294f979ab00aaff2f702314425caf61a72790/core/aot/guest_provider_calls.py)). `pthread_getspecific` results are cached while the key's generation is current, and a completed `pthread_once` costs one acquire load.
- **For us:**
  - The bridges already call AnyPS5 directly. The cost is the full `Flush` and `Reload` around every import.
  - Unlike guest code, a bridge follows the SysV ABI by construction: it reads the argument registers it takes and the stack, and writes only its result registers. `generate_bridges.py` knows both sets from the prototype, so it can emit them per NID for `ps5lift` to read. Before the call, store only the arguments; after it, reload only the results and `rsp`.
  - Keep the full flush for imports that unwind, jump or raise (`__cxa_throw`, the `GUEST_STATE_EXPORTS` in `generate_bridges.py`, `sceKernelRaiseException`), for bridges that take a function pointer, whose callbacks run guest code on the same state, and for implementations that read the guest state through `GuestCpu*`.
  - Lowering `memcpy`, `memset` and `strlen` imports to LLVM intrinsics would let LLVM optimize around them. Titles often bring their own libc as a guest module, as EVO Player does, and there these are guest functions rather than imports. Count how often each import runs before choosing any.
- **Check:** `ctest`, in particular `libc`, `exceptions`, `callbacks` and `setjmp`.

### Profile-guided optimization and LTO

- **Theirs:** their builder can instrument a build, run it, merge the profiles with `llvm-profdata merge` and rebuild with them, and can link with LTO. A profile build records the profile's hash and checks its inputs for changes.
- **For us:**
  - `Codegen.cpp` runs `buildPerModuleDefaultPipeline(O2)` on each part. `llvm::PGOOptions` on the `PassBuilder` adds instrumentation or profile use, and the lifted program links compiler-rt's profile runtime to write the profile at exit. Function names (`guest_<address>`) stay the same between lifts of the same input, so a profile carries over; key it by the input's hash.
  - The larger win is likely a variant of our own: count the targets of each `ps5lift_dispatch` site in an instrumented run, and lift a hot site as `target == hot ? guest_hot(state) : dispatch`. LLVM's indirect-call promotion cannot do this, since the target travels in the state rather than as a function pointer. The titles have 190,000–300,000 indirect call sites (see the instruction mix).
  - ThinLTO over the parts is one way to inline across parts, the roadmap item.
- **Caveat:** a profile covers only what the training run reached, EVO Player's or a title's screens today; everything else gets no profile.

## Done

### Hash the dispatch lookup

Landed on 2026-10-31. `Ps5Lift::LookupFunction` in [Runtime.cpp](../runtime/src/Runtime.cpp) used to binary-search the sorted function table the lifter emits, about 20 dependent loads over a title's 8 MB table on every indirect call. It now probes an open-addressing table keyed by guest address, with Fibonacci hashing and linear probing, a power of two at least twice the function count. `ps5lift_use_program` builds it from the sorted table before any guest thread starts, and threads only read it.

- The `dispatch` kernel went from 92.3 ms to 35.7 ms relaxed and from 96.2 ms to 39.1 ms under TSO, from 88 ns a call to 34 ns.
- For 507,412 functions, a large title's count, the build takes 7 ms and the table 16 MB at start-up, so building it at lift time instead would not pay for the larger executable.
- A perfect hash would shrink the table to about 8 MB but not speed it up: at most half full, the table finds most functions in one cache line, where a perfect hash reads a displacement table first, and it still has to compare the stored address to reject one that starts no function.

### Leave a frame unordered only when its address does not escape

Landed on 2026-10-31. Accesses through `rsp` used to be plain in every function. A function that handed out the address of a local, such as a completion flag other threads set, then read it unordered, and LLVM could hoist the load out of a spin loop on it. `ScanInstructions` in [Lifter.cpp](../lifter/src/Lifter.cpp) now decides per function whether its frame is private (`HandsOutFrameAddress`), and whether it keeps a frame pointer (`KeepsFramePointer`). Only a private frame's accesses through `rsp`, and through `rbp` where the function keeps a frame pointer, are plain, so the titles' accesses through `rbp`, 19–24% of their memory operands, are plain wherever the frame is private. [DESIGN.md](DESIGN.md)'s memory-ordering section states the rule, and the stack message case in `tests/fixtures/threads.c` covers the hole.

### Leave loads from read-only segments unordered

Landed on 2026-10-31. An access through `rip` with no index or segment override has a known address. When the whole access lies in a segment without `PF_W` (`InReadOnlySegment` in [Lifter.cpp](../lifter/src/Lifter.cpp)), no other thread writes it, so the load is plain. Many of these are float constants loaded with `movss` or `movsd`, which took an `ldapr` into a general register and a move to the vector unit. The runtime keeps the assumption checked: AnyPS5's `DoMprotect` calls `GuestCpuMakingWritable` in [GuestCpu.cpp](../runtime/src/GuestCpu.cpp) for every range the guest makes writable, which stops the program if the range overlaps such a segment.

### Stop at a closed jump table's fallback

Landed on 2026-10-31. A recovered table lifts as a `switch` on the target, which fell back to dispatch: storing every register and resolving the target at run time. Discovery now marks a table closed when a recognised guard bounds it, it lies in a segment without `PF_W` and no relocation writes it (`RecoveredTable::Closed` in [Discovery.cpp](../lifter/src/Discovery.cpp)); its targets are then all the jump can take. A closed table's fallback calls `ps5lift_left_jump_table`, which stops the program, so the `switch` keeps no path that stores the state. AnyPS5's ARM64 compiler ([above](#from-anyps5s-arm64-compiler)) skips re-validating targets under the same three conditions. The lift report counts closed tables.

### Store and reload only what a callee names and writes

Landed on 2026-10-31, after AnyPS5's ARM64 compiler ([above](#from-anyps5s-arm64-compiler)), whose [guest_value_leaves.py](https://github.com/yabanana/AnyPS5/blob/151294f979ab00aaff2f702314425caf61a72790/core/aot/guest_value_leaves.py) reloads only a callee's writes after a direct call. `CallGuest` in [Lifter.cpp](../lifter/src/Lifter.cpp) used to store every register the caller names before every guest call and reload them all after it. Discovery now gives each function its `RegisterEffects` ([Discovery.cpp](../lifter/src/Discovery.cpp), `CloseEffects`), closed over direct calls and jumps to a fixpoint, and a direct call stores only the registers the callee names and reloads only those it may write.

- The store set is the registers the callee names, not only those it reads: a callee loads every register it caches at entry and stores it back at its return, so it carries a value through even on paths that do not change it.
- Dispatch, imports, interrupts, `fxsave` and `fxrstor` name and write everything, so wherever the runtime or the unwinder reads the state, the registers there are current. A callee that pushes a callee-saved register names it, so the values the unwinder restores for a landing pad are still stored.
- The lifter checks that the registers a function caches are within its effects, since its callers store only those.
- Handlers that `sceKernelRaiseException` delivers read the registers as of the last call, and registers that callers did not store are older still.

## Ruled out

Each of these was considered for the ordering cost and rejected. Do not pick them up without new evidence.

- **Reusing a loaded or stored value across another load.**
  - Forwarding a store to a later load of the same address, or reusing a load for a repeated one, is safe only when no other load sits between them, and never across loop iterations, where it would hoist a spin loop's load as the old `rsp` rule let LLVM do ([Done](#done)).
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
