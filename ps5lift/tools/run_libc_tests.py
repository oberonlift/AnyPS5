#!/usr/bin/env python3
import argparse
import concurrent.futures
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anyps5_build import LIBS, Build, compiler_arguments
from build_runtime import build as build_runtime, link_arguments
from run_fixture import ROOT, host_triple

TESTS = {
    "Formatting": ["libc"], "GuestStrings": ["libc", "libScePosixForWebKit"], "GuestWideFormatting": ["libc"], "GuestStdioVariadic": ["libc"],
    "GuestAllocatingStrings": ["libc"], "GuestSyslog": ["libc"], "GuestWideStreamScan": ["libc"],
    "GuestNetworkOffline": ["libSceNetCtl", "libScePosixForWebKit", "libkernel", "libc"], "GuestTcp": ["libkernel", "libc"],
    "GuestSocketOptions": ["libkernel", "libc"], "GuestResolver": ["libScePosixForWebKit", "libSceNet", "libc"], "ResolverRaw": ["libScePosixForWebKit", "libc"],
}
IMAGE_STUB = ROOT / "tests" / "libc" / "image_stub.cpp"


def up_to_date(output):
    depfile = output.with_suffix(".d")
    if not output.exists() or not depfile.exists():
        return False
    dependencies = depfile.read_text().replace("\\\n", " ").split(":", 1)[1].split()
    built = output.stat().st_mtime
    return all(Path(dependency).exists() and Path(dependency).stat().st_mtime <= built for dependency in dependencies)


def compile_all(clangxx, units):
    def one(unit):
        source, arguments, output = unit
        if up_to_date(output):
            return None
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [clangxx, *arguments, "-MMD", "-MF", str(output.with_suffix(".d")), "-c", str(source), "-o", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        return None if result.returncode == 0 else f"{source}:\n{result.stderr}"
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        errors = [error for error in pool.map(one, units) if error]
    if errors:
        raise SystemExit("\n".join(errors))


def main():
    parser = argparse.ArgumentParser(description="Run upstream's libc tests against AnyPS5's libraries built for a lifted guest")
    parser.add_argument("--anyps5-build", required=True, help="AnyPS5's build directory, configured with ANYPS5_GUEST_LIFTED and BUILD_TESTING")
    parser.add_argument("--clang", required=True)
    parser.add_argument("--clangxx", required=True)
    parser.add_argument("--build", default=str(ROOT / "build"))
    parser.add_argument("--test", action="append", choices=sorted(TESTS))
    args = parser.parse_args()

    anyps5 = Build(args.anyps5_build)
    triple = host_triple()
    work = Path(args.build) / f"libc-tests-{triple.split('-')[0]}"
    tests = args.test or list(TESTS)
    stub = work / "image_stub.o"
    units = [(IMAGE_STUB, ["-std=c++20", "-O1", "-fPIC", f"-I{ROOT / 'runtime' / 'include'}"], stub)]
    for test in tests:
        source = LIBS / "tests" / f"{test}.cpp"
        units.append((source, compiler_arguments(anyps5.source(source)), work / f"{test}.o"))
    compile_all(args.clangxx, units)
    runtime, _ = build_runtime(triple, Path(args.build) / f"runtime-{triple.split('-')[0]}", args.clang, args.clangxx, os.cpu_count())

    failures = 0
    for test in tests:
        program = work / test
        libraries = [str(anyps5.library(name)) for name in TESTS[test]]
        link = [args.clangxx, "-fuse-ld=lld", str(work / f"{test}.o"), str(stub), *libraries, *link_arguments(runtime), f"-Wl,-rpath,{anyps5.libraries_directory}",
                "-latomic", "-pthread", "-ldl", "-o", str(program)]
        linked = subprocess.run(link, capture_output=True, text=True)
        if linked.returncode != 0:
            print(f"FAIL {test}: link\n{linked.stderr[-2000:]}")
            failures += 1
            continue
        result = subprocess.run([str(program)], capture_output=True, text=True, cwd=work, timeout=600)
        ok = result.returncode == 0
        print(f"{'PASS' if ok else 'FAIL'} {test}")
        if not ok:
            sys.stdout.write(f"exit code {result.returncode}\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
