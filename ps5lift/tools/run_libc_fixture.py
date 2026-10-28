#!/usr/bin/env python3
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_fixture import FIXTURES, GUEST_FLAGS, ROOT, compare, host_triple, lifter_features, run, runner_for, runtime_library
from run_libc_tests import build_libc

IMPORTS = ["printf", "snprintf", "vsnprintf", "sscanf", "puts", "strtold", "strtod", "modfl", "fabsl"]


def compute_nid(name):
    from generate_bridges import compute_nid as nid
    return nid(name)


def main():
    parser = argparse.ArgumentParser(description="Lift a guest that calls AnyPS5's libc through generated bridges and compare its output")
    parser.add_argument("--ps5lift", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--triple", default=host_triple())
    parser.add_argument("--optimization", action="append")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    runner = runner_for(args.triple)
    build = Path(args.ps5lift).resolve().parent.parent
    expected = (FIXTURES / "libc.expected").read_text()
    work = Path(tempfile.mkdtemp(prefix="ps5lift-libc-"))
    failures = 0
    try:
        header = "".join(f'#define NID_{name} "{compute_nid(name)}"\n' for name in IMPORTS)
        (work / "libc_nids.h").write_text(header)
        native_library = work / "libhostlibc.so"
        run([args.clang, "--target=x86_64-unknown-linux-gnu", "-O2", "-fPIC", "-shared", "-fuse-ld=lld", f"-I{work}", FIXTURES / "libc_native.c", "-o", native_library])
        bridges = work / "bridges.cpp"
        run([sys.executable, ROOT / "tools" / "generate_bridges.py", "libc", "-o", bridges])
        libc = build_libc(args.triple, args.clangxx, build)
        runtime = runtime_library(args.ps5lift, args.triple, args.clang, args.clangxx)
        for optimization in args.optimization or ["-O0", "-O2"]:
            guest = work / f"libc{optimization}"
            run([args.clang, optimization, *GUEST_FLAGS, "-fno-vectorize", "-fno-slp-vectorize", f"-I{work}", f"-I{FIXTURES}", "-fno-pic", "-no-pie",
                 "-Wl,--image-base=0x20000000", "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", FIXTURES / "libc.c", f"-L{work}", "-lhostlibc", "-o", guest])
            label = f"{args.triple} {optimization}"
            lifted = work / f"{guest.name}.o"
            report = run([args.ps5lift, guest, "-o", lifted, "--triple", args.triple, *lifter_features(args.triple, runner)]).stderr
            program = work / f"{guest.name}-lifted"
            run([args.clangxx, f"--target={args.triple}", "-std=c++20", "-O1", f"-I{ROOT / 'runtime' / 'include'}", "-fuse-ld=lld", "-Wl,--gc-sections",
                 lifted, bridges, FIXTURES / "host_bridge.cpp", *libc, *runtime, "-latomic", "-pthread", "-ldl", "-o", program])
            result = subprocess.run([*runner, program], capture_output=True, text=True, timeout=600)
            ok = result.returncode == 0 and compare(expected, result.stdout, label)
            if result.returncode != 0:
                sys.stderr.write(f"{label}: exit code {result.returncode}\n{result.stderr}")
            print(f"{'PASS' if ok else 'FAIL'} libc {label}: {report.strip()}")
            failures += not ok
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
