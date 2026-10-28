#!/usr/bin/env python3
import argparse
import concurrent.futures
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOFTFLOAT = ROOT / "3rdparty" / "softfloat"
RUNTIME_SOURCES = ["runtime/src/Runtime.cpp", "runtime/src/GuestCpu.cpp", "runtime/src/X87.cpp"]
SOFTFLOAT_FLAGS = [
    "-DSOFTFLOAT_FAST_INT64", "-DSOFTFLOAT_ROUND_ODD", "-DINLINE_LEVEL=5", "-DSOFTFLOAT_FAST_DIV32TO16", "-DSOFTFLOAT_FAST_DIV64TO32",
    "-DTHREAD_LOCAL=_Thread_local", f"-I{SOFTFLOAT / 'build' / 'Linux-x86_64-GCC'}", f"-I{SOFTFLOAT / 'source' / '8086-SSE'}", f"-I{SOFTFLOAT / 'source' / 'include'}",
]


def softfloat_sources():
    return [ROOT / line.strip() for line in (ROOT / "runtime" / "softfloat_sources.txt").read_text().splitlines() if line.strip()]


def needs_build(source, output, headers):
    if not output.exists():
        return True
    newest = max([source.stat().st_mtime, *(header.stat().st_mtime for header in headers)])
    return newest > output.stat().st_mtime


def compile_one(command, source, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([*command, "-c", str(source), "-o", str(output)], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"compiling {source} failed:\n{result.stderr}")


def build(triple, output_directory, clang, clangxx, jobs):
    output_directory = Path(output_directory)
    library = output_directory / "libps5lift_runtime.a"
    headers = list((ROOT / "runtime" / "include" / "ps5lift").glob("*")) + list((SOFTFLOAT / "source" / "include").glob("*.h"))
    target = [f"--target={triple}", "-O2", "-fPIC"]
    units = []
    for path in softfloat_sources():
        units.append(([clang, *target, *SOFTFLOAT_FLAGS], path, output_directory / "softfloat" / (path.stem + ".o")))
    for relative in RUNTIME_SOURCES:
        path = ROOT / relative
        units.append(([clangxx, *target, "-std=c++20", f"-I{ROOT / 'runtime' / 'include'}", f"-I{SOFTFLOAT / 'source' / 'include'}", "-DTHREAD_LOCAL=thread_local", "-DSOFTFLOAT_FAST_INT64", "-DLITTLEENDIAN=1"], path, output_directory / "runtime" / (path.stem + ".o")))
    stale = [unit for unit in units if needs_build(unit[1], unit[2], headers)]
    if stale or not library.exists():
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            list(pool.map(lambda unit: compile_one(*unit), stale))
        if library.exists():
            library.unlink()
        archiver = os.environ.get("AR", "llvm-ar")
        subprocess.run([archiver, "crs", str(library), *(str(unit[2]) for unit in units)], check=True)
    return library


def main():
    parser = argparse.ArgumentParser(description="Build the ps5lift runtime and SoftFloat as a static library for a target")
    parser.add_argument("--triple", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()
    print(build(args.triple, args.output, args.clang, args.clangxx, args.jobs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
