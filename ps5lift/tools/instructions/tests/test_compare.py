import dataclasses
import lzma
import struct
import tempfile
import unittest
from pathlib import Path

from instructions import compare, lifted, records, suite
from instructions.cases import CONSOLE_MXCSR, BlockOffset
from instructions.model import LOCATIONS, Shape
from instructions.records import Header, Outcome, Result
from instructions.tests.support import (
    GUARD,
    LOADER,
    case_of,
    completed,
    fault,
    fields,
    find_case,
    flags,
    group,
    judge,
    primary,
    vector_field,
    with_lanes,
)

EXACT, FAST = compare.EXACT, compare.FAST
_DAZ = 1 << 6
_FTZ = 1 << 15
_UNMASK_INVALID = 0x1F80 & ~(1 << 7)
ONE_80 = 0x3FFF8000000000000000
SIN_1 = 0x3FFED76AA47848677021
TAN_1 = 0x3FFFC75922E5F71D2DC5


def _gpr(case, name, value):
    return {records.gpr_offset(records.GPR_NAMES.index(name)): value.to_bytes(8, "little")}


def _register_value(case, name):
    offset = records.gpr_offset(records.GPR_NAMES.index(name))
    return int.from_bytes(case.input.slot[offset:offset + 8], "little")


def _lane(case, operand, value, lane=0, bits=32):
    number = LOCATIONS[case.instance.operands[operand].name][1]
    offset = records.ymm_offset(number) + lane * bits // 8
    return {offset: value.to_bytes(bits // 8, "little")}


def _mxcsr(case):
    return struct.unpack_from("<I", case.input.slot, records.FX_MXCSR)[0]


def _mxcsr_bytes(value):
    return {records.FX_MXCSR: struct.pack("<I", value)}


class FlagsTest(unittest.TestCase):
    def test_undefined_flags_ignored(self):
        case = find_case("base", primary("bsf", operand_size=32))
        self.assertLessEqual({"CF", "PF", "AF", "SF", "OF"}, compare.rules_for(case.instance).undefined_flags)
        expected = completed(case, flags(ZF=1))
        actual = completed(case, flags(ZF=1, CF=1, PF=1, AF=1, SF=1, OF=1))
        self.assertEqual(judge(case, expected, actual), [])
        self.assertEqual(fields(judge(case, expected, completed(case, flags()))), {"ZF"})

    def test_status_flags_ignored_for_call_ret_indirect_jump(self):
        for shape in (Shape.CALL, Shape.RETURN, Shape.INDIRECT_JUMP):
            with self.subTest(shape=shape.value):
                case = find_case("base", lambda instance: instance.shape is shape and instance.role == "primary")
                self.assertTrue(compare.rules_for(case.instance).status_flags_dropped)
                expected = completed(case, flags())
                actual = completed(case, flags(CF=1, PF=1, AF=1, ZF=1, SF=1, OF=1))
                self.assertEqual(judge(case, expected, actual), [])
        branch = find_case("base", lambda instance: instance.shape is Shape.BRANCH and instance.role == "primary")
        self.assertFalse(compare.rules_for(branch.instance).status_flags_dropped)
        self.assertEqual(fields(judge(branch, completed(branch, flags()), completed(branch, flags(CF=1)))),
                         {"CF"})

    def test_df_compared_for_call(self):
        case = find_case("base", lambda instance: instance.shape is Shape.CALL and instance.role == "primary")
        mismatches = judge(case, completed(case, flags()), completed(case, flags(DF=1)))
        self.assertEqual(fields(mismatches), {"DF"})


class FastModeTest(unittest.TestCase):
    def _addps(self, accept=lambda spec: spec.mxcsr == CONSOLE_MXCSR):
        return find_case("sse", primary("addps"), accept)

    def test_fast_nan_any(self):
        case = self._addps()
        expected = completed(case, _lane(case, 0, 0xFFC00000))
        actual = completed(case, _lane(case, 0, 0x7FC12345))
        self.assertEqual(judge(case, expected, actual, FAST), [])
        self.assertEqual(fields(judge(case, expected, actual, EXACT)), {vector_field(case, 0)})
        not_nan = completed(case, _lane(case, 0, 0x3F800000))
        self.assertEqual(fields(judge(case, expected, not_nan, FAST)), {vector_field(case, 0)})
        other_lane = completed(case, {**_lane(case, 0, 0x7FC12345), **_lane(case, 0, 0x3F800000, lane=1)})
        self.assertEqual(fields(judge(case, expected, other_lane, FAST)), {vector_field(case, 0)})

    def test_fast_min_max_daz(self):
        case = find_case("sse", primary("minps"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        self.assertTrue(_mxcsr(case) & _DAZ)
        case = with_lanes(with_lanes(case, 1, 0x00000001), 0, 0x3F800000)
        expected = completed(case, _lane(case, 0, 0x00000000))
        denormal = completed(case, _lane(case, 0, 0x00000001))
        self.assertEqual(judge(case, expected, denormal, FAST), [])
        self.assertEqual(fields(judge(case, expected, denormal, EXACT)), {vector_field(case, 0)})
        other_sign = completed(case, _lane(case, 0, 0x80000001))
        self.assertEqual(fields(judge(case, expected, other_sign, FAST)), {vector_field(case, 0)})
        no_operand = completed(case, _lane(case, 0, 0x00000003))
        self.assertEqual(fields(judge(case, expected, no_operand, FAST)), {vector_field(case, 0)})
        first_operand = with_lanes(case, 0, 0x00000003)
        expected = completed(first_operand, _lane(first_operand, 0, 0x00000000))
        self.assertEqual(judge(first_operand, expected, completed(first_operand, _lane(first_operand, 0, 3)), FAST),
                         [])
        without_daz = find_case("sse", primary("minps"), lambda spec: spec.mxcsr == 0x1F80)
        expected = completed(without_daz, _lane(without_daz, 0, 0x00000000))
        denormal = completed(without_daz, _lane(without_daz, 0, 0x00000001))
        self.assertEqual(fields(judge(without_daz, expected, denormal, FAST)), {vector_field(without_daz, 0)})
        addps = self._addps()
        expected = completed(addps, _lane(addps, 0, 0x00000000))
        denormal = completed(addps, _lane(addps, 0, 0x00000001))
        self.assertEqual(fields(judge(addps, expected, denormal, FAST)), {vector_field(addps, 0)})

    def test_fast_allowances_only_where_lanes_compute(self):
        mask = find_case("sse", primary("cmpps"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        recorded = completed(mask, _lane(mask, 0, 0xFFFFFFFF))
        self.assertEqual(fields(judge(mask, recorded, completed(mask, _lane(mask, 0, 0x7FC00000)), FAST)),
                         {vector_field(mask, 0)})
        logic = find_case("sse", primary("xorps"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        recorded = completed(logic, _lane(logic, 0, 0x7FC00001))
        self.assertEqual(fields(judge(logic, recorded, completed(logic, _lane(logic, 0, 0x7FC00002)), FAST)),
                         {vector_field(logic, 0)})
        move = find_case("sse", primary("movaps"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        recorded = completed(move, _lane(move, 0, 0x00800000))
        self.assertEqual(fields(judge(move, recorded, completed(move, _lane(move, 0, 0x00000000)), FAST)),
                         {vector_field(move, 0)})
        scalar = find_case("avx", primary("vaddss"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        recorded = completed(scalar, {**_lane(scalar, 0, 0x7FC00001), **_lane(scalar, 0, 0x7FC00001, lane=1)})
        computed = completed(scalar, {**_lane(scalar, 0, 0x7FC00002), **_lane(scalar, 0, 0x7FC00001, lane=1)})
        self.assertEqual(judge(scalar, recorded, computed, FAST), [])
        copied = completed(scalar, {**_lane(scalar, 0, 0x7FC00001), **_lane(scalar, 0, 0x7FC00002, lane=1)})
        self.assertEqual(fields(judge(scalar, recorded, copied, FAST)), {vector_field(scalar, 0)})

    def test_fast_exception_flags_ignored(self):
        case = self._addps()
        initial = _mxcsr(case)
        expected = completed(case, _mxcsr_bytes(initial | 0x20))
        actual = completed(case, _mxcsr_bytes(initial | 0x01))
        self.assertEqual(judge(case, expected, actual, FAST), [])
        self.assertEqual(fields(judge(case, expected, actual, EXACT)), {"mxcsr"})
        rounding = completed(case, _mxcsr_bytes(initial | 0x20 | 1 << 13))
        self.assertEqual(fields(judge(case, expected, rounding, FAST)), {"mxcsr"})

    def test_fast_flush_twin(self):
        _, specs, _ = group("sse")
        case = self._addps(lambda spec: spec.twin is not None and spec.mxcsr == CONSOLE_MXCSR & ~_FTZ)
        twin = case_of("sse", specs[case.spec.twin])
        self.assertEqual(compare.reference_case(case.spec, FAST), twin.spec.index)
        self.assertEqual(compare.reference_case(case.spec, EXACT), case.spec.index)
        self.assertEqual(compare.reference_case(twin.spec, FAST), twin.spec.index)
        recorded = completed(twin, _lane(twin, 0, 0x00000000))
        flushed = completed(case, _lane(case, 0, 0x00000000))
        self.assertEqual(judge(case, recorded, flushed, FAST, expected_slot=twin.input.slot), [])
        both_bits = completed(case, {**_lane(case, 0, 0x00000000), **_mxcsr_bytes(CONSOLE_MXCSR)})
        self.assertEqual(fields(judge(case, recorded, both_bits, FAST, expected_slot=twin.input.slot)), {"mxcsr"})
        unflushed = completed(case, _lane(case, 0, 0x00000005))
        self.assertEqual(fields(judge(case, recorded, unflushed, FAST, expected_slot=twin.input.slot)),
                         {vector_field(case, 0)})

    def test_fast_tininess(self):
        case = self._addps()
        self.assertTrue(_mxcsr(case) & (_FTZ | _DAZ))
        for normal, zero in ((0x00800000, 0x00000000), (0x80800000, 0x80000000)):
            with self.subTest(normal=hex(normal)):
                expected = completed(case, _lane(case, 0, normal))
                self.assertEqual(judge(case, expected, completed(case, _lane(case, 0, zero)), FAST), [])
                self.assertEqual(fields(judge(case, expected, completed(case, _lane(case, 0, zero)), EXACT)),
                                 {vector_field(case, 0)})
                other = completed(case, _lane(case, 0, zero ^ 0x80000000))
                self.assertEqual(fields(judge(case, expected, other, FAST)), {vector_field(case, 0)})
        plain = self._addps(lambda spec: spec.mxcsr == 0x1F80)
        expected = completed(plain, _lane(plain, 0, 0x00800000))
        self.assertEqual(fields(judge(plain, expected, completed(plain, _lane(plain, 0, 0)), FAST)),
                         {vector_field(plain, 0)})

    def test_fast_unmask_stop_at_loader(self):
        case = self._addps(lambda spec: spec.mxcsr == _UNMASK_INVALID)
        recorded = completed(case, _lane(case, 0, 0x3F800000))
        stop = fault(case, Outcome.NEEDS_EXACT_FLOAT, LOADER)
        self.assertEqual(judge(case, recorded, stop, FAST), [])
        at_instance = fault(case, Outcome.NEEDS_EXACT_FLOAT, case.layout.instances[case.instance.index].address)
        self.assertEqual(fields(judge(case, recorded, at_instance, FAST)), {"address"})
        self.assertEqual(fields(judge(case, recorded, recorded, FAST)), {"outcome"})
        self.assertEqual(judge(case, recorded, recorded, EXACT), [])

    def test_fast_unmask_stop_at_ldmxcsr(self):
        def unmasks(spec):
            return spec.loads_mxcsr is not None and spec.loads_mxcsr & 0x1F80 != 0x1F80 and spec.mxcsr == CONSOLE_MXCSR
        case = find_case("sse", lambda instance: instance.form.definition.mnemonic == "ldmxcsr"
                     and instance.role == "primary", unmasks)
        address = case.layout.instances[case.instance.index].address
        recorded = completed(case, _mxcsr_bytes(case.spec.loads_mxcsr))
        self.assertEqual(judge(case, recorded, fault(case, Outcome.NEEDS_EXACT_FLOAT, address), FAST), [])
        self.assertEqual(fields(judge(case, recorded, fault(case, Outcome.NEEDS_EXACT_FLOAT, LOADER), FAST)),
                         {"address"})
        self.assertEqual(judge(case, recorded, recorded, EXACT), [])
        protection = fault(case, Outcome.GENERAL_PROTECTION, address)
        self.assertEqual(judge(case, protection, protection, FAST), [])

    def test_exact_unmasked_is_simd_fault(self):
        case = self._addps(lambda spec: spec.mxcsr == _UNMASK_INVALID)
        address = case.layout.instances[case.instance.index].address
        recorded = fault(case, Outcome.SIMD_FLOATING_POINT, address)
        self.assertEqual(judge(case, recorded, recorded, EXACT), [])
        self.assertEqual(fields(judge(case, recorded, completed(case), EXACT)), {"outcome"})
        stop = fault(case, Outcome.NEEDS_EXACT_FLOAT, LOADER)
        self.assertEqual(fields(judge(case, recorded, stop, EXACT)), {"outcome"})


class StateTest(unittest.TestCase):
    def test_storer_fields_ignored(self):
        case = find_case("x87", primary("fadd"))
        ignored = {
            records.FX_FOP: b"\x12\x34", 8: bytes(range(1, 17)), records.FX_MXCSR_MASK: b"\xff\xff\x00\x00",
            records.FX_XMM: b"\x55" * 256, records.FX_RESERVED: b"\xaa" * 96,
        }
        self.assertEqual(judge(case, completed(case), completed(case, ignored)), [])
        control = struct.unpack_from("<H", case.input.slot, records.FX_FCW)[0] ^ 0x0400
        self.assertEqual(fields(judge(case, completed(case), completed(case, {0: struct.pack("<H", control)}))),
                         {"fcw"})

    def _x87_case(self, mnemonic, control, tags, *registers):
        case = find_case("x87", lambda instance: instance.form.definition.mnemonic == mnemonic)
        slot = bytearray(case.input.slot)
        struct.pack_into("<HHB", slot, records.FX_FCW, control, 0, tags)
        for number, value in enumerate(registers):
            slot[records.fx_st_offset(number):records.fx_st_offset(number) + 10] = value.to_bytes(10, "little")
        return dataclasses.replace(case, input=dataclasses.replace(case.input, slot=bytes(slot)))

    def test_fsin_compared_with_reference(self):
        case = self._x87_case("fsin", 0x037F, 0x01, ONE_80)
        st0 = records.fx_st_offset(0)
        recorded = completed(case, {st0: (SIN_1 + 5).to_bytes(10, "little"), 2: struct.pack("<H", 0x0200)})
        within = completed(case, {st0: (SIN_1 - 1).to_bytes(10, "little")})
        self.assertEqual(judge(case, recorded, within), [])
        outside = completed(case, {st0: (SIN_1 + 2).to_bytes(10, "little")})
        self.assertEqual(fields(judge(case, recorded, outside)), {"st0"})

    def test_x87_reference_ignores_precision_control(self):
        case = self._x87_case("fsin", 0x007F, 0x01, ONE_80)
        st0 = records.fx_st_offset(0)
        recorded = completed(case, {st0: SIN_1.to_bytes(10, "little")})
        wrong_low_bits = completed(case, {st0: (SIN_1 + (1 << 39)).to_bytes(10, "little")})
        self.assertEqual(fields(judge(case, recorded, wrong_low_bits)), {"st0"})
        self.assertEqual(judge(case, recorded, completed(case, {st0: (SIN_1 - 1).to_bytes(10, "little")})), [])

    def test_fptan_pushed_one_exact(self):
        case = self._x87_case("fptan", 0x037F, 0x01, ONE_80)
        st0, st1 = records.fx_st_offset(0), records.fx_st_offset(1)
        recorded = completed(case, {st0: ONE_80.to_bytes(10, "little"), st1: TAN_1.to_bytes(10, "little")})
        tangent_off_by_one = completed(case, {st0: ONE_80.to_bytes(10, "little"),
                                               st1: (TAN_1 + 1).to_bytes(10, "little")})
        self.assertEqual(judge(case, recorded, tangent_off_by_one), [])
        one_off_by_one = completed(case, {st0: (ONE_80 + 1).to_bytes(10, "little"), st1: TAN_1.to_bytes(10, "little")})
        self.assertEqual(fields(judge(case, recorded, one_off_by_one)), {"st0"})

    def test_rcp_lanes_within_tolerance(self):
        case = find_case("sse", primary("rcpps"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        case = with_lanes(case, 1, 0x3F800000)
        recorded = completed(case, _lane(case, 0, 0x3F7FF000))
        self.assertEqual(judge(case, recorded, completed(case, _lane(case, 0, 0x3F800C00))), [])
        self.assertEqual(fields(judge(case, recorded, completed(case, _lane(case, 0, 0x3F800C01)))),
                         {vector_field(case, 0)})

    def test_scalar_rcp_copies_compared_exactly(self):
        case = find_case("avx", primary("vrcpss"), lambda spec: spec.mxcsr == CONSOLE_MXCSR)
        case = with_lanes(case, 2, 0x3F800000)
        case = with_lanes(case, 1, 0x40000000, 0x3F800000, 0x3F800000, 0x3F800000)
        copies = {**_lane(case, 0, 0x3F800000), **_lane(case, 0, 0x3F800000, lane=1),
                  **_lane(case, 0, 0x3F800000, lane=2), **_lane(case, 0, 0x3F800000, lane=3)}
        recorded = completed(case, copies)
        self.assertEqual(judge(case, recorded, completed(case, {**copies, **_lane(case, 0, 0x3F800C00)})), [])
        drifted = completed(case, {**copies, **_lane(case, 0, 0x3F800800, lane=1)})
        self.assertEqual(fields(judge(case, recorded, drifted)), {vector_field(case, 0)})

    def test_page_fault_data_exact(self):
        case = find_case("base", primary("mov", registers_only=False, operand_size=64))
        address = case.layout.instances[case.instance.index].address
        recorded = fault(case, Outcome.PAGE_FAULT, address, GUARD)
        self.assertEqual(judge(case, recorded, fault(case, Outcome.PAGE_FAULT, address, GUARD)), [])
        self.assertEqual(fields(judge(case, recorded, fault(case, Outcome.PAGE_FAULT, address, GUARD + 1))),
                         {"data"})

    def test_fxsave_page_fault_data_inside_guard_and_access(self):
        def crossing(spec):
            return BlockOffset(records.BLOCK_SIZE) in spec.gprs
        case = find_case("sse", primary("fxsave", registers_only=False), crossing)
        address = case.layout.instances[case.instance.index].address
        recorded = fault(case, Outcome.PAGE_FAULT, address, GUARD + 0x1FF)
        for data, accepted in ((GUARD, True), (GUARD + 0x1FF, True), (GUARD + 511, True), (GUARD + 512, False),
                               (GUARD - 1, False)):
            with self.subTest(data=hex(data)):
                mismatches = judge(case, recorded, fault(case, Outcome.PAGE_FAULT, address, data))
                self.assertEqual(fields(mismatches), set() if accepted else {"data"})


class PropertyTest(unittest.TestCase):
    def test_rdrand_properties(self):
        for size in (16, 32, 64):
            with self.subTest(size=size):
                case = find_case("rdrand", primary("rdrand", operand_size=size))
                name = case.instance.operands[0].name
                full = records.GPR_NAMES[LOCATIONS[name][1]]
                initial = _register_value(case, full)
                mask = (1 << size) - 1
                written = initial & ~mask & ((1 << 64) - 1 if size == 16 else 0) | 0x1234 & mask
                recorded = completed(case, {**_gpr(case, full, written), **flags(CF=1)})
                other = written & ~mask | 0xBEEF & mask
                self.assertEqual(judge(case, recorded, completed(case, {**_gpr(case, full, other), **flags(CF=1)})),
                                 [])
                if size < 64:
                    touched = completed(case, {**_gpr(case, full, other ^ 1 << 40), **flags(CF=1)})
                    self.assertEqual(fields(judge(case, recorded, touched)), {full})
                self.assertEqual(fields(judge(case, recorded, completed(case, {**_gpr(case, full, other),
                                                                                    **flags()}))), {"CF"})
                zero = completed(case, {**_gpr(case, full, other), **flags(CF=1, ZF=1)})
                self.assertEqual(fields(judge(case, recorded, zero)), {"ZF"})
                failed = completed(case, {**_gpr(case, full, written), **flags()})
                actual = completed(case, {**_gpr(case, full, other), **flags(CF=1)})
                self.assertEqual(judge(case, failed, actual), [])

    def test_rdrand_flags_as_recorded(self):
        case = find_case("rdrand", primary("rdrand", operand_size=64))
        full = records.GPR_NAMES[LOCATIONS[case.instance.operands[0].name][1]]
        recorded = completed(case, {**_gpr(case, full, 0x1234), **flags(CF=1, ZF=1)})
        self.assertEqual(judge(case, recorded, completed(case, {**_gpr(case, full, 0x5678), **flags(CF=1, ZF=1)})),
                         [])
        self.assertEqual(fields(judge(case, recorded, completed(case, {**_gpr(case, full, 0x5678),
                                                                            **flags(CF=1)}))), {"ZF"})

    def test_rdrand_writes_checked(self):
        case = find_case("rdrand", primary("rdrand", operand_size=16))
        full = records.GPR_NAMES[LOCATIONS[case.instance.operands[0].name][1]]
        initial = _register_value(case, full)
        recorded = completed(case, {**_gpr(case, full, initial ^ 0x1234), **flags(CF=1)})
        unwritten = completed(case, {**_gpr(case, full, initial), **flags(CF=1)})
        run = compare.Run(4)
        self.assertEqual(judge(case, recorded, unwritten, run=run), [])
        self.assertEqual(compare.form_mismatches(run), {})
        self.assertEqual(judge(case, recorded, completed(case, {**_gpr(case, full, initial ^ 1), **flags(CF=1)}),
                                run=run), [])
        self.assertEqual(compare.form_mismatches(run), {})
        self.assertEqual(judge(case, recorded, unwritten, run=run), [])
        mismatches = compare.form_mismatches(run)
        self.assertEqual(list(mismatches), [case.instance.form.key])
        (mismatch,) = mismatches[case.instance.form.key]
        self.assertEqual(mismatch.field, "destination written")
        self.assertIn("2 cases", mismatch.actual)
        line = lifted.form_line("rdrand", EXACT, case.instance.form.key, [], mismatches[case.instance.form.key])
        self.assertTrue(line.startswith(f"FAIL rdrand exact {case.instance.form.key}: 0 cases"))
        self.assertIn("destination written", line)

    def test_rdtsc_properties(self):
        case = find_case("base", lambda instance: instance.form.definition.mnemonic == "rdtsc")
        recorded = completed(case, {**_gpr(case, "rax", 0x89ABCDEF), **_gpr(case, "rdx", 0x77)})
        run = compare.Run(8)

        def counter(low, high):
            return completed(case, {**_gpr(case, "rax", low), **_gpr(case, "rdx", high)})

        self.assertEqual(judge(case, recorded, counter(0x1234, 5), run=run), [])
        self.assertEqual(judge(case, recorded, counter(0x1235, 5), run=run), [])
        self.assertEqual(judge(case, recorded, counter(0x1235, 5), run=run), [])
        self.assertEqual(fields(judge(case, recorded, counter(0x1000, 5), run=run)), {"counter"})
        self.assertEqual(judge(case, recorded, counter(0x10, 6), run=run), [])
        self.assertEqual(fields(judge(case, recorded, counter(1 << 32 | 0x20, 6), run=run)), {"rax"})
        self.assertEqual(fields(judge(case, recorded, counter(0x30, 1 << 40 | 6), run=run)), {"rdx"})
        rdtscp = find_case("rdtscp", lambda instance: instance.form.definition.mnemonic == "rdtscp")
        run = compare.Run(8)
        recorded = completed(rdtscp, {**_gpr(rdtscp, "rax", 1), **_gpr(rdtscp, "rdx", 0), **_gpr(rdtscp, "rcx", 3)})
        below = completed(rdtscp, {**_gpr(rdtscp, "rax", 2), **_gpr(rdtscp, "rdx", 0), **_gpr(rdtscp, "rcx", 7)})
        self.assertEqual(judge(rdtscp, recorded, below, run=run), [])
        beyond = completed(rdtscp, {**_gpr(rdtscp, "rax", 3), **_gpr(rdtscp, "rdx", 0), **_gpr(rdtscp, "rcx", 8)})
        self.assertEqual(fields(judge(rdtscp, recorded, beyond, run=run)), {"rcx"})
        rdpid = find_case("rdpid", primary("rdpid"))
        destination = records.GPR_NAMES[LOCATIONS[rdpid.instance.operands[0].name][1]]
        recorded = completed(rdpid, _gpr(rdpid, destination, 2))
        self.assertEqual(judge(rdpid, recorded, completed(rdpid, _gpr(rdpid, destination, 7)), run=run), [])
        self.assertEqual(fields(judge(rdpid, recorded, completed(rdpid, _gpr(rdpid, destination, 8)), run=run)),
                         {destination})

    def test_cpuid_console_values(self):
        case = find_case("base", lambda instance: instance.form.definition.mnemonic == "cpuid")
        intel = (0x23, 0x756E6547, 0x6C65746E, 0x49656E69)
        console = (0xD, 0x68747541, 0x444D4163, 0x69746E65)
        names = ("rax", "rbx", "rcx", "rdx")

        def registers(values):
            changes = {}
            for name, value in zip(names, values):
                changes.update(_gpr(case, name, value))
            return changes

        recorded = completed(case, registers(intel), console=())
        self.assertEqual(judge(case, recorded, completed(case, registers(console), console=console)), [])
        self.assertEqual(fields(judge(case, recorded, completed(case, registers(intel), console=console))),
                         set(names))
        self.assertEqual(fields(judge(case, recorded, completed(case, registers(console), console=()))),
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
        case = find_case("base", primary("bsf", operand_size=32))
        source = records.GPR_NAMES[LOCATIONS[case.instance.operands[1].name][1]]
        destination = records.GPR_NAMES[LOCATIONS[case.instance.operands[0].name][1]]
        expected = completed(case, {**_gpr(case, destination, 0x1F), **flags()})
        actual = completed(case, {**_gpr(case, destination, 0x1E), **flags(ZF=1)})
        mismatches = judge(case, expected, actual)
        self.assertEqual(fields(mismatches), {destination, "ZF"})
        text = compare.format_mismatch(case, mismatches)
        self.assertIn(case.instance.text, text)
        self.assertIn(f"{source}=0x{_register_value(case, source):016x}", text)
        self.assertIn(f"{destination}: expected 0x{0x1F:016x}, actual 0x{0x1E:016x}", text)
        self.assertIn("ZF: expected 0, actual 1", text)


if __name__ == "__main__":
    unittest.main()
