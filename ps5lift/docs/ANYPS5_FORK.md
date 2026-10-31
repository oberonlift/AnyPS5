# The AnyPS5 fork

AnyPS5-lift builds AnyPS5's libraries from `3rdparty/anyps5`, a fork that carries a patch set on top of upstream `main` (`https://github.com/boykopovar/AnyPS5`). This file is for agents who rebase that patch set, test it, or turn it into upstream pull requests. [Design](DESIGN.md) says how the lifted libraries work; this file treats the patches as patches.

Rules:

- Refer to a patch by its subject. Every rebase changes the hashes; the subjects are unique within the patch set.
- Another agent may be rebasing the submodule. Check `git -C 3rdparty/anyps5 status` and its `HEAD` before touching it, and commit with explicit paths.
- Commits in both repositories are authored as `Claude <noreply@anthropic.com>`.

## Open decisions

Only the user can settle these. Ask before starting the work that depends on one.

1. **Whether [phase 2](#phase-2-the-lifted-target) goes upstream.** Unless ps5lift is public, upstream can only compile the lifted paths. Phase 1 does not depend on this.
2. **Whether to name titles upstream.** An `Unblocks:` line gives a title ID and a public Game bring-up discussion, posted from the account that opens the pull request. Without it, phase 1's title fixes go up with `Unblocks: none` and stand on their tests.
3. **The `Claude-Session:` trailers** on twelve patches: remove them or keep them, before the patches go anywhere new (a new remote, an upstream pull request).
4. **Which x86-64 Linux machine** runs upstream's build and tests, and the titles phase 1 names, for the `Tested:` lines.

## The patch set

On 2026-10-31: 45 commits on upstream `ac551a1`, ending at the commit the superproject pins. The branch `aarch64-host`, locally and on the fork's remote, sits on an older upstream and is superseded.

The submodule has no `upstream` remote. Fetch by URL, then list the patch set:

```sh
git -C 3rdparty/anyps5 fetch https://github.com/boykopovar/AnyPS5 main:refs/remotes/upstream/main
git -C 3rdparty/anyps5 log --reverse --format='%h %s' upstream/main..HEAD
```

The patches fall in four groups, listed one by one under [upstream pull requests](#upstream-pull-requests):

- **Building for a lifted guest on aarch64 Linux and macOS**, mostly behind `ANYPS5_GUEST_LIFTED`: the build switch and the libraries' link to ps5lift's runtime, the guest FPU state and timestamp counter through `GuestCpu`, x87 `long double` converted at libc's boundaries and then kept exactly in a class whose arithmetic runs on the runtime's x87, guest `va_list`s, unwinding, `dl_iterate_phdr`, fibers (which stop when resumed on another thread), libkernel's macOS calls, the GPU driver on macOS through host-pointer imports, patched libraries signed again on arm64, and fixes for Clang and for upstream's macOS build (the write watch tests, the host thread's stack, a numeric host's canonical name, Mach-O libraries in the import audit), and the formatting tests' x87 digits checked only where the host's `long double` holds them.
- **libkernel for lifted programs**: guest thread stacks of the requested size, thread objects in guest memory, `dlopen` of lifted modules, and module information and module lists for lifted images, which the runtime's image list holds.
- **A network policy**: `APS5_NETWORK=0` takes the guest offline, and `APS5_NETWORK_ALLOW` and `APS5_NETWORK_DENY` limit the domains it may resolve. Networking stays on by default, as upstream has it.
- **Fixes found on titles**, none specific to lifting: HTTP/2 (the redirect callback, asynchronous send and read, HTTP/1.x lookups) for the EOS SDK in two Unreal titles, 16-bit access to converted images, `sceNpGetSandboxInfo`, a GPU snapshot that overlaps a writable range, and helper functions that NVIDIA's driver may inline.

Twelve patches carry a `Claude-Session:` trailer, a link to a claude.ai session; see [open decisions](#open-decisions).

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
| name a numeric host as its own canonical name | With `AI_CANONNAME`, a numeric node's first result names the node itself, as glibc's already did; one or two `inet_pton` calls per such lookup. |
| build the GPU driver on macOS | None on Linux and Windows: the dma-buf imports build on Linux, where they always ran. |
| read Mach-O libraries in the import audit | None: a third reader, for images that start with Mach-O's magic. |
| query the host thread's stack in one place | On Windows the AGC driver's guest memory check asks `GetCurrentThreadStackLimits` on every call instead of once per thread, as the other checks did. |
| register the write watch fault tests once; build the libraries for a lifted guest on macOS | None on Linux and Windows. On macOS, where upstream `main` no longer compiles libkernel (`st_atim`, `sched_getcpu`, `RUSAGE_THREAD`), libkernel takes macOS calls for those and for pipes, `fsync`, `statfs`, directory descriptors, shared memory and direct memory, and the libraries' tests configure. |

To confirm: build upstream `main` and the patch set for x86-64 Linux with the same compiler and options, disassemble each library with `objdump -d`, strip the addresses, and diff by function. Only the functions these patches change may differ.

## Tests and CI

What runs today:

- Upstream's CI builds and tests x86-64 only: the full Linux build on upstream's self-hosted runner and Windows with g++ under MSYS2, on every push to `main`, and for a pull request only when a collaborator starts the run. A relinker-only matrix covers Linux, Windows and macOS, and an Emscripten workflow compiles the libraries for WebAssembly. Nothing builds for aarch64, builds the full tree with Clang, or defines `ANYPS5_GUEST_LIFTED`.
- AnyPS5-lift has no CI. Its `ctest` (the fixtures and `tests/libc`) runs by hand on the GB10 and, since 2026-10-31, on an Apple Silicon Mac.

What follows from that:

- The lifted paths compile only when someone builds AnyPS5-lift, so upstream changes break them unseen. The 2026-10-30 rebase needed two new patches, `default-construct HostGainRamp implicitly` and `cast vtable entries to functions in one place`, for new upstream code that GCC accepts and Clang rejects.
- 31 of the 45 patches change no test file. Most compile only in the lifted build, which AnyPS5-lift's tests cover indirectly. `allocate thread objects in guest memory` changes the x86 build and has no test.

Work items, in order:

1. **CI for AnyPS5-lift on a Linux arm64 runner.** Build `ps5lift` and the runtime with LLVM 18, build AnyPS5 with Clang and `ANYPS5_GUEST_LIFTED` as the test build does in `build/anyps5`, and run `ctest`. It runs on every superproject commit, submodule bumps included, so a rebase that breaks the lifted build fails there. Find out first whether hosted arm64 runners, and enough minutes, are available for the repository's visibility; how long the AnyPS5 build takes on four cores (time it on the GB10 with `-j4`); and which tests need a display (run those under Xvfb). Titles never run in CI. A self-hosted runner on one of the user's machines needs the user's agreement.
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
| 1 | Build with Clang | make Json object members a complete type; default-construct HostGainRamp implicitly; cast vtable entries to functions in one place | none | The first pull request. Confirm first that upstream `main` fails to build with Clang on x86-64, and send only the fixes it needs. |
| 2 | HTTP/2 redirect callback | keep the redirect callback a template or request sets | none | Unblocks an Unreal title (the EOS SDK). |
| 3 | Asynchronous HTTP/2 | send and read asynchronously | 2 | Unblocks an Unreal title. Also changes libSceNet. The event codes come from the title's HTTP module, not from a console; its debt entry says so. |
| 4 | HTTP/1.x lookups | look an HTTP/1.x request's host up before stopping on it | 3 | Unblocks an Unreal title. |
| 5 | Empty sandbox info | report an empty sandbox from sceNpGetSandboxInfo | none | Unblocks an Unreal title. What a console writes is unmeasured, so expect questions under "generic behaviour". |
| 6 | Snapshot over writable memory | serve a snapshot that overlaps a writable range from live memory | none | GPU checklist. |
| 7 | 16-bit converted images | load and store converted images with 16-bit data | none | Unblocks an Unreal title. GPU checklist, and the source of the semantics. |
| 8 | Network policy | take the guest offline with APS5_NETWORK=0 and limit it to allowed domains | none | Networking stays on by default, so nothing changes unless a variable is set. |
| 9 | Inlinable helpers on NVIDIA | let NVIDIA's driver inline helper functions | none | GPU checklist. The patch message says the validation-layer runs were not made. |
| 10 | Write watch tests on macOS | register the write watch fault tests once | none | Upstream's CI does not build that directory, which only macOS builds. |
| 11 | The host thread's stack | query the host thread's stack in one place | none | No change on Linux; on Windows see [the x86 cost](#what-the-patch-set-costs-an-x86-build). Its macOS branch is what upstream's macOS build lacked. |
| 12 | Numeric canonical names | name a numeric host as its own canonical name | none | Upstream's macOS build has the same `getaddrinfo`. |
| 13 | Mach-O import audit | read Mach-O libraries in the import audit | none | Upstream's macOS build makes Mach-O libraries too. |

### Phase 2: the lifted target

Only if upstream wants a lifted aarch64 target in its tree; ask in a discussion before the first of these. Upstream already carries another target that does not run guest code natively, Emscripten, with its own code paths and workflow, and `GuestLongDouble.hpp` already had a branch for a `long double` that is not x87. This phase waits on the first of the [open decisions](#open-decisions).

| # | Pull request | Patches | Depends on | Notes |
|---|---|---|---|---|
| 14 | Portable CPU helpers | reach the guest FPU state through GuestCpu; use portable timestamp and spin pause; spin and diff guest memory portably; take the FFmpeg package for the target's architecture; the `ReadTsc` part of build for a lifted guest (libkernel) | none | No change on x86, so it can also go in phase 1 as a refactor. Split `ReadTsc` out of the libkernel patch. |
| 15 | The build switch and its CI | add ANYPS5_GUEST_LIFTED to build the libraries for ps5lift; link lifted libraries against the ps5lift runtime | 14 | Add a workflow that builds the libraries with Clang and `ANYPS5_GUEST_LIFTED` on an arm64 runner. |
| 16 | Guest long double | convert guest long double at its boundaries; keep the guest's long double exactly in lifted builds | 15 | Needs the runtime's `GuestCpuExtended*` entry points. |
| 17 | Guest va_lists | read guest va_lists portably in lifted builds; start err and warn va_lists with APS5_VA_BEGIN | 16 | Uses `GuestLongDoubleRound` from 16. |
| 18 | x86-only libc helpers | build the x86-only helpers portably for a lifted guest | 15 | |
| 19 | Unwinding and exception pointers | unwind guest frames from the lifted guest state; keep the host's exception pointers in lifted builds | 15 | The `host_exception_pointer` test runs on x86 too. |
| 20 | Lifted threads | the rest of build for a lifted guest (libkernel); call a lifted thread entry as the native callback it is; run a lifted guest thread on the stack it asked for; allocate thread objects in guest memory | 14, 15 | Add the thread-object test from [tests and CI](#tests-and-ci). |
| 21 | Lifted images and modules | iterate the lifted program's guest images in dl_iterate_phdr; find lifted guest images in sceKernelGetModuleInfoFromAddr; load modules that are part of a lifted program; find a lifted guest's images through the runtime | 20 | Unblocks an Unreal title (module information). |
| 22 | Fibers on aarch64 | switch fibers for a lifted guest on aarch64; stop a lifted fiber resumed on another thread | 20 | |
| 23 | Lifted macOS | build the libraries for a lifted guest on macOS; check x87 long double digits only where the host holds them; build the GPU driver on macOS; sign the NID-patched libraries again on arm64 macOS; patch NIDs on hosts that do not sign their libraries; map without replacing through mach_vm_allocate on macOS | 11, 20, 21 | Split out libkernel's macOS calls, which upstream's own macOS build needs, as a phase 1 pull request. Squash the NID patch fix into the signing patch it fixes. |
