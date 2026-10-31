#!/usr/bin/env python3
import argparse
import concurrent.futures
import fcntl
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOFTFLOAT = ROOT.parent / "3rdparty" / "softfloat"
RUNTIME_SOURCES = ["runtime/src/Runtime.cpp", "runtime/src/GuestCpu.cpp", "runtime/src/X87.cpp", "runtime/src/Jump.cpp", "runtime/src/Cpuid.cpp", "runtime/src/StringCompare.cpp", "runtime/src/NativeCall.cpp"]
MAIN_SOURCE = "runtime/src/Main.cpp"
SOFTFLOAT_FLAGS = [
    "-DSOFTFLOAT_FAST_INT64", "-DSOFTFLOAT_ROUND_ODD", "-DINLINE_LEVEL=5", "-DSOFTFLOAT_FAST_DIV32TO16", "-DSOFTFLOAT_FAST_DIV64TO32",
    "-DTHREAD_LOCAL=_Thread_local", f"-I{SOFTFLOAT / 'build' / 'Linux-x86_64-GCC'}", f"-I{SOFTFLOAT / 'source' / '8086-SSE'}", f"-I{SOFTFLOAT / 'source' / 'include'}",
]


def softfloat_sources():
    return [ROOT.parent / line.strip() for line in (ROOT / "runtime" / "softfloat_sources.txt").read_text().splitlines() if line.strip()]


def directory_for(build_root, triple):
    return Path(build_root) / f"runtime-{triple}"


def fingerprint(*parts):
    return hashlib.sha256("\0".join(map(str, parts)).encode()).hexdigest()


def dependencies(depfile):
    body = depfile.read_text().replace("\\\n", " ").split(":", 1)[1]
    return [Path(token.replace("\\ ", " ")) for token in re.split(r"(?<!\\)\s+", body.strip()) if token]


def needs_build(record, output, depfile):
    if not output.exists() or not depfile.exists() or not output.with_suffix(".cmd").exists():
        return True
    if output.with_suffix(".cmd").read_text() != record:
        return True
    built = output.stat().st_mtime
    return any(not path.exists() or path.stat().st_mtime > built for path in dependencies(depfile))


def compile_one(command, source, output, record):
    output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([*command, "-MMD", "-MF", str(output.with_suffix(".d")), "-c", str(source), "-o", str(output)], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"compiling {source} failed:\n{result.stderr}")
    output.with_suffix(".cmd").write_text(record)


def compiler_version(compiler):
    return subprocess.run([compiler, "--version"], capture_output=True, text=True, check=True).stdout


def build(triple, output_directory, clang, clangxx, jobs):
    output_directory = Path(output_directory)
    library = output_directory / "libps5lift_runtime.so"
    main_library = output_directory / "libps5lift_main.a"
    target = [f"--target={triple}", "-O2", "-fPIC"]
    units = []
    for path in softfloat_sources():
        units.append(([clang, *target, *SOFTFLOAT_FLAGS], path, output_directory / "softfloat" / (path.stem + ".o")))
    for relative in [*RUNTIME_SOURCES, MAIN_SOURCE]:
        path = ROOT / relative
        units.append(([clangxx, *target, "-std=c++20", f"-I{ROOT / 'runtime' / 'include'}", f"-I{SOFTFLOAT / 'source' / 'include'}", "-DTHREAD_LOCAL=thread_local", "-DSOFTFLOAT_FAST_INT64", "-DLITTLEENDIAN=1"], path, output_directory / "runtime" / (path.stem + ".o")))
    versions = {compiler: compiler_version(compiler) for compiler in (clang, clangxx)}
    units = [(command, source, output, fingerprint(versions[command[0]], *command, source)) for command, source, output in units]
    shared = [clangxx, f"--target={triple}", "-shared", "-fuse-ld=lld", f"-Wl,-soname,{library.name}", "-pthread", "-ldl"]
    archiver = subprocess.run([clang, "--print-prog-name=llvm-ar"], capture_output=True, text=True, check=True).stdout.strip()
    outputs = ((library, units[:-1], lambda temporary, objects: [*shared, *objects, "-o", temporary]),
               (main_library, units[-1:], lambda temporary, objects: [archiver, "crs", temporary, *objects]))
    # Parallel tests share this directory: build under a lock, and replace outputs whole so a link never sees one missing.
    output_directory.mkdir(parents=True, exist_ok=True)
    with open(output_directory / ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        stale = [unit for unit in units if needs_build(unit[3], unit[2], unit[2].with_suffix(".d"))]
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            list(pool.map(lambda unit: compile_one(*unit), stale))
        for output, members, command in outputs:
            objects = [str(unit[2]) for unit in members]
            record = fingerprint(*command("", objects))
            listing = output.with_name(output.name + ".cmd")
            if output.exists() and listing.exists() and listing.read_text() == record and not any(unit in stale for unit in members):
                continue
            temporary = output.with_name(output.name + ".tmp")
            temporary.unlink(missing_ok=True)
            subprocess.run(command(str(temporary), objects), check=True)
            temporary.replace(output)
            listing.write_text(record)
    return library, main_library


def link_arguments(library, main_library=None):
    return [*([str(main_library)] if main_library else []), str(library), f"-Wl,-rpath,{library.parent}"]


def main():
    parser = argparse.ArgumentParser(description="Build the ps5lift runtime and SoftFloat as a shared library for a target, and its main as an archive")
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
