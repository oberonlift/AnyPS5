import json
import shlex
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
