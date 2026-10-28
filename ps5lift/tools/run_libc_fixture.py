#!/usr/bin/env python3
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_fixture import FIXTURES, GUEST_FLAGS, ROOT, compare, host_triple, lifter_target, run, runner_for, runtime_library
from run_libc_tests import build_libraries

IMPORTS = ["printf", "snprintf", "vsnprintf", "sscanf", "puts", "strtold", "strtod", "modfl", "fabsl", "fprintf", "_Stdout"]
KERNEL_IMPORTS = ["printf", "exit", "scePthreadCreate", "scePthreadJoin", "scePthreadMutexInit", "scePthreadMutexLock", "scePthreadMutexUnlock",
                  "scePthreadMutexDestroy", "sceKernelUsleep", "sceKernelGetProcessTime", "sceKernelReadTsc", "sceKernelGetTscFrequency"]
FIBER_IMPORTS = ["printf", "exit", "_sceFiberInitializeImpl", "sceFiberFinalize", "sceFiberRun", "sceFiberSwitch", "sceFiberReturnToThread", "sceFiberGetSelf"]
LOG_LINE = re.compile(r"^\[[^\]\s]+:\d+ [^\]]+\] .*\n", re.MULTILINE)
GUEST_LINK = ["-no-pie", "-Wl,--image-base=0x20000000", "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2"]


def compute_nid(name):
    from generate_bridges import compute_nid as nid
    return nid(name)


def build_libc_guest(args, work, optimization):
    header = "".join(f'#define NID_{name} "{compute_nid(name)}"\n' for name in IMPORTS)
    (work / "libc_nids.h").write_text(header)
    native_library = work / "libhostlibc.so"
    if not native_library.exists():
        run([args.clang, "--target=x86_64-unknown-linux-gnu", "-O2", "-fPIC", "-shared", "-fuse-ld=lld", f"-I{work}", FIXTURES / "libc_native.c", "-o", native_library])
    guest = work / f"libc{optimization}"
    run([args.clang, optimization, *GUEST_FLAGS, "-fno-vectorize", "-fno-slp-vectorize", f"-I{work}", f"-I{FIXTURES}", "-fpie", *GUEST_LINK,
         FIXTURES / "libc.c", f"-L{work}", "-lhostlibc", "-o", guest])
    return guest, False, [FIXTURES / "host_bridge.cpp"], []


def build_exceptions_guest(args, work, optimization):
    guest = work / f"exceptions{optimization}"
    run([args.clangxx, optimization, *GUEST_FLAGS, "-fno-vectorize", "-fno-slp-vectorize", "-fexceptions", "-fpie", *GUEST_LINK,
         FIXTURES / "exceptions.cpp", "-lstdc++", "-lc", "-lgcc_s", "-o", guest])
    return guest, True, [], ["--import-nids"]


def build_stubbed_guest(args, work, optimization, fixture, imports):
    (work / f"{fixture}_nids.h").write_text("".join(f'#define NID_{name} "{compute_nid(name)}"\n' for name in imports))
    stub = work / f"{fixture}_stub.c"
    stub.write_text("".join(f'void stub_{index}(void) __asm__("{compute_nid(name)}");\nvoid stub_{index}(void) {{}}\n' for index, name in enumerate(imports)))
    library = work / f"lib{fixture}stub.so"
    if not library.exists():
        run([args.clang, *GUEST_FLAGS, "-O2", "-fPIC", "-shared", stub, "-o", library])
    guest = work / f"{fixture}{optimization}"
    run([args.clang, optimization, *GUEST_FLAGS, "-fno-vectorize", "-fno-slp-vectorize", f"-I{work}", "-fpie", *GUEST_LINK,
         FIXTURES / f"{fixture}.c", f"-L{work}", f"-l{fixture}stub", "-o", guest])
    return guest, False, [], []


def build_kernel_guest(args, work, optimization):
    return build_stubbed_guest(args, work, optimization, "kernel", KERNEL_IMPORTS)


def build_fiber_guest(args, work, optimization):
    return build_stubbed_guest(args, work, optimization, "fiber", FIBER_IMPORTS)


GUESTS = {"libc": build_libc_guest, "exceptions": build_exceptions_guest, "kernel": build_kernel_guest, "fiber": build_fiber_guest}
LIBRARIES = {"libc": ["libc"], "exceptions": ["libc"], "kernel": ["libc", "libkernel"], "fiber": ["libc", "libkernel", "libSceFiber"]}


def main():
    parser = argparse.ArgumentParser(description="Lift a guest that calls AnyPS5's libc through generated bridges and compare its output")
    parser.add_argument("--ps5lift", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--triple", default=host_triple())
    parser.add_argument("--fixture", choices=sorted(GUESTS), default="libc")
    parser.add_argument("--optimization", action="append")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    runner = runner_for(args.triple)
    build = Path(args.ps5lift).resolve().parent.parent
    expected = (FIXTURES / f"{args.fixture}.expected").read_text()
    work = Path(tempfile.mkdtemp(prefix=f"ps5lift-{args.fixture}-"))
    failures = 0
    try:
        bridges = work / "bridges.cpp"
        run([sys.executable, ROOT / "tools" / "generate_bridges.py", *LIBRARIES[args.fixture], "-o", bridges])
        libraries = build_libraries(args.triple, args.clangxx, build, LIBRARIES[args.fixture])
        runtime = runtime_library(args.ps5lift, args.triple, args.clang, args.clangxx)
        for optimization in args.optimization or ["-O0", "-O2"]:
            guest, native, host_sources, lift_flags = GUESTS[args.fixture](args, work, optimization)
            if native and platform.machine() == "x86_64":
                failures += not compare(expected, run([guest], env={**os.environ, "LD_LIBRARY_PATH": str(work)}).stdout, f"native {optimization}")
            label = f"{args.triple} {optimization}"
            lifted = work / f"{guest.name}.o"
            report = run([args.ps5lift, guest, "-o", lifted, "--triple", args.triple, *lift_flags, *lifter_target(args.triple, runner)]).stderr
            program = work / f"{guest.name}-lifted"
            run([args.clangxx, f"--target={args.triple}", "-std=c++20", "-O1", f"-I{ROOT / 'runtime' / 'include'}", "-fuse-ld=lld", "-Wl,--gc-sections",
                 "-Wl,--allow-multiple-definition", lifted, bridges, *host_sources, *libraries, *runtime, "-latomic", "-pthread", "-ldl", "-o", program])
            result = subprocess.run([*runner, program], capture_output=True, text=True, timeout=600)
            ok = result.returncode == 0 and compare(expected, LOG_LINE.sub("", result.stdout), label)
            if result.returncode != 0:
                sys.stderr.write(f"{label}: exit code {result.returncode}\n{result.stderr}")
            print(f"{'PASS' if ok else 'FAIL'} {args.fixture} {label}: {report.strip()}")
            failures += not ok
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
