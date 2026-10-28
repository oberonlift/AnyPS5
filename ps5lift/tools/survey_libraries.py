#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anyps5_build import LIBS


def bridge_report(library, anyps5_build):
    with tempfile.NamedTemporaryFile(suffix=".cpp") as output:
        result = subprocess.run([sys.executable, Path(__file__).resolve().parent / "generate_bridges.py", library, "--anyps5-build", anyps5_build, "-o", output.name],
                                capture_output=True, text=True)
    lines = result.stdout.splitlines()
    if result.returncode != 0 or not lines:
        return {"error": (result.stderr.strip().splitlines() or ["generator failed"])[-1]}
    words = lines[0].split()
    return {"bridges": int(words[1]), "data": int(words[3]), "unbridged": int(words[6]), "parse_errors": int(words[10]), "reasons": [line.strip() for line in lines[1:]]}


def main():
    parser = argparse.ArgumentParser(description="Generate the bridges of every AnyPS5 library and report how many of its exports they cover")
    parser.add_argument("--anyps5-build", required=True, help="AnyPS5's build directory, configured with CMAKE_EXPORT_COMPILE_COMMANDS=ON")
    parser.add_argument("--library", action="append")
    parser.add_argument("--json", help="also write the results to this file")
    args = parser.parse_args()

    libraries = args.library or sorted(path.name for path in (LIBS / "prx").iterdir() if path.is_dir())
    results = {}
    for library in libraries:
        entry = results[library] = bridge_report(library, args.anyps5_build)
        if "error" in entry:
            print(f"{library}: {entry['error']}", flush=True)
        else:
            print(f"{library}: {entry['bridges']} bridges, {entry['data']} data, {entry['unbridged']} unbridged, {entry['parse_errors']} parse errors", flush=True)
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
