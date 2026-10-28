#!/usr/bin/env python3
import argparse
import difflib
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
FIXTURES = ROOT / "tests" / "fixtures"
GUEST_FLAGS = [
    "--target=x86_64-unknown-linux-gnu",
    "-march=znver2",
    "-fno-stack-protector",
    "-fno-builtin",
    "-ffreestanding",
    "-fcf-protection=none",
    "-nostdlib",
    "-fuse-ld=lld",
    "-DPS5LIFT_GUEST",
]


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        sys.stderr.write(f"$ {' '.join(map(str, command))}\n{result.stdout}{result.stderr}")
        raise SystemExit(f"command failed with exit code {result.returncode}")
    return result


def runtime_link_arguments(ps5lift, triple, clang, clangxx):
    from build_runtime import build, link_arguments
    directory = Path(ps5lift).resolve().parent.parent / f"runtime-{triple.split('-')[0]}"
    return link_arguments(*build(triple, directory, clang, clangxx, os.cpu_count()))


def host_triple():
    machine = platform.machine()
    if machine in ("aarch64", "arm64"):
        return "aarch64-unknown-linux-gnu"
    if machine in ("x86_64", "AMD64"):
        return "x86_64-unknown-linux-gnu"
    raise SystemExit(f"unsupported host {machine}")


def runner_for(triple):
    if triple.startswith(platform.machine()) or (triple.startswith("aarch64") and platform.machine() == "arm64"):
        return []
    if triple.startswith("aarch64"):
        qemu = shutil.which("qemu-aarch64") or shutil.which("qemu-aarch64-static")
        if qemu is None:
            raise SystemExit("running aarch64 code on this host needs qemu-aarch64")
        sysroot = os.environ.get("PS5LIFT_AARCH64_SYSROOT", "/usr/aarch64-linux-gnu")
        return [qemu, "-one-insn-per-tb", "-L", sysroot]
    raise SystemExit(f"cannot run {triple} on {platform.machine()}")


def lifter_target(triple, runner, cpu=None):
    if cpu is None and not runner:
        cpu = "native"
    flags = ["--cpu", cpu] if cpu else []
    if runner and triple.startswith("aarch64"):
        flags += ["--features", "-rcpc-immo"]
    return flags


CONSOLE_OUTPUT = {"cpu"}
HOST_LIBRARIES = {"threads": ("hostthread", "host_thread.c"), "callbacks": ("hostcallbacks", "host_callbacks.c"), "x87": ("hostx87", "host_x87.c"), "setjmp": ("hostsetjmp", "host_setjmp.c")}


def build_guest(clang, work, name, optimization, defines=(), vectorize=False, suffix=""):
    library = work / "libhost.so"
    if not library.exists():
        run([clang, *GUEST_FLAGS, "-O2", "-fPIC", "-shared", FIXTURES / "host_stub.c", "-o", library])
    extra = []
    if name in HOST_LIBRARIES:
        library_name, source = HOST_LIBRARIES[name]
        extra_library = work / f"lib{library_name}.so"
        if not extra_library.exists():
            run([clang, "--target=x86_64-unknown-linux-gnu", "-O2", "-fPIC", "-shared", "-fuse-ld=lld", FIXTURES / source, "-pthread", "-o", extra_library])
        extra = [f"-l{library_name}"]
    guest = work / f"{name}{optimization}{suffix}"
    vector_flags = [] if vectorize else ["-fno-vectorize", "-fno-slp-vectorize"]
    run([clang, optimization, *GUEST_FLAGS, *vector_flags, *defines, "-fno-pic", "-no-pie", "-Wl,--image-base=0x20000000",
         "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", FIXTURES / f"{name}.c", f"-L{work}", "-lhost", *extra, "-o", guest])
    return guest


def compare(expected, actual, label):
    if expected == actual:
        return True
    sys.stderr.write(f"{label}: output differs\n")
    sys.stderr.writelines(difflib.unified_diff(expected.splitlines(True), actual.splitlines(True), "expected", label))
    return False


def main():
    parser = argparse.ArgumentParser(description="Lift an x86-64 fixture, build it for a target and compare its output")
    parser.add_argument("--ps5lift", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--triple", default=host_triple())
    parser.add_argument("--fixture", default="basic")
    parser.add_argument("--optimization", action="append")
    parser.add_argument("--memory", action="append", choices=["tso", "relaxed"])
    parser.add_argument("--cpu", help="CPU for the lifted code; native on the host by default")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    optimizations = args.optimization or ["-O0", "-O1", "-O2", "-O3"]
    memories = args.memory or ["tso", "relaxed"]

    def expected_for(optimization):
        specific = FIXTURES / f"{args.fixture}{optimization}.expected"
        return (specific if specific.exists() else FIXTURES / f"{args.fixture}.expected").read_text()

    runner = runner_for(args.triple)
    work = Path(tempfile.mkdtemp(prefix="ps5lift-"))
    failures = 0
    try:
        for optimization in optimizations:
            expected = expected_for(optimization)
            guest = build_guest(args.clang, work, args.fixture, optimization, ["-DPS5LIFT_EMULATED"] if runner else [])
            if platform.machine() == "x86_64" and args.fixture not in CONSOLE_OUTPUT:
                native = run([guest], env={**os.environ, "LD_LIBRARY_PATH": str(work)}).stdout
                failures += not compare(expected, native, f"native {optimization}")
            for memory in memories:
                label = f"{args.triple} {optimization} {memory}"
                lifted = work / f"{guest.name}-{memory}.a"
                report = run([args.ps5lift, guest, "-o", lifted, "--triple", args.triple, "--memory", memory, *lifter_target(args.triple, runner, args.cpu)]).stderr
                program = work / f"{guest.name}-{memory}"
                run([args.clangxx, f"--target={args.triple}", "-std=c++20", "-O2", f"-I{ROOT / 'runtime' / 'include'}", FIXTURES / "host_bridge.cpp", lifted, *runtime_link_arguments(args.ps5lift, args.triple, args.clang, args.clangxx), "-fuse-ld=lld", "-pthread", "-o", program])
                result = subprocess.run([*runner, program], capture_output=True, text=True)
                ok = result.returncode == 0 and compare(expected, result.stdout, label)
                if result.returncode != 0:
                    sys.stderr.write(f"{label}: exit code {result.returncode}\n{result.stderr}")
                print(f"{'PASS' if ok else 'FAIL'} {label}: {report.strip()}")
                failures += not ok
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
