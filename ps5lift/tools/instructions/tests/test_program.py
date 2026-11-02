import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

import ps5lift
from instructions import cases, decoder, program, records
from instructions.cases import BlockOffset
from instructions.encoding import Field, Mem, placeholder
from instructions.tests.support import generated

GROUP = "base"
HOST_STUB = """int host_run_instructions(const void* table) { (void)table; return 0; }
void host_exit(int code) { (void)code; for (;;) {} }
"""
ELF_EXECUTABLE = 2
ELF_X86_64 = 62
TARGET_HELPERS = {cases.Shape.CALL: "callee", cases.Shape.INDIRECT_JUMP: "target"}
FIELD_KINDS = ("branch", "rip", "absolute")
MASK64 = 2**64 - 1


@dataclass(frozen=True)
class Built:
    assembly: str
    elf: Path
    layout: program.Layout
    image: program.Elf


def _compiler():
    compiler = os.environ.get("PS5LIFT_CLANG")
    if compiler is None:
        raise AssertionError("PS5LIFT_CLANG is not set: it names the clang executable")
    return compiler


def _gpr(slot, number):
    return struct.unpack_from("<Q", slot, records.gpr_offset(number))[0]


def _block_address(layout, expression):
    match = re.fullmatch(rf"{cases.BLOCK}\+(\d+)", expression)
    if match is None:
        raise AssertionError(f"{expression!r} names no block offset")
    return layout.block + int(match.group(1))


def _effective_address(layout, slot, memory):
    if memory.base == "rip":
        return _block_address(layout, memory.displacement)
    address = _gpr(slot, records.GPR_NAMES.index(memory.base)) + memory.displacement
    if memory.index is not None:
        address += _gpr(slot, records.GPR_NAMES.index(memory.index)) * memory.scale
    return address & MASK64


def _build(compiler, work, group, instances):
    assembly = program.emit(group, instances)
    source = work / f"{group}.s"
    source.write_text(assembly)
    elf = program.link(compiler, source, work, None, ps5lift.GUEST_FLAGS, ps5lift.GUEST_BASE)
    return Built(assembly, elf, program.read_layout(elf), program.Elf(elf))


def _only(values, what):
    if len(values) != 1:
        raise AssertionError(f"{len(values)} {what}, not 1")
    return values[0]


class ProgramTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = _compiler()
        cls.instances = generated()[1][GROUP]
        cls.specs = generated()[2][GROUP]
        cls.work = Path(tempfile.mkdtemp(prefix="ps5lift-program-"))
        stub = cls.work / "host_stub.c"
        stub.write_text(HOST_STUB)
        library = cls.work / "libinstructionhost.so"
        subprocess.run([compiler, *ps5lift.GUEST_FLAGS, "-O2", "-fPIC", "-shared", stub, "-o", library], check=True)
        cls.programs = {group: _build(compiler, cls.work, group, members) for group, members in generated()[1].items()}
        base = cls.programs[GROUP]
        cls.assembly, cls.elf, cls.layout, cls.image = base.assembly, base.elf, base.layout, base.image

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def test_emit_links(self):
        header = self.elf.read_bytes()[:20]
        self.assertEqual(header[:4], b"\x7fELF")
        self.assertEqual(struct.unpack_from("<HH", header, 16), (ELF_EXECUTABLE, ELF_X86_64))
        self.assertEqual(len(self.layout.instances), len(self.instances))
        self.assertGreaterEqual(self.layout.table, ps5lift.GUEST_BASE)
        self.assertEqual(program.emit(GROUP, self.instances), self.assembly)

    def test_instance_bytes(self):
        encodings = [self.image.read(entry.address, entry.length) for entry in self.layout.instances]
        decoded = decoder.from_environment().decode(encodings)
        for instance, entry, result in zip(self.instances, self.layout.instances, decoded):
            with self.subTest(instance=instance.index, text=instance.text):
                self.assertEqual(result.mnemonic, instance.form.definition.mnemonic)
                self.assertEqual(result.length, entry.length)
                self.assertEqual(entry.length, len(placeholder(instance.encoded)))

    def test_fields_resolve_to_their_targets(self):
        checked = Counter()
        for group, built in self.programs.items():
            pairs = [
                (instance, entry) for instance, entry in zip(generated()[1][group], built.layout.instances)
                if any(isinstance(piece, Field) for piece in instance.encoded)
            ]
            encodings = [built.image.read(entry.address, entry.length) for _, entry in pairs]
            for (instance, entry), result in zip(pairs, decoder.from_environment().decode(encodings)):
                end = entry.address + entry.length
                for field in (piece for piece in instance.encoded if isinstance(piece, Field)):
                    what = f"{group} instance {instance.index} ({instance.text}) {field.expression}"
                    if field.expression == cases.TARGET:
                        immediates = [operand.immediate for operand in result.operands if operand.type == "immediate"]
                        reached, kind = end + _only(immediates, "immediates"), "branch"
                        intended = entry.label(cases.TARGET)
                    elif field.relative:
                        rip = [operand.displacement for operand in result.operands if operand.base == "rip"]
                        reached, kind = end + _only(rip, "RIP-relative operands"), "rip"
                        intended = _block_address(built.layout, field.expression)
                    else:
                        absolute = [operand.displacement for operand in result.operands
                                    if operand.type == "memory" and operand.base == "none"]
                        reached, kind = _only(absolute, "absolute addresses") & MASK64, "absolute"
                        intended = _block_address(built.layout, field.expression)
                    self.assertEqual(hex(reached), hex(intended), what)
                    checked[kind] += 1
        self.assertTrue(all(checked[kind] for kind in FIELD_KINDS), f"checked only {dict(checked)}")

    def test_layout_alignment(self):
        layout = self.layout
        self.assertEqual((layout.block + records.BLOCK_SIZE) % records.BLOCK_END_ALIGNMENT, 0)
        self.assertEqual(layout.guard, layout.block + records.BLOCK_SIZE)
        for slot in (layout.input, layout.output):
            self.assertEqual(slot % records.SLOT_ALIGNMENT, 0)
            outside = slot + records.SLOT_SIZE <= layout.block or layout.guard + records.GUARD_SIZE <= slot
            self.assertTrue(outside, f"a slot at {slot:#x} lies in the block region")
        self.assertGreaterEqual(abs(layout.output - layout.input), records.SLOT_SIZE)

    def test_table_lists_the_instances(self):
        header = records.TABLE_HEADER.struct()
        entry = records.TABLE_ENTRY.struct()
        magic, count, slot_in, slot_out, block, guard = header.unpack(self.image.read(self.layout.table, header.size))
        self.assertEqual(magic, records.TABLE_MAGIC)
        self.assertEqual(count, len(self.instances))
        self.assertEqual((slot_in, slot_out, block, guard),
                         (self.layout.input, self.layout.output, self.layout.block, self.layout.guard))
        entries = self.image.read(self.layout.table + header.size, entry.size * count)
        for number, expected in enumerate(self.layout.instances):
            function, address, length = entry.unpack_from(entries, number * entry.size)
            self.assertEqual((function, address, length), (expected.function, expected.address, expected.length))

    def test_case_inputs_resolve(self):
        inputs = program.case_inputs(self.layout, self.specs)
        self.assertEqual(len(inputs), len(self.specs))
        for spec, case in zip(self.specs, inputs):
            self.assertEqual((case.case, case.instance, case.fill_seed, case.console_defined),
                             (spec.index, spec.instance, spec.fill_seed, spec.console_defined))
            self.assertEqual(len(case.slot), records.SLOT_SIZE)
            for number, value in enumerate(spec.gprs):
                expected = self.layout.block + value.offset if isinstance(value, BlockOffset) else value
                self.assertEqual(_gpr(case.slot, number), expected, f"case {spec.index} {records.GPR_NAMES[number]}")
            for offset, data in case.patches:
                self.assertIsInstance(offset, int)
                self.assertIsInstance(data, bytes)
                self.assertTrue(0 <= offset and offset + len(data) <= records.BLOCK_SIZE, f"case {spec.index}")

    def test_case_slots(self):
        for spec, case in zip(self.specs, program.case_inputs(self.layout, self.specs)):
            slot = case.slot
            fx = records.FX_OFFSET
            self.assertEqual(slot[fx:fx + records.FX_MXCSR], spec.x87[:records.FX_MXCSR])
            self.assertEqual(struct.unpack_from("<I", slot, fx + records.FX_MXCSR)[0], spec.mxcsr)
            self.assertEqual(slot[fx + records.FX_ST:fx + records.FX_XMM], spec.x87[records.FX_ST:])
            for number, vector in enumerate(spec.vectors):
                xmm = records.fx_xmm_offset(number)
                self.assertEqual(slot[xmm:xmm + records.FX_XMM_STRIDE], vector[:records.FX_XMM_STRIDE])
                ymm = records.ymm_offset(number)
                self.assertEqual(slot[ymm:ymm + records.YMM_STRIDE], vector)
            loads = (records.LOAD_AH, records.LOAD_OF_LEFT, records.LOAD_OF_RIGHT, records.LOAD_DF)
            self.assertEqual(tuple(slot[offset] for offset in loads), records.load_bytes(spec.flags))
            self.assertEqual(slot[records.PATH], records.PATH_UNSET)

    def test_memory_fixups_patch_their_own_address(self):
        inputs = program.case_inputs(self.layout, self.specs)
        offsets = defaultdict(set)
        for spec, case in zip(self.specs, inputs):
            instance = self.instances[spec.instance]
            for location, label in spec.fixups:
                if not isinstance(location, BlockOffset):
                    continue
                memory = instance.operands[0]
                self.assertIsInstance(memory, Mem)
                offset = _effective_address(self.layout, case.slot, memory) - self.layout.block
                target = self.layout.instances[spec.instance].helpers[TARGET_HELPERS[instance.shape]]
                self.assertIn((offset, target.to_bytes(8, "little")), case.patches, f"case {spec.index}")
                offsets[instance.text].add(offset)
        for text in ("call [rax]", "jmp [rax]"):
            base = min(offsets[text])
            self.assertLessEqual({base, base + 1, records.BLOCK_SIZE - 8}, offsets[text], text)

    def test_fingerprints(self):
        program_digest, inputs_digest = program.fingerprints(self.assembly, b"inputs")
        self.assertEqual(len(program_digest), records.FINGERPRINT_SIZE)
        self.assertEqual(len(inputs_digest), records.FINGERPRINT_SIZE)
        self.assertNotEqual(program.fingerprints(self.assembly + "\n", b"inputs")[0], program_digest)
        self.assertNotEqual(program.fingerprints(self.assembly, b"other")[1], inputs_digest)


if __name__ == "__main__":
    unittest.main()
