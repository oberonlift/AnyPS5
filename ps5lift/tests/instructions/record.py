#!/usr/bin/env python3
"""Record the instruction cases of the bundle this script sits in, on an x86-64 Linux host.

`ps5lift.py instructions-bundle` writes the bundle. The recorder first refuses every group that needs a flag (from
groups.json) that one of this host's processors lacks in /proc/cpuinfo, naming the flags. Then it runs the self-test,
whose results must be the ones `self-test/expected.json` gives. With `--self-test` that is all. Otherwise it runs each
group's program through the runner, writes `results/<group>.results` and prints the group's outcomes, with the stderr of
any case that crashed or timed out and the signal of any that ended in a host signal. It exits nonzero if any did."""
import argparse
import json
import os
import sys
import time
from collections import Counter
from pathlib import Path

from instructions import records, runner

BUNDLE = Path(__file__).resolve().parent
GROUPS_FILE = BUNDLE / "groups.json"
SELF_TEST = "self-test"
EXPECTED = "expected.json"
RESULTS = BUNDLE / "results"
CPUINFO = Path("/proc/cpuinfo")
FLAGS_FIELD = "flags"
TIMEOUT_SECONDS = 10.0
UNFINISHED = (records.Outcome.CRASHED, records.Outcome.TIMED_OUT, records.Outcome.HOST_SIGNAL)


def cpu_flags(path):
    flag_sets = [
        set(value.split()) for field, _, value in (line.partition(":") for line in path.read_text().splitlines())
        if field.strip() == FLAGS_FIELD
    ]
    if not flag_sets:
        raise SystemExit(f"record.py: {path} lists no processor flags")
    return set.intersection(*flag_sets)


def run_program(name, results_path, timeout):
    directory = BUNDLE / name
    environment = {**os.environ, "LD_LIBRARY_PATH": str(directory)}
    inputs_path = directory / f"{name}.inputs"
    return runner.run([str(directory / name)], environment, inputs_path, results_path, timeout)


def _value(value):
    return "absent" if value is None else f"{value:#x}"


def check(expected, result):
    problems = []
    outcome = records.Outcome[expected["outcome"]]
    if result.case != expected["case"]:
        problems.append(f"the result is case {result.case}'s")
    if result.outcome is not outcome:
        problems.append(f"outcome {result.outcome.name} (detail {result.detail}), not {outcome.name}")
    for field in ("address", "data"):
        if getattr(result, field) != expected[field]:
            problems.append(f"{field} {getattr(result, field):#x}, not {expected[field]:#x}")
    slot = {int(offset): value for offset, value in expected["slot"].items()}
    memory = {int(offset): value for offset, value in expected["memory"].items()}
    if outcome is records.Outcome.COMPLETED:
        for offset, value in slot.items():
            if result.slot_words.get(offset) != value:
                problems.append(f"slot word {offset} is {_value(result.slot_words.get(offset))}, not {value:#x}")
    elif result.slot_words:
        problems.append(f"{len(result.slot_words)} slot words after a fault")
    differing = sorted(offset for offset in set(memory) | set(result.memory_words)
                       if memory.get(offset) != result.memory_words.get(offset))
    if differing:
        offset = differing[0]
        problems.append(f"{len(differing)} memory words differ; block word {offset} is "
                        f"{_value(result.memory_words.get(offset))}, not {_value(memory.get(offset))}")
    return problems


def self_test(timeout):
    directory = BUNDLE / SELF_TEST
    results_path = directory / f"{SELF_TEST}.results"
    stops = run_program(SELF_TEST, results_path, timeout)
    _, results = records.read_results(results_path.read_bytes())
    expected = json.loads((directory / EXPECTED).read_text())
    failures = [f"case {case} ended the driver:\n{stderr}" for case, stderr in stops.items()]
    if len(results) != len(expected):
        failures.append(f"{len(results)} results for {len(expected)} cases")
    for case, result in zip(expected, results):
        failures += [f"{case['name']}: {problem}" for problem in check(case, result)]
    for failure in failures:
        print(f"self-test FAILED: {failure}", file=sys.stderr)
    if not failures:
        print(f"self-test: all {len(results)} cases as expected")
    return not failures


def record(group, timeout):
    RESULTS.mkdir(exist_ok=True)
    results_path = RESULTS / f"{group}.results"
    start = time.monotonic()
    stops = run_program(group, results_path, timeout)
    elapsed = time.monotonic() - start
    _, results = records.read_results(results_path.read_bytes())
    counts = Counter(result.outcome for result in results)
    summary = ", ".join(f"{outcome.name} {count:,}" for outcome, count in sorted(counts.items()))
    print(f"{group}: {len(results):,} cases in {elapsed:.1f} s: {summary}", flush=True)
    unfinished = [result for result in results if result.outcome in UNFINISHED]
    for result in unfinished:
        print(f"  case {result.case}: {result.outcome.name}, detail {result.detail}, address {result.address:#x}")
        if result.case in stops:
            print("    " + stops[result.case].rstrip().replace("\n", "\n    "))
    return not unfinished


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--group", action="append", help="a group to record; every group in groups.json by default")
    parser.add_argument("--self-test", action="store_true", help="run the self-test and record nothing")
    parser.add_argument("--cpuinfo", type=Path, default=CPUINFO, help="the processor description to check flags in")
    parser.add_argument("--timeout", type=float, default=TIMEOUT_SECONDS,
                        help="seconds without a new result after which a case counts as timed out")
    args = parser.parse_args()
    groups = json.loads(GROUPS_FILE.read_text())
    selected = args.group or sorted(groups)
    unknown = [group for group in selected if group not in groups]
    if unknown:
        parser.error(f"the bundle has no group {', '.join(unknown)}")
    flags = cpu_flags(args.cpuinfo)
    refused = {group: [flag for flag in groups[group] if flag not in flags] for group in selected}
    refused = {group: missing for group, missing in refused.items() if missing}
    for group, missing in refused.items():
        print(f"record.py: refusing {group}: this CPU lacks {', '.join(missing)}", file=sys.stderr)
    if refused or not self_test(args.timeout):
        return 1
    if args.self_test:
        return 0
    recorded = [record(group, args.timeout) for group in selected]
    return 0 if all(recorded) else 1


if __name__ == "__main__":
    sys.exit(main())
