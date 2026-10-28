#!/usr/bin/env python3
import argparse
import concurrent.futures
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_runtime import build as build_runtime
from check_libc_port import ANYPS5, LIBS, sources
from run_fixture import ROOT, host_triple, runner_for

TESTS = [
    "Formatting", "GuestStrings", "GuestWideFormatting", "GuestStdioVariadic", "GuestAllocatingStrings", "GuestSyslog", "GuestWideStreamScan",
]
EXTRA_SOURCES = {"GuestStrings": ["prx/libScePosixForWebKit/Export.cpp"]}
IMAGE_STUB = ROOT / "tests" / "libc" / "image_stub.cpp"
ZARCHIVE_STUB = ROOT / "tests" / "libc" / "zarchive_stub.cpp"


def up_to_date(output):
    depfile = output.with_suffix(".d")
    if not output.exists() or not depfile.exists():
        return False
    dependencies = depfile.read_text().replace("\\\n", " ").split(":", 1)[1].split()
    built = output.stat().st_mtime
    return all(Path(dependency).exists() and Path(dependency).stat().st_mtime <= built for dependency in dependencies)


def compile_all(clangxx, triple, units):
    def one(unit):
        source, output = unit
        if up_to_date(output):
            return None
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [clangxx, f"--target={triple}", "-std=c++20", "-O1", "-fPIC", "-ffunction-sections", "-fdata-sections", "-w", "-DANYPS5_GUEST_LIFTED", f"-I{LIBS}", f"-I{LIBS / 'prx' / 'libc' / 'include'}",
                   f"-I{ANYPS5 / '3rdparty' / 'ZArchive' / 'include'}", f"-I{ROOT / 'runtime' / 'include'}", "-MMD", "-MF", str(output.with_suffix(".d")), "-c", str(source), "-o", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        return None if result.returncode == 0 else f"{source}:\n{result.stderr}"
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        errors = [error for error in pool.map(one, units) if error]
    if errors:
        raise SystemExit("\n".join(errors))


def libc_units(triple, build):
    work = Path(build) / f"libc-{triple.split('-')[0]}"
    units = [(source, work / "libc" / (str(source.relative_to(LIBS)).replace("/", "_") + ".o")) for source in sources("libc")]
    return units + [(ZARCHIVE_STUB, work / "stubs" / "zarchive_stub.o")]


def build_libc(triple, clangxx, build):
    units = libc_units(triple, build)
    compile_all(clangxx, triple, units)
    return [output for _, output in units]


def main():
    parser = argparse.ArgumentParser(description="Build AnyPS5's libc for a lifted guest and run upstream's libc tests against it")
    parser.add_argument("--triple", default=host_triple())
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--build", default=str(ROOT / "build"))
    parser.add_argument("--test", action="append", choices=TESTS)
    args = parser.parse_args()

    arch = args.triple.split("-")[0]
    work = Path(args.build) / f"libc-{arch}"
    library_units = libc_units(args.triple, args.build)
    stub_units = [(IMAGE_STUB, work / "stubs" / "image_stub.o")]
    tests = args.test or TESTS
    test_units = [(LIBS / "tests" / f"{test}.cpp", work / "tests" / f"{test}.o") for test in tests]
    extra_units = {test: [(LIBS / path, work / "extra" / (path.replace("/", "_") + ".o")) for path in EXTRA_SOURCES.get(test, [])] for test in tests}
    compile_all(args.clangxx, args.triple, library_units + stub_units + test_units + [unit for units in extra_units.values() for unit in units])
    runtime, _ = build_runtime(args.triple, Path(args.build) / f"runtime-{arch}", args.clang, args.clangxx, os.cpu_count())

    runner = runner_for(args.triple)
    failures = 0
    for test, (_, test_object) in zip(tests, test_units):
        program = work / test
        link = [args.clangxx, f"--target={args.triple}", "-fuse-ld=lld", "-Wl,--gc-sections", str(test_object), *(str(output) for _, output in extra_units[test] + library_units + stub_units),
                str(runtime), "-latomic", "-pthread", "-ldl", "-o", str(program)]
        linked = subprocess.run(link, capture_output=True, text=True)
        if linked.returncode != 0:
            print(f"FAIL {test} {args.triple}: link\n{linked.stderr[-2000:]}")
            failures += 1
            continue
        result = subprocess.run([*runner, str(program)], capture_output=True, text=True, cwd=work, timeout=600)
        ok = result.returncode == 0
        print(f"{'PASS' if ok else 'FAIL'} {test} {args.triple}")
        if not ok:
            sys.stdout.write(f"exit code {result.returncode}\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
