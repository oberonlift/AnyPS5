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
            (self.form("mov", opcode=0x22), [Reg("cr8"), Reg("rax")], "440f22c0"),
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

    def test_address_size_follows_adsize_map(self):
        add = self.form("add", 64, opcode=0x03, modrm_mod="!3")
        self.assertEqual(encoding.encode(add, [Reg("rax"), Mem("ebx")]), [bytes.fromhex("67480303")])
        bndcl = self.form("bndcl", modrm_mod="!3")
        self.assertEqual(bndcl.definition.adsize_map, "ignored")
        self.assertEqual(encoding.encode(bndcl, [Reg("bnd0"), Mem("rbx")]), [bytes.fromhex("f30f1a03")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{bndcl.key}: .*adsize_map 'ignored'"):
            encoding.encode(bndcl, [Reg("bnd0"), Mem("ebx")])

    def test_ignore_modrm_mod_is_a_register_form(self):
        mov = self.form("mov", opcode=0x20)
        self.assertIn("ignore_modrm_mod", mov.definition.flags)
        self.assertEqual(encoding.encode(mov, [Reg("rax"), Reg("cr0")]), [bytes.fromhex("0f20c0")])
        movaps = self.form("movaps", encoding=None, opcode=0x28, modrm_mod="!3")
        with_flag = dataclasses.replace(movaps.definition, flags=movaps.definition.flags | {"ignore_modrm_mod"})
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{movaps.key}: .*ignore_modrm_mod"):
            encoding.encode(database.Form(with_flag, None, movaps.key), [Reg("xmm0"), Mem("rbx")])

    def test_multisource_names_a_block_of_four(self):
        v4fmaddps = self.form("v4fmaddps")
        self.assertEqual(encoding.default_operands(v4fmaddps), [Reg("zmm0"), Reg("k0"), Reg("zmm0"), Mem("rbx")])
        encoding.encode(v4fmaddps, [Reg("zmm1"), Reg("k1"), Reg("zmm4"), Mem("rbx")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{v4fmaddps.key}: .*zmm5.*multiple of 4"):
            encoding.encode(v4fmaddps, [Reg("zmm1"), Reg("k1"), Reg("zmm5"), Mem("rbx")])

    def test_unknown_value_names_the_form(self):
        addps = self.form("addps", modrm_mod="3")
        definition = addps.definition
        broken = {
            "'4'": dataclasses.replace(definition, filters={**definition.filters, "modrm_mod": "4"}),
            "'new_flag'": dataclasses.replace(definition, flags=definition.flags | {"new_flag"}),
            "'accepts_new'": dataclasses.replace(definition, prefix_flags=definition.prefix_flags | {"accepts_new"}),
            "'shrunk'": dataclasses.replace(definition, adsize_map="shrunk"),
            "'new_key'": dataclasses.replace(definition, operands=({**definition.operands[0], "new_key": 1}, *definition.operands[1:])),
            "'sometimes'": dataclasses.replace(definition, operands=({**definition.operands[0], "is_multisource4": "sometimes"}, *definition.operands[1:])),
        }
        for value, changed in broken.items():
            with self.subTest(value=value):
                with self.assertRaisesRegex(encoding.EncodingError, rf"^{addps.key}: .*{value}"):
                    encoding.encode(database.Form(changed, None, addps.key), [Reg("xmm0"), Reg("xmm1")])

    def test_default_operands_leave_a_siblings_bytes(self):
        nop = self.form("nop", 32, opcode=0x1C, modrm_mod="!3")
        self.assertEqual(encoding.default_operands(nop), [Mem("rbx"), Reg("ecx")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{nop.key}: .*modrm.reg 0"):
            encoding.encode(nop, [Mem("rbx"), Reg("eax")])


if __name__ == "__main__":
    unittest.main()
