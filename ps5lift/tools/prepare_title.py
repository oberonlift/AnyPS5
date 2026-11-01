#!/usr/bin/env python3
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_fixture import GUEST_BASE, ROOT

sys.path.insert(0, str(ROOT / "3rdparty" / "anyps5" / "tools"))
import audit_dump
import import_audit

DESCRIPTION = """Prepare an extracted PS5 title for ps5lift. Unwraps its eboot.bin and the modules it ships, as AnyPS5's
audit_dump.py finds them, into WORK/source, and relinks them at the guest base into WORK/output/app.elf, with the
modules it loads beside it under WORK/output/app0. With --libs, also audits the imports against AnyPS5's libraries and
exits with the audit's status."""


def main():
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument("dump", type=Path, help="the title's extracted app0 directory, holding eboot.bin")
    parser.add_argument("work", type=Path, help="a directory for the unwrapped and relinked files; its source and output must not exist yet")
    parser.add_argument("--relinker", type=Path, default=ROOT / "build" / "anyps5" / "core" / "relinker" / "relinker")
    parser.add_argument("--libs", type=Path, help="AnyPS5's built .prx directory, to audit the imports against")
    parser.add_argument("--json", help="also write the audit's JSON report here; needs --libs")
    args = parser.parse_args()
    if args.json and not args.libs:
        parser.error("--json needs --libs")
    source = args.work / "source"
    output = args.work / "output"
    for directory in (source, output):
        if directory.exists():
            parser.error(f"{directory} already exists")
    output.mkdir(parents=True)
    module_directories, extra = audit_dump.prepare(args.dump, source)
    command = [str(args.relinker), "--registry", "--linux-fixed-base", f"{GUEST_BASE:#x}"]
    for relative in extra:
        command += ["--module-dir", relative]
    subprocess.run(command + [str(source / "eboot.elf"), str(output / "app.elf")], check=True)
    if not args.libs:
        return 0
    audit = sorted(str(path) for path in output.rglob("*.registry.json")) + ["--libs", str(args.libs)]
    audit += [option for directory in module_directories for option in ("--modules", str(directory))]
    if args.json:
        audit += ["--json", args.json]
    return import_audit.main(audit)


if __name__ == "__main__":
    sys.exit(main())
