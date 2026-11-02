import hashlib
import json
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import ps5lift
from instructions import cases, decoder, program, records, suite
from instructions.records import CaseInput, Header, Outcome, Result
from instructions.tests.support import generated

ELF_EXECUTABLE = 2
ELF_SHARED = 3
ELF_X86_64 = 62
GROUP = "base"
BUNDLED = ("base", "avx2")
CPUINFO_WITHOUT_AVX2 = (
    "processor\t: 0\nflags\t\t: fpu sse sse2 avx avx2\n\n"
    "processor\t: 1\nflags\t\t: fpu sse sse2 avx\n\n"
)


def _compiler():
    compiler = os.environ.get("PS5LIFT_CLANG")
    if compiler is None:
        raise AssertionError("PS5LIFT_CLANG is not set: it names the clang executable")
    return compiler


def _elf_kind(path):
    return struct.unpack_from("<HH", path.read_bytes(), 16)


class BundleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = _compiler()
        cls.work = Path(tempfile.mkdtemp(prefix="ps5lift-bundle-"))
        cls.bundle = cls.work / "bundle"
        suite.bundle(cls.bundle, cls.compiler, BUNDLED, generated(), decoder.from_environment(), ps5lift.GUEST_FLAGS,
                     ps5lift.GUEST_BASE)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def test_bundle_contents(self):
        directory = self.bundle / GROUP
        self.assertEqual(sorted(path.name for path in directory.iterdir()),
                         [GROUP, f"{GROUP}.inputs", "libinstructionhost.so"])
        self.assertEqual(_elf_kind(directory / GROUP), (ELF_EXECUTABLE, ELF_X86_64))
        driver = directory / "libinstructionhost.so"
        self.assertEqual(_elf_kind(driver), (ELF_SHARED, ELF_X86_64))
        self.assertLessEqual({"host_run_instructions", "host_exit"}, set(program.Elf(driver).symbols))
        self.assertEqual(json.loads((self.bundle / "groups.json").read_text()), {"base": [], "avx2": ["avx2"]})
        for name in ("record.py", "instructions/__init__.py", "instructions/records.py", "instructions/runner.py"):
            self.assertTrue((self.bundle / name).is_file(), name)
        self_test = self.bundle / suite.SELF_TEST
        self.assertEqual(sorted(path.name for path in self_test.iterdir()),
                         ["expected.json", "libinstructionhost.so", suite.SELF_TEST, f"{suite.SELF_TEST}.inputs"])

    def test_written_cases_left_out(self):
        instances, specs = generated()[1][GROUP], generated()[2][GROUP]
        header, inputs = records.read_inputs(self.bundle / GROUP / f"{GROUP}.inputs")
        recorded = [spec.index for spec in specs if not cases.expectation_written(instances[spec.instance].form)]
        self.assertEqual([case.case for case in inputs], recorded)
        self.assertLess(len(recorded), len(specs))
        layout = program.read_layout(self.bundle / GROUP / GROUP)
        everything = {case.case: case for case in program.case_inputs(layout, specs)}
        self.assertEqual(inputs, [everything[case] for case in recorded])
        assembly = program.emit(GROUP, instances)
        self.assertEqual(header, Header(*program.fingerprints(assembly, records.pack_input_records(inputs))))

    def test_self_test_expects_the_five_outcomes(self):
        expected = json.loads((self.bundle / suite.SELF_TEST / "expected.json").read_text())
        outcomes = [case["outcome"] for case in expected]
        self.assertEqual(outcomes[:5], ["COMPLETED", "INVALID_OPCODE", "DIVIDE_ERROR", "PAGE_FAULT", "BREAKPOINT"])
        layout = program.read_layout(self.bundle / suite.SELF_TEST / suite.SELF_TEST)
        self.assertEqual(expected[3]["data"], layout.guard)
        self.assertEqual(expected[4]["address"], layout.instances[4].address)
        fills = expected[5:]
        self.assertTrue(fills and all(case["outcome"] == "COMPLETED" and case["memory"] for case in fills))
        _, inputs = records.read_inputs(self.bundle / suite.SELF_TEST / f"{suite.SELF_TEST}.inputs")
        for case in fills:
            block = records.fill_block(inputs[case["case"]].fill_seed)
            for offset, value in case["memory"].items():
                source = int(offset) ^ records.BLOCK_SIZE // 2
                self.assertEqual(value, int.from_bytes(block[source:source + 8], "little"), case["name"])

    def test_missing_cpu_flag_refused(self):
        cpuinfo = self.work / "cpuinfo"
        cpuinfo.write_text(CPUINFO_WITHOUT_AVX2)
        command = [sys.executable, str(self.bundle / "record.py"), "--cpuinfo", str(cpuinfo), "--group", "avx2"]
        completed = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("avx2", completed.stderr)
        self.assertFalse((self.bundle / "results").exists())

    def test_written_group_not_bundled(self):
        with self.assertRaisesRegex(suite.SuiteError, "sse4a"):
            suite.bundle(self.work / "sse4a", self.compiler, ["sse4a"], generated(), decoder.from_environment(),
                         ps5lift.GUEST_FLAGS, ps5lift.GUEST_BASE)
        self.assertFalse((self.work / "sse4a").exists())

    def test_unknown_group_refused(self):
        with self.assertRaisesRegex(suite.SuiteError, "nonexistent"):
            suite.bundle(self.work / "unknown", self.compiler, ["nonexistent"], generated(),
                         decoder.from_environment(), ps5lift.GUEST_FLAGS, ps5lift.GUEST_BASE)

    def test_every_recorded_group_has_cpu_flags(self):
        _, instances, _ = generated()
        recorded = {
            group for group, members in instances.items()
            if not all(cases.expectation_written(instance.form) for instance in members)
        }
        self.assertEqual(set(suite.CPU_FLAGS), recorded)


class ImportTest(unittest.TestCase):
    CASES = (0, 1, 3)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.bundle = Path(self.directory.name) / "bundle"
        self.recordings = Path(self.directory.name) / "recordings"
        self.recordings.mkdir()
        (self.bundle / "alpha").mkdir(parents=True)
        (self.bundle / "groups.json").write_text(json.dumps({"alpha": []}))
        self.inputs = [CaseInput(case, 0, case, case == 1, bytes(records.SLOT_SIZE), ()) for case in self.CASES]
        digest = hashlib.sha256(records.pack_input_records(self.inputs)).digest()
        self.header = Header(bytes(range(32)), digest)
        records.write_inputs(self.bundle / "alpha" / "alpha.inputs", self.header, self.inputs)

    def tearDown(self):
        self.directory.cleanup()

    def _results(self, results, header=None):
        (self.bundle / "results").mkdir(exist_ok=True)
        data = records.write_results(header or self.header, results)
        (self.bundle / "results" / "alpha.results").write_bytes(data)
        return data

    def _completed(self, case, outcome=Outcome.COMPLETED):
        return Result(case, outcome, 0, 0, 0, {8: case}, {}, () if case == 1 else None)

    def _refused(self, pattern):
        with self.assertRaisesRegex(suite.SuiteError, pattern):
            suite.import_bundle(self.bundle, self.recordings)
        self.assertEqual(list(self.recordings.iterdir()), [])

    def test_import_compresses_the_results(self):
        data = self._results([self._completed(case) for case in self.CASES])
        sizes = suite.import_bundle(self.bundle, self.recordings)
        recording = self.recordings / "alpha.results.xz"
        self.assertEqual(sizes, {"alpha": recording.stat().st_size})
        self.assertEqual(lzma.decompress(recording.read_bytes()), data)

    def test_import_refuses_a_missing_case(self):
        self._results([self._completed(case) for case in self.CASES[:2]])
        self._refused("case 3")

    def test_import_refuses_unfinished_outcomes(self):
        for outcome in (Outcome.CRASHED, Outcome.TIMED_OUT, Outcome.HOST_SIGNAL):
            with self.subTest(outcome=outcome.name):
                self._results([self._completed(case, outcome if case == 3 else Outcome.COMPLETED)
                               for case in self.CASES])
                self._refused(outcome.name)

    def test_import_refuses_another_programs_results(self):
        header = Header(bytes(32), self.header.inputs)
        self._results([self._completed(case) for case in self.CASES], header)
        self._refused("fingerprint")

    def test_import_refuses_a_changed_bundle(self):
        self._results([self._completed(case) for case in self.CASES])
        changed = [CaseInput(case.case, case.instance, case.fill_seed + 1, case.console_defined, case.slot, ())
                   for case in self.inputs]
        records.write_inputs(self.bundle / "alpha" / "alpha.inputs", self.header, changed)
        self._refused("alpha.inputs")

    def test_import_checks_the_console_flag(self):
        results = [self._completed(case) for case in self.CASES]
        results[1] = Result(1, Outcome.COMPLETED, 0, 0, 0, {}, {}, None)
        self._results(results)
        self._refused("console")

    def test_import_needs_every_group(self):
        self._refused("alpha.results")


if __name__ == "__main__":
    unittest.main()
