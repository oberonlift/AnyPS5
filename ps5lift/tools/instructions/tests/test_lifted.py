import contextlib
import dataclasses
import functools
import io
import unittest

from instructions import compare, expectations, lifted, program
from instructions.compare import Mismatch, Verdict
from instructions.machine import FTZ_ALONE
from instructions.state import Case
from instructions.tests.support import completed, flags, group

GROUP = "sse"
LIFT_REPORT = "ps5lift: 9 functions, 133 instructions"


def _guest(specs):
    instances, _, layout = group(GROUP)
    inputs = program.case_inputs(layout, specs)
    return lifted.Guest(GROUP, instances, specs, "", None, layout, inputs, None)


def _case(guest, number):
    return Case(guest.specs[number], guest.instances[guest.specs[number].instance], guest.inputs[number], guest.layout)


def _unchanged(case):
    return completed(case, flags(**case.spec.flags))


def _report(judged, by_form, mode=compare.FAST):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        passed = lifted._report(GROUP, mode, judged, by_form, {}, LIFT_REPORT, 0.1)
    return passed, output.getvalue()


@dataclasses.dataclass(frozen=True)
class _FlushRun:
    guest: lifted.Guest
    twin: Case
    flushed: Case
    results: list
    recorded: dict


@functools.lru_cache(maxsize=None)
def _flush_run():
    instances, specs, _ = group(GROUP)
    flushed = next(spec for spec in specs if spec.mxcsr == FTZ_ALONE and spec.twin is not None
                   and instances[spec.instance].form.definition.mnemonic == "addps"
                   and instances[spec.instance].role == "primary")
    guest = _guest([specs[flushed.twin], flushed])
    twin, flushed = _case(guest, 0), _case(guest, 1)
    return _FlushRun(guest, twin, flushed, [_unchanged(twin), _unchanged(flushed)], {twin.spec.index: _unchanged(twin)})


class JudgeTest(unittest.TestCase):
    def setUp(self):
        run = _flush_run()
        self.guest, self.twin, self.flushed, self.results, self.recorded = (
            run.guest, run.twin, run.flushed, run.results, run.recorded)

    def test_fast_mode_judges_a_flush_case_by_its_twin(self):
        judged, by_form = lifted._judge(self.guest, self.results, self.recorded, compare.FAST)
        self.assertEqual([(item.case.spec.index, item.verdict) for item in judged],
                         [(self.twin.spec.index, Verdict.PASS), (self.flushed.spec.index, Verdict.PASS)])
        self.assertEqual(by_form, {})

    def test_exact_mode_finds_no_expectation_for_the_flush_case(self):
        judged, by_form = lifted._judge(self.guest, self.results, self.recorded, compare.EXACT)
        self.assertEqual([item.verdict for item in judged], [Verdict.PASS, Verdict.MISSING])
        self.assertEqual(judged[1].mismatches, [])
        self.assertEqual(by_form, {})

    def test_twin_with_other_inputs_refused(self):
        changed = dataclasses.replace(self.guest.inputs[1], fill_seed=self.guest.inputs[1].fill_seed + 1)
        guest = dataclasses.replace(self.guest, inputs=[self.guest.inputs[0], changed])
        with self.assertRaisesRegex(compare.CompareError, "differ in more than MXCSR"):
            lifted._judge(guest, self.results, self.recorded, compare.FAST)

    def test_results_for_other_cases_refused(self):
        with self.assertRaisesRegex(lifted.LiftError, "other cases than its inputs"):
            lifted._judge(self.guest, self.results[::-1], self.recorded, compare.FAST)


class ReportTest(unittest.TestCase):
    def setUp(self):
        run = _flush_run()
        self.guest, self.results, self.recorded = run.guest, run.results, run.recorded
        self.flushed, self.key = run.flushed, run.flushed.instance.form.key

    def test_passing_run(self):
        judged, by_form = lifted._judge(self.guest, self.results, self.recorded, compare.FAST)
        passed, text = _report(judged, by_form)
        self.assertTrue(passed)
        self.assertIn(f"PASS {GROUP} {compare.FAST} {self.key}: 2 cases", text)
        self.assertIn("2 cases, 2 pass, 0 mismatched, 0 missing", text)
        self.assertIn(f"    {LIFT_REPORT}", text)

    def test_missing_case_fails_the_run(self):
        judged, by_form = lifted._judge(self.guest, self.results, self.recorded, compare.EXACT)
        passed, text = _report(judged, by_form, compare.EXACT)
        self.assertFalse(passed)
        self.assertIn(f"FAIL {GROUP} {compare.EXACT} {self.key}: 2 cases, 0 mismatched, 1 missing", text)
        self.assertIn(f"case {self.results[1].case}: {self.flushed.instance.text}: {expectations.NO_EXPECTATION}", text)

    def test_form_check_fails_the_run(self):
        judged, _ = lifted._judge(self.guest, self.results, self.recorded, compare.FAST)
        by_form = {self.key: [Mismatch("destination written", "a check", "another")]}
        passed, text = _report(judged, by_form)
        self.assertFalse(passed)
        self.assertIn("; destination written: expected a check, actual another", text)


if __name__ == "__main__":
    unittest.main()
