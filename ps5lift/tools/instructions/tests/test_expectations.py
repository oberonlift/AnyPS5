import collections
import dataclasses
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

from instructions import expectations, records
from instructions.cases import BlockOffset, form_model
from instructions.encoding import Imm, Mem, Reg
from instructions.expectations import ExpectationError, NoExpectation, expected_result
from instructions.machine import CONSOLE_MXCSR
from instructions.model import LOCATIONS
from instructions.records import Outcome
from instructions.state import State, changed_words, initial_block, operand_address
from instructions.tests.support import (
    GUARD,
    completed,
    fault,
    fields,
    find_case,
    flags,
    judge,
    linked_group,
    primary,
    vector_field,
    with_slot,
)

MM_LOAD = CONSOLE_MXCSR | 1 << 17
AMD_MXCSR_MASK = 0x0002FFFF
INTEL_MXCSR_MASK = 0x0000FFFF
FIELD_SOURCE = 0x0123456789ABCDEF
FIELD_DESTINATION = 0xFEDCBA9876543210
HIGH_QUADWORD = 0x0F1E2D3C4B5A6978
STORED = 0x1122334455667788
ONE = 0x3F800000
TWO = 0x40000000
TWO_TO_127 = 0x7F000000
MINUS_TWO_TO_127 = 0xFF000000
QUIET_NAN_1 = 0x7FC00001
QUIET_NAN_2 = 0x7FC00002
DEFAULT_NAN = 0xFFC00000
TOWARD_ZERO_MXCSR = 0x1F80 | 3 << 13
FLAGS_WORD = records.LOAD_AH & ~7
ES_PENDING = 0x8081
SAVE_GROUPS = {"fxsave": "sse", "fxsave64": "sse", "fnstenv": "x87", "fnsave": "x87", "xsave": "xsave"}
LOADS = frozenset({"ldmxcsr", "vldmxcsr", "fxrstor", "fxrstor64", "xrstor", "xrstor64"})
CACHE_LINE = 64


def _immediates(instance):
    return [choice.value for choice in instance.operands if isinstance(choice, Imm)]


def _vector(case, choice):
    return LOCATIONS[case.instance.operands[choice].name][1]


def _quadwords(case, choice, low, high):
    return {records.ymm_offset(_vector(case, choice)): struct.pack("<QQ", low, high)}


def _gpr_offset(case, choice):
    return records.gpr_offset(LOCATIONS[case.instance.operands[choice].name][1])


def _gpr(case, choice, value):
    return {_gpr_offset(case, choice): value.to_bytes(8, "little")}


def _with_block(case, offset, data):
    patches = case.input.patches + ((offset, data),)
    return dataclasses.replace(case, input=dataclasses.replace(case.input, patches=patches))


def _final_slot(case, result):
    return State.of(result, case.input.slot).slot


def _final_block(case, result):
    block = bytearray(initial_block(case.input))
    for offset, value in result.memory_words.items():
        struct.pack_into("<Q", block, offset, value)
    return bytes(block)


def _captured_flags(slot):
    return slot[records.STORE_AX:records.STORE_DF + 1]


def _flag_bytes(**values):
    return b"".join(flags(**values).values())


def _sse4a(mnemonic, immediates):
    return find_case("sse4a", lambda instance: instance.form.definition.mnemonic == mnemonic
                     and instance.role == "primary" and _immediates(instance) == immediates)


def _flipped(case, result, byte):
    slot = bytearray(_final_slot(case, result))
    slot[records.ymm_offset(_vector(case, 0)) + byte] ^= 0xFF
    return dataclasses.replace(result, slot_words=changed_words(case.input.slot, bytes(slot)))


def _at(case, address, length, target):
    entry = case.layout.instances[case.instance.index]
    moved = dataclasses.replace(entry, address=address, length=length,
                                helpers={name: target for name in entry.helpers})
    instances = list(case.layout.instances)
    instances[case.instance.index] = moved
    return dataclasses.replace(case, layout=dataclasses.replace(case.layout, instances=tuple(instances)))


def _branch(mnemonic, accept=lambda spec: True):
    return find_case("base", primary(mnemonic, operand_size=16), accept)


def _sign_differs(spec):
    return spec.flags["ZF"] == 0 and spec.flags["SF"] != spec.flags["OF"]


def _save_width(width):
    def choose(instance):
        model = form_model(instance.form)
        return (instance.form.definition.mnemonic in ("fnstenv", "fnsave") and instance.role == "primary"
                and model.memory_width(model.explicit[0]) == width)
    return choose


class Sse4aTest(unittest.TestCase):
    def _field(self, mnemonic, immediates, destination, source=None):
        case = _sse4a(mnemonic, immediates)
        changes = _quadwords(case, 0, destination, HIGH_QUADWORD)
        if source is not None:
            changes.update(_quadwords(case, 1, *source))
        case = with_slot(case, changes)
        result = expected_result(case, None)
        self.assertIs(result.outcome, Outcome.COMPLETED)
        slot = _final_slot(case, result)
        start = records.ymm_offset(_vector(case, 0))
        self.assertEqual(slot[start + 16:start + 32], case.input.slot[start + 16:start + 32])
        self.assertEqual(_captured_flags(slot), _flag_bytes(**case.spec.flags))
        self.assertEqual(set(changed_words(case.input.slot, slot)) - {start, start + 8}, {FLAGS_WORD})
        return struct.unpack_from("<Q", slot, start)[0]

    def test_extrq_model(self):
        self.assertEqual(self._field("extrq", [0x1B, 0x4E], FIELD_SOURCE), 0x59E26AF)
        self.assertEqual(self._field("extrq", [], FIELD_SOURCE, (0x2010, 0)), 0x4567)
        self.assertEqual(self._field("extrq", [], FIELD_SOURCE, (0xFFFFC0C0, 0)), FIELD_SOURCE)

    def test_insertq_model(self):
        self.assertEqual(self._field("insertq", [0x1B, 0x4E], FIELD_DESTINATION, (FIELD_SOURCE, 0)),
                         0xFEDCBA6AF37BF210)
        self.assertEqual(self._field("insertq", [], FIELD_DESTINATION, (FIELD_SOURCE, 0x2010)), 0xFEDCCDEF76543210)
        self.assertEqual(self._field("insertq", [], FIELD_DESTINATION, (FIELD_SOURCE, 0xC0C0)), FIELD_SOURCE)

    def test_sse4a_undefined_bits_not_compared(self):
        defined = _sse4a("extrq", [0x1B, 0x4E])
        expected = expected_result(defined, None)
        self.assertEqual(judge(defined, expected, _flipped(defined, expected, 8)), [])
        self.assertEqual(fields(judge(defined, expected, _flipped(defined, expected, 0))), {vector_field(defined, 0)})
        for immediates in ([0xAA, 0xE4], [0x80, 0xAA], [0x00, 0x01]):
            with self.subTest(immediates=immediates):
                undefined = _sse4a("extrq", immediates)
                expected = expected_result(undefined, None)
                self.assertEqual(judge(undefined, expected, _flipped(undefined, expected, 0)), [])
                self.assertEqual(judge(undefined, expected, _flipped(undefined, expected, 15)), [])
                self.assertEqual(fields(judge(undefined, expected, _flipped(undefined, expected, 16))),
                                 {vector_field(undefined, 0)})

    def test_movnt_stores_the_low_element(self):
        for mnemonic, size in (("movntsd", 8), ("movntss", 4)):
            with self.subTest(mnemonic=mnemonic):
                case = find_case("sse4a", primary(mnemonic, registers_only=False))
                case = with_slot(case, _quadwords(case, 1, STORED, HIGH_QUADWORD))
                result = expected_result(case, None)
                start = operand_address(case, 0) - case.layout.block
                stored = bytearray(initial_block(case.input))
                stored[start:start + size] = STORED.to_bytes(8, "little")[:size]
                self.assertEqual(_final_block(case, result), bytes(stored))
                crossing = find_case("sse4a", primary(mnemonic, registers_only=False),
                                     lambda spec: BlockOffset(records.BLOCK_SIZE - size + 1) in spec.gprs)
                self.assertEqual(expected_result(crossing, None),
                                 fault(crossing, Outcome.PAGE_FAULT, crossing.address, GUARD))

    def test_written_case_with_a_recording_refused(self):
        case = _sse4a("extrq", [0x1B, 0x4E])
        with self.assertRaisesRegex(ExpectationError, "has a recording, but its form's expectations are written"):
            expected_result(case, completed(case))


class MonitorxClzeroTest(unittest.TestCase):
    def test_monitorx_and_mwaitx_change_nothing(self):
        for mnemonic in ("monitorx", "mwaitx"):
            with self.subTest(mnemonic=mnemonic):
                case = find_case("monitorx", primary(mnemonic))
                result = expected_result(case, None)
                self.assertIs(result.outcome, Outcome.COMPLETED)
                slot = _final_slot(case, result)
                self.assertEqual(set(changed_words(case.input.slot, slot)), {FLAGS_WORD})
                self.assertEqual(_captured_flags(slot), _flag_bytes(**case.spec.flags))
                self.assertEqual(result.memory_words, {})
                self.assertEqual(judge(case, result, completed(case, flags(**case.spec.flags))), [])

    def test_clzero_zeroes_the_line_that_holds_rax(self):
        case = find_case("clzero", primary("clzero"), lambda spec: isinstance(spec.gprs[0], BlockOffset)
                         and spec.gprs[0].offset < records.BLOCK_SIZE and spec.gprs[0].offset % CACHE_LINE)
        line = case.spec.gprs[0].offset - case.spec.gprs[0].offset % CACHE_LINE
        result = expected_result(case, None)
        self.assertIs(result.outcome, Outcome.COMPLETED)
        zeroed = bytearray(initial_block(case.input))
        zeroed[line:line + CACHE_LINE] = bytes(CACHE_LINE)
        self.assertEqual(_final_block(case, result), bytes(zeroed))
        self.assertEqual(set(changed_words(case.input.slot, _final_slot(case, result))), {FLAGS_WORD})

    def test_clzero_in_the_guard_faults(self):
        case = find_case("clzero", primary("clzero"), lambda spec: spec.gprs[0] == BlockOffset(records.BLOCK_SIZE))
        self.assertEqual(expected_result(case, None), fault(case, Outcome.PAGE_FAULT, case.address, GUARD))


class BitScanTest(unittest.TestCase):
    def test_bsf_zero_source_keeps_destination(self):
        case = find_case("base", primary("bsf", operand_size=32))
        case = with_slot(case, {**_gpr(case, 0, FIELD_SOURCE), **_gpr(case, 1, 0xFFFFFFFF00000000)})
        intel = completed(case, {**_gpr(case, 0, 0x89ABCDEF), **flags(CF=1)})
        slot = _final_slot(case, expected_result(case, intel))
        self.assertEqual(slot[_gpr_offset(case, 0):_gpr_offset(case, 0) + 8], FIELD_SOURCE.to_bytes(8, "little"))
        self.assertEqual(_captured_flags(slot), _flag_bytes(CF=1, ZF=1))
        self.assertEqual(set(changed_words(_final_slot(case, intel), slot)), {_gpr_offset(case, 0), FLAGS_WORD})
        nonzero = with_slot(case, _gpr(case, 1, 0x100))
        recorded = completed(nonzero, {**_gpr(nonzero, 0, 8), **flags()})
        self.assertIs(expected_result(nonzero, recorded), recorded)

    def test_bsr_zero_memory_source(self):
        case = find_case("base", lambda instance: primary("bsr", registers_only=False, operand_size=64)(instance)
                         and isinstance(instance.operands[1], Mem))
        start = operand_address(case, 1) - case.layout.block
        case = _with_block(with_slot(case, _gpr(case, 0, FIELD_SOURCE)), start, bytes(8))
        slot = _final_slot(case, expected_result(case, completed(case, {**_gpr(case, 0, 0), **flags(SF=1)})))
        self.assertEqual(slot[_gpr_offset(case, 0):_gpr_offset(case, 0) + 8], FIELD_SOURCE.to_bytes(8, "little"))
        self.assertEqual(_captured_flags(slot), _flag_bytes(SF=1, ZF=1))
        faulted = fault(case, Outcome.PAGE_FAULT, case.address, GUARD)
        self.assertIs(expected_result(case, faulted), faulted)


class StateSaveTest(unittest.TestCase):
    def _save(self, choose, group, image, pending=False):
        case = find_case(group, choose)
        if pending:
            status = struct.unpack_from("<H", case.input.slot, records.FX_FSW)[0] | ES_PENDING
            case = with_slot(case, {records.FX_FSW: struct.pack("<H", status)})
        start = operand_address(case, 0) - case.layout.block
        block = bytearray(initial_block(case.input))
        for offset, data in image.items():
            block[start + offset:start + offset + len(data)] = data
        recorded = completed(case, flags(**case.spec.flags), changed_words(initial_block(case.input), bytes(block)))
        expected = expected_result(case, recorded)
        return _final_block(case, expected)[start:start + 32], recorded, expected

    def _legacy(self, mnemonic, image, pending=False):
        return self._save(primary(mnemonic, registers_only=False), SAVE_GROUPS[mnemonic], image, pending)

    def test_fxsave_pointer_fields_zero_without_pending_exception(self):
        pointers = bytes(range(1, 19))
        image, _, _ = self._legacy("fxsave64", {6: pointers})
        self.assertEqual(image[6:24], bytes(18))
        image, _, _ = self._legacy("fxsave64", {6: pointers}, pending=True)
        self.assertEqual(image[6:24], pointers)
        image, _, _ = self._legacy("fxsave", {6: pointers})
        self.assertEqual(image[6:24], bytes(8) + pointers[8:10] + bytes(6) + pointers[16:18])
        image, _, _ = self._legacy("xsave", {6: pointers})
        self.assertEqual(image[6:24], bytes(8) + pointers[8:10] + bytes(6) + pointers[16:18])
        environment, _, _ = self._save(_save_width(28), "x87", {12: pointers[:14], 26: b"\xff\xff"})
        self.assertEqual(environment[12:28], bytes(14) + b"\xff\xff")
        environment, _, _ = self._save(_save_width(14), "x87", {6: pointers[:8]})
        self.assertEqual(environment[6:14], bytes(8))
        environment, _, _ = self._save(_save_width(108), "x87", {12: pointers[:14]}, pending=True)
        self.assertEqual(environment[12:26], pointers[:14])
        _, recorded, expected = self._legacy("xsave", {})
        self.assertEqual(expected, recorded)

    def test_mxcsr_mask_amd(self):
        for mnemonic in ("fxsave", "fxsave64", "xsave"):
            with self.subTest(mnemonic=mnemonic):
                image, _, _ = self._legacy(mnemonic, {28: struct.pack("<I", INTEL_MXCSR_MASK)})
                self.assertEqual(struct.unpack_from("<I", image, 28)[0], AMD_MXCSR_MASK)
                image, _, _ = self._legacy(mnemonic, {28: struct.pack("<I", INTEL_MXCSR_MASK)}, pending=True)
                self.assertEqual(struct.unpack_from("<I", image, 28)[0], AMD_MXCSR_MASK)
        case = find_case("sse", primary("fxsave", registers_only=False))
        faulted = fault(case, Outcome.PAGE_FAULT, case.address, GUARD)
        self.assertIs(expected_result(case, faulted), faulted)


class MxcsrLoadTest(unittest.TestCase):
    def _load(self, group, mnemonic):
        return find_case(group, primary(mnemonic, registers_only=False), lambda spec: spec.loads_mxcsr == MM_LOAD)

    def _image(self, case, size):
        start = operand_address(case, 0) - case.layout.block
        return initial_block(case.input)[start:start + size]

    def test_ldmxcsr_mm_bit_accepted(self):
        for group, mnemonic in (("sse", "ldmxcsr"), ("avx", "vldmxcsr")):
            with self.subTest(mnemonic=mnemonic):
                case = self._load(group, mnemonic)
                self.assertEqual(struct.unpack("<I", self._image(case, 4))[0], MM_LOAD)
                refused = fault(case, Outcome.GENERAL_PROTECTION, case.address)
                loaded = completed(case, {records.FX_MXCSR: struct.pack("<I", MM_LOAD), **flags(**case.spec.flags)})
                self.assertEqual(expected_result(case, refused), loaded)
                with self.assertRaisesRegex(ExpectationError, "Intel refuses"):
                    expected_result(case, completed(case))
                reserved = dataclasses.replace(case, spec=dataclasses.replace(case.spec, loads_mxcsr=MM_LOAD | 1 << 31))
                self.assertIs(expected_result(reserved, refused), refused)
                faulted = fault(case, Outcome.PAGE_FAULT, case.address, GUARD)
                self.assertIs(expected_result(case, faulted), faulted)

    def test_fxrstor_mm_bit_accepted(self):
        for mnemonic in ("fxrstor", "fxrstor64"):
            with self.subTest(mnemonic=mnemonic):
                case = self._load("sse", mnemonic)
                image = self._image(case, 512)
                slot = _final_slot(case, expected_result(case, fault(case, Outcome.GENERAL_PROTECTION, case.address)))
                self.assertEqual(slot[records.FX_FCW:records.FX_FTW + 1], image[records.FX_FCW:records.FX_FTW + 1])
                self.assertEqual(struct.unpack_from("<I", slot, records.FX_MXCSR)[0], MM_LOAD)
                for number in range(8):
                    offset = records.fx_st_offset(number)
                    self.assertEqual(slot[offset:offset + 10], image[offset:offset + 10])
                for number in range(records.YMM_COUNT):
                    offset = records.ymm_offset(number)
                    self.assertEqual(slot[offset:offset + 16], image[records.fx_xmm_offset(number):][:16])
                    self.assertEqual(slot[offset + 16:offset + 32], case.input.slot[offset + 16:offset + 32])
                self.assertEqual(_captured_flags(slot), _flag_bytes(**case.spec.flags))
                base = records.gpr_offset(LOCATIONS[case.instance.operands[0].base][1])
                address = int.from_bytes(case.input.slot[base:base + 8], "little")
                misaligned = with_slot(case, {base: (address + 8).to_bytes(8, "little")})
                refused = fault(misaligned, Outcome.GENERAL_PROTECTION, misaligned.address)
                self.assertIs(expected_result(misaligned, refused), refused)

    def test_xrstor_mm_bit_accepted(self):
        for mnemonic in ("xrstor", "xrstor64"):
            with self.subTest(mnemonic=mnemonic):
                case = self._load("xsave", mnemonic)
                self.assertEqual(case.spec.gprs[0] & 0xFFFFFFFF, 7)
                image = self._image(case, 832)
                self.assertEqual(struct.unpack_from("<QQ", image, 512), (7, 0))
                slot = _final_slot(case, expected_result(case, fault(case, Outcome.GENERAL_PROTECTION, case.address)))
                self.assertEqual(slot[records.FX_FCW:records.FX_FTW + 1], image[records.FX_FCW:records.FX_FTW + 1])
                self.assertEqual(struct.unpack_from("<I", slot, records.FX_MXCSR)[0], MM_LOAD)
                for number in range(records.YMM_COUNT):
                    offset = records.ymm_offset(number)
                    self.assertEqual(slot[offset:offset + 32], image[records.fx_xmm_offset(number):][:16]
                                     + image[576 + 16 * number:592 + 16 * number])


class NearBranch16Test(unittest.TestCase):
    def test_jmp_rel16_faults_at_truncated_target(self):
        for mnemonic in ("jmp", "call"):
            with self.subTest(mnemonic=mnemonic):
                case = _at(_branch(mnemonic), 0xFC0001FFF0, 4, 0xFC00020010)
                self.assertEqual(expected_result(case, None), fault(case, Outcome.PAGE_FAULT, 0x0010, 0x0010))
        bound = _at(_branch("jmp"), 0xFC00008000, 4, 0xFC00000004)
        self.assertEqual(expected_result(bound, None), fault(bound, Outcome.PAGE_FAULT, 0x0004, 0x0004))

    def test_call_rel16_push_into_guard_faults_at_instance(self):
        call = _at(_branch("call"), 0xFC0001FFF0, 4, 0xFC00020010)
        rsp = records.gpr_offset(LOCATIONS["rsp"][1])
        for top, data in ((GUARD + 1, GUARD), (GUARD + 2, GUARD), (GUARD + 3, GUARD + 1)):
            with self.subTest(rsp=hex(top)):
                case = with_slot(call, {rsp: top.to_bytes(8, "little")})
                self.assertEqual(expected_result(case, None), fault(case, Outcome.PAGE_FAULT, case.address, data))
        below = with_slot(call, {rsp: GUARD.to_bytes(8, "little")})
        self.assertEqual(expected_result(below, None), fault(below, Outcome.PAGE_FAULT, 0x0010, 0x0010))

    def test_jcc_rel16_not_taken_completes(self):
        case = _at(_branch("jz", lambda spec: spec.flags["ZF"] == 0), 0xFC0001FFF0, 6, 0xFC00020010)
        not_taken = completed(case, {**flags(**case.spec.flags), records.PATH: bytes([records.PATH_NOT_TAKEN])})
        self.assertEqual(expected_result(case, None), not_taken)
        taken = _at(_branch("jz", lambda spec: spec.flags["ZF"] == 1), 0xFC0001FFF0, 6, 0xFC00020010)
        self.assertEqual(expected_result(taken, None), fault(taken, Outcome.PAGE_FAULT, 0x0010, 0x0010))
        for mnemonic, outcome in (("jl", Outcome.PAGE_FAULT), ("jle", Outcome.PAGE_FAULT),
                                  ("jnl", Outcome.COMPLETED), ("jnle", Outcome.COMPLETED)):
            with self.subTest(mnemonic=mnemonic):
                self.assertIs(expected_result(_branch(mnemonic, _sign_differs), None).outcome, outcome)

    def test_branch_target_beyond_rel16_refused(self):
        case = _at(_branch("jmp"), 0xFC00000000, 4, 0xFC00010004)
        with self.assertRaisesRegex(ExpectationError, "beyond a rel16"):
            expected_result(case, None)


class DotProductTest(unittest.TestCase):
    def _dpps(self, control, first, second, accept=lambda spec: spec.mxcsr == CONSOLE_MXCSR):
        def choose(instance):
            return (instance.form.definition.mnemonic == "dpps" and instance.role == "primary"
                    and _immediates(instance) == [control] and isinstance(instance.operands[1], Reg))
        case = find_case("sse4", choose, accept)
        return with_slot(case, {records.ymm_offset(_vector(case, 0)): struct.pack("<4I", *first),
                                records.ymm_offset(_vector(case, 1)): struct.pack("<4I", *second)})

    def _lanes(self, case, lane):
        return completed(case, {records.ymm_offset(_vector(case, 0)): struct.pack("<4I", *[lane] * 4)})

    def _differs(self, case, expected_lane, actual_lane):
        return fields(judge(case, self._lanes(case, expected_lane), self._lanes(case, actual_lane)))

    def test_dpps_canonical_nan(self):
        case = self._dpps(0xFF, (QUIET_NAN_1, QUIET_NAN_2, ONE, ONE), (ONE,) * 4)
        self.assertEqual(self._differs(case, QUIET_NAN_1, QUIET_NAN_2), set())
        self.assertEqual(self._differs(case, QUIET_NAN_1, DEFAULT_NAN), set())
        self.assertEqual(self._differs(case, QUIET_NAN_1, ONE), {vector_field(case, 0)})
        single = self._dpps(0xFF, (QUIET_NAN_1, ONE, ONE, ONE), (ONE,) * 4)
        self.assertEqual(self._differs(single, QUIET_NAN_1, QUIET_NAN_2), {vector_field(single, 0)})
        unselected = self._dpps(0x1B, (QUIET_NAN_1, QUIET_NAN_2, ONE, ONE), (ONE,) * 4)
        self.assertEqual(self._differs(unselected, QUIET_NAN_1, QUIET_NAN_2), {vector_field(unselected, 0)})

    def test_dpps_overflowing_products_depend_on_rounding(self):
        lanes = ((TWO_TO_127, MINUS_TWO_TO_127, ONE, ONE), (TWO, TWO, ONE, ONE))
        nearest = self._dpps(0xFF, *lanes)
        self.assertEqual(self._differs(nearest, DEFAULT_NAN, QUIET_NAN_1), set())
        toward_zero = self._dpps(0xFF, *lanes, accept=lambda spec: spec.mxcsr == TOWARD_ZERO_MXCSR)
        self.assertEqual(self._differs(toward_zero, DEFAULT_NAN, QUIET_NAN_1), {vector_field(toward_zero, 0)})


class RecordedTest(unittest.TestCase):
    def test_recording_kept(self):
        case = find_case("base", primary("add", operand_size=64))
        with self.assertRaises(NoExpectation):
            expected_result(case, None)
        recorded = completed(case, flags(CF=1))
        self.assertIs(expected_result(case, recorded), recorded)


class RecordedLoadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path(tempfile.mkdtemp(prefix="ps5lift-expectations-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def test_load_models_reproduce_recorded_loads(self):
        checked = collections.Counter()
        for group in ("sse", "avx", "xsave"):
            found, recorded = linked_group(group, self.work)
            for case in found:
                mnemonic = case.instance.form.definition.mnemonic
                result = recorded.get(case.spec.index)
                if mnemonic not in LOADS or result is None or result.outcome is not Outcome.COMPLETED:
                    continue
                with self.subTest(case=case.spec.index, instance=case.instance.text):
                    self.assertEqual(State.of(expectations.loaded_state(case), case.input.slot),
                                     State.of(result, case.input.slot))
                checked[mnemonic] += 1
        self.assertEqual(set(checked), LOADS, checked)


if __name__ == "__main__":
    unittest.main()
