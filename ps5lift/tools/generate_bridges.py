#!/usr/bin/env python3
import argparse
import base64
import collections
import hashlib
import os
import re
import sys
from pathlib import Path

import clang.cindex as cindex

ROOT = Path(__file__).resolve().parent.parent
ANYPS5_LIBS = ROOT / "3rdparty" / "anyps5" / "core" / "libs"
NID_SUFFIX = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])
NID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
POSTFIX = "_nid_postfix"
VARIADIC_SIBLINGS = {"libc_printf": "vprintf"}
GUEST_STATE_EXPORTS = {"setjmp", "longjmp"}
EXPORT_PATTERN = re.compile(r'APS5_EXPORT\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)')
SKIPPED_DIRECTORIES = {"windows", "macos"}

K = cindex.TypeKind
INTEGER_KINDS = {
    K.BOOL, K.CHAR_U, K.UCHAR, K.CHAR16, K.CHAR32, K.USHORT, K.UINT, K.ULONG, K.ULONGLONG,
    K.CHAR_S, K.SCHAR, K.WCHAR, K.SHORT, K.INT, K.LONG, K.LONGLONG,
}
FLOAT_KINDS = {K.FLOAT, K.DOUBLE}
POINTER_KINDS = {K.POINTER, K.LVALUEREFERENCE, K.RVALUEREFERENCE}
FUNCTION_KINDS = {K.FUNCTIONPROTO, K.FUNCTIONNOPROTO}


def compute_nid(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    reversed_bytes = digest[:8][::-1]
    encoded = base64.b64encode(reversed_bytes).decode().rstrip("=")
    return encoded.translate(str.maketrans("+/", "+-"))[:11]


def bridge_symbol(name):
    symbol = "ps5lift_import_"
    for character in name:
        if character == "_":
            symbol += "__"
        elif character == "+":
            symbol += "_p"
        elif character == "-":
            symbol += "_m"
        elif character == "#":
            symbol += "_h"
        elif character.isascii() and character.isalnum():
            symbol += character
        else:
            raise ValueError(f"import name {name} has a character a bridge symbol cannot carry")
    return symbol


class Unsupported(Exception):
    pass


def classify(type_, role):
    canonical = type_.get_canonical()
    kind = canonical.kind
    if kind == K.VOID and role.endswith("return"):
        return ("void", "void")
    if kind in INTEGER_KINDS:
        return ("integer", canonical.spelling.replace("const ", "").replace("volatile ", ""))
    if kind == K.ENUM:
        underlying = canonical.get_declaration().enum_type.get_canonical()
        return ("integer", underlying.spelling)
    if kind in FLOAT_KINDS:
        return ("float", canonical.spelling.replace("const ", ""))
    if kind in POINTER_KINDS:
        pointee = canonical.get_pointee().get_canonical()
        if pointee.kind in FUNCTION_KINDS:
            if role != "parameter":
                raise Unsupported(f"function pointer as a {role}")
            return ("callback", callback_signature(pointee))
        return ("pointer", "void*")
    if kind == K.LONGDOUBLE:
        return ("extended", "long double")
    if kind == K.RECORD:
        raise Unsupported(f"{role} passed by value as a {canonical.get_size()}-byte record")
    if kind in (K.INT128, K.UINT128):
        return ("integer128", "unsigned __int128" if kind == K.UINT128 else "__int128")
    raise Unsupported(f"{role} of kind {kind.spelling}")


def callback_signature(function):
    if function.kind == K.FUNCTIONPROTO and function.is_function_variadic():
        raise Unsupported("variadic callback")
    result_class, result = classify(function.get_result(), "callback return")
    if result_class not in ("void", "integer", "pointer", "float"):
        raise Unsupported(f"callback returning {result_class}")
    arguments = []
    for argument in function.argument_types():
        argument_class, spelling = classify(argument, "callback argument")
        if argument_class not in ("integer", "pointer", "float"):
            raise Unsupported(f"callback taking {argument_class}")
        arguments.append(spelling)
    return f"{result}({', '.join(arguments)})"


def describe(cursor, symbol):
    variadic = cursor.type.kind == K.FUNCTIONPROTO and cursor.type.is_function_variadic()
    return_class = classify(cursor.result_type, "return")
    parameters = []
    integers = 0
    floats = 0
    stack = 0
    for argument in cursor.get_arguments():
        parameter_class, spelling = classify(argument.type, "parameter")
        if parameter_class == "extended":
            stack += stack % 2
            access = f"Extended(state, {stack})"
            stack += 2
        elif parameter_class == "integer128":
            if integers + 2 <= 6:
                access = f"Integer128<{spelling}>(state, {integers})"
                integers += 2
            else:
                stack += stack % 2
                access = f"Stack128<{spelling}>(state, {stack})"
                stack += 2
        elif parameter_class == "float":
            access = f"Floating<{spelling}>(state, {floats})" if floats < 8 else f"Stack<{spelling}>(state, {stack})"
            if floats < 8:
                floats += 1
            else:
                stack += 1
        elif parameter_class == "callback":
            source = f"Integer<std::uint64_t>(state, {integers})" if integers < 6 else f"Stack<std::uint64_t>(state, {stack})"
            access = f"reinterpret_cast<void*>(Callback<{spelling}>({source}))"
            spelling = "void*"
            if integers < 6:
                integers += 1
            else:
                stack += 1
        else:
            access = f"Integer<{spelling}>(state, {integers})" if integers < 6 else f"Stack<{spelling}>(state, {stack})"
            if integers < 6:
                integers += 1
            else:
                stack += 1
        parameters.append((spelling, access))
    return {"symbol": symbol, "return": return_class, "parameters": parameters, "variadic": (integers, floats, stack) if variadic else None}


def translation_unit_arguments(library):
    third_party = ANYPS5_LIBS.parent.parent / "3rdparty"
    return ["-x", "c++", "-std=c++20", f"-I{ANYPS5_LIBS}", f"-I{library / 'include'}", f"-I{third_party / 'ZArchive' / 'include'}", "-D__linux__", "-fno-delayed-template-parsing"]


def library_sources(library):
    for path in sorted(library.rglob("*.cpp")):
        if SKIPPED_DIRECTORIES.intersection(path.relative_to(library).parts):
            continue
        yield path


def collect(library, index):
    explicit = {}
    for source in library_sources(library):
        for nid, function in EXPORT_PATTERN.findall(source.read_text(errors="replace")):
            explicit[function] = nid

    exports = {}
    data = {}
    unsupported = {}
    parse_errors = 0
    for source in library_sources(library):
        unit = index.parse(str(source), args=translation_unit_arguments(library), options=cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
        parse_errors += sum(1 for diagnostic in unit.diagnostics if diagnostic.severity >= cindex.Diagnostic.Error)
        stack = [unit.cursor]
        while stack:
            cursor = stack.pop()
            if cursor.kind in (cindex.CursorKind.TRANSLATION_UNIT, cindex.CursorKind.LINKAGE_SPEC, cindex.CursorKind.NAMESPACE, cindex.CursorKind.UNEXPOSED_DECL):
                stack.extend(cursor.get_children())
                continue
            if cursor.kind not in (cindex.CursorKind.FUNCTION_DECL, cindex.CursorKind.VAR_DECL):
                continue
            name = cursor.spelling
            if name.endswith(POSTFIX):
                nid = compute_nid(name[: -len(POSTFIX)])
            elif name in explicit:
                nid = explicit[name]
            else:
                continue
            if cursor.mangled_name not in (name, "_" + name):
                unsupported.setdefault(nid, (name, "C++ linkage"))
                continue
            if cursor.kind == cindex.CursorKind.VAR_DECL:
                data.setdefault(nid, name)
                continue
            if nid in exports or nid in unsupported:
                continue
            if (name[: -len(POSTFIX)] if name.endswith(POSTFIX) else name) in GUEST_STATE_EXPORTS:
                unsupported[nid] = (name, "saves or restores the guest's own registers")
                continue
            try:
                exports[nid] = describe(cursor, name)
                exports[nid]["name"] = name
            except Unsupported as reason:
                unsupported[nid] = (name, str(reason))
    by_name = {export["name"]: export for export in exports.values()}
    for nid in [nid for nid, export in exports.items() if export["variadic"]]:
        export = exports.pop(nid)
        base = export["name"][: -len(POSTFIX)] if export["name"].endswith(POSTFIX) else export["name"]
        sibling = by_name.get(VARIADIC_SIBLINGS.get(base, "v" + base) + POSTFIX)
        named = [spelling for spelling, _ in export["parameters"]]
        if sibling is None or sibling["variadic"] or [spelling for spelling, _ in sibling["parameters"]] != named + ["void*"]:
            unsupported[nid] = (export["name"], "variadic without a va_list sibling")
            continue
        export["target"] = sibling["symbol"]
        exports[nid] = export
    return exports, data, unsupported, parse_errors


def data_symbol(nid):
    return "ps5lift_data_" + bridge_symbol(nid)[len("ps5lift_import_"):]


def emit(exports, data, library_name):
    lines = [f"#include <ps5lift/Bridge.hpp>", "", "#include <cstddef>", "#include <cstdint>", "", "using namespace Ps5Lift::Bridge;", ""]
    for nid in sorted(exports):
        export = exports[nid]
        if export["variadic"]:
            continue
        return_class, return_spelling = export["return"]
        declaration = ", ".join(spelling for spelling, _ in export["parameters"])
        lines.append(f'extern "C" {return_spelling} {export["symbol"]}({declaration});')
    for nid in sorted(data):
        lines.append(f'extern "C" char {data[nid]};')
        lines.append(f'extern "C" void* const {data_symbol(nid)} = &{data[nid]};')
    lines.append("")
    for nid in sorted(exports):
        export = exports[nid]
        return_class, _ = export["return"]
        accesses = [access for _, access in export["parameters"]]
        if export["variadic"]:
            accesses.append("variadic.List()")
        call = f'{export.get("target", export["symbol"])}({", ".join(accesses)})'
        lines.append(f'extern "C" void {bridge_symbol(nid)}(Ps5LiftState* state) {{')
        lines.append("    try {")
        if export["variadic"]:
            integers, floats, stack = export["variadic"]
            lines.append(f"        VariadicArguments variadic(state, {integers}, {floats}, {stack});")
        if return_class == "extended" or any(access.startswith("Extended(") for _, access in export["parameters"]):
            lines.append("        const ExtendedScope scope(state);")
        if return_class == "void":
            lines.append(f"        {call};")
        elif return_class == "extended":
            lines.append(f"        ReturnExtended(state, {call});")
        else:
            lines.append(f"        Return(state, {call});")
        lines.append("    } catch (const GuestUnwind&) {")
        lines.append("    }")
        lines.append("}")
        lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Generate SysV-to-native import bridges for an AnyPS5 library")
    parser.add_argument("library", help="library directory name under core/libs/prx, such as libc")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--libclang", default="/usr/lib/llvm-18/lib/libclang-18.so.1")
    parser.add_argument("--report", action="store_true", help="list every export without a bridge and why")
    args = parser.parse_args()

    if os.path.exists(args.libclang):
        cindex.Config.set_library_file(args.libclang)
    library = ANYPS5_LIBS / "prx" / args.library
    if not library.is_dir():
        raise SystemExit(f"{library} is not an AnyPS5 library directory")
    exports, data, unsupported, parse_errors = collect(library, cindex.Index.create())
    Path(args.output).write_text(emit(exports, data, args.library))

    reasons = collections.Counter(re.sub(r"\d+-byte", "N-byte", reason) for _, reason in unsupported.values())
    print(f"{args.library}: {len(exports)} bridges, {len(data)} data exports, {len(unsupported)} exports without one, {parse_errors} parse errors")
    for reason, count in reasons.most_common():
        print(f"  {count:5} {reason}")
    if args.report:
        for nid, (name, reason) in sorted(unsupported.items(), key=lambda item: item[1]):
            print(f"  {nid} {name}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
