# The AnyPS5 fork

ps5lift lives in [oberonlift/AnyPS5](https://github.com/oberonlift/AnyPS5), a fork of AnyPS5 whose `main` carries a patch set on top of upstream `main` (`https://github.com/boykopovar/AnyPS5`), and builds AnyPS5's libraries from the tree around it. This file is for agents who rebase that patch set, test it, or turn it into upstream pull requests. [Design](DESIGN.md) says how the lifted libraries work; this file treats the patches as patches.

Rules:

- Refer to a patch by its subject. Every rebase changes the hashes; the subjects are unique within the patch set.
- Another agent may be working in the same checkout. Check `git status` and `HEAD` before rebasing, and commit with explicit paths.
- Commits are authored as `oberonlift <oberonlift@gmail.com>`.

## Open decisions

Only the user can settle these. Ask before starting the work that depends on one.

1. **Whether [phase 2](#phase-2-the-lifted-target) goes upstream.** Unless ps5lift is public, upstream can only compile the lifted paths. Phase 1 does not depend on this.
2. **Whether to name titles upstream.** An `Unblocks:` line gives a title ID and a public Game bring-up discussion, posted from the account that opens the pull request. Without it, phase 1's title fixes go up with `Unblocks: none` and stand on their tests.
3. **Which x86-64 Linux machine** runs upstream's build and tests, and the titles phase 1 names, for the `Tested:` lines.

## The patch set

On 2026-10-31: 35 commits on upstream `8a4fc62` change files outside `ps5lift/`: the patch set's 33, and two that make room for ps5lift (its submodules under `3rdparty/`, and the convention check leaving `ps5lift/` to its own conventions). The merge that brought ps5lift in under `ps5lift/` and ps5lift's own commits sit among them on `main`. The macOS patches follow on the `macos` branch ([the macOS plan](MACOS.md)).

Fetch upstream by URL, then list the patch set, which is every commit since upstream outside `ps5lift/`:

```sh
git fetch https://github.com/boykopovar/AnyPS5 main:refs/remotes/upstream/main
git log --reverse --no-merges --format='%h %s' upstream/main..HEAD -- . ':!ps5lift'
```

The patches fall in four groups, listed one by one under [upstream pull requests](#upstream-pull-requests):

- **Building for a lifted guest on aarch64**, mostly behind `ANYPS5_GUEST_LIFTED`: the build switch and the libraries' link to ps5lift's runtime, the guest FPU state and timestamp counter through `GuestCpu`, x87 `long double` converted at libc's boundaries and then kept exactly in a class whose arithmetic runs on the runtime's x87, guest `va_list`s, unwinding, `dl_iterate_phdr`, fibers (which stop when resumed on another thread), and fixes for Clang.
- **libkernel for lifted programs**: guest thread stacks of the requested size, thread objects in guest memory, `dlopen` of lifted modules, and module information and module lists for lifted images, which the runtime's image list holds.
- **A network policy**: `APS5_NETWORK=0` takes the guest offline, and `APS5_NETWORK_ALLOW` and `APS5_NETWORK_DENY` limit the domains it may resolve. Networking stays on by default, as upstream has it.
- **Fixes found on titles**, none specific to lifting: HTTP/2 (the redirect callback, asynchronous send and read, HTTP/1.x lookups) for the EOS SDK in two Unreal titles, 16-bit access to converted images, `sceNpGetSandboxInfo`, a GPU snapshot that overlaps a writable range, and helper functions that NVIDIA's driver may inline.


## What the patch set costs an x86 build

Read from the diffs on 2026-10-30 and 2026-10-31, not measured. Most patches compile only under `ANYPS5_GUEST_LIFTED`, `__aarch64__` or a `long double` that is not x87. These compile into the x86 build:

| Patch | Effect on x86 |
|---|---|
| reach the guest FPU state through GuestCpu | The same x87 and SSE instructions, now inline functions in `GuestCpu.hpp`. |
| build for a lifted guest (libkernel); use portable timestamp and spin pause | `GuestCpu::ReadTsc` is `__rdtsc` and `PreciseSpinPause` is `_mm_pause`. |
| spin and diff guest memory portably | `pause` as before. `WriteChanged` keeps its SSE2 loop: upstream builds only with GCC or Clang, and both define `__SSE2__` on x86-64. |
| convert guest long double at its boundaries | A `memcpy` where `long double` is the x87 format. |
| keep the guest's long double exactly in lifted builds | The same `std` calls, through inline `LibcDetail` templates. `printf` formats an infinite `long double` through `double`, with the same text. |
| read guest va_lists portably | `FormatArguments` tests one flag per argument, and a `%L` float scan goes through a local `long double`. |
| take the guest offline with APS5_NETWORK=0 and limit it to allowed domains | A cached policy read per `connect` and `sendto`, and a numeric-only `getaddrinfo` before each name lookup. With `APS5_NETWORK_ALLOW` set, every `sendto` to an address takes a global mutex and a set lookup. |
| allocate thread objects in guest memory | One guest-heap allocation per thread instead of a host one. |
| serve a snapshot that overlaps a writable range from live memory | Was a hard error; such memory now uploads as writable. |
| let NVIDIA's driver inline helper functions | On NVIDIA's proprietary driver the recompiler's helper functions are no longer `DontInline`, and the shader cache key records it. Other drivers are unchanged. |
| the HTTP/2 patches, 16-bit converted images, sceNpGetSandboxInfo | Paths that threw or were refused before. |
| the Clang fixes | Compile time only. |

To confirm: build upstream `main` and the patch set for x86-64 Linux with the same compiler and options, disassemble each library with `objdump -d`, strip the addresses, and diff by function. Only the functions these patches change may differ.

## Tests and CI

What runs today:

- Upstream's CI builds and tests x86-64 only: the full Linux build on upstream's self-hosted runner and Windows with g++ under MSYS2, on every push to `main`, and for a pull request only when a collaborator starts the run. A relinker-only matrix covers Linux, Windows and macOS, and an Emscripten workflow compiles the libraries for WebAssembly. Nothing builds for aarch64, builds the full tree with Clang, or defines `ANYPS5_GUEST_LIFTED`.
- ps5lift has no CI. Its `ctest` (the fixtures and `tests/libc`) runs on the GB10 by hand.

What follows from that:

- The lifted paths compile only when someone builds ps5lift, so upstream changes break them unseen. The 2026-10-30 rebase needed two new patches, `default-construct HostGainRamp implicitly` and `cast vtable entries to functions in one place`, for new upstream code that GCC accepts and Clang rejects; upstream made the same changes itself, and the 2026-10-31 rebase dropped both.
- 24 of the 36 patches change no test file. Most compile only in the lifted build, which ps5lift's tests cover indirectly. `allocate thread objects in guest memory` changes the x86 build and has no test.

Work items, in order:

1. **CI for ps5lift on a Linux arm64 runner.** Build `ps5lift` and the runtime with LLVM 18, build AnyPS5 with Clang and `ANYPS5_GUEST_LIFTED` as the test build does in `build/anyps5`, and run `ctest`. It runs on every commit to `main`, rebases included, so a rebase that breaks the lifted build fails there. Find out first whether hosted arm64 runners, and enough minutes, are available for the repository's visibility; how long the AnyPS5 build takes on four cores (time it on the GB10 with `-j4`); and which tests need a display (run those under Xvfb). Titles never run in CI. A self-hosted runner on one of the user's machines needs the user's agreement.
2. **A scheduled rebase check.** Fetch upstream `main`, rebase the patch set onto it in a scratch clone, and build the lifted configuration, so conflicts and Clang breaks show up before anyone rebases by hand.
3. **A test for thread objects in guest memory**: the handle `scePthreadCreate` returns lies in the guest heap's memory. It belongs in AnyPS5's tests (`core/libs/tests`), where upstream's x86 CI runs it.
4. **No timings in CI.** Hosted runners are too noisy, and upstream accepts a speed claim only with traces from one machine. The disassembly diff above settles the x86 question once.

## Upstream pull requests

A proposed split; nothing has been sent. Upstream's [contribution rules](https://github.com/boykopovar/AnyPS5/blob/main/CONTRIBUTING.md) and pull request template decide it:

- One topic per pull request, branched from current `main`. Follow-ups go in new pull requests, and a pull request that needs another says `Depends on: #N`.
- An author may have 1 open pull request until one is merged, then 20, and no limit once 20 are merged. The first one has to be small and easy to merge.
- A title fix names the title ID and its Game bring-up discussion (`Unblocks:`).
- AGC, shader recompiler and VideoOut changes list every GPU the GPU tests ran on, and run the focused tests on `main` and on the branch with the Vulkan validation layer. A change to shader instruction semantics says where the semantics come from.
- `Tested:` gives the OS and the title or homebrew. These patches were tested on aarch64 under ps5lift. A fix that is not specific to lifting also needs upstream's x86 build and tests, and the title where one is named, on an x86-64 Linux machine ([open decisions](#open-decisions)).
- `AI-assisted: yes`.
- Each test finishes within 30 s on the CI runners.

The patch messages mention lifted builds and ps5lift, so each pull request description says what those are. Cherry-pick a pull request's patches onto current `main` in the patch set's order. The dependencies below are the known ones; add any that a cherry-pick turns up.

### Phase 1: fixes that stand on their own

Each applies to the x86 build too, and each one merged shrinks the patch set.

| # | Pull request | Patches | Depends on | Notes |
|---|---|---|---|---|
| 1 | Build with Clang | make Json object members a complete type | none | The first pull request. Confirm first that upstream `main` fails to build with Clang on x86-64, and send only the fixes it needs. |
| 2 | HTTP/2 redirect callback | keep the redirect callback a template or request sets | none | Unblocks an Unreal title (the EOS SDK). |
| 3 | Asynchronous HTTP/2 | send and read asynchronously | 2 | Unblocks an Unreal title. Also changes libSceNet. The event codes come from the title's HTTP module, not from a console; its debt entry says so. |
| 4 | HTTP/1.x lookups | look an HTTP/1.x request's host up before stopping on it | 3 | Unblocks an Unreal title. |
| 5 | Empty sandbox info | report an empty sandbox from sceNpGetSandboxInfo | none | Unblocks an Unreal title. What a console writes is unmeasured, so expect questions under "generic behaviour". |
| 6 | Snapshot over writable memory | serve a snapshot that overlaps a writable range from live memory | none | GPU checklist. |
| 7 | 16-bit converted images | load and store converted images with 16-bit data | none | Unblocks an Unreal title. GPU checklist, and the source of the semantics. |
| 8 | Network policy | take the guest offline with APS5_NETWORK=0 and limit it to allowed domains | none | Networking stays on by default, so nothing changes unless a variable is set. |
| 9 | Inlinable helpers on NVIDIA | let NVIDIA's driver inline helper functions | none | GPU checklist. The patch message says the validation-layer runs were not made. |

### Phase 2: the lifted target

Only if upstream wants a lifted aarch64 target in its tree; ask in a discussion before the first of these. Upstream already carries another target that does not run guest code natively, Emscripten, with its own code paths and workflow, and `GuestLongDouble.hpp` already had a branch for a `long double` that is not x87. This phase waits on the first of the [open decisions](#open-decisions).

| # | Pull request | Patches | Depends on | Notes |
|---|---|---|---|---|
| 10 | Portable CPU helpers | reach the guest FPU state through GuestCpu; use portable timestamp and spin pause; spin and diff guest memory portably; take the FFmpeg package for the target's architecture; the `ReadTsc` part of build for a lifted guest (libkernel) | none | No change on x86, so it can also go in phase 1 as a refactor. Split `ReadTsc` out of the libkernel patch. |
| 11 | The build switch and its CI | add ANYPS5_GUEST_LIFTED to build the libraries for ps5lift; link lifted libraries against the ps5lift runtime | 10 | Add a workflow that builds the libraries with Clang and `ANYPS5_GUEST_LIFTED` on an arm64 runner. |
| 12 | Guest long double | convert guest long double at its boundaries; keep the guest's long double exactly in lifted builds | 11 | Needs the runtime's `GuestCpuExtended*` entry points. |
| 13 | Guest va_lists | read guest va_lists portably in lifted builds; start err and warn va_lists with APS5_VA_BEGIN; hand report handlers a SysV va_list in lifted builds | 12 | Uses `GuestLongDoubleRound` from 12. |
| 14 | x86-only libc helpers | build the x86-only helpers portably for a lifted guest | 11 | |
| 15 | Unwinding and exception pointers | unwind guest frames from the lifted guest state; keep the host's exception pointers in lifted builds | 11 | The `host_exception_pointer` test runs on x86 too. |
| 16 | Lifted threads | the rest of build for a lifted guest (libkernel); call a lifted thread entry as the native callback it is; run a lifted guest thread on the stack it asked for; allocate thread objects in guest memory | 10, 11 | Add the thread-object test from [tests and CI](#tests-and-ci). |
| 17 | Lifted images and modules | iterate the lifted program's guest images in dl_iterate_phdr; find lifted guest images in sceKernelGetModuleInfoFromAddr; load modules that are part of a lifted program; find a lifted guest's images through the runtime | 16 | Unblocks an Unreal title (module information). |
| 18 | Fibers on aarch64 | switch fibers for a lifted guest on aarch64; stop a lifted fiber resumed on another thread | 16 | |
