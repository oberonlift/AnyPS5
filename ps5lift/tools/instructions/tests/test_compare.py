import dataclasses
import lzma
import struct
import tempfile
import unittest
from pathlib import Path

from instructions import compare, program, records, suite, tolerance
from instructions.cases import CONSOLE_MXCSR, BlockOffset
from instructions.encoding import Mem
from instructions.model import LOCATIONS, Shape
from instructions.records import Header, Outcome, Result
from instructions.tests.support import generated

CODE = 0xFC00001000
TABLE = 0xFC00000000
INPUT = 0xFC00010000
OUTPUT = 0xFC00010500
BLOCK = 0xFC0002F000
GUARD = BLOCK + records.BLOCK_SIZE
LOADER = 0xFC00000100
INSTANCE_STRIDE = 0x100
EXACT, FAST = compare.EXACT, compare.FAST
_TARGET_PARTS = {Shape.BRANCH: "taken", Shape.CALL: "callee", Shape.INDIRECT_JUMP: "target"}
_AH_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7}
_DAZ = 1 << 6
_FTZ = 1 << 15
_UNMASK_INVALID = 0x1F80 & ~(1 << 7)
_GROUPS = {}


def _layout(instances):
    entries = []
    for instance in instances:
        function = CODE + INSTANCE_STRIDE * instance.index
        helpers = {_TARGET_PARTS[instance.shape]: function + 0x80} if instance.shape in _TARGET_PARTS else {}
        entries.append(program.InstanceLayout(function, function + 0x40, 8, helpers))
    return program.Layout(TABLE, INPUT, OUTPUT, BLOCK, GUARD, LOADER, tuple(entries))


def _group(group):
    if group not in _GROUPS:
        _, instances, specs = generated()
        _GROUPS[group] = (instances[group], specs[group], _layout(instances[group]))
    return _GROUPS[group]


def _primary(mnemonic, registers_only=True, operand_size=None):
    def choose(instance):
        return (instance.form.definition.mnemonic == mnemonic and instance.role == "primary"
                and (operand_size is None or instance.form.operand_size == operand_size)
                and (not registers_only or all(not isinstance(operand, Mem) for operand in instance.operands)))
    return choose


def _case_of(group, spec):
    instances, _, layout = _group(group)
    (case_input,) = program.case_inputs(layout, [spec])
    return compare.Case(spec, instances[spec.instance], case_input, layout)


def _case(group, choose, accept=lambda spec: True):
    instances, specs, _ = _group(group)
    instance = next(instance for instance in instances if choose(instance))
    spec = next(spec for spec in specs if spec.instance == instance.index and accept(spec))
    return _case_of(group, spec)


def _slot_words(case, changes):
    slot = bytearray(case.input.slot)
    for offset, data in changes.items():
        slot[offset:offset + len(data)] = data
    return {
        offset: int.from_bytes(slot[offset:offset + 8], "little")
        for offset in range(0, records.SLOT_SIZE, 8)
        if slot[offset:offset + 8] != case.input.slot[offset:offset + 8]
    }


def _completed(case, changes=(), memory=None, console=None):
    return Result(case.spec.index, Outcome.COMPLETED, 0, 0, 0, _slot_words(case, dict(changes)), memory or {}, console)


def _fault(case, outcome, address, data=0):
    return Result(case.spec.index, outcome, 0, address, data, {}, {}, None)


def _flags(**values):
    flags = {name: 0 for name in records.FLAG_NAMES}
    flags.update(values)
    ah = 0x02
    for name, bit in _AH_BITS.items():
        ah |= flags[name] << bit
    return {records.STORE_AX: bytes([flags["OF"], ah]), records.STORE_DF: bytes([flags["DF"]])}


def _gpr(case, name, value):
    return {records.gpr_offset(records.GPR_NAMES.index(name)): value.to_bytes(8, "little")}


def _register_value(case, name):
    offset = records.gpr_offset(records.GPR_NAMES.index(name))
    return int.from_bytes(case.input.slot[offset:offset + 8], "little")


def _lane(case, operand, value, lane=0, bits=32):
    number = LOCATIONS[case.instance.operands[operand].name][1]
    offset = records.ymm_offset(number) + lane * bits // 8
    return {offset: value.to_bytes(bits // 8, "little")}


def _vector_field(case, operand):
    return f"ymm{LOCATIONS[case.instance.operands[operand].name][1]}"


def _mxcsr(case):
    return struct.unpack_from("<I", case.input.slot, records.FX_MXCSR)[0]


def _mxcsr_bytes(value):
    return {records.FX_MXCSR: struct.pack("<I", value)}


def _judge(case, expected, actual, mode=EXACT, run=None, expected_slot=None):
    rules = compare.rules_for(case.instance)
    expected_state = compare.State.of(expected, case.input.slot if expected_slot is None else expected_slot)
    actual_state = compare.State.of(actual, case.input.slot)
    return compare.compare(case, rules, expected_state, actual_state, mode, run or compare.Run(4))


def _fields(mismatches):
    return {mismatch.field for mismatch in mismatches}


class FlagsTest(unittest.TestCase):
    def test_undefined_flags_ignored(self):
        case = _case("base", _primary("bsf", operand_size=32))
        self.assertLessEqual({"CF", "PF", "AF", "SF", "OF"}, compare.rules_for(case.instance).undefined_flags)
        expected = _completed(case, _flags(ZF=1))
        actual = _completed(case, _flags(ZF=1, CF=1, PF=1, AF=1, SF=1, OF=1))
        self.assertEqual(_judge(case, expected, actual), [])
        self.assertEqual(_fields(_judge(case, expected, _completed(case, _flags()))), {"ZF"})

    def test_status_flags_ignored_for_call_ret_indirect_jump(self):
        for shape in (Shape.CALL, Shape.RETURN, Shape.INDIRECT_JUMP):
            with self.subTest(shape=shape.value):
                case = _case("base", lambda instance: instance.shape is shape and instance.role == "primary")
                self.assertTrue(compare.rules_for(case.instance).status_flags_dropped)
                expected = _completed(case, _flags())
                actual = _completed(case, _flags(CF=1, PF=1, AF=1, ZF=1, SF=1, OF=1))
                self.assertEqual(_judge(case, expected, actual), [])
        branch = _case("base", lambda instance: instance.shape is Shape.BRANCH and instance.role == "primary")
        self.assertFalse(compare.rules_for(branch.instance).status_flags_dropped)
        self.assertEqual(_fields(_judge(branch, _completed(branch, _flags()), _completed(branch, _flags(CF=1)))),
                         {"CF"})

    def test_df_compared_for_call(self):
        case = _case("base", lambda instance: instance.shape is Shape.CALL and instance.role == "primary")
        mismatches = _judge(case, _completed(case, _flags()), _completed(case, _flags(DF=1)))
        self.assertEqual(_fields(mismatches), {"DF"})


class FastModeTest(unittest.TestCase):
    def _addps(self, accept=lambda spec: spec.mxcsr == CONSOLE_MXCSR):
        return _case("sse", _primary("addps"), accept)

    def test_fast_nan_any(self):
        case = self._addps()
        expected = _completed(case, _lane(case, 0, 0xFFC00000))
        actual = _completed(case, _lane(case, 0, 0x7FC12345))
        self.assertEqual(_judge(case, expected, actual, FAST), [])
        self.assertEqual(_fields(_judge(case, expected, actual, EXACT)), {_vector_field(case, 0)})
        not_nan = _completed(case, _lane(case, 0, 0x3F800000))
        self.assertEqual(_fields(_judge(case, expected, not_nan, FAST)), {_vector_field(case, 0)})
        other_lane = _completed(case, {**_lane(case, 0, 0x7FC12345), **_lane(case, 0, 0x3F800000, lane=1)})
        self.assertEqual(_fields(_judge(case, expected, other_lane, FAST)), {_vector_field(case, 0)})

    def test_fast_min_max_daz(self):
        case = _case("sse", _primary("minps"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        self.assertTrue(_mxcsr(case) & _DAZ)
        expected = _completed(case, _lane(case, 0, 0x00000000))
        denormal = _completed(case, _lane(case, 0, 0x00000001))
        self.assertEqual(_judge(case, expected, denormal, FAST), [])
        self.assertEqual(_fields(_judge(case, expected, denormal, EXACT)), {_vector_field(case, 0)})
        other_sign = _completed(case, _lane(case, 0, 0x80000001))
        self.assertEqual(_fields(_judge(case, expected, other_sign, FAST)), {_vector_field(case, 0)})
        without_daz = _case("sse", _primary("minps"), lambda spec: spec.mxcsr == 0x1F80)
        expected = _completed(without_daz, _lane(without_daz, 0, 0x00000000))
        denormal = _completed(without_daz, _lane(without_daz, 0, 0x00000001))
        self.assertEqual(_fields(_judge(without_daz, expected, denormal, FAST)), {_vector_field(without_daz, 0)})
        addps = self._addps()
        expected = _completed(addps, _lane(addps, 0, 0x00000000))
        denormal = _completed(addps, _lane(addps, 0, 0x00000001))
        self.assertEqual(_fields(_judge(addps, expected, denormal, FAST)), {_vector_field(addps, 0)})

    def test_fast_exception_flags_ignored(self):
        case = self._addps()
        initial = _mxcsr(case)
        expected = _completed(case, _mxcsr_bytes(initial | 0x20))
        actual = _completed(case, _mxcsr_bytes(initial | 0x01))
        self.assertEqual(_judge(case, expected, actual, FAST), [])
        self.assertEqual(_fields(_judge(case, expected, actual, EXACT)), {"mxcsr"})
        rounding = _completed(case, _mxcsr_bytes(initial | 0x20 | 1 << 13))
        self.assertEqual(_fields(_judge(case, expected, rounding, FAST)), {"mxcsr"})

    def test_fast_flush_twin(self):
        _, specs, _ = _group("sse")
        case = self._addps(lambda spec: spec.twin is not None and spec.mxcsr == CONSOLE_MXCSR & ~_FTZ)
        twin = _case_of("sse", specs[case.spec.twin])
        self.assertEqual(compare.reference_case(case.spec, FAST), twin.spec.index)
        self.assertEqual(compare.reference_case(case.spec, EXACT), case.spec.index)
        self.assertEqual(compare.reference_case(twin.spec, FAST), twin.spec.index)
        recorded = _completed(twin, _lane(twin, 0, 0x00000000))
        flushed = _completed(case, _lane(case, 0, 0x00000000))
        self.assertEqual(_judge(case, recorded, flushed, FAST, expected_slot=twin.input.slot), [])
        both_bits = _completed(case, {**_lane(case, 0, 0x00000000), **_mxcsr_bytes(CONSOLE_MXCSR)})
        self.assertEqual(_fields(_judge(case, recorded, both_bits, FAST, expected_slot=twin.input.slot)), {"mxcsr"})
        unflushed = _completed(case, _lane(case, 0, 0x00000005))
        self.assertEqual(_fields(_judge(case, recorded, unflushed, FAST, expected_slot=twin.input.slot)),
                         {_vector_field(case, 0)})

    def test_fast_tininess(self):
        case = self._addps()
        self.assertTrue(_mxcsr(case) & (_FTZ | _DAZ))
        for normal, zero in ((0x00800000, 0x00000000), (0x80800000, 0x80000000)):
            with self.subTest(normal=hex(normal)):
                expected = _completed(case, _lane(case, 0, normal))
                self.assertEqual(_judge(case, expected, _completed(case, _lane(case, 0, zero)), FAST), [])
                self.assertEqual(_fields(_judge(case, expected, _completed(case, _lane(case, 0, zero)), EXACT)),
                                 {_vector_field(case, 0)})
                other = _completed(case, _lane(case, 0, zero ^ 0x80000000))
                self.assertEqual(_fields(_judge(case, expected, other, FAST)), {_vector_field(case, 0)})
        plain = self._addps(lambda spec: spec.mxcsr == 0x1F80)
        expected = _completed(plain, _lane(plain, 0, 0x00800000))
        self.assertEqual(_fields(_judge(plain, expected, _completed(plain, _lane(plain, 0, 0)), FAST)),
                         {_vector_field(plain, 0)})

    def test_fast_unmask_stop_at_loader(self):
        case = self._addps(lambda spec: spec.mxcsr == _UNMASK_INVALID)
        recorded = _completed(case, _lane(case, 0, 0x3F800000))
        stop = _fault(case, Outcome.NEEDS_EXACT_FLOAT, LOADER)
        self.assertEqual(_judge(case, recorded, stop, FAST), [])
        at_instance = _fault(case, Outcome.NEEDS_EXACT_FLOAT, case.layout.instances[case.instance.index].address)
        self.assertEqual(_fields(_judge(case, recorded, at_instance, FAST)), {"address"})
        self.assertEqual(_fields(_judge(case, recorded, recorded, FAST)), {"outcome"})
        self.assertEqual(_judge(case, recorded, recorded, EXACT), [])

    def test_fast_unmask_stop_at_ldmxcsr(self):
        def unmasks(spec):
            return spec.loads_mxcsr is not None and spec.loads_mxcsr & 0x1F80 != 0x1F80 and spec.mxcsr == CONSOLE_MXCSR
        case = _case("sse", lambda instance: instance.form.definition.mnemonic == "ldmxcsr"
                     and instance.role == "primary", unmasks)
        address = case.layout.instances[case.instance.index].address
        recorded = _completed(case, _mxcsr_bytes(case.spec.loads_mxcsr))
        self.assertEqual(_judge(case, recorded, _fault(case, Outcome.NEEDS_EXACT_FLOAT, address), FAST), [])
        self.assertEqual(_fields(_judge(case, recorded, _fault(case, Outcome.NEEDS_EXACT_FLOAT, LOADER), FAST)),
                         {"address"})
        self.assertEqual(_judge(case, recorded, recorded, EXACT), [])
        protection = _fault(case, Outcome.GENERAL_PROTECTION, address)
        self.assertEqual(_judge(case, protection, protection, FAST), [])

    def test_exact_unmasked_is_simd_fault(self):
        case = self._addps(lambda spec: spec.mxcsr == _UNMASK_INVALID)
        address = case.layout.instances[case.instance.index].address
        recorded = _fault(case, Outcome.SIMD_FLOATING_POINT, address)
        self.assertEqual(_judge(case, recorded, recorded, EXACT), [])
        self.assertEqual(_fields(_judge(case, recorded, _completed(case), EXACT)), {"outcome"})
        stop = _fault(case, Outcome.NEEDS_EXACT_FLOAT, LOADER)
        self.assertEqual(_fields(_judge(case, recorded, stop, EXACT)), {"outcome"})


class StateTest(unittest.TestCase):
    def test_storer_fields_ignored(self):
        case = _case("x87", _primary("fadd"))
        ignored = {
            records.FX_FOP: b"\x12\x34", 8: bytes(range(1, 17)), records.FX_MXCSR_MASK: b"\xff\xff\x00\x00",
            records.FX_XMM: b"\x55" * 256, records.FX_RESERVED: b"\xaa" * 96,
        }
        self.assertEqual(_judge(case, _completed(case), _completed(case, ignored)), [])
        control = struct.unpack_from("<H", case.input.slot, records.FX_FCW)[0] ^ 0x0400
        self.assertEqual(_fields(_judge(case, _completed(case), _completed(case, {0: struct.pack("<H", control)}))),
                         {"fcw"})

    def test_fsin_compared_with_reference(self):
        case = _case("x87", lambda instance: instance.form.definition.mnemonic == "fsin")
        slot = bytearray(case.input.slot)
        struct.pack_into("<HHB", slot, records.FX_FCW, 0x037F, 0, 0x01)
        slot[records.fx_st_offset(0):records.fx_st_offset(0) + 10] = (0x3FFF8000000000000000).to_bytes(10, "little")
        case = dataclasses.replace(case, input=dataclasses.replace(case.input, slot=bytes(slot)))
        (reference,) = tolerance.x87_reference("fsin", (0x3FFF8000000000000000,), 64, 0)
        st0 = records.fx_st_offset(0)
        recorded = _completed(case, {st0: (reference + 5).to_bytes(10, "little"), 2: struct.pack("<H", 0x0200)})
        within = _completed(case, {st0: (reference - 1).to_bytes(10, "little")})
        self.assertEqual(_judge(case, recorded, within), [])
        outside = _completed(case, {st0: (reference + 2).to_bytes(10, "little")})
        self.assertEqual(_fields(_judge(case, recorded, outside)), {"st0"})

    def test_rcp_lanes_within_tolerance(self):
        case = _case("sse", _primary("rcpps"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        expected = _completed(case, _lane(case, 0, 0x3F800000))
        self.assertEqual(_judge(case, expected, _completed(case, _lane(case, 0, 0x3F800C00))), [])
        self.assertEqual(_fields(_judge(case, expected, _completed(case, _lane(case, 0, 0x3F800C01)))),
                         {_vector_field(case, 0)})

    def test_page_fault_data_exact(self):
        case = _case("base", _primary("mov", registers_only=False, operand_size=64))
        address = case.layout.instances[case.instance.index].address
        recorded = _fault(case, Outcome.PAGE_FAULT, address, GUARD)
        self.assertEqual(_judge(case, recorded, _fault(case, Outcome.PAGE_FAULT, address, GUARD)), [])
        self.assertEqual(_fields(_judge(case, recorded, _fault(case, Outcome.PAGE_FAULT, address, GUARD + 1))),
                         {"data"})

    def test_fxsave_page_fault_data_inside_guard_and_access(self):
        def crossing(spec):
            return BlockOffset(records.BLOCK_SIZE) in spec.gprs
        case = _case("sse", _primary("fxsave", registers_only=False), crossing)
        address = case.layout.instances[case.instance.index].address
        recorded = _fault(case, Outcome.PAGE_FAULT, address, GUARD + 0x1FF)
        for data, accepted in ((GUARD, True), (GUARD + 0x1FF, True), (GUARD + 511, True), (GUARD + 512, False),
                               (GUARD - 1, False)):
            with self.subTest(data=hex(data)):
                mismatches = _judge(case, recorded, _fault(case, Outcome.PAGE_FAULT, address, data))
                self.assertEqual(_fields(mismatches), set() if accepted else {"data"})


class PropertyTest(unittest.TestCase):
    def test_rdrand_properties(self):
        for size in (16, 32, 64):
            with self.subTest(size=size):
                case = _case("rdrand", _primary("rdrand", operand_size=size))
                name = case.instance.operands[0].name
                full = records.GPR_NAMES[LOCATIONS[name][1]]
                initial = _register_value(case, full)
                mask = (1 << size) - 1
                written = initial & ~mask & ((1 << 64) - 1 if size == 16 else 0) | 0x1234 & mask
                recorded = _completed(case, {**_gpr(case, full, written), **_flags(CF=1)})
                other = written & ~mask | 0xBEEF & mask
                self.assertEqual(_judge(case, recorded, _completed(case, {**_gpr(case, full, other), **_flags(CF=1)})),
                                 [])
                if size < 64:
                    touched = _completed(case, {**_gpr(case, full, other ^ 1 << 40), **_flags(CF=1)})
                    self.assertEqual(_fields(_judge(case, recorded, touched)), {full})
                self.assertEqual(_fields(_judge(case, recorded, _completed(case, {**_gpr(case, full, other),
                                                                                    **_flags()}))), {"CF"})
                zero = _completed(case, {**_gpr(case, full, other), **_flags(CF=1, ZF=1)})
                self.assertEqual(_fields(_judge(case, recorded, zero)), {"ZF"})
                failed = _completed(case, {**_gpr(case, full, written), **_flags()})
                actual = _completed(case, {**_gpr(case, full, other), **_flags(CF=1)})
                self.assertEqual(_judge(case, failed, actual), [])

    def test_rdtsc_properties(self):
        case = _case("base", lambda instance: instance.form.definition.mnemonic == "rdtsc")
        recorded = _completed(case, {**_gpr(case, "rax", 0x89ABCDEF), **_gpr(case, "rdx", 0x77)})
        run = compare.Run(8)

        def counter(low, high):
            return _completed(case, {**_gpr(case, "rax", low), **_gpr(case, "rdx", high)})

        self.assertEqual(_judge(case, recorded, counter(0x1234, 5), run=run), [])
        self.assertEqual(_judge(case, recorded, counter(0x1235, 5), run=run), [])
        self.assertEqual(_judge(case, recorded, counter(0x1235, 5), run=run), [])
        self.assertEqual(_fields(_judge(case, recorded, counter(0x1000, 5), run=run)), {"counter"})
        self.assertEqual(_judge(case, recorded, counter(0x10, 6), run=run), [])
        self.assertEqual(_fields(_judge(case, recorded, counter(1 << 32 | 0x20, 6), run=run)), {"rax"})
        self.assertEqual(_fields(_judge(case, recorded, counter(0x30, 1 << 40 | 6), run=run)), {"rdx"})
        rdtscp = _case("rdtscp", lambda instance: instance.form.definition.mnemonic == "rdtscp")
        run = compare.Run(8)
        recorded = _completed(rdtscp, {**_gpr(rdtscp, "rax", 1), **_gpr(rdtscp, "rdx", 0), **_gpr(rdtscp, "rcx", 3)})
        below = _completed(rdtscp, {**_gpr(rdtscp, "rax", 2), **_gpr(rdtscp, "rdx", 0), **_gpr(rdtscp, "rcx", 7)})
        self.assertEqual(_judge(rdtscp, recorded, below, run=run), [])
        beyond = _completed(rdtscp, {**_gpr(rdtscp, "rax", 3), **_gpr(rdtscp, "rdx", 0), **_gpr(rdtscp, "rcx", 8)})
        self.assertEqual(_fields(_judge(rdtscp, recorded, beyond, run=run)), {"rcx"})
        rdpid = _case("rdpid", _primary("rdpid"))
        destination = records.GPR_NAMES[LOCATIONS[rdpid.instance.operands[0].name][1]]
        recorded = _completed(rdpid, _gpr(rdpid, destination, 2))
        self.assertEqual(_judge(rdpid, recorded, _completed(rdpid, _gpr(rdpid, destination, 7)), run=run), [])
        self.assertEqual(_fields(_judge(rdpid, recorded, _completed(rdpid, _gpr(rdpid, destination, 8)), run=run)),
                         {destination})

    def test_cpuid_console_values(self):
        case = _case("base", lambda instance: instance.form.definition.mnemonic == "cpuid")
        intel = (0x23, 0x756E6547, 0x6C65746E, 0x49656E69)
        console = (0xD, 0x68747541, 0x444D4163, 0x69746E65)
        names = ("rax", "rbx", "rcx", "rdx")

        def registers(values):
            changes = {}
            for name, value in zip(names, values):
                changes.update(_gpr(case, name, value))
            return changes

        recorded = _completed(case, registers(intel), console=())
        self.assertEqual(_judge(case, recorded, _completed(case, registers(console), console=console)), [])
        self.assertEqual(_fields(_judge(case, recorded, _completed(case, registers(intel), console=console))),
                         set(names))
        self.assertEqual(_fields(_judge(case, recorded, _completed(case, registers(console), console=()))),
                         {"console values"})


class RecordingTest(unittest.TestCase):
    def test_stale_recording_refused(self):
        header = Header(bytes(range(32)), bytes(range(32, 64)))
        results = [Result(0, Outcome.COMPLETED, 0, 0, 0, {8: 1}, {}, None)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "alpha.results.xz"
            path.write_bytes(lzma.compress(records.write_results(header, results)))
            self.assertEqual(suite.read_recording(path, header), {0: results[0]})
            with self.assertRaisesRegex(suite.SuiteError,
                                        "record again: the guest program differs from the recording's"):
                suite.read_recording(path, Header(bytes(32), header.inputs))
            with self.assertRaisesRegex(suite.SuiteError,
                                        "record again: the program's layout differs from the recording's"):
                suite.read_recording(path, Header(header.program, bytes(32)))


class MessageTest(unittest.TestCase):
    def test_mismatch_message(self):
        case = _case("base", _primary("bsf", operand_size=32))
        source = records.GPR_NAMES[LOCATIONS[case.instance.operands[1].name][1]]
        destination = records.GPR_NAMES[LOCATIONS[case.instance.operands[0].name][1]]
        expected = _completed(case, {**_gpr(case, destination, 0x1F), **_flags()})
        actual = _completed(case, {**_gpr(case, destination, 0x1E), **_flags(ZF=1)})
        mismatches = _judge(case, expected, actual)
        self.assertEqual(_fields(mismatches), {destination, "ZF"})
        text = compare.format_mismatch(case, mismatches)
        self.assertIn(case.instance.text, text)
        self.assertIn(f"{source}=0x{_register_value(case, source):016x}", text)
        self.assertIn(f"{destination}: expected 0x{0x1F:016x}, actual 0x{0x1E:016x}", text)
        self.assertIn("ZF: expected 0, actual 1", text)


if __name__ == "__main__":
    unittest.main()
