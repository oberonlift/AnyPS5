#!/usr/bin/env python3
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_libc_port import LIBS, compile_one, sources


def bridge_report(library):
    with tempfile.NamedTemporaryFile(suffix=".cpp") as output:
        result = subprocess.run([sys.executable, Path(__file__).resolve().parent / "generate_bridges.py", library, "-o", output.name], capture_output=True, text=True)
    lines = result.stdout.splitlines()
    if result.returncode != 0 or not lines:
        return {"error": (result.stderr.strip().splitlines() or ["generator failed"])[-1]}
    words = lines[0].split()
    return {"bridges": int(words[1]), "data": int(words[3]), "unbridged": int(words[6]), "parse_errors": int(words[10]), "reasons": [line.strip() for line in lines[1:]]}


def main():
    parser = argparse.ArgumentParser(description="Compile every AnyPS5 library for a lifted guest and generate its bridges, and report the results")
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--triple", default="aarch64-unknown-linux-gnu")
    parser.add_argument("--library", action="append")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("--json", help="also write the results to this file")
    args = parser.parse_args()

    libraries = args.library or sorted(path.name for path in (LIBS / "prx").iterdir() if path.is_dir())
    results = {}
    with tempfile.TemporaryDirectory(prefix="ps5lift-survey-") as work, concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for library in libraries:
            files = sources(library)
            compiled = list(pool.map(lambda source: compile_one(args.compiler, library, [f"--target={args.triple}"], source, work), files))
            failures = {str(source.relative_to(LIBS)): errors[:1] for source, ok, errors in compiled if not ok}
            results[library] = {"sources": len(files), "failures": failures, **bridge_report(library)}
            entry = results[library]
            print(f"{library}: {len(files) - len(failures)}/{len(files)} compile, {entry.get('bridges', '?')} bridges, {entry.get('unbridged', '?')} unbridged", flush=True)
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
