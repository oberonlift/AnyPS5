# ps5lift preview

This branch, `ps5lift-preview`, is [AnyPS5](https://github.com/boykopovar/AnyPS5) with **ps5lift** added. ps5lift is a static recompiler: it turns a PS5 game's x86-64 code into a native arm64 Linux program that runs on AnyPS5's system libraries. ps5lift is on its way into AnyPS5 as a series of pull requests, and this branch merges all of them so you can build and try it before they land. This page exists only on this branch; it is not part of those pull requests.

## How it works

AnyPS5 converts PS5 executables into native Linux executables and implements the PS5's system libraries. On an x86-64 PC the game's own code then runs as it is. An arm64 machine cannot run that code, so ps5lift translates it ahead of time:

1. ps5lift reads the game's executable and modules, once AnyPS5 has relinked them, and lifts their x86-64 code to LLVM IR. That includes SSE, AVX2, FMA, F16C and x87 code.
2. LLVM optimizes the IR and compiles it for arm64. By default the result keeps x86's memory ordering and reproduces its floating-point results bit for bit.
3. The result links against ps5lift's runtime and AnyPS5's libraries built for arm64. Generated bridges carry each call from the game's x86-64 calling convention to the native one.
4. Graphics go through AnyPS5's GPU driver, which recompiles the game's shaders to SPIR-V for Vulkan.

There is no interpreter or JIT while the game runs: it is one native program. [The design](docs/dev/PS5LIFT_DESIGN.md) has the details.

## Status

- Test programs built for x86-64 print the same output lifted on arm64 as they do natively on x86-64.
- 2,293 of AnyPS5's own tests pass natively in an arm64 build of its libraries for lifted games ([the list](ps5lift/tests/lifted_tests.txt)).
- [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5), a homebrew media player, runs as far as its home screen.
- The commercial games tried so far lift, link and start, but none is playable yet. Each stops at a library function AnyPS5 does not implement yet, a GPU feature, or another gap. [Known gaps](docs/dev/TechnicalDebt.md#ps5lift) lists what is open.

## What you need

- **An arm64 Linux machine.** ps5lift is developed on an NVIDIA GB10 (DGX Spark) and tested on GitHub's Ubuntu 24.04 arm64 runners.
- **An arm64 CPU with SVE2, for games.** `ps5lift.py game` lifts for the `neoverse-v2` CPU target, whose code uses SVE. The GB10 and Neoverse V2 servers have it; Ampere Altra, the Raspberry Pi 5 and Apple's chips do not. The lifter itself takes `--cpu native` for other CPUs, and the tests already use it.
- **A Vulkan driver for your GPU**, to run games. The tests that need Vulkan use Mesa's lavapipe.
- **Memory and time to lift a game.** A large Unreal Engine 5 game took about 11 minutes and 13 GB of memory on the GB10.
- **Your own decrypted dumps of games you own.** Neither AnyPS5 nor ps5lift includes any game, firmware or keys.

On Ubuntu 24.04 the build needs:

```sh
sudo apt-get install cmake ninja-build python3 clang-18 lld-18 llvm-18-dev libzstd-dev zlib1g-dev python3-clang-18 \
  g++-x86-64-linux-gnu libx11-dev libxext-dev libxrandr-dev libwayland-dev libxkbcommon-dev libegl-dev \
  libdecor-0-dev libpipewire-0.3-dev libpulse-dev libasound2-dev
```

## Build and run

```sh
git clone --branch ps5lift-preview https://github.com/oberonlift/AnyPS5.git
cd AnyPS5
git submodule update --init --recursive
cd ps5lift
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/lib/llvm-18/bin/clang -DCMAKE_CXX_COMPILER=/usr/lib/llvm-18/bin/clang++ \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
ninja -C build
ctest --test-dir build --output-on-failure
```

The build compiles ps5lift and every AnyPS5 library for lifted games, so it takes a while.

To run a game, point `game` at its extracted `app0` directory, the one holding `eboot.bin`:

```sh
python3 tools/ps5lift.py game path/to/app0 out
python3 tools/ps5lift.py run out
```

`game` prepares the game, generates the bridges it needs, lifts it into `out/app.a` and links `out/game`. It lifts again only when the lifter or the game has changed. `run` starts the game. AnyPS5's `APS5_` options go after `run` as `NAME=VALUE`. [The ps5lift guide](docs/dev/PS5LIFT.md) covers the lifter's options and the other commands.

## What this branch contains

The branch is the top of the pull-request stack with its conventions fix merged. The pull requests are open on AnyPS5, each with a copy on this fork, and merge in this order:

| Order | AnyPS5 | This fork | What it adds |
|---|---|---|---|
| 1 | [#15797](https://github.com/boykopovar/AnyPS5/pull/15797) | [#20](https://github.com/oberonlift/AnyPS5/pull/20) | The libraries' fenv, TSC, cpuid and pause instructions, through helpers |
| 2 | [#15798](https://github.com/boykopovar/AnyPS5/pull/15798) | [#18](https://github.com/oberonlift/AnyPS5/pull/18) | ps5lift's runtime |
| 3 | [#15799](https://github.com/boykopovar/AnyPS5/pull/15799) | [#9](https://github.com/oberonlift/AnyPS5/pull/9) | `ANYPS5_GUEST_LIFTED`, the build of the libraries for lifted games |
| 4 | [#15800](https://github.com/boykopovar/AnyPS5/pull/15800) | [#10](https://github.com/oberonlift/AnyPS5/pull/10) | Unwinding and exceptions in libc |
| 5 | [#15801](https://github.com/boykopovar/AnyPS5/pull/15801) | [#11](https://github.com/oberonlift/AnyPS5/pull/11) | The game's `va_list`s, read portably |
| 6 | [#15802](https://github.com/boykopovar/AnyPS5/pull/15802) | [#12](https://github.com/oberonlift/AnyPS5/pull/12) | The game's x87 `long double`, kept exactly |
| 7 | [#15803](https://github.com/boykopovar/AnyPS5/pull/15803) | [#13](https://github.com/oberonlift/AnyPS5/pull/13) | Threads for lifted games |
| 8 | [#15804](https://github.com/boykopovar/AnyPS5/pull/15804) | [#14](https://github.com/oberonlift/AnyPS5/pull/14) | The game's images, its modules and its fibers |
| 9 | [#15805](https://github.com/boykopovar/AnyPS5/pull/15805) | [#17](https://github.com/oberonlift/AnyPS5/pull/17) | Lets AnyPS5's conventions check accept LLVM from the system |
| 10 | [#15806](https://github.com/boykopovar/AnyPS5/pull/15806) | [#15](https://github.com/oberonlift/AnyPS5/pull/15) | The lifter |
| 11 | [#15807](https://github.com/boykopovar/AnyPS5/pull/15807) | [#16](https://github.com/oberonlift/AnyPS5/pull/16) | Lifted programs and games on AnyPS5's libraries |

The first eight make AnyPS5's libraries build and run for a lifted program. The lifter (AnyPS5 #15806) needs the conventions fix (#15805), which lets the check accept the system LLVM it builds against, and the last one (#15807) needs both #15804 and #15806.

When the pull requests change, this branch is rebuilt from them, so its history can be rewritten. Update with `git fetch origin && git reset --hard origin/ps5lift-preview` rather than `git pull`.

Running lifted games natively on Apple Silicon Macs is progressing on the `macos-native` branch, but it is not part of this preview, because the Mac is harder:

- **Memory layout.** macOS keeps parts of the address range PS5 games use for its own libraries and GPU, so every guest address is moved by a fixed offset and converted wherever it crosses between the game and native code.
- **Shaders.** Apple's GPUs, reached through MoltenVK, lack features the shader recompiler relies on, such as mesh shaders, 64-bit buffer atomics and a shader clock, so each of those has to be emulated.

## License

ps5lift is part of AnyPS5 and shares its license, the GNU General Public License version 2 only. As AnyPS5's [README](README.md) says, the project is for interoperability, research, preservation and compatibility, and it is not affiliated with, authorized or endorsed by Sony Interactive Entertainment.
