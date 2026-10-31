import json
import platform
import shlex
import struct
from pathlib import Path

ANYPS5 = Path(__file__).resolve().parents[2]
LIBS = ANYPS5 / "core" / "libs"
# What a program that links AnyPS5's libraries links from the host besides them; macOS's libSystem holds libatomic and libdl.
HOST_LIBRARIES = ["-pthread"] if platform.system() == "Darwin" else ["-pthread", "-latomic", "-ldl"]
DEPENDENCY_OPTIONS = {"-o", "-MF", "-MT", "-MQ"}


class Build:
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        commands = self.directory / "compile_commands.json"
        if not commands.exists():
            raise SystemExit(f"{commands} does not exist: configure AnyPS5 with CMAKE_EXPORT_COMPILE_COMMANDS=ON")
        self.entries = json.loads(commands.read_text())
        self.libraries_directory = self.directory / "core" / "libs" / "libs" / "unpatched"

    def library(self, name):
        path = self.libraries_directory / f"{name}.prx"
        if not path.exists():
            raise SystemExit(f"{path} does not exist: build AnyPS5's libs target")
        return path

    def library_exports(self, name):
        data = self.library(name).read_bytes()
        if data[:4] == MACH_O_MAGIC:
            return mach_o_exports(data, self.library(name))
        offset, = struct.unpack_from("<Q", data, 0x28)
        size, count = struct.unpack_from("<HH", data, 0x3a)
        sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + index * size) for index in range(count)]
        for _, kind, _, _, symbols, length, link, _, _, _ in sections:
            if kind != 11:
                continue
            strings = sections[link][4]
            names = set()
            for symbol in range(symbols + 24, symbols + length, 24):
                name, info, _, section = struct.unpack_from("<IBBH", data, symbol)
                if section != 0 and info >> 4 != 0:
                    names.add(data[strings + name:data.index(b"\0", strings + name)].decode())
            return names
        raise SystemExit(f"{self.library(name)} has no dynamic symbol table")

    def library_sources(self, name):
        target = f"/CMakeFiles/{name}.dir/"
        sources = {Path(entry["file"]): entry for entry in self.entries
                   if target in "/" + entry["output"] and entry["file"].endswith(".cpp") and not is_test(Path(entry["file"]))}
        if not sources:
            raise SystemExit(f"AnyPS5's build has no target {name}")
        return sorted(sources.items())

    def source(self, path):
        entries = [entry for entry in self.entries if Path(entry["file"]) == Path(path)]
        if len(entries) != 1:
            raise SystemExit(f"AnyPS5's build compiles {path} {len(entries)} times, expected once")
        return entries[0]


MACH_O_MAGIC = struct.pack("<I", 0xfeedfacf)
LC_SYMTAB = 0x2
N_EXT = 0x01
N_TYPE = 0x0e
N_SECT = 0x0e


def mach_o_exports(data, path):
    # The defined external symbols of a 64-bit Mach-O library, whose C names carry a leading underscore.
    commands, = struct.unpack_from("<I", data, 16)
    offset = 32
    for _ in range(commands):
        command, size = struct.unpack_from("<II", data, offset)
        if command == LC_SYMTAB:
            symbols, count, strings, _ = struct.unpack_from("<IIII", data, offset + 8)
            names = set()
            for index in range(count):
                name, kind, _, _, _ = struct.unpack_from("<IBBHQ", data, symbols + 16 * index)
                if kind & N_EXT and kind & N_TYPE == N_SECT:
                    text = data[strings + name:data.index(b"\0", strings + name)].decode()
                    names.add(text[1:] if text.startswith("_") else text)
            return names
        offset += size
    raise SystemExit(f"{path} has no symbol table")


def is_test(path):
    return path.is_relative_to(ANYPS5) and "tests" in path.relative_to(ANYPS5).parts


def compiler_arguments(entry):
    words = shlex.split(entry["command"])[1:]
    arguments = []
    skip = False
    for word in words:
        if skip:
            skip = False
        elif word in DEPENDENCY_OPTIONS:
            skip = True
        elif word not in ("-c", "-MD", "-MMD", entry["file"]):
            arguments.append(word)
    return arguments
