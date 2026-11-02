import sys
import tempfile
import unittest
from pathlib import Path

from instructions import records, runner
from instructions.records import CaseInput, Header, Outcome

CASES = 6
TOOLS = Path(records.__file__).resolve().parents[1]
STUB = f"""import os
import sys
import time

sys.path.insert(0, {str(TOOLS)!r})
from instructions import records

behaviour, at = sys.argv[1], int(sys.argv[2])
if behaviour == "fail":
    sys.stderr.write("cannot start\\n")
    sys.exit(records.DRIVER_FAILED)
_, inputs = records.read_inputs(os.environ[records.INPUTS_VARIABLE])
first = int(os.environ[records.FIRST_VARIABLE])
with open(os.environ[records.RESULTS_VARIABLE], "ab", buffering=0) as results:
    for case in inputs[first:]:
        record = records.pack_result(
            records.Result(case.case, records.Outcome.COMPLETED, 0, 0, 0, {{8: case.case}}, {{}}, None))
        if case.case == at:
            sys.stderr.write(f"stopped at case {{at}}\\n")
            if behaviour == "crash":
                sys.exit(70)
            if behaviour == "partial":
                results.write(record[:5])
                sys.exit(70)
            if behaviour == "hang":
                time.sleep(1000)
            if behaviour == "signal":
                os.kill(os.getpid(), 9)
        results.write(record)
"""


def _inputs(path, cases):
    header = Header(bytes(range(32)), bytes(range(32, 64)))
    slot = bytes(records.SLOT_SIZE)
    records.write_inputs(path, header, [CaseInput(case, 0, case, False, slot, ()) for case in cases])
    return header


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.work = Path(self.directory.name)
        self.stub = self.work / "stub.py"
        self.stub.write_text(STUB)
        self.inputs = self.work / "cases.inputs"
        self.results = self.work / "cases.results"

    def tearDown(self):
        self.directory.cleanup()

    def _run(self, behaviour, at, cases=range(CASES), timeout=30):
        header = _inputs(self.inputs, cases)
        stderr = runner.run([sys.executable, str(self.stub), behaviour, str(at)], {}, self.inputs, self.results, timeout)
        found, results = records.read_results(self.results.read_bytes())
        self.assertEqual(found, header)
        self.assertEqual([result.case for result in results], list(cases))
        return stderr, {result.case: result for result in results}

    def _assert_completed(self, results, except_case):
        for case, result in results.items():
            if case != except_case:
                self.assertEqual((result.outcome, result.slot_words), (Outcome.COMPLETED, {8: case}), f"case {case}")

    def test_runner_resumes_after_crash(self):
        stderr, results = self._run("crash", 2)
        self.assertEqual(results[2].outcome, Outcome.CRASHED)
        self.assertEqual(results[2].detail, 70)
        self.assertEqual((results[2].slot_words, results[2].memory_words), ({}, {}))
        self._assert_completed(results, 2)
        self.assertEqual(list(stderr), [2])
        self.assertIn("stopped at case 2", stderr[2])

    def test_runner_records_timeout(self):
        stderr, results = self._run("hang", 1, timeout=1)
        self.assertEqual(results[1].outcome, Outcome.TIMED_OUT)
        self._assert_completed(results, 1)
        self.assertIn("stopped at case 1", stderr[1])

    def test_runner_drops_a_partial_record(self):
        stderr, results = self._run("partial", 3)
        self.assertEqual(results[3].outcome, Outcome.CRASHED)
        self._assert_completed(results, 3)

    def test_runner_names_the_signal(self):
        _, results = self._run("signal", 0)
        self.assertEqual((results[0].outcome, results[0].detail), (Outcome.CRASHED, 128 + 9))
        self._assert_completed(results, 0)

    def test_runner_keeps_case_indices(self):
        stderr, results = self._run("crash", 7, cases=(0, 4, 7, 9))
        self.assertEqual(results[7].outcome, Outcome.CRASHED)
        self._assert_completed(results, 7)

    def test_runner_keeps_the_end_of_stderr(self):
        stub = self.work / "noisy.py"
        stub.write_text("import sys\nsys.stderr.write('x' * 10000 + 'end')\nsys.exit(70)\n")
        _inputs(self.inputs, range(1))
        stderr = runner.run([sys.executable, str(stub)], {}, self.inputs, self.results, 30)
        self.assertEqual(len(stderr[0]), runner.STDERR_KEPT)
        self.assertTrue(stderr[0].endswith("end"))

    def test_driver_failure_ends_the_run(self):
        _inputs(self.inputs, range(CASES))
        with self.assertRaisesRegex(runner.RunnerError, "cannot start"):
            runner.run([sys.executable, str(self.stub), "fail", "0"], {}, self.inputs, self.results, 30)


if __name__ == "__main__":
    unittest.main()
