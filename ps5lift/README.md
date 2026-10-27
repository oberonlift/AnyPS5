# anyps5-lift

A static recompiler for PS5 executables. `ps5lift` lifts guest x86-64 code to LLVM IR and compiles it to native code for the host, so a title can run on aarch64 Linux machines such as the NVIDIA DGX Spark (GB10) without an x86 emulator. The system libraries come from [AnyPS5](https://github.com/boykopovar/anyps5), which is a submodule; its ELF reader and `.eh_frame` scanner are the lifter's frontend.

Status: **milestone 1**. Guest code built for Zen 2 lifts and runs on aarch64 with the same output as on x86-64. Real PS5 titles do not run yet; see [the design](docs/DESIGN.md) and [known gaps](docs/DEBT.md).

## Build

Requirements: CMake 3.22+, Ninja, Clang and LLD, LLVM 18 or newer with development files (`llvm-18-dev libzstd-dev` on Ubuntu 24.04 / DGX OS), Python 3.

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
ninja -C build
ctest --test-dir build --output-on-failure
```

On an aarch64 host the test lifts the fixture and runs it natively. On an x86-64 host it also runs it natively and, when `qemu-aarch64` and an aarch64 sysroot are installed (`qemu-user g++-aarch64-linux-gnu`), lifts it for aarch64 and runs it under qemu.

## Use

```sh
build/lifter/ps5lift guest.elf -o guest.o --cpu native
clang++ -O2 -Iruntime/include runtime/src/Runtime.cpp bridges.cpp guest.o -fuse-ld=lld -pthread -o guest
./guest
```

| Option | Default | Effect |
|---|---|---|
| `--triple` | `aarch64-unknown-linux-gnu` | Target for the generated code. |
| `--cpu` | `neoverse-v2` on aarch64 | Target CPU; `native` uses the host CPU and features. |
| `--features` | none | LLVM target features, such as `-rcpc-immo`. |
| `--memory tso\|relaxed` | `tso` | `tso` keeps x86 ordering with acquire loads and release stores; `relaxed` uses plain accesses and is only correct for single-threaded guests. |
| `--emit-ir file.ll` | none | Writes the optimized LLVM IR. |
| `-O0` | optimized | Skips the LLVM optimization pipeline. |

The lifter prints how many functions and instructions it lifted and lists every unsupported instruction it found. Reaching one at run time stops the program with its address.
