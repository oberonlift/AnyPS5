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
    found = set()
    skipped = []
    for line in (LIBS / "prx" / library / "CMakeLists.txt").read_text().splitlines():
        condition = re.match(r"\s*(if|elseif|else|endif)\s*\((.*)\)", line)
        if condition:
            keyword, expression = condition.groups()
            if keyword == "if":
                skipped.append(bool(re.fullmatch(r"\s*(WIN32|APPLE|MSVC)\s*", expression)))
            elif keyword == "endif":
                skipped.pop()
            elif skipped:
                skipped[-1] = False
            continue
        if any(skipped):
            continue
        found.update(LIBS / "prx" / library / path for path in re.findall(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^\s)]+\.cpp)", line))
        found.update(LIBS / path for path in re.findall(r"\$\{LIBS_INCLUDE_DIR\}/([^\s)]+\.cpp)", line) if not path.startswith("tests/"))
    return sorted(found)


def include_flags(library):
    return [f"-I{LIBS}", f"-I{LIBS / 'nid' / 'include'}", f"-I{LIBS / 'prx' / library / 'include'}", f"-I{LIBS / 'prx' / 'libc' / 'include'}",
            f"-I{ANYPS5 / '3rdparty' / 'ZArchive' / 'include'}", f"-I{ROOT / 'runtime' / 'include'}"]


def compile_one(compiler, library, flags, source, work):
    output = Path(work) / (source.name + ".o")
    command = [compiler, "-std=c++20", "-O1", "-fPIC", "-c", "-DANYPS5_GUEST_LIFTED", *include_flags(library), *flags, str(source), "-o", str(output)]
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
        for source, ok, errors in pool.map(lambda path: compile_one(args.compiler, args.library, args.flag, path, work), files):
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
