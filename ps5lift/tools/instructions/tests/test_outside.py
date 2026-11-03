import dataclasses
import shutil
import tempfile
import unittest
from pathlib import Path

import ps5lift
from instructions import cases, compare, database, decoder, encoding, expectations, suite
from instructions.compare import Verdict
from instructions.database import Group
from instructions.encoding import Imm
from instructions.records import Outcome
from instructions.state import State
from instructions.tests.support import case_of, compiler, completed, fault, find_case, generated, group, judge

GROUPS = {
    Group.NOT_ON_PS5: "outside-not-on-ps5",
    Group.PRIVILEGED: "outside-privileged",
    Group.SEGMENT_TABLES: "outside-segment-tables",
    Group.CONSOLE_KERNEL: "outside-console-kernel",
}
VECTOR = 0x29


def _instance_of(mnemonic, **filters):
    def choose(instance):
        definition = instance.form.definition
        return definition.mnemonic == mnemonic and all(
            getattr(definition, name) == value for name, value in filters.items()
        )
    return choose


class OutsideCasesTest(unittest.TestCase):
    def test_outside_expectations(self):
        examples = (
            ("outside-not-on-ps5", _instance_of("vaddps", encoding="evex"), Outcome.INVALID_OPCODE),
            ("outside-privileged", _instance_of("wrmsr"), Outcome.GENERAL_PROTECTION),
            ("outside-segment-tables", _instance_of("lar"), Outcome.SEGMENT_TABLES),
            ("outside-console-kernel", _instance_of("syscall"), Outcome.CONSOLE_KERNEL),
        )
        for name, choose, outcome in examples:
            with self.subTest(group=name, outcome=outcome.name):
                case = find_case(name, choose)
                expected = expectations.expected_result(case, None)
                self.assertEqual((expected.outcome, expected.address, expected.detail), (outcome, case.address, 0))
                self.assertEqual((expected.slot_words, expected.memory_words), ({}, {}))

    def test_outside_case_with_a_recording_refused(self):
        case = find_case("outside-privileged", _instance_of("wrmsr"))
        with self.assertRaisesRegex(expectations.ExpectationError, "has a recording"):
            expectations.expected_result(case, fault(case, Outcome.GENERAL_PROTECTION, case.address))

    def test_int_expects_its_vector(self):
        case = find_case("outside-console-kernel", _instance_of("int"))
        self.assertEqual(case.instance.operands, (Imm(0),))
        vector = dataclasses.replace(case, instance=dataclasses.replace(case.instance, operands=(Imm(VECTOR),)))
        expected = expectations.expected_result(vector, None)
        self.assertEqual((expected.outcome, expected.detail), (Outcome.CONSOLE_KERNEL, VECTOR))

    def test_one_default_instance_and_case_per_definition(self):
        _, instances, specs = generated()
        definitions = database.load()
        for outside, name in GROUPS.items():
            with self.subTest(group=name):
                members = instances[name]
                expected = [definition.index for definition in definitions if database.classify(definition) is outside]
                self.assertEqual([instance.form.definition.index for instance in members], expected)
                for instance in members:
                    self.assertEqual(instance.form, database.forms([instance.form.definition])[0])
                    self.assertEqual(instance.operands, tuple(encoding.default_operands(instance.form)))
                    self.assertEqual(instance.prefixes, ())
                self.assertEqual([spec.instance for spec in specs[name]], list(range(len(members))))
                self.assertEqual([spec.index for spec in specs[name]], list(range(len(members))))

    def test_branch_targets_have_a_label(self):
        (xbegin,) = [instance for instance in generated()[1]["outside-not-on-ps5"]
                     if instance.form.definition.mnemonic == "xbegin"]
        self.assertIs(xbegin.shape, cases.Shape.BRANCH)
        (lar,) = [instance for instance in generated()[1]["outside-segment-tables"]
                  if instance.form.definition.mnemonic == "lar"
                  and instance.form.definition.filters["modrm_mod"] == "3"]
        self.assertIs(lar.shape, cases.Shape.STRAIGHT)

    def test_outside_cases_are_never_recorded(self):
        _, instances, specs = generated()
        for name in GROUPS.values():
            self.assertTrue(all(cases.expectation_written(instance.form) for instance in instances[name]), name)
        recordable = suite.recordable_groups(instances)
        self.assertEqual([name for name in recordable if name.startswith("outside-")], [])
        self.assertIn("base", recordable)
        work = Path(tempfile.mkdtemp(prefix="ps5lift-outside-"))
        try:
            with self.assertRaisesRegex(suite.SuiteError, "outside-privileged has no case to record"):
                suite.bundle(work / "bundle", compiler(), ["outside-privileged"], generated(),
                             decoder.from_environment(), ps5lift.GUEST_FLAGS, ps5lift.GUEST_BASE)
            self.assertFalse((work / "bundle").exists())
        finally:
            shutil.rmtree(work)

    def test_judged_by_the_stop(self):
        case = find_case("outside-segment-tables", _instance_of("lar"))
        expected = expectations.expected_result(case, None)
        for mode in compare.FLOAT_MODES:
            with self.subTest(mode=mode):
                self.assertEqual(judge(case, expected, expected, mode), [])
                missing = fault(case, Outcome.MISSING_TRANSLATION, case.address)
                mismatches = judge(case, expected, missing, mode)
                self.assertEqual([mismatch.field for mismatch in mismatches], ["outcome"])
                self.assertIs(compare.verdict(State.of(missing, case.input.slot), mismatches), Verdict.STOPPED)
                other = fault(case, Outcome.GENERAL_PROTECTION, case.address)
                self.assertEqual([mismatch.field for mismatch in judge(case, expected, other, mode)], ["outcome"])
                elsewhere = fault(case, Outcome.SEGMENT_TABLES, case.address + 1)
                self.assertEqual([mismatch.field for mismatch in judge(case, expected, elsewhere, mode)], ["address"])
                self.assertEqual([mismatch.field for mismatch in judge(case, expected, completed(case), mode)],
                                 ["outcome"])

    def test_every_outside_instance_has_rules(self):
        for name in GROUPS.values():
            instances, specs, _ = group(name)
            for instance in instances:
                rules = compare.rules_for(instance)
                self.assertEqual((rules.mnemonic, rules.inputs, rules.float_lanes),
                                 (instance.form.definition.mnemonic, (), ()))
            case = case_of(name, specs[0])
            text = compare.format_mismatch(case, [compare.Mismatch("outcome", "expected", "actual")])
            self.assertIn(case.instance.text, text)


if __name__ == "__main__":
    unittest.main()
