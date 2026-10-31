# The AnyPS5 fork

AnyPS5-lift builds AnyPS5's libraries from `3rdparty/anyps5`, a fork that carries a patch set on top of upstream `main` (`https://github.com/boykopovar/AnyPS5`). This file is for agents who rebase that patch set, test it, or turn it into upstream pull requests. [Design](DESIGN.md) says how the lifted libraries work; this file treats the patches as patches.

Rules:

- Refer to a patch by its subject. Every rebase changes the hashes; the subjects are unique within the patch set.
- Another agent may be rebasing the submodule. Check `git -C 3rdparty/anyps5 status` and its `HEAD` before touching it, and commit with explicit paths.
- Commits in both repositories are authored as `Claude <noreply@anthropic.com>`.

## The patch set

On 2026-10-30: 30 commits on upstream `ac551a1`, on the branch `aarch64-host-rebase`, which the superproject pins. The branch `aarch64-host`, locally and on the fork's remote, sits on an older upstream and is superseded.

The submodule has no `upstream` remote. Fetch by URL, then list the patch set:

```sh
git -C 3rdparty/anyps5 fetch https://github.com/boykopovar/AnyPS5 main:refs/remotes/upstream/main
git -C 3rdparty/anyps5 log --reverse --format='%h %s' upstream/main..HEAD
```

The patches fall in four groups, listed one by one under [upstream pull requests](#upstream-pull-requests):

- **Building for a lifted guest on aarch64**, mostly behind `ANYPS5_GUEST_LIFTED`: the build switch, the guest FPU state and timestamp counter through `GuestCpu`, x87 `long double` converted at libc's boundaries, guest `va_list`s, unwinding, `dl_iterate_phdr`, fibers, and fixes for Clang.
- **libkernel for lifted programs**: guest thread stacks of the requested size, thread objects in guest memory, `dlopen` of lifted modules, and module information for lifted images.
- **A network gate**: the guest is offline unless `APS5_NETWORK=1`, and `APS5_NETWORK_ALLOW` and `APS5_NETWORK_DENY` limit the domains it may resolve.
- **Fixes found on titles**, none specific to lifting: HTTP/2 (the redirect callback, asynchronous send and read, HTTP/1.x lookups) for the EOS SDK in two Unreal titles, 16-bit access to converted images, `sceNpGetSandboxInfo`, and a GPU snapshot that overlaps a writable range.

Twelve patches carry a `Claude-Session:` trailer, a link to a claude.ai session. Before the patches go anywhere new (a new remote, an upstream pull request), ask the user whether to remove the trailers.

## What the patch set costs an x86 build

Read from the diffs on 2026-10-30, not measured. Most patches compile only under `ANYPS5_GUEST_LIFTED`, `__aarch64__` or a `long double` that is not x87. These compile into the x86 build:

| Patch | Effect on x86 |
|---|---|
| reach the guest FPU state through GuestCpu | The same x87 and SSE instructions, now inline functions in `GuestCpu.hpp`. |
| build for a lifted guest (libkernel); use portable timestamp and spin pause | `GuestCpu::ReadTsc` is `__rdtsc` and `PreciseSpinPause` is `_mm_pause`. |
| spin and diff guest memory portably | `pause` as before. `WriteChanged` keeps its SSE2 loop: upstream builds only with GCC or Clang, and both define `__SSE2__` on x86-64. |
| convert guest long double at its boundaries | A `memcpy` where `long double` is the x87 format. |
| read guest va_lists portably | `FormatArguments` tests one flag per argument, and a `%L` float scan goes through a local `long double`. |
| keep the guest offline unless APS5_NETWORK=1; limit networking to allowed domains | A cached policy read per `connect` and `sendto`, and a numeric-only `getaddrinfo` before each name lookup. With `APS5_NETWORK_ALLOW` set, every `sendto` to an address takes a global mutex and a set lookup. Offline by default is a change of behaviour. |
| allocate thread objects in guest memory | One guest-heap allocation per thread instead of a host one. |
| serve a snapshot that overlaps a writable range from live memory | Was a hard error; such memory now uploads as writable. |
| the HTTP/2 patches, 16-bit converted images, sceNpGetSandboxInfo | Paths that threw or were refused before. |
| the Clang fixes | Compile time only. |

To confirm: build upstream `main` and the patch set for x86-64 Linux with the same compiler and options, disassemble each library with `objdump -d`, strip the addresses, and diff by function. Only the functions these patches change may differ.

## Tests and CI

What runs today:

- Upstream's CI builds and tests x86-64 only: the full Linux build on upstream's self-hosted runner and Windows with g++ under MSYS2, on every push to `main`, and for a pull request only when a collaborator starts the run. A relinker-only matrix covers Linux, Windows and macOS, and an Emscripten workflow compiles the libraries for WebAssembly. Nothing builds for aarch64, builds the full tree with Clang, or defines `ANYPS5_GUEST_LIFTED`.
- AnyPS5-lift has no CI. Its `ctest` (the fixtures and `tests/libc`) runs on the GB10 by hand.

What follows from that:

- The lifted paths compile only when someone builds AnyPS5-lift, so upstream changes break them unseen. The 2026-10-30 rebase needed two new patches, `default-construct HostGainRamp implicitly` and `cast vtable entries to functions in one place`, for new upstream code that GCC accepts and Clang rejects.
- 20 of the 30 patches change no test file. Most compile only in the lifted build, which AnyPS5-lift's tests cover indirectly. `allocate thread objects in guest memory` changes the x86 build and has no test.

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
- `Tested:` gives the OS and the title or homebrew. These patches were tested on aarch64 under ps5lift. A fix that is not specific to lifting also needs upstream's x86 build and tests, and the title where one is named, on an x86-64 Linux machine; ask the user which machine.
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
| 8 | Offline by default | keep the guest offline unless APS5_NETWORK=1 | none | Changes the default for every user, so open a discussion first. If upstream wants the network on by default, AnyPS5-lift's runs set `APS5_NETWORK=0`. |
| 9 | Allowed and denied domains | limit networking to allowed domains and stop denied ones | 8 | |

### Phase 2: the lifted target

Only if upstream wants a lifted aarch64 target in its tree; ask in a discussion before the first of these. Upstream already carries another target that does not run guest code natively, Emscripten, with its own code paths and workflow, and `GuestLongDouble.hpp` already had a branch for a `long double` that is not x87. Unless ps5lift is public, upstream can only compile these paths, so this phase waits on the user's decision about that.

| # | Pull request | Patches | Depends on | Notes |
|---|---|---|---|---|
| 10 | Portable CPU helpers | reach the guest FPU state through GuestCpu; use portable timestamp and spin pause; spin and diff guest memory portably; take the FFmpeg package for the target's architecture; the `ReadTsc` part of build for a lifted guest (libkernel) | none | No change on x86, so it can also go in phase 1 as a refactor. Split `ReadTsc` out of the libkernel patch. |
| 11 | The build switch and its CI | add ANYPS5_GUEST_LIFTED to build the libraries for ps5lift | 10 | Add a workflow that builds the libraries with Clang and `ANYPS5_GUEST_LIFTED` on an arm64 runner. |
| 12 | Guest long double | convert guest long double at its boundaries | 11 | |
| 13 | Guest va_lists | read guest va_lists portably in lifted builds; start err and warn va_lists with APS5_VA_BEGIN | 12 | Uses `GuestLongDoubleRound` from 12. |
| 14 | x86-only libc helpers | build the x86-only helpers portably for a lifted guest | 11 | |
| 15 | Unwinding and exception pointers | unwind guest frames from the lifted guest state; keep the host's exception pointers in lifted builds | 11 | The `host_exception_pointer` test runs on x86 too. |
| 16 | Lifted threads | the rest of build for a lifted guest (libkernel); call a lifted thread entry as the native callback it is; run a lifted guest thread on the stack it asked for; allocate thread objects in guest memory | 10, 11 | Add the thread-object test from [tests and CI](#tests-and-ci). |
| 17 | Lifted images and modules | iterate the lifted program's guest images in dl_iterate_phdr; find lifted guest images in sceKernelGetModuleInfoFromAddr; load modules that are part of a lifted program | 16 | Unblocks an Unreal title (module information). |
| 18 | Fibers on aarch64 | switch fibers for a lifted guest on aarch64 | 16 | |
