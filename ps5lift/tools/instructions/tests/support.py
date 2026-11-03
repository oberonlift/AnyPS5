import dataclasses
import os
from pathlib import Path

import ps5lift
from instructions import compare, decoder, program, records, suite
from instructions.encoding import Mem
from instructions.model import LOCATIONS, Shape
from instructions.records import Outcome, Result
from instructions.state import Case, State

CODE = 0xFC00001000
TABLE = 0xFC00000000
INPUT = 0xFC00010000
OUTPUT = 0xFC00010500
BLOCK = 0xFC0002F000
GUARD = BLOCK + records.BLOCK_SIZE
LOADER = 0xFC00000100
INSTANCE_STRIDE = 0x100
_TARGET_PARTS = {Shape.BRANCH: "taken", Shape.CALL: "callee", Shape.INDIRECT_JUMP: "target"}
_AH_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7}


def generate():
    return suite.generate(decoder.from_environment())


_GENERATED = []
_GROUPS = {}


def generated():
    if not _GENERATED:
        _GENERATED.append(generate())
    return _GENERATED[0]


def find_form(test, forms, mnemonic, operand_size=None, **conditions):
    matches = [
        form
        for form in forms
        if form.definition.mnemonic == mnemonic
        and form.operand_size == operand_size
        and all(
            getattr(form.definition, name) == value
            if hasattr(form.definition, name)
            else form.definition.filters.get(name) == value
            for name, value in conditions.items()
        )
    ]
    test.assertEqual(len(matches), 1, f"{mnemonic} o{operand_size} {conditions} matched {len(matches)} forms")
    return matches[0]


def compiler():
    found = os.environ.get("PS5LIFT_CLANG")
    if found is None:
        raise AssertionError("PS5LIFT_CLANG is not set: it names the clang executable")
    return found


def sysroot():
    return os.environ.get("PS5LIFT_X86_64_SYSROOT")


def fake_layout(instances):
    entries = []
    for instance in instances:
        function = CODE + INSTANCE_STRIDE * instance.index
        helpers = {_TARGET_PARTS[instance.shape]: function + 0x80} if instance.shape in _TARGET_PARTS else {}
        entries.append(program.InstanceLayout(function, function + 0x40, 8, helpers))
    return program.Layout(TABLE, INPUT, OUTPUT, BLOCK, GUARD, LOADER, tuple(entries))


def group(name):
    if name not in _GROUPS:
        _, instances, specs = generated()
        _GROUPS[name] = (instances[name], specs[name], fake_layout(instances[name]))
    return _GROUPS[name]


def primary(mnemonic, registers_only=True, operand_size=None):
    def choose(instance):
        return (instance.form.definition.mnemonic == mnemonic and instance.role == "primary"
                and (operand_size is None or instance.form.operand_size == operand_size)
                and (not registers_only or all(not isinstance(operand, Mem) for operand in instance.operands)))
    return choose


def case_of(name, spec):
    instances, _, layout = group(name)
    (case_input,) = program.case_inputs(layout, [spec])
    return Case(spec, instances[spec.instance], case_input, layout)


def find_case(name, choose, accept=lambda spec: True):
    instances, specs, _ = group(name)
    instance = next(instance for instance in instances if choose(instance))
    spec = next(spec for spec in specs if spec.instance == instance.index and accept(spec))
    return case_of(name, spec)


def slot_words(case, changes):
    slot = bytearray(case.input.slot)
    for offset, data in changes.items():
        slot[offset:offset + len(data)] = data
    return {
        offset: int.from_bytes(slot[offset:offset + 8], "little")
        for offset in range(0, records.SLOT_SIZE, 8)
        if slot[offset:offset + 8] != case.input.slot[offset:offset + 8]
    }


def completed(case, changes=(), memory=None, console=None):
    return Result(case.spec.index, Outcome.COMPLETED, 0, 0, 0, slot_words(case, dict(changes)), memory or {}, console)


def fault(case, outcome, address, data=0):
    return Result(case.spec.index, outcome, 0, address, data, {}, {}, None)


def flags(**values):
    found = {name: 0 for name in records.FLAG_NAMES}
    found.update(values)
    ah = 0x02
    for name, bit in _AH_BITS.items():
        ah |= found[name] << bit
    return {records.STORE_AX: bytes([found["OF"], ah]), records.STORE_DF: bytes([found["DF"]])}


def linked_group(name, work):
    _, instances, specs = generated()
    directory = Path(work) / name
    directory.mkdir()
    suite.compile_native_driver(compiler(), directory, sysroot())
    linked = program.build(compiler(), directory / f"{name}.s", directory, sysroot(), ps5lift.GUEST_FLAGS,
                           ps5lift.GUEST_BASE, name, instances[name], specs[name])
    header = suite.recording_header(linked.assembly, suite.recorded_inputs(instances[name], linked.inputs))
    recorded = suite.read_recording(suite.RECORDINGS / f"{name}{suite.RECORDING_SUFFIX}", header)
    found = [Case(spec, instances[name][spec.instance], case_input, linked.layout)
             for spec, case_input in zip(specs[name], linked.inputs)]
    return found, recorded


def with_slot(case, changes):
    slot = bytearray(case.input.slot)
    for offset, data in changes.items():
        slot[offset:offset + len(data)] = data
    return dataclasses.replace(case, input=dataclasses.replace(case.input, slot=bytes(slot)))


def lane_values(case, operand, values, bits=32):
    number = LOCATIONS[case.instance.operands[operand].name][1]
    return {records.ymm_offset(number) + lane * bits // 8: value.to_bytes(bits // 8, "little")
            for lane, value in enumerate(values)}


def with_lanes(case, operand, *values, bits=32):
    return with_slot(case, lane_values(case, operand, values, bits))


def vector_field(case, operand):
    return f"ymm{LOCATIONS[case.instance.operands[operand].name][1]}"


def judge(case, expected, actual, mode=compare.EXACT, run=None, expected_slot=None):
    rules = compare.rules_for(case.instance)
    expected_state = State.of(expected, case.input.slot if expected_slot is None else expected_slot)
    actual_state = State.of(actual, case.input.slot)
    return compare.compare(case, rules, expected_state, actual_state, mode, run or compare.Run(4))


def fields(mismatches):
    return {mismatch.field for mismatch in mismatches}
