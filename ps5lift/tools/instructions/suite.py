"""The instruction suite's commands: `instructions-bundle`, which packs what a recording host needs, and
`instructions-import`, which checks a recording and commits it.

`generate(decoder)` builds every test group's instances and case specs from the database. `bundle` writes into a new
directory, for each group with cases to record, `<group>/<group>` (the guest program, linked at the image base its
caller gives), `<group>/<group>.inputs` and `<group>/libinstructionhost.so` (the native driver); the self-test the
same way in `self-test/`, with `expected.json`; `record.py` with the `instructions` modules it imports; and
`groups.json`, which gives the `/proc/cpuinfo` flags each group needs (`CPU_FLAGS`). The inputs leave out the cases
`cases.expectation_written` names and keep the other cases' indices, so a group with no other case is not bundled.
`recording_header` gives the fingerprints of a group's assembly and of the input records its recording ran.

`import_bundle` checks the results in the bundle's `results/` against the bundle: each group's results carry its inputs'
fingerprints, hold one result for each of its cases in order, with each case's console-defined flag, and none crashed,
timed out or ended by a host signal. Only when every group passes does it write each group's results as
`<group>.results.xz` among the recordings."""
import hashlib
import json
import lzma
import shutil
import subprocess
import tempfile
from collections import defaultdict
from pathlib import Path

from . import cases, database, decoder, program, records, selftest
from .database import Group
from .records import Outcome

PACKAGE = Path(__file__).resolve().parent
TESTS = PACKAGE.parents[1] / "tests" / "instructions"
NATIVE_DRIVER = TESTS / "NativeDriver.c"
RECORDER = TESTS / "record.py"
RECORDINGS = TESTS / "recordings"
RECORDER_MODULES = ("__init__.py", "records.py", "runner.py")
SELF_TEST = "self-test"
GROUPS_FILE = "groups.json"
RESULTS = "results"
EXPECTED = "expected.json"
RECORDING_SUFFIX = ".results.xz"
RECORDING_PRESET = 9 | lzma.PRESET_EXTREME
DRIVER = f"lib{program.HOST_LIBRARY}.so"
DRIVER_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
    "-fuse-ld=lld",
)
UNFINISHED = (Outcome.CRASHED, Outcome.TIMED_OUT, Outcome.HOST_SIGNAL)
LISTED_CASES = 20

CPU_FLAGS = {
    "adox_adcx": ("adx",),
    "aes": ("aes",),
    "amd3dnow_prefetch": ("3dnowprefetch",),
    "avx": ("avx",),
    "avx2": ("avx2",),
    "avx2gather": ("avx2",),
    "avxaes": ("avx", "aes"),
    "base": (),
    "bmi1": ("bmi1",),
    "bmi2": ("bmi2",),
    "clflushopt": ("clflushopt",),
    "clfsh": ("clflush",),
    "clwb": ("clwb",),
    "f16c": ("f16c",),
    "fma": ("fma",),
    "hint": ("3dnowprefetch",),
    "longmode": ("lm", "cx16"),
    "lzcnt": ("abm",),
    "mmx": ("mmx", "sse"),
    "movbe": ("movbe",),
    "pause": (),
    "pclmulqdq": ("pclmulqdq",),
    "rdpid": ("rdpid",),
    "rdrand": ("rdrand",),
    "rdseed": ("rdseed",),
    "rdtscp": ("rdtscp",),
    "sha": ("sha_ni",),
    "sse": ("sse", "fxsr"),
    "sse2": ("sse2",),
    "sse3": ("pni",),
    "sse4": ("sse4_1", "sse4_2", "popcnt"),
    "ssse3": ("ssse3",),
    "x87": ("fpu", "cmov"),
    "xsave": ("xsave",),
    "xsavec": ("xsavec",),
    "xsaveopt": ("xsaveopt",),
}


class SuiteError(Exception):
    pass


def generate(decoder):
    forms = [form for form in database.forms(database.load()) if database.classify(form.definition) is Group.SET]
    by_group = defaultdict(list)
    for instance in cases.instances(forms, decoder):
        by_group[database.test_group(instance.form)].append(instance)
    specs = {group: cases.case_specs(members) for group, members in by_group.items()}
    return forms, dict(by_group), specs


def recorded_inputs(instances, inputs):
    return [case for case in inputs if not cases.expectation_written(instances[case.instance].form)]


def recording_header(assembly, inputs):
    return records.Header(*program.fingerprints(assembly, records.pack_input_records(inputs)))


def _recordable(instances):
    return sorted(
        group for group, members in instances.items()
        if not all(cases.expectation_written(instance.form) for instance in members)
    )


def _selected(groups, instances):
    recordable = _recordable(instances)
    if not groups:
        return recordable
    for group in groups:
        if group not in instances:
            raise SuiteError(f"there is no test group {group}; the groups are {', '.join(sorted(instances))}")
        if group not in recordable:
            raise SuiteError(f"{group} has no case to record: its expectations are written from AMD's manual")
    return list(dict.fromkeys(groups))


def _cpu_flags(group):
    if group not in CPU_FLAGS:
        raise SuiteError(f"CPU_FLAGS has no entry for {group}: give the /proc/cpuinfo flags its instructions need")
    return list(CPU_FLAGS[group])


def _compile_driver(clang, work):
    (work / "records.h").write_text(records.header_text())
    driver = work / DRIVER
    command = [str(clang), *DRIVER_FLAGS, f"-I{work}", str(NATIVE_DRIVER), "-o", str(driver)]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise SuiteError(f"compiling the native driver failed with exit code {completed.returncode}:\n"
                         f"$ {' '.join(command)}\n{completed.stdout}{completed.stderr}")
    return driver


def _write_program(clang, work, driver, directory, name, group, instances, specs, guest_flags, guest_base):
    directory.mkdir()
    shutil.copy(driver, directory / DRIVER)
    assembly = program.emit(group, instances)
    source = work / f"{name}.s"
    source.write_text(assembly)
    elf = program.link(clang, source, directory, None, guest_flags, guest_base)
    layout = program.read_layout(elf)
    inputs = recorded_inputs(instances, program.case_inputs(layout, specs))
    records.write_inputs(directory / f"{name}.inputs", recording_header(assembly, inputs), inputs)
    return layout, len(inputs)


def _write_bundle(output, clang, flags, generated, decode, guest_flags, guest_base):
    forms, instances, specs = generated
    counts = {}
    with tempfile.TemporaryDirectory(prefix="ps5lift-bundle-") as work:
        work = Path(work)
        driver = _compile_driver(clang, work)
        for group in flags:
            _, counts[group] = _write_program(clang, work, driver, output / group, group, group, instances[group],
                                              specs[group], guest_flags, guest_base)
        test_instances, test_specs = selftest.program(forms, decode)
        layout, _ = _write_program(clang, work, driver, output / SELF_TEST, SELF_TEST, selftest.GROUP, test_instances,
                                   test_specs, guest_flags, guest_base)
    expected = selftest.expectations(layout, test_instances, test_specs)
    (output / SELF_TEST / EXPECTED).write_text(json.dumps(expected, indent=1) + "\n")
    shutil.copy(RECORDER, output / RECORDER.name)
    package = output / PACKAGE.name
    package.mkdir()
    for module in RECORDER_MODULES:
        shutil.copy(PACKAGE / module, package / module)
    (output / GROUPS_FILE).write_text(json.dumps(flags, indent=1, sort_keys=True) + "\n")
    return counts


def bundle(output, clang, groups, generated, decode, guest_flags, guest_base):
    output = Path(output)
    flags = {group: _cpu_flags(group) for group in _selected(groups, generated[1])}
    if output.exists():
        raise SuiteError(f"{output} exists: a bundle goes into a new directory")
    output.mkdir(parents=True)
    try:
        return _write_bundle(output, clang, flags, generated, decode, guest_flags, guest_base)
    except BaseException:
        shutil.rmtree(output)
        raise


def _listed(numbers):
    shown = ", ".join(str(number) for number in numbers[:LISTED_CASES])
    more = len(numbers) - LISTED_CASES
    return shown + (f" and {more} more" if more > 0 else "")


def _check_group(bundle_directory, group):
    inputs_path = bundle_directory / group / f"{group}.inputs"
    results_path = bundle_directory / RESULTS / f"{group}.results"
    header, inputs = records.read_inputs(inputs_path)
    problems = []
    if hashlib.sha256(records.pack_input_records(inputs)).digest() != header.inputs:
        problems.append(f"{inputs_path} does not match the inputs fingerprint in its own header")
    if not results_path.exists():
        return None, problems + [f"{results_path} does not exist: record {group}"]
    data = results_path.read_bytes()
    try:
        found, results = records.read_results(data)
    except records.RecordError as error:
        return None, problems + [f"{results_path} is not a result file: {error}"]
    if found != header:
        problems.append(f"{results_path} has other fingerprints than {inputs_path}: it was recorded from another bundle")
    expected_cases = [case.case for case in inputs]
    by_case = {result.case: result for result in results}
    missing = [case for case in expected_cases if case not in by_case]
    extra = sorted(set(by_case) - set(expected_cases))
    if missing:
        problems.append(f"{results_path} has no result for case {_listed(missing)}")
    if extra:
        problems.append(f"{results_path} has results for case {_listed(extra)}, which {inputs_path} does not hold")
    if not missing and not extra and [result.case for result in results] != expected_cases:
        problems.append(f"{results_path} does not list its cases once each in the order of {inputs_path}")
    mismatched = [case.case for case in inputs
                  if case.case in by_case and (by_case[case.case].expected is not None) != case.console_defined]
    if mismatched:
        problems.append(f"{results_path} has the wrong console-defined flag in case {_listed(mismatched)}")
    for outcome in UNFINISHED:
        ended = [result.case for result in results if result.outcome is outcome]
        if ended:
            problems.append(f"{results_path}: {outcome.name} in case {_listed(ended)}")
    return data, problems


def import_bundle(bundle_directory, recordings):
    bundle_directory, recordings = Path(bundle_directory), Path(recordings)
    groups = json.loads((bundle_directory / GROUPS_FILE).read_text())
    accepted = {}
    problems = []
    for group in groups:
        data, found = _check_group(bundle_directory, group)
        accepted[group] = data
        problems += found
    if problems:
        raise SuiteError("the recording is not imported:\n" + "\n".join(problems))
    recordings.mkdir(exist_ok=True)
    sizes = {}
    for group, data in accepted.items():
        path = recordings / f"{group}{RECORDING_SUFFIX}"
        path.write_bytes(lzma.compress(data, preset=RECORDING_PRESET))
        sizes[group] = path.stat().st_size
    return sizes


def bundle_command(args, guest_flags, guest_base):
    decode = decoder.from_environment()
    try:
        counts = bundle(args.output, args.clang, args.group, generate(decode), decode, guest_flags, guest_base)
    except SuiteError as error:
        raise SystemExit(str(error)) from error
    for group, count in counts.items():
        print(f"{group}: {count:,} cases")
    print(f"bundled {len(counts)} groups, {sum(counts.values()):,} cases, into {args.output}")
    return 0


def import_command(args):
    try:
        sizes = import_bundle(args.bundle, RECORDINGS)
    except SuiteError as error:
        raise SystemExit(str(error)) from error
    for group, size in sizes.items():
        print(f"{group}: {size:,} bytes")
    print(f"total: {sum(sizes.values()):,} bytes")
    return 0
