"""`ps5lift.py instructions`: run test groups lifted and judge every case against its expectation.

For each group it links the guest program as the recording host ran it, checks the group's recording against that
program and its inputs (`suite.read_recording`), and for each `--float` mode lifts the program (`-O0` with
`--unoptimized-lift`), links it with `LiftedDriver.cpp` and the runtime, and runs every case through the runner, the
cases without a recording included. `compare` judges each case against its expectation (`expectations`), and each
form by the checks that span its cases (`compare.form_mismatches`).

The report has one line per form, `PASS` or `FAIL`, then the group, the mode and the form, with the counts of cases that
mismatched, are missing (no recording and no written expectation), stopped at a missing translation, crashed and timed
out, and any check of the form that failed; then the failing cases of each form, at most `SHOWN_PER_FORM`; then the
group's outcomes. The command exits 0 only when every case and every form passes."""
import collections
import os
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from . import compare, decoder, expectations, program, records, runner, suite
from .compare import Verdict
from .state import Case, State

LIFTED_DRIVER = suite.TESTS / "LiftedDriver.cpp"
DRIVER_FLAGS = ("-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-fuse-ld=lld", "-pthread")
SHOWN_PER_FORM = 5
RUN_TIMEOUT = 30
_COUNTED = (Verdict.MISMATCHED, Verdict.MISSING, Verdict.STOPPED, Verdict.CRASHED, Verdict.TIMED_OUT)


class LiftError(Exception):
    pass


@dataclass(frozen=True)
class Toolchain:
    ps5lift: Path
    clang: str
    clangxx: str
    triple: str
    guest_flags: tuple
    guest_base: int
    runner: tuple
    lift_flags: tuple
    runtime: tuple
    include: Path


@dataclass(frozen=True)
class Guest:
    group: str
    instances: list
    specs: list
    assembly: str
    elf: Path
    layout: program.Layout
    inputs: list
    inputs_path: Path


@dataclass(frozen=True)
class Judged:
    case: Case
    verdict: Verdict
    mismatches: list
    actual: State


def _run(command, what):
    completed = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if completed.returncode != 0:
        raise LiftError(f"{what} failed with exit code {completed.returncode}:\n$ {' '.join(map(str, command))}\n"
                        f"{completed.stdout}{completed.stderr}")
    return completed


def _guest(toolchain, work, group, instances, specs):
    directory = work / group
    directory.mkdir()
    suite.compile_native_driver(toolchain.clang, directory)
    assembly = program.emit(group, instances)
    source = directory / f"{group}.s"
    source.write_text(assembly)
    elf = program.link(toolchain.clang, source, directory, None, toolchain.guest_flags, toolchain.guest_base)
    layout = program.read_layout(elf)
    inputs = program.case_inputs(layout, specs)
    inputs_path = directory / f"{group}.inputs"
    header = records.Header(*program.fingerprints(assembly, records.pack_input_records(inputs)))
    records.write_inputs(inputs_path, header, inputs)
    return Guest(group, instances, specs, assembly, elf, layout, inputs, inputs_path)


def _recording(guest):
    path = suite.RECORDINGS / f"{guest.group}{suite.RECORDING_SUFFIX}"
    recorded = suite.recorded_inputs(guest.instances, guest.inputs)
    if not path.exists():
        if recorded:
            raise suite.SuiteError(f"{path} does not exist: record {guest.group}")
        return {}
    return suite.read_recording(path, suite.recording_header(guest.assembly, recorded))


def _lifted_program(toolchain, guest, float_mode, unoptimized):
    directory = guest.elf.parent
    archive = directory / f"{guest.group}-{float_mode}.a"
    lift = [toolchain.ps5lift, guest.elf, "-o", archive, "--triple", toolchain.triple, "--float", float_mode,
            *toolchain.lift_flags, *(["-O0"] if unoptimized else [])]
    report = _run(lift, f"lifting {guest.group}").stderr.strip()
    executable = directory / f"{guest.group}-{float_mode}"
    link = [toolchain.clangxx, f"--target={toolchain.triple}", *DRIVER_FLAGS, f"-I{toolchain.include}",
            f"-I{directory}", LIFTED_DRIVER, archive, *toolchain.runtime, "-o", executable]
    _run(link, f"linking the lifted {guest.group}")
    return executable, report


def _results(toolchain, guest, executable, float_mode):
    results_path = executable.parent / f"{guest.group}-{float_mode}.results"
    stops = runner.run([*toolchain.runner, str(executable)], dict(os.environ), guest.inputs_path, results_path,
                       RUN_TIMEOUT)
    _, results = records.read_results(results_path.read_bytes())
    return results, stops


def _check_twin(case, twin):
    mxcsr = slice(records.FX_MXCSR, records.FX_MXCSR + 4)
    slot, twin_slot = bytearray(case.input.slot), bytearray(twin.input.slot)
    slot[mxcsr] = twin_slot[mxcsr] = bytes(4)
    if slot != twin_slot or case.input.fill_seed != twin.input.fill_seed or case.input.patches != twin.input.patches:
        raise compare.CompareError(f"case {case.spec.index} names case {twin.spec.index} as its twin, but their inputs "
                                   "differ in more than MXCSR")


def _judge(guest, results, recorded, float_mode):
    cases = [Case(spec, guest.instances[spec.instance], case_input, guest.layout)
             for spec, case_input in zip(guest.specs, guest.inputs)]
    if [result.case for result in results] != [case.spec.index for case in cases]:
        raise LiftError(f"the lifted {guest.group} gave results for other cases than its inputs")
    by_index = {case.spec.index: case for case in cases}
    rules = {}
    run = compare.Run(os.cpu_count())
    judged = []
    for case, result in zip(cases, results):
        actual = State.of(result, case.input.slot)
        reference = by_index[compare.reference_case(case.spec, float_mode)]
        if reference is not case:
            _check_twin(case, reference)
        try:
            expected = expectations.expected_result(reference, recorded.get(reference.spec.index))
        except expectations.NoExpectation:
            judged.append(Judged(case, Verdict.MISSING, [], actual))
            continue
        if case.instance.index not in rules:
            rules[case.instance.index] = compare.rules_for(case.instance)
        expected_state = State.of(expected, reference.input.slot)
        mismatches = compare.compare(case, rules[case.instance.index], expected_state, actual, float_mode, run)
        judged.append(Judged(case, compare.verdict(actual, mismatches), mismatches, actual))
    return judged, compare.form_mismatches(run)


def _mode(float_mode, unoptimized):
    return f"{float_mode},unoptimized" if unoptimized else float_mode


def _mismatch_text(mismatch):
    return f"{mismatch.field}: expected {mismatch.expected}, actual {mismatch.actual}"


def form_line(group, mode, form, members, form_mismatches):
    counts = collections.Counter(item.verdict for item in members)
    if counts[Verdict.PASS] == len(members) and not form_mismatches:
        return f"PASS {group} {mode} {form}: {len(members)} cases"
    counted = ", ".join(f"{counts[verdict]} {verdict.value}" for verdict in _COUNTED)
    checks = "".join(f"; {_mismatch_text(mismatch)}" for mismatch in form_mismatches)
    return f"FAIL {group} {mode} {form}: {len(members)} cases, {counted}{checks}"


def _details(item, stops):
    if item.verdict is Verdict.MISSING:
        return (f"{item.case.instance.form.key} case {item.case.spec.index}: {item.case.instance.text}: "
                f"{expectations.NO_EXPECTATION}")
    text = compare.format_mismatch(item.case, item.mismatches)
    stderr = stops.get(item.case.spec.index, "").strip()
    if stderr:
        text += "\n  stderr: " + stderr.replace("\n", "\n          ")
    return text


def _report(group, mode, judged, by_form, stops, lift_report, elapsed):
    forms = collections.defaultdict(list)
    for item in judged:
        forms[item.case.instance.form.key].append(item)
    for form, members in forms.items():
        print(form_line(group, mode, form, members, by_form.get(form, [])))
    failing = {form: [item for item in members if item.verdict is not Verdict.PASS] for form, members in forms.items()}
    failing = {form: members for form, members in failing.items() if members}
    if failing:
        print(f"--- failing cases of {group} {mode}, at most {SHOWN_PER_FORM} per form")
        for members in failing.values():
            for item in members[:SHOWN_PER_FORM]:
                print(_details(item, stops))
    verdicts = collections.Counter(item.verdict for item in judged)
    outcomes = collections.Counter(item.actual.outcome for item in judged)
    passed_forms = sum(1 for form, members in forms.items()
                       if all(item.verdict is Verdict.PASS for item in members) and form not in by_form)
    print(f"=== {group} {mode}: {len(forms)} forms, {passed_forms} pass, {len(forms) - passed_forms} fail; "
          f"{len(judged)} cases, " + ", ".join(f"{verdicts[verdict]} {verdict.value}" for verdict in Verdict))
    print(f"=== {group} {mode} outcomes: " + ", ".join(f"{outcome.name} {count}"
                                                    for outcome, count in sorted(outcomes.items())))
    print(f"=== {group} {mode}: lifted, ran and judged in {elapsed:.1f} s; the lift report:")
    print("\n".join(f"    {line}" for line in lift_report.splitlines()), flush=True)
    return verdicts[Verdict.PASS] == len(judged) and not by_form


def _selected(groups, instances):
    if not groups:
        return sorted(instances)
    unknown = [group for group in groups if group not in instances]
    if unknown:
        raise suite.SuiteError(f"there is no test group {', '.join(unknown)}; "
                               f"the groups are {', '.join(sorted(instances))}")
    return list(dict.fromkeys(groups))


def run_groups(toolchain, groups, float_modes, unoptimized, work):
    _, instances, specs = suite.generate(decoder.from_environment())
    passed = True
    for group in _selected(groups, instances):
        guest = _guest(toolchain, work, group, instances[group], specs[group])
        recorded = _recording(guest)
        for float_mode in float_modes:
            start = time.monotonic()
            executable, lift_report = _lifted_program(toolchain, guest, float_mode, unoptimized)
            results, stops = _results(toolchain, guest, executable, float_mode)
            judged, by_form = _judge(guest, results, recorded, float_mode)
            passed &= _report(group, _mode(float_mode, unoptimized), judged, by_form, stops, lift_report,
                              time.monotonic() - start)
    return passed


def instructions_command(args, toolchain):
    float_modes = list(dict.fromkeys(args.float or compare.FLOAT_MODES))
    work = Path(tempfile.mkdtemp(prefix="ps5lift-instructions-"))
    try:
        passed = run_groups(toolchain, args.group, float_modes, args.unoptimized_lift, work)
    except (LiftError, suite.SuiteError, runner.RunnerError) as error:
        raise SystemExit(str(error)) from error
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work)
    return 0 if passed else 1
