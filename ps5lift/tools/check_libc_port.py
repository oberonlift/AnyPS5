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


SKIPPED_PLATFORMS = re.compile(r"\s*(WIN32|APPLE|MSVC)\s*")


def cmake_commands(text):
    text = re.sub(r"#[^\n]*", "", text)
    for match in re.finditer(r"([A-Za-z_]+)\s*\(", text):
        depth, position = 1, match.end()
        while depth and position < len(text):
            depth += {"(": 1, ")": -1}.get(text[position], 0)
            position += 1
        yield match.start(), position, match.group(1).lower(), text[match.end():position - 1].split()


def sources(library):
    directory = LIBS / "prx" / library
    listing = directory / "CMakeLists.txt"
    if not listing.exists():
        return [directory / "Export.cpp"]
    found = set()
    skipped = []
    consumed = 0
    for start, end, command, arguments in cmake_commands(listing.read_text()):
        if start < consumed:
            continue
        consumed = end
        if command == "if":
            skipped.append(bool(SKIPPED_PLATFORMS.fullmatch(" ".join(arguments))))
        elif command in ("elseif", "else") and skipped:
            skipped[-1] = False
        elif command == "endif" and skipped:
            skipped.pop()
        elif command in ("add_library", "target_sources") and arguments and arguments[0] == library and not any(skipped):
            for argument in arguments[1:]:
                if not argument.endswith(".cpp"):
                    continue
                path = argument.replace("${CMAKE_CURRENT_SOURCE_DIR}", str(directory)).replace("${LIBS_INCLUDE_DIR}", str(LIBS))
                path = Path(path) if Path(path).is_absolute() else directory / path
                if "tests" not in path.relative_to(LIBS).parts:
                    found.add(path)
    return sorted(found)


THIRD_PARTY_INCLUDES = ["SDL2/include", "Vulkan-Headers/include", "VulkanMemoryAllocator/include", "stb", "freetype/include", "mbedtls/include", "zstd/lib",
                        "ogg/include", "opus/include", "vorbis/include", "nghttp2/lib/includes", "SPIRV-Headers/include", "LibAtrac9/C/src"]
COMPILE_DEFINITIONS = ["-DAPS5_ENABLE_TIMING_LOG=0"]


def include_flags(library):
    third_party = [f"-I{ANYPS5 / '3rdparty' / path}" for path in THIRD_PARTY_INCLUDES if (ANYPS5 / "3rdparty" / path).is_dir()]
    decoders = [f"-I{path / 'include'}" for path in sorted((ANYPS5 / "core" / "Decoder").iterdir()) if (path / "include").is_dir()]
    return COMPILE_DEFINITIONS + third_party + decoders + [f"-I{LIBS}", f"-I{LIBS / 'nid' / 'include'}", f"-I{LIBS / 'prx' / library / 'include'}", f"-I{LIBS / 'prx' / 'libc' / 'include'}",
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
