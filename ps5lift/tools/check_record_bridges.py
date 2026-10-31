#!/usr/bin/env python3
import argparse
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import clang.cindex as cindex

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_bridges

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tests" / "fixtures" / "records.cpp"
GUEST_TARGET = "--target=x86_64-unknown-linux-gnu"
FLAGS = ["-x", "c++", "-std=c++20", GUEST_TARGET]
# The hosts the bridges are compiled for: both Linux targets on Linux, which has headers for each, and the Mac itself.
HOST_TARGETS = ["arm64-apple-macosx"] if platform.system() == "Darwin" else ["x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"]
UNSUPPORTED = {
    "Packed": "a packed record",
    "Bits": "a record with bit-fields",
    "HoldsFunction": "a record holding a function pointer",
    "PaddedEightbyte": "a record with an eightbyte of padding only",
}
DECLARATION = re.compile(r"^declare (?P<result>.+?) @(?P<name>(?:take|give)_\w+)\((?P<parameters>.*)\)", re.MULTILINE)


def ir_classes(text):
    if "byval(" in text or "sret(" in text:
        return "memory"
    classes = []
    for part in re.split(r",\s*(?![^<]*>)", text.strip().strip("{}").strip()):
        match = re.search(r"<2 x float>|\bi(\d+)\b|\b(ptr)\b|\bfloat\b|\bdouble\b", part)
        if match is None:
            raise ValueError(f"unexpected lowered type {part!r} in {text!r}")
        if match.group(1):
            classes += ["integer"] * ((int(match.group(1)) + 63) // 64)
        else:
            classes.append("integer" if match.group(2) else "sse")
    return tuple(classes)


def lowered(clang):
    ir = subprocess.run([clang, *FLAGS, "-O0", "-S", "-emit-llvm", "-o", "-", SOURCE], check=True, capture_output=True, text=True).stdout
    result = {}
    for match in DECLARATION.finditer(ir):
        kind, record = match.group("name").split("_", 1)
        if kind == "take":
            result[("parameter", record)] = ir_classes(match.group("parameters"))
        elif match.group("result").strip() == "void":
            result[("return", record)] = ir_classes(match.group("parameters"))
        else:
            result[("return", record)] = ir_classes(match.group("result"))
    return result


def classified(extra):
    unit = cindex.Index.create().parse(str(SOURCE), args=[*FLAGS, *extra])
    errors = [diagnostic.spelling for diagnostic in unit.diagnostics if diagnostic.severity >= cindex.Diagnostic.Error]
    if errors:
        raise SystemExit(f"{SOURCE}: {errors}")
    result = {}
    stack = list(unit.cursor.get_children())
    while stack:
        cursor = stack.pop()
        if cursor.kind in (cindex.CursorKind.LINKAGE_SPEC, cindex.CursorKind.UNEXPOSED_DECL):
            stack.extend(cursor.get_children())
            continue
        if cursor.kind != cindex.CursorKind.FUNCTION_DECL or not cursor.spelling.startswith(("take_", "give_")):
            continue
        kind, record = cursor.spelling.split("_", 1)
        role = "parameter" if kind == "take" else "return"
        type_ = next(cursor.get_arguments()).type if kind == "take" else cursor.result_type
        try:
            result[(role, record)] = generate_bridges.record(type_)
        except generate_bridges.Unsupported as reason:
            result[(role, record)] = str(reason)
    return result


def main():
    parser = argparse.ArgumentParser(description="Check the bridge generator's SysV classification of records against clang's own lowering")
    parser.add_argument("--clang", required=True, help="clang, to lower calls for x86-64 and to compile the generated record layouts")
    parser.add_argument("--libclang", type=Path, help="the libclang library to parse with; the bindings find one by default")
    args = parser.parse_args()

    extra = []
    if args.libclang is not None:
        generate_bridges.use_libclang(str(args.libclang))
        extra = ["-resource-dir", str(generate_bridges.resource_directory(args.libclang))]
    expected = lowered(args.clang)
    actual = classified(extra)
    failures = [f"{record} as a {role}: lowered by clang but not classified" for role, record in sorted(set(expected) - set(actual))]
    definitions = []
    for key in sorted(actual):
        role, record = key
        result = actual[key]
        if record in UNSUPPORTED:
            if result != UNSUPPORTED[record]:
                failures.append(f"{record} as a {role}: expected refusal {UNSUPPORTED[record]!r}, got {result!r}")
            continue
        if isinstance(result, str):
            failures.append(f"{record} as a {role}: refused ({result})")
            continue
        if key not in expected:
            failures.append(f"{record} as a {role}: clang lowered no declaration")
        elif result.eightbytes != expected[key]:
            failures.append(f"{record} as a {role}: classified {result.eightbytes}, clang passes {expected[key]}")
        definitions.extend(definition for definition in result.definitions if definition not in definitions)
    with tempfile.TemporaryDirectory() as work:
        layouts = Path(work) / "layouts.cpp"
        layouts.write_text("#include <cstddef>\n\n" + "\n".join(definitions) + "\n")
        for triple in HOST_TARGETS:
            compiled = subprocess.run([args.clang, "-x", "c++", "-std=c++20", f"--target={triple}", "-fsyntax-only", layouts], capture_output=True, text=True)
            if compiled.returncode != 0:
                failures.append(f"generated layouts do not compile for {triple}:\n{compiled.stderr}")
    for failure in failures:
        print(f"FAIL {failure}")
    checked = sum(1 for record in {record for _, record in actual} if record not in UNSUPPORTED)
    if not checked:
        failures.append("no record was checked")
        print("FAIL no record was checked")
    print(f"{'FAIL' if failures else 'PASS'} records: {checked} shapes as parameters and returns, {len(UNSUPPORTED)} refused")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
