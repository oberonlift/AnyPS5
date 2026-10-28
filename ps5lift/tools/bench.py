#!/usr/bin/env python3
import argparse
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_fixture import runtime_library, FIXTURES, ROOT, build_guest, host_triple, lifter_features, run, runner_for


def parse(output):
    results = {}
    times = {}
    for line in output.splitlines():
        kind, name, value = line.split()
        (results if kind == "result" else times)[name] = int(value)
    return results, times


def main():
    parser = argparse.ArgumentParser(description="Time the memory benchmark natively and lifted with each memory model")
    parser.add_argument("--ps5lift", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--triple", default=host_triple())
    parser.add_argument("--cpu", default=None, help="CPU for the lifted code; native on the host by default")
    parser.add_argument("--scale", type=int, default=16, help="working set multiplier; 16 uses about 128 MiB")
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()

    runner = runner_for(args.triple)
    cpu = args.cpu or ("native" if not runner else None)
    work = Path(tempfile.mkdtemp(prefix="ps5lift-bench-"))
    try:
        scale = [f"-DBENCH_SCALE={args.scale}"]
        guests = {"": build_guest(args.clang, work, "bench", "-O2", scale), "-vec": build_guest(args.clang, work, "bench", "-O2", scale, vectorize=True, suffix="-vec")}
        programs = {}
        for vectorize in (True, False):
            name = "native" if vectorize else "native-novec"
            flags = [] if vectorize else ["-fno-vectorize", "-fno-slp-vectorize"]
            cpu_flag = ([] if runner else ["-march=native"] if args.triple.startswith("x86_64") else ["-mcpu=native"])
            programs[name] = work / name
            run([args.clang, f"--target={args.triple}", "-O2", *cpu_flag, *flags, *scale, FIXTURES / "bench.c", FIXTURES / "host_native.c", "-fuse-ld=lld", "-o", programs[name]])
        for memory, suffix in (("relaxed", ""), ("tso", ""), ("tso", "-vec")):
            name = f"lifted-{memory}{suffix}"
            lifted = work / f"{name}.o"
            cpu_args = ["--cpu", cpu] if cpu else []
            report = run([args.ps5lift, guests[suffix], "-o", lifted, "--triple", args.triple, "--memory", memory, *cpu_args, *lifter_features(args.triple, runner)]).stderr.strip()
            print(f"{name}: {report}")
            programs[name] = work / name
            run([args.clangxx, f"--target={args.triple}", "-std=c++20", "-O2", f"-I{ROOT / 'runtime' / 'include'}", FIXTURES / "host_bridge.cpp", lifted, *runtime_library(args.ps5lift, args.triple, args.clang, args.clangxx), "-fuse-ld=lld", "-pthread", "-o", programs[name]])

        reference = None
        best = {}
        for name, program in programs.items():
            for _ in range(args.repeat):
                results, times = parse(run([*runner, program]).stdout)
                if reference is None:
                    reference = results
                elif results != reference:
                    raise SystemExit(f"{name} computed different results: {results} != {reference}")
                for kernel, value in times.items():
                    best[(name, kernel)] = min(best.get((name, kernel), value), value)

        if runner:
            print(f"note: {args.triple} runs under {Path(runner[0]).name}; the times measure the emulator, not the hardware")
        print(f"host {platform.machine()} {platform.node()}, triple {args.triple}, scale {args.scale}, best of {args.repeat}")
        columns = list(programs)
        print(f"{'kernel':8}" + "".join(f"{name:>16}" for name in columns) + f"{'tso/relaxed':>14}" + f"{'tso/native':>12}" + f"{'vec/native':>12}")
        for kernel in reference:
            row = f"{kernel:8}" + "".join(f"{best[(name, kernel)] / 1e6:14.1f}ms" for name in columns)
            row += f"{best[('lifted-tso', kernel)] / best[('lifted-relaxed', kernel)]:13.2f}x"
            row += f"{best[('lifted-tso', kernel)] / best[('native-novec', kernel)]:11.2f}x"
            row += f"{best[('lifted-tso-vec', kernel)] / best[('native', kernel)]:11.2f}x"
            print(row)
    finally:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
