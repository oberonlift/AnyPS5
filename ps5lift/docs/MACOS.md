# macOS

A plan for running lifted programs natively on Apple Silicon Macs. Phase 1 is done; the rest is not built yet. The facts below were checked on 2026-10-29 on an M4 running macOS 27.2 with Xcode's toolchain and Homebrew's LLVM.

## Goal

The Mac becomes a second host next to the GB10: `ps5lift` builds and runs on it, lifts for `arm64-apple-macosx`, and the result runs natively, in this order:

1. the twelve fixtures that need nothing from AnyPS5 (`basic`, `vector`, `float`, `threads`, `callbacks`, `x87`, `setjmp`, `cpu`, `mmx`, `modules`, `strings`, `sse41`);
2. AnyPS5's PS5-style `import` and `tls` fixtures;
3. the four that call AnyPS5's libraries through generated bridges (`libc`, `exceptions`, `kernel`, `fiber`), and upstream's libc tests;
4. EVO Player, to its home screen.

Linux stays the primary host, and every step keeps its tests passing there.

Not goals: Intel Macs; running guest code under Rosetta, which AnyPS5's own macOS build does and which the lifter replaces; Apple's hardware TSO mode, which macOS keeps for Rosetta, so `--memory tso` keeps lowering to acquire and release accesses; iOS.

## What exists

- **The CPU.** The M4 has every extension the runtime requires or uses: AES, PMULL, CRC32, SHA-256 and SHA-512, LSE and LSE2, LRCPC and LRCPC2, so TSO loads become `LDAPR` and `LDAPUR` as on the GB10, and FEAT_AFP, which exact DAZ and FTZ need. It has no SVE, so the `neoverse-v2` default CPU is wrong for it. Pages are 16 KiB, the PS5's own page size. A process can map up to `0x7ffffe000000`, 32 MiB short of 128 TiB. AnyPS5 bounds guest addresses at `0x7fff00000000` in libkernel's direct memory, which fits, and at `0x800000000000` in its POSIX memory calls, which does not quite. Below 448 GiB a process keeps its `__PAGEZERO` over the low 4 GiB, its executable just above, the shared region from 6 to 63 GiB (`mach/shared_region.h`) and a GPU carve-out from 64 to 448 GiB (`mach/arm/vm_param.h`).
- **ps5lift** builds against Ubuntu's LLVM 18 and, since phase 1, Homebrew's LLVM 23. Its output depends on the platform in two places only: it writes a GNU-format archive, and `main.cpp` picks the default CPU from the triple. Hidden visibility, the only symbol attribute it sets, means the same in Mach-O. AnyPS5's ELF reader, its other dependency, is part of AnyPS5's relinker, which already builds on Apple Silicon.
- **The runtime** makes four Linux calls: `mmap` with `MAP_FIXED_NOREPLACE` for the guest images, `getauxval` for the CPU features it checks at start, `sched_getcpu` for `rdpid` and `getrandom` for `rdrand`. FPCR, `cntvct_el0` and the `dc` cache maintenance instructions are AArch64, not Linux; whether macOS lets user code run `dc civac` is unchecked, and `sys_dcache_flush` is its documented interface.
- **Linking.** `build_runtime.py` and the fixture runners link with ELF `lld` options (`--undefined-glob`, `--export-dynamic-symbol`, `-rpath $ORIGIN`) so that AnyPS5's libraries, which are shared objects, bind to the runtime's `GuestCpu*_nid_no_patch` functions in the executable.
- **The guest fixtures** are freestanding x86-64 Linux executables (`-nostdlib`, linked by `ld.lld`), so any clang with `lld` builds all but three of them on a Mac; Homebrew packages `lld` apart from `llvm`. `threads` and `callbacks` link host libraries that use pthreads, and `modules` links libc, so those three need an x86-64 Linux sysroot. They are position-independent code linked at 1008 GiB, where the relinker places PS5 executables too (see below).
- **AnyPS5** has had a full macOS build since upstream commit `8ca4c1009`, for x86-64 under Rosetta: 291 `__APPLE__` paths, most in libc, libkernel and libSceAgcDriver (Mach VM calls, dispatch semaphores, `@loader_path` rpaths, DWARF unwind tables instead of compact unwind), MoltenVK handling in libSceAgcDriver, and FFmpeg prebuilts for macOS that include arm64. Its CMake stops on any other macOS architecture. Its lifted mode (`ANYPS5_GUEST_LIFTED`, 49 sites) was written for Linux on aarch64. `GuestLongDouble` requires a host `long double` that holds every x87 value, and on macOS arm64 `long double` is `double`, so the lifted libc does not compile there. 13 of libc's files use `long double`, and its math functions use their own polynomial approximations rather than the host's `sinl` and the like.

## Decisions

### Where guest memory goes

Guest memory is identity mapped ([the design](DESIGN.md#machine-model)), and every guest image sits above 4 GiB, on every host. PS5 titles are the only goal, and PS5 executables are position-independent (`ET_SCE_DYNEXEC`, loaded at virtual address 0), so the relinker places each at 1008 GiB with `--linux-fixed-base 0xFC00000000`; the fixtures are position-independent code linked at the same base, and the lifter places modules after the executable as before. 1008 GiB is where the PS5 application area ends, so it is clear of the ranges a title reserves inside it, and it lies above everything a macOS process keeps below 448 GiB, so whether macOS lets an arm64 program shrink its `__PAGEZERO` no longer matters. Code that must sit below 4 GiB, such as code with 32-bit absolute addresses, is not PS5 code; the one fixture case that needed it is gone. A title that keeps pointers in 32 bits would break on every host; none is known.

### Target and CPU

Lift for `arm64-apple-macosx`. The default CPU follows the triple: `apple-m1` for Apple triples, the baseline every Apple Silicon Mac runs, and `neoverse-v2` for Linux as now; tests keep using `--cpu native`. The archive format follows the triple too (Darwin's for Apple). The runtime already checks FEAT_AFP at start and falls back to flushing both ways without it, which is what an M1 gets.

### How AnyPS5's libraries reach the runtime

macOS binds a library's undefined symbols to a named library (the two-level namespace), so the Linux arrangement, where the executable exports `GuestCpu*` and the libraries find them at load, needs `-undefined dynamic_lookup` on every AnyPS5 library, which defers every missing symbol to run time. Instead, build the runtime as a dynamic library that AnyPS5's libraries link against, on both hosts, and have `main` hand it the lifted program's table rather than the runtime naming `ps5lift_program` at link time. A missing interface function is then a link error on both. This replaces `--undefined-glob` and `--export-dynamic-symbol` in `build_runtime.py`.

### Guest long double

Replace the host `long double` in the lifted libc with a class over SoftFloat's `extFloat80_t`, which the runtime already links for x87, on every host whose `long double` is not x87's: macOS and Linux on aarch64. One implementation serves both, and it removes the differences [known gaps](DEBT.md) lists for today's binary128 type, since every step then rounds as the console's x87 does. Constants written as `long double` literals become exact 80-bit values, because on macOS such a literal is a `double`. This is the largest change to AnyPS5 in the port: `strtold`, `printf`'s `%Lf` and the `l` math functions all compute through it.

### Toolchain

Build AnyPS5, the runtime, the bridges and the lifted program with Xcode's clang and the system libc++, as AnyPS5's macOS build does, so that only one C++ runtime is in the process. Link the lifted program with Xcode's linker, which signs it ad hoc and inserts branch islands when the text outgrows a branch's 128 MiB reach, which EVO Player's may. `ps5lift` links Homebrew's shared `libLLVM`; the guest fixtures build with the configured C compiler and Homebrew's `lld`, against an x86-64 Linux sysroot; the bridge generator uses Homebrew's libclang and its Python bindings, so its default `--libclang` path comes from the build rather than a Linux path.

## Work, in order

Each phase ends with something that runs, and Linux's ctest passes after every change.

### 0. Answers before code

- Writing FPCR's AH, FIZ and FZ bits from user code on macOS, and reading `cntvct_el0` and `cntfrq_el0`, whose frequency differs from the GB10's (Apple's chips have counted at 24 MHz); check how the runtime scales `rdtsc` to it.
- AnyPS5's `0x800000000000` bound, which macOS cannot map up to.
- Whether `dc civac` and `dc cvac` run at EL0 on macOS, or `sys_dcache_flush` is needed.
- Whether Metal uses the GPU carve-out in a process that has reserved the PS5 user range.

### 1. ps5lift on the Mac (done)

Done on 2026-10-29. `ps5lift` builds against Homebrew's LLVM 23, and ctest on the Mac lifts every fixture for aarch64 and x86-64 Linux without running it (24 tests, under a minute). EVO Player lifts for `aarch64-unknown-linux-gnu` in 7 minutes 14 seconds with a 13.4 GB peak memory footprint, where the GB10 takes 2 minutes 24 seconds, and reports the same functions, instructions, jump tables and unsupported instructions as the GB10's last lift. It took three LLVM API changes between 18 and 23, one of them `getTerminator()`, which no longer says whether a block has a terminator and made every lifted module invalid; linking the shared `libLLVM`, since LLVM's static libraries name the z3 installed on the day they were built, which a Homebrew upgrade removes; `+crc32` for x86-64 output; and the x86-64 Linux sysroot above.

### 2. Mach-O output and the runtime

- `ps5lift`: the triple's default CPU and archive format.
- The runtime: replace `MAP_FIXED_NOREPLACE` with a fixed `mach_vm_allocate`, which fails rather than replacing a mapping; `getauxval` with `sysctlbyname("hw.optional.arm.FEAT_*")`; `getrandom` with `getentropy`; `sched_getcpu` with `pthread_cpu_number_np`. Build it as the dynamic library described above.
- `build_runtime.py`, `run_fixture.py` and `tests/CMakeLists.txt`: Apple triples, Xcode's linker and no qemu path.

Done when the twelve fixtures that need no AnyPS5 pass on the Mac, at `-O0` to `-O3`, with `--memory tso` for `threads`.

### 3. The relinker's fixtures

Build AnyPS5's relinker on the Mac (its relinker-only build already supports Apple Silicon) and run the `import` and `tls` fixtures, which the tests relink at 1008 GiB. Done when both pass.

### 4. AnyPS5's libraries for a lifted guest on macOS

- AnyPS5's CMake accepts arm64 on macOS when `ANYPS5_GUEST_LIFTED` is set, and keeps requiring x86-64 for the Rosetta build.
- Go through the 291 `__APPLE__` paths for what assumes x86-64 or guest code running natively under Rosetta (thread state, unwinding, fiber switching, signal contexts), and the 49 lifted-mode paths for what assumes Linux. Where they overlap, lifted mode's behaviour wins and the macOS system call underneath it stays: `sceKernelRaiseException`, guest threads and their stacks, libSceFiber's stack switch (whose assembly needs Darwin's symbol names and directives), libc's unwinder, which finds guest frames through `GuestCpuIterateImages`, `setjmp`, and the float and atomic builtins.
- The guest `long double` type above.
- AnyPS5's guest arena, which on macOS reserves from 8 GiB up to 1008 GiB and starts its allocations at 8 GiB: on arm64 that is inside the shared region and across the GPU carve-out, so the arena has to start above 448 GiB there or leave those ranges out.
- The bridge generator on the Mac, against AnyPS5's macOS compile commands.

Done when `libc`, `exceptions`, `kernel` and `fiber` pass and `run_libc_tests.py` passes the same seven upstream tests as on Linux.

### 5. EVO Player

Build the rest of the libraries it imports (libSceAgcDriver and its shader recompiler on MoltenVK, libSceVideoOut, audio, pad, system service, networking), relink it at 1008 GiB, lift it, and run it in a Cocoa window rather than under Xvfb. Done when it reaches its home screen. A screen that matches the GB10's, with the same colour fringes, puts those fringes in the shared code, not in either platform's renderer.

### 6. Both hosts, documented

The README's build section for macOS (Xcode, Homebrew `llvm` and `lld`, MoltenVK), a platform section in [the design](DESIGN.md) covering the guest layout, the runtime library and the long double type, and the macOS entries in [known gaps](DEBT.md).

## Risks

- **Low addresses.** A PS5 title that relies on its executable sitting below 4 GiB, if any, now breaks on every host, not only on macOS.
- **Overlap of Rosetta and lifted code in AnyPS5.** The two were written apart; the overlap is where most of phase 4's time goes, and it is not sized beyond the counts above.
- **MoltenVK.** It ran AnyPS5's renderer under Rosetta, and the GPU side does not change with the CPU, but nothing has run on it from an arm64 process yet.
- **Precision of the new long double.** Exact x87 rounding is the point, but it changes the lifted libc's results on Linux too; the libc fixtures' expected output has to be checked against a native x86-64 run, not just updated.
