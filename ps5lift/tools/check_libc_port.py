#!/usr/bin/env python3
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ANYPS5 = ROOT / "3rdparty" / "anyps5"
LIBS = ANYPS5 / "core" / "libs"


def sources(library):
    listing = (LIBS / "prx" / library / "CMakeLists.txt").read_text()
    found = re.findall(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^\s)]+\.cpp)", listing)
    return sorted({LIBS / "prx" / library / path for path in found})


def compile_one(compiler, flags, source, work):
    output = Path(work) / (source.name + ".o")
    command = [compiler, "-std=c++20", "-O1", "-fPIC", "-c", "-DANYPS5_GUEST_LIFTED", f"-I{LIBS}", f"-I{LIBS / 'prx' / 'libc' / 'include'}",
               f"-I{ANYPS5 / '3rdparty' / 'ZArchive' / 'include'}", f"-I{ROOT / 'runtime' / 'include'}", *flags, str(source), "-o", str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    errors = [line for line in result.stderr.splitlines() if "error" in line]
    return source, result.returncode == 0, errors[:3]


def main():
    parser = argparse.ArgumentParser(description="Compile an AnyPS5 library's sources for the lifted guest and report which fail")
    parser.add_argument("--library", default="libc")
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--flag", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()

    files = sources(args.library)
    failed = []
    with tempfile.TemporaryDirectory(prefix="ps5lift-port-") as work, concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for source, ok, errors in pool.map(lambda path: compile_one(args.compiler, args.flag, path, work), files):
            if not ok:
                failed.append((source, errors))
    print(f"{args.library} with {args.compiler}: {len(files) - len(failed)} of {len(files)} sources compile")
    for source, errors in failed:
        print(f"  {source.relative_to(LIBS)}")
        for error in errors:
            print(f"      {error.replace(str(LIBS) + '/', '')[:200]}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
