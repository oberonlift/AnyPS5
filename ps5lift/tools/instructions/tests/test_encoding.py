import dataclasses
import unittest

from instructions import database, encoding
from instructions.encoding import Field, Imm, Mem, Reg, Target
from instructions.tests.support import find_form


class EncodingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.forms = database.forms(database.load())

    def form(self, mnemonic, operand_size=None, **conditions):
        return find_form(self, self.forms, mnemonic, operand_size, **conditions)

    def assert_encodes(self, cases):
        for form, operands, prefixes, expected in cases:
            with self.subTest(form=form.key, operands=operands, prefixes=prefixes):
                self.assertEqual(encoding.encode(form, operands, prefixes), expected)

    def test_known_bytes(self):
        cases = [
            (self.form("add", 64, opcode=0x01, modrm_mod="3"), [Reg("rax"), Reg("rbx")], "4801d8"),
            (self.form("add", 16, opcode=0x83, modrm_mod="3"), [Reg("ax"), Imm(1)], "6683c001"),
            (self.form("vaddps", encoding="vex", vector_length="256", modrm_mod="3"), [Reg("ymm1"), Reg("ymm2"), Reg("ymm3")], "c5ec58cb"),
            (self.form("vfmadd231ps", encoding="vex", vector_length="128", modrm_mod="!3"), [Reg("xmm0"), Reg("xmm1"), Mem("rax")], "c4e271b800"),
            (self.form("endbr64"), [], "f30f1efa"),
            (self.form("movaps", encoding=None, opcode=0x28, modrm_mod="!3"), [Reg("xmm8"), Mem("r12")], "450f280424"),
            (self.form("pshufb", encoding=None, mandatory_prefix="none", modrm_mod="3"), [Reg("mm0"), Reg("mm1")], "0f3800c1"),
            (self.form("pi2fd", modrm_mod="3"), [Reg("mm0"), Reg("mm1")], "0f0fc10d"),
        ]
        self.assert_encodes([(form, operands, (), [bytes.fromhex(expected)]) for form, operands, expected in cases])

    def test_fields(self):
        mov_load = self.form("mov", 64, opcode=0x8B, modrm_mod="!3")
        self.assert_encodes([
            (self.form("jmp", 64, opcode=0xE9, feature_amd="1"), [Target("taken")], (), [b"\xe9", Field(4, "taken", True)]),
            (mov_load, [Reg("rax"), Mem("rip", displacement="slot", displacement_size=32)], (), [bytes.fromhex("488b05"), Field(4, "slot", True)]),
            (mov_load, [Reg("rax"), Mem("rbx", displacement="offset", displacement_size=8)], (), [bytes.fromhex("488b43"), Field(1, "offset", False)]),
            (mov_load, [Reg("r9"), Mem("rsp", "r13", 8, -2, 32)], (), [bytes.fromhex("4e8b8cec") + (-2).to_bytes(4, "little", signed=True)]),
            (self.form("mov", 64, opcode=0xA1), [Mem(None, displacement="block", displacement_size=64)], (), [bytes.fromhex("48a1"), Field(8, "block", False)]),
        ])

    def test_prefixes(self):
        self.assert_encodes([
            (self.form("add", 64, opcode=0x01, modrm_mod="!3"), [Mem("rbx"), Reg("rax")], ("lock",), [bytes.fromhex("f0480103")]),
            (self.form("movsb", mandatory_prefix="f3"), [], (), [bytes.fromhex("f3a4")]),
            (self.form("cmpsb", mandatory_prefix="f2"), [], (), [bytes.fromhex("f2a6")]),
            (self.form("jmp", 64, opcode=0xE9, feature_amd="1"), [Target("taken")], ("bnd",), [b"\xf2\xe9", Field(4, "taken", True)]),
        ])
        with self.assertRaisesRegex(encoding.EncodingError, r"^add\.\d+\.o64: .*rep"):
            encoding.encode(self.form("add", 64, opcode=0x01, modrm_mod="3"), [Reg("rax"), Reg("rbx")], ("rep",))
        with self.assertRaisesRegex(encoding.EncodingError, r"^movsb\.\d+: .*f3.*more specific row"):
            encoding.encode(self.form("movsb", mandatory_prefix="ignore"), [], ("rep",))

    def test_unknown_value_names_the_form(self):
        addps = self.form("addps", modrm_mod="3")
        broken = dataclasses.replace(addps.definition, filters={**addps.definition.filters, "modrm_mod": "4"})
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{addps.key}: .*'4'"):
            encoding.encode(database.Form(broken, None, addps.key), [Reg("xmm0"), Reg("xmm1")])

    def test_default_operands_leave_a_siblings_bytes(self):
        nop = self.form("nop", 32, opcode=0x1C, modrm_mod="!3")
        self.assertEqual(encoding.default_operands(nop), [Mem("rbx"), Reg("ecx")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{nop.key}: .*modrm.reg 0"):
            encoding.encode(nop, [Mem("rbx"), Reg("eax")])


if __name__ == "__main__":
    unittest.main()
