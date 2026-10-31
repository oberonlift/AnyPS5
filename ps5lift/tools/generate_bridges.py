#!/usr/bin/env python3
import argparse
import base64
import collections
import concurrent.futures
import hashlib
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

import clang.cindex as cindex

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anyps5_build import Build, compiler_arguments

NID_SUFFIX = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])
NID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"
POSTFIX = "_nid_postfix"
UNPATCHED = "_nid_no_patch"
VARIADIC_SIBLINGS = {"libc_printf": "vprintf"}
GUEST_STATE_EXPORTS = {"setjmp", "longjmp", "sigsetjmp", "siglongjmp", "_setjmp", "_longjmp"}
EXPORT_PATTERN = re.compile(r'APS5_EXPORT\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)')

K = cindex.TypeKind
INTEGER_KINDS = {
    K.BOOL, K.CHAR_U, K.UCHAR, K.CHAR16, K.CHAR32, K.USHORT, K.UINT, K.ULONG, K.ULONGLONG,
    K.CHAR_S, K.SCHAR, K.WCHAR, K.SHORT, K.INT, K.LONG, K.LONGLONG,
}
FLOAT_KINDS = {K.FLOAT, K.DOUBLE}
# A lifted AnyPS5 libc keeps the guest's long double, x87's 80 bits, in this class, which crosses a bridge as long
# double does: in a 16-byte stack slot, and back on the x87 stack. Bridges declare it as the runtime's own type for the
# guest's long double, which is laid out and passed the same way.
GUEST_LONG_DOUBLE = "LibcDetail::GuestLongDouble"
BRIDGE_LONG_DOUBLE = "Ps5Lift::Bridge::LongDouble"
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


Record = collections.namedtuple("Record", "name definitions eightbytes size")


def scalar(canonical):
    if canonical.kind in INTEGER_KINDS or canonical.kind in (K.INT128, K.UINT128):
        return canonical.spelling.replace("const ", "").replace("volatile ", ""), "integer"
    if canonical.kind in FLOAT_KINDS:
        return canonical.spelling.replace("const ", "").replace("volatile ", ""), "sse"
    if canonical.kind == K.ENUM:
        return canonical.get_declaration().enum_type.get_canonical().spelling, "integer"
    if canonical.kind in POINTER_KINDS:
        if canonical.get_pointee().get_canonical().kind in FUNCTION_KINDS:
            raise Unsupported("a record holding a function pointer")
        return "void*", "integer"
    raise Unsupported(f"a record holding a {canonical.kind.spelling} field")


def fields(canonical):
    for field in canonical.get_fields():
        if field.is_bitfield():
            raise Unsupported("a record with bit-fields")
        yield field, field.type.get_canonical(), field.get_field_offsetof() // 8


def leaves(canonical, offset):
    if canonical.kind == K.RECORD:
        for _, field_type, field_offset in fields(canonical):
            yield from leaves(field_type, offset + field_offset)
    elif canonical.kind == K.CONSTANTARRAY:
        element = canonical.element_type.get_canonical()
        for index in range(canonical.element_count):
            yield from leaves(element, offset + index * element.get_size())
    else:
        yield offset, canonical.get_size(), canonical.get_align(), scalar(canonical)[1]


def eightbytes(canonical):
    size = canonical.get_size()
    if size > 16:
        return "memory"
    classes = [None] * ((size + 7) // 8)
    for offset, length, alignment, kind in leaves(canonical, 0):
        if offset % alignment:
            return "memory"
        for eightbyte in range(offset // 8, (offset + length - 1) // 8 + 1):
            if classes[eightbyte] != "integer":
                classes[eightbyte] = kind
    if None in classes:
        raise Unsupported("a record with an eightbyte of padding only")
    return tuple(classes)


def declarator(canonical, definitions):
    if canonical.kind == K.RECORD:
        nested = record(canonical)
        definitions.extend(definition for definition in nested.definitions if definition not in definitions)
        return nested.name, ""
    if canonical.kind == K.CONSTANTARRAY:
        if canonical.element_count == 0:
            raise Unsupported("a record with a zero-length array")
        spelling, extent = declarator(canonical.element_type.get_canonical(), definitions)
        return spelling, f"[{canonical.element_count}]{extent}"
    return scalar(canonical)[0], ""


def record(type_):
    canonical = type_.get_canonical()
    declaration = canonical.get_declaration()
    size, alignment = canonical.get_size(), canonical.get_align()
    if size < 0 or alignment < 0:
        raise Unsupported("a record of unknown layout")
    if not canonical.is_pod():
        raise Unsupported("a record that is not plain old data")
    if any(child.kind == cindex.CursorKind.CXX_BASE_SPECIFIER for child in declaration.get_children()):
        raise Unsupported("a record with a base class")
    union = declaration.kind == cindex.CursorKind.UNION_DECL
    definitions = []
    members = []
    offsets = []
    position = 0
    for index, (_, field_type, offset) in enumerate(fields(canonical)):
        spelling, extent = declarator(field_type, definitions)
        field_alignment = field_type.get_align()
        if field_alignment > alignment or offset % field_alignment or (not union and offset < position):
            raise Unsupported("a packed record")
        if not union and offset != (position + field_alignment - 1) // field_alignment * field_alignment:
            members.append(f"unsigned char padding{index}[{offset - position}];")
        members.append(f"{spelling} field{index}{extent};")
        offsets.append((f"field{index}", offset))
        position = max(position, offset + field_type.get_size())
    if not members:
        raise Unsupported("an empty record")
    if (position + alignment - 1) // alignment * alignment != size:
        members.append(f"unsigned char padding[{size if union else size - position}];")
    keyword = "union" if union else "struct"
    body = " ".join(members)
    name = "Ps5LiftRecord" + hashlib.sha1(f"{keyword} {alignment} {body}".encode()).hexdigest()[:12]
    checks = [f"sizeof({name}) == {size}", f"alignof({name}) == {alignment}"] + [f"offsetof({name}, {field}) == {offset}" for field, offset in offsets]
    definitions.append("\n".join([f"{keyword} alignas({alignment}) {name} {{ {body} }};"] + [f"static_assert({check});" for check in checks]))
    return Record(name, tuple(definitions), eightbytes(canonical), size)


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
            if role == "parameter":
                return ("callback", callback_signature(pointee))
            if role == "return":
                return ("function", callback_signature(pointee))
            raise Unsupported(f"function pointer as a {role}")
        return ("pointer", "void*")
    if kind == K.LONGDOUBLE:
        return ("extended", "long double")
    if kind == K.RECORD and canonical.spelling == GUEST_LONG_DOUBLE:
        return ("extended", BRIDGE_LONG_DOUBLE)
    if kind in (K.INCOMPLETEARRAY, K.CONSTANTARRAY) and role == "parameter":
        return ("pointer", "void*")
    if kind == K.RECORD:
        try:
            return ("record", record(canonical))
        except Unsupported as reason:
            raise Unsupported(f"{role} passed by value as {reason}")
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
    records = list(return_class[1].definitions) if return_class[0] == "record" else []
    parameters = []
    integers = 1 if return_class[0] == "record" and return_class[1].eightbytes == "memory" else 0
    floats = 0
    stack = 0
    for argument in cursor.get_arguments():
        parameter_class, spelling = classify(argument.type, "parameter")
        if parameter_class == "record":
            argument_record = spelling
            spelling = argument_record.name
            records.extend(definition for definition in argument_record.definitions if definition not in records)
            classes = argument_record.eightbytes
            in_registers = classes != "memory" and integers + classes.count("integer") <= 6 and floats + classes.count("sse") <= 8
            if in_registers:
                sources = []
                for kind in classes:
                    if kind == "integer":
                        sources.append(f"Integer<std::uint64_t>(state, {integers})")
                        integers += 1
                    else:
                        sources.append(f"Floating<std::uint64_t>(state, {floats})")
                        floats += 1
                access = f"Record<{spelling}>({', '.join(sources)})"
            else:
                stack += stack % 2 if argument.type.get_canonical().get_align() > 8 else 0
                access = f"StackRecord<{spelling}>(state, {stack})"
                stack += (argument_record.size + 7) // 8
        elif parameter_class == "extended":
            stack += stack % 2
            access = f"HostExtended(state, {stack})" if spelling == "long double" else f"Extended<{spelling}>(state, {stack})"
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
    if return_class[0] == "record":
        return_class = ("record", (return_class[1].name, return_class[1].eightbytes))
    return {"symbol": symbol, "return": return_class, "parameters": parameters, "records": records, "variadic": (integers, floats, stack) if variadic else None}


DISAMBIGUATION = re.compile(r"_nid_disambig\d+$")


def export_base(name):
    if name.endswith(POSTFIX):
        name = name[: -len(POSTFIX)]
    return DISAMBIGUATION.sub("", name)


class Definitions:
    def __init__(self):
        self.texts = {}

    def __call__(self, cursor):
        location = cursor.location.file
        if location is None or not str(location).endswith(".cpp"):
            return False
        text = self.texts.setdefault(str(location), Path(str(location)).read_bytes())
        if cursor.kind == cindex.CursorKind.VAR_DECL:
            declaration = text[cursor.extent.start.offset:cursor.extent.end.offset]
            return not declaration.lstrip().startswith(b"extern") or b"=" in declaration
        position = cursor.extent.end.offset
        while position < len(text) and text[position:position + 1] not in (b"{", b";", b"}"):
            position += 1
        return text[position:position + 1] == b"{"


def scan_source(job):
    source, arguments, explicit = job
    unit = cindex.Index.create().parse(str(source), args=arguments, options=cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES)
    errors = [f"{diagnostic.location.file}:{diagnostic.location.line}: {diagnostic.spelling}" for diagnostic in unit.diagnostics if diagnostic.severity >= cindex.Diagnostic.Error]
    defined = Definitions()
    candidates = []
    stack = [unit.cursor]
    while stack:
        cursor = stack.pop()
        if cursor.kind in (cindex.CursorKind.TRANSLATION_UNIT, cindex.CursorKind.LINKAGE_SPEC, cindex.CursorKind.NAMESPACE, cindex.CursorKind.UNEXPOSED_DECL):
            stack.extend(cursor.get_children())
            continue
        if cursor.kind not in (cindex.CursorKind.FUNCTION_DECL, cindex.CursorKind.VAR_DECL) or cursor.linkage != cindex.LinkageKind.EXTERNAL:
            continue
        name = cursor.spelling
        c_linkage = cursor.mangled_name in (name, "_" + name)
        unpatched = name.endswith(UNPATCHED)
        if name.endswith(POSTFIX) or name in explicit:
            marked = True
        elif not name.startswith("SDL_") and c_linkage and defined(cursor):
            marked = False
        else:
            continue
        candidate = {"name": name, "marked": marked, "unpatched": unpatched, "c_linkage": c_linkage, "data": cursor.kind == cindex.CursorKind.VAR_DECL}
        if c_linkage and not candidate["data"]:
            try:
                candidate["export"] = describe(cursor, name)
            except Unsupported as reason:
                candidate["reason"] = str(reason)
        candidates.append(candidate)
    return errors, candidates


def collect(sources, exported, pool, extra):
    explicit = {}
    for source, _ in sources:
        for nid, function in EXPORT_PATTERN.findall(source.read_text(errors="replace")):
            explicit[function] = nid

    exports = {}
    data = {}
    unsupported = {}
    parse_errors = []
    candidates = []
    for errors, found in pool.map(scan_source, [(source, [*compiler_arguments(entry), *extra], explicit) for source, entry in sources]):
        parse_errors.extend(errors)
        candidates.extend(candidate for candidate in found if not candidate["c_linkage"] or candidate["name"] in exported)
    named = {candidate["name"] for candidate in candidates}
    for candidate in candidates:
        name = candidate["name"]
        if name in explicit:
            nid = explicit[name]
        elif candidate["unpatched"]:
            nid = name
        elif candidate["marked"]:
            nid = compute_nid(export_base(name))
        elif name + POSTFIX in named:
            continue
        else:
            nid = compute_nid(export_base(name))
        if not candidate["c_linkage"]:
            unsupported.setdefault(nid, (name, "C++ linkage"))
            continue
        if candidate["data"]:
            data.setdefault(nid, name)
            continue
        if nid in exports or nid in unsupported:
            continue
        if export_base(name) in GUEST_STATE_EXPORTS:
            unsupported[nid] = (name, "saves or restores the guest's own registers")
            continue
        if "export" in candidate:
            exports[nid] = dict(candidate["export"], name=name)
        else:
            unsupported[nid] = (name, candidate["reason"])
    by_name = {export["name"]: export for export in exports.values()}
    for nid in [nid for nid, export in exports.items() if export["variadic"]]:
        export = exports.pop(nid)
        base = export["name"][: -len(POSTFIX)] if export["name"].endswith(POSTFIX) else export["name"]
        sibling = by_name.get(VARIADIC_SIBLINGS.get(base, "v" + base) + POSTFIX)
        named = [spelling for spelling, _ in export["parameters"]]
        if sibling is not None and not sibling["variadic"] and [spelling for spelling, _ in sibling["parameters"]] == named + ["void*"]:
            export["target"] = sibling["symbol"]
        elif export["variadic"][0] < 6:
            export["forward"] = True
        else:
            unsupported[nid] = (export["name"], "variadic without a va_list sibling or a free integer register")
            continue
        exports[nid] = export
    return exports, data, unsupported, parse_errors


def data_symbol(nid):
    return "ps5lift_data_" + bridge_symbol(nid)[len("ps5lift_import_"):]


def emit(exports, data, library_name):
    lines = [f"#include <ps5lift/Bridge.hpp>", "", "#include <cstddef>", "#include <cstdint>", "#include <type_traits>", "", "using namespace Ps5Lift::Bridge;", ""]
    records = []
    for nid in sorted(exports):
        records.extend(definition for definition in exports[nid]["records"] if definition not in records)
    lines.extend(records + ([""] if records else []))
    for nid in sorted(exports):
        export = exports[nid]
        if export["variadic"] and not export.get("forward"):
            continue
        return_class, return_spelling = export["return"]
        if return_class == "function":
            return_spelling = f"std::add_pointer_t<{return_spelling}>"
        elif return_class == "record":
            return_spelling = return_spelling[0]
        declaration = ", ".join([spelling for spelling, _ in export["parameters"]] + (["..."] if export.get("forward") else []))
        lines.append(f'extern "C" {return_spelling} {export["symbol"]}({declaration});')
    for nid in sorted(data):
        lines.append(f'extern "C" char {data[nid]};')
        lines.append(f'extern "C" void* const {data_symbol(nid)} = &{data[nid]};')
    lines.append("")
    for nid in sorted(exports):
        export = exports[nid]
        return_class, return_spelling = export["return"]
        accesses = [access for _, access in export["parameters"]]
        if export.get("forward"):
            accesses += [f"Integer<std::uint64_t>(state, {index})" for index in range(export["variadic"][0], 6)]
        elif export["variadic"]:
            accesses.append("variadic.List()")
        call = f'{export.get("target", export["symbol"])}({", ".join(accesses)})'
        lines.append(f'extern "C" void {bridge_symbol(nid)}(Ps5LiftState* state) {{')
        lines.append("    try {")
        if export["variadic"] and not export.get("forward"):
            integers, floats, stack = export["variadic"]
            lines.append(f"        VariadicArguments variadic(state, {integers}, {floats}, {stack});")
        if return_spelling == "long double" or any(access.startswith("HostExtended(") for _, access in export["parameters"]):
            lines.append("        const ExtendedScope scope(state);")
        if return_class == "void":
            lines.append(f"        {call};")
        elif return_class == "extended":
            lines.append(f"        ReturnExtended(state, {call});")
        elif return_class == "function":
            lines.append(f"        Return(state, GuestFunction<{return_spelling}>({call}));")
        elif return_class == "record" and return_spelling[1] == "memory":
            lines.append("        const auto result = Integer<std::uint64_t>(state, 0);")
            lines.append(f"        ReturnIndirect(state, result, {call});")
        elif return_class == "record":
            classes = ", ".join("Eightbyte::Integer" if kind == "integer" else "Eightbyte::Sse" for kind in return_spelling[1])
            lines.append(f"        ReturnRecord<{classes}>(state, {call});")
        else:
            lines.append(f"        Return(state, {call});")
        lines.append("    } catch (const GuestUnwind&) {")
        lines.append("    }")
        lines.append("}")
        lines.append("")
    return "\n".join(lines)


def use_libclang(path):
    cindex.Config.set_library_file(path)


def resource_directory(libclang):
    # Homebrew's libclang on macOS does not find clang's builtin headers, such as stdarg.h, beside itself.
    versions = list((libclang.resolve().parent / "clang").iterdir())
    if len(versions) != 1:
        raise SystemExit(f"--libclang: expected one clang resource directory beside {libclang}, found {versions}")
    return versions[0]


def host_sdk():
    # Apple's compiler driver adds the SDK xcrun names, so AnyPS5's compile commands leave it out and libclang needs it.
    if platform.system() != "Darwin":
        return []
    return ["-isysroot", subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout.strip()]


def main():
    parser = argparse.ArgumentParser(description="Generate SysV-to-native import bridges for AnyPS5 libraries")
    parser.add_argument("library", nargs="+", help="AnyPS5 library targets, such as libc; for a NID that several export, the first one's export gets the bridge")
    parser.add_argument("--anyps5-build", required=True, help="AnyPS5's build directory, configured with CMAKE_EXPORT_COMPILE_COMMANDS=ON")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--libclang", type=Path, help="the libclang library to parse with; the bindings find one by default")
    parser.add_argument("--report", action="store_true", help="list every export without a bridge and why")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()

    initializer, initargs, extra = None, (), host_sdk()
    if args.libclang is not None:
        if not args.libclang.is_file():
            parser.error(f"--libclang: {args.libclang} does not exist")
        initializer, initargs = use_libclang, (str(args.libclang),)
        extra += ["-resource-dir", str(resource_directory(args.libclang))]
    pool = concurrent.futures.ProcessPoolExecutor(args.jobs, initializer=initializer, initargs=initargs)
    exports, data, unsupported, parse_errors = {}, {}, {}, []
    build = Build(args.anyps5_build)
    for name in args.library:
        library_exports, library_data, library_unsupported, library_errors = collect(build.library_sources(name), build.library_exports(name), pool, extra)
        for nid, export in library_exports.items():
            exports.setdefault(nid, export)
        for nid, symbol in library_data.items():
            data.setdefault(nid, symbol)
        for nid, entry in library_unsupported.items():
            unsupported.setdefault(nid, entry)
        parse_errors.extend(library_errors)
    if parse_errors:
        sys.stderr.writelines(f"{error}\n" for error in parse_errors)
        sys.stderr.write(f"{len(parse_errors)} parse errors: libclang's recovered declarations would give wrong prototypes, so no bridges were written\n")
        return 1
    for nid in list(unsupported):
        if nid in exports or nid in data:
            del unsupported[nid]
    Path(args.output).write_text(emit(exports, data, " ".join(args.library)))

    reasons = collections.Counter(re.sub(r"\d+-byte", "N-byte", reason) for _, reason in unsupported.values())
    print(f"{' '.join(args.library)}: {len(exports)} bridges, {len(data)} data exports, {len(unsupported)} exports without one")
    for reason, count in reasons.most_common():
        print(f"  {count:5} {reason}")
    if args.report:
        for nid, (name, reason) in sorted(unsupported.items(), key=lambda item: item[1]):
            print(f"  {nid} {name}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
