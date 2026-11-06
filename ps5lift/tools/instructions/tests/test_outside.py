import re
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
from instructions.tests.support import case_of, compiler, completed, fault, find_case, generated, group, judge, sysroot

GROUPS = {
    Group.NOT_ON_PS5: "outside-not-on-ps5",
    Group.PRIVILEGED: "outside-privileged",
    Group.SEGMENT_TABLES: "outside-segment-tables",
    Group.CONSOLE_KERNEL: "outside-console-kernel",
}
OUTCOMES = {
    Group.NOT_ON_PS5: Outcome.INVALID_OPCODE,
    Group.PRIVILEGED: Outcome.GENERAL_PROTECTION,
    Group.SEGMENT_TABLES: Outcome.SEGMENT_TABLES,
    Group.CONSOLE_KERNEL: Outcome.CONSOLE_KERNEL,
}
VECTOR = 0x29
NOT_ON_PS5 = 1
STATE_HEADER = Path(__file__).resolve().parents[3] / "runtime" / "include" / "ps5lift" / "State.h"


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
            ("outside-not-on-ps5", _instance_of("vaddps", encoding="evex"), Outcome.INVALID_OPCODE, NOT_ON_PS5),
            ("outside-privileged", _instance_of("wrmsr"), Outcome.GENERAL_PROTECTION, 0),
            ("outside-segment-tables", _instance_of("lar"), Outcome.SEGMENT_TABLES, 0),
            ("outside-console-kernel", _instance_of("syscall"), Outcome.CONSOLE_KERNEL, 0),
        )
        for name, choose, outcome, detail in examples:
            with self.subTest(group=name, outcome=outcome.name):
                case = find_case(name, choose)
                expected = expectations.expected_result(case, None)
                self.assertEqual((expected.outcome, expected.address, expected.detail),
                                 (outcome, case.address, detail))
                self.assertEqual((expected.slot_words, expected.memory_words), ({}, {}))

    def test_every_outside_case_expects_its_group_stop(self):
        for outside, name in GROUPS.items():
            _, specs, _ = group(name)
            details = {NOT_ON_PS5} if outside is Group.NOT_ON_PS5 else {0}
            with self.subTest(group=name):
                found = set()
                for spec in specs:
                    case = case_of(name, spec)
                    expected = expectations.expected_result(case, None)
                    self.assertEqual((expected.outcome, expected.address), (OUTCOMES[outside], case.address))
                    found.add(expected.detail)
                self.assertEqual(found, {VECTOR, 0} if outside is Group.CONSOLE_KERNEL else details)

    def test_not_on_ps5_detail_is_the_runtimes(self):
        declared = re.search(r"PS5LIFT_INVALID_OPCODE_NOT_ON_PS5 = (\d+)", STATE_HEADER.read_text())
        self.assertIsNotNone(declared, f"{STATE_HEADER} declares no PS5LIFT_INVALID_OPCODE_NOT_ON_PS5")
        self.assertEqual(int(declared.group(1)), NOT_ON_PS5)
        case = find_case("outside-not-on-ps5", _instance_of("vaddps", encoding="evex"))
        expected = expectations.expected_result(case, None)
        self.assertEqual(expected.detail, int(declared.group(1)))
        by_definition = fault(case, Outcome.INVALID_OPCODE, case.address)
        self.assertEqual([mismatch.field for mismatch in judge(case, expected, by_definition)], ["detail"])

    def test_outside_case_with_a_recording_refused(self):
        case = find_case("outside-privileged", _instance_of("wrmsr"))
        with self.assertRaisesRegex(expectations.ExpectationError, "has a recording"):
            expectations.expected_result(case, fault(case, Outcome.GENERAL_PROTECTION, case.address))

    def test_int_expects_its_vector(self):
        case = find_case("outside-console-kernel", _instance_of("int"))
        self.assertEqual(case.instance.operands, (Imm(VECTOR),))
        self.assertIn("0x29", case.instance.text)
        expected = expectations.expected_result(case, None)
        self.assertEqual((expected.outcome, expected.address, expected.detail),
                         (Outcome.CONSOLE_KERNEL, case.address, VECTOR))
        without_vector = fault(case, Outcome.CONSOLE_KERNEL, case.address)
        self.assertEqual([mismatch.field for mismatch in judge(case, expected, without_vector)], ["detail"])

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
                    defaults = encoding.default_operands(instance.form)
                    if outside is Group.CONSOLE_KERNEL:
                        defaults = [Imm(VECTOR) if isinstance(choice, Imm) else choice for choice in defaults]
                    self.assertEqual(instance.operands, tuple(defaults))
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
                suite.bundle(work / "bundle", compiler(), sysroot(), ["outside-privileged"], generated(),
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
