import json
import shlex
import struct
from pathlib import Path

ANYPS5 = Path(__file__).resolve().parent.parent / "3rdparty" / "anyps5"
LIBS = ANYPS5 / "core" / "libs"
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
