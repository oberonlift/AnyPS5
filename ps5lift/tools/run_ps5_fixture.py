#!/usr/bin/env python3
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_fixture import FIXTURES, ROOT, host_triple, lifter_features, run, runner_for

GENERATORS = ROOT / "3rdparty" / "anyps5" / "core" / "relinker" / "relinker" / "tests" / "macos"
EXPECTED = {
    "import": ("hello from a PS5 ELF on macOS\n", 0),
    "tls": ("", 0x1288 & 0xff),
}


def main():
    parser = argparse.ArgumentParser(description="Relink an AnyPS5 PS5-style fixture, lift it and check its output and exit code")
    parser.add_argument("--ps5lift", required=True)
    parser.add_argument("--relinker", required=True)
    parser.add_argument("--clangxx", default="clang++")
    parser.add_argument("--triple", default=host_triple())
    parser.add_argument("--fixture", choices=sorted(EXPECTED), required=True)
    args = parser.parse_args()

    runner = runner_for(args.triple)
    expected_output, expected_code = EXPECTED[args.fixture]
    work = Path(tempfile.mkdtemp(prefix="ps5lift-ps5-"))
    failures = 0
    try:
        source = work / f"{args.fixture}.elf"
        run([sys.executable, "-I", GENERATORS / f"{args.fixture}_fixture.py", source])
        relinked = work / f"{args.fixture}.out"
        run([args.relinker, "--skip-sce-module", source, relinked])
        for memory in ("tso", "relaxed"):
            lifted = work / f"{args.fixture}-{memory}.o"
            report = run([args.ps5lift, relinked, "-o", lifted, "--triple", args.triple, "--memory", memory, *lifter_features(args.triple, runner)]).stderr.strip()
            program = work / f"{args.fixture}-{memory}"
            run([args.clangxx, f"--target={args.triple}", "-std=c++20", "-O2", f"-I{ROOT / 'runtime' / 'include'}", ROOT / "runtime" / "src" / "Runtime.cpp",
                 FIXTURES / "ps5_bridge.cpp", lifted, "-fuse-ld=lld", "-pthread", "-o", program])
            result = subprocess.run([*runner, program], capture_output=True, text=True)
            ok = result.stdout == expected_output and result.returncode == expected_code
            if not ok:
                sys.stderr.write(f"{memory}: exit code {result.returncode} (expected {expected_code}), output {result.stdout!r} (expected {expected_output!r})\n{result.stderr}")
            print(f"{'PASS' if ok else 'FAIL'} {args.fixture} {args.triple} {memory}: {report}")
            failures += not ok
    finally:
        shutil.rmtree(work)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
