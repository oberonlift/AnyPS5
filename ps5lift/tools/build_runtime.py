#!/usr/bin/env python3
import argparse
import concurrent.futures
import fcntl
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOFTFLOAT = ROOT / "3rdparty" / "softfloat"
RUNTIME_SOURCES = ["runtime/src/Runtime.cpp", "runtime/src/GuestCpu.cpp", "runtime/src/X87.cpp", "runtime/src/Jump.cpp", "runtime/src/Cpuid.cpp"]
MAIN_SOURCE = "runtime/src/Main.cpp"
GUEST_CPU_INTERFACE = "GuestCpu*_nid_no_patch"
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
    main_library = output_directory / "libps5lift_main.a"
    headers = list((ROOT / "runtime" / "include" / "ps5lift").glob("*")) + list((SOFTFLOAT / "source" / "include").glob("*.h"))
    target = [f"--target={triple}", "-O2", "-fPIC"]
    units = []
    for path in softfloat_sources():
        units.append(([clang, *target, *SOFTFLOAT_FLAGS], path, output_directory / "softfloat" / (path.stem + ".o")))
    for relative in [*RUNTIME_SOURCES, MAIN_SOURCE]:
        path = ROOT / relative
        units.append(([clangxx, *target, "-std=c++20", f"-I{ROOT / 'runtime' / 'include'}", f"-I{SOFTFLOAT / 'source' / 'include'}", "-DTHREAD_LOCAL=thread_local", "-DSOFTFLOAT_FAST_INT64", "-DLITTLEENDIAN=1"], path, output_directory / "runtime" / (path.stem + ".o")))
    # Parallel tests share this directory: build under a lock, and replace archives whole so a link never sees one missing.
    output_directory.mkdir(parents=True, exist_ok=True)
    with open(output_directory / ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        stale = [unit for unit in units if needs_build(unit[1], unit[2], headers)]
        if stale or not library.exists() or not main_library.exists():
            with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
                list(pool.map(lambda unit: compile_one(*unit), stale))
            archiver = subprocess.run([clang, "--print-prog-name=llvm-ar"], capture_output=True, text=True, check=True).stdout.strip()
            for archive, members in ((library, units[:-1]), (main_library, units[-1:])):
                temporary = archive.with_suffix(".a.tmp")
                temporary.unlink(missing_ok=True)
                subprocess.run([archiver, "crs", str(temporary), *(str(unit[2]) for unit in members)], check=True)
                temporary.replace(archive)
    return library, main_library


def link_arguments(library, main_library=None):
    # AnyPS5's lifted libraries import GuestCpu from the program, also when a library needs it only as another library's dependency.
    return [f"-Wl,--undefined-glob={GUEST_CPU_INTERFACE}", f"-Wl,--export-dynamic-symbol={GUEST_CPU_INTERFACE}", str(library), *([str(main_library)] if main_library else [])]


def main():
    parser = argparse.ArgumentParser(description="Build the ps5lift runtime and SoftFloat as a static library for a target")
    parser.add_argument("--triple", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()
    for library in build(args.triple, args.output, args.clang, args.clangxx, args.jobs):
        print(library)
    return 0


if __name__ == "__main__":
    sys.exit(main())
