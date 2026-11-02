import dataclasses
import unittest

from instructions import database, decoder, encoding
from instructions.database import Group
from instructions.encoding import Field, Imm, Mem, Reg, Target


class EncodingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decoder = decoder.from_environment()
        cls.definitions = database.load()
        cls.forms = database.forms(cls.definitions)

    def form(self, mnemonic, operand_size=None, **conditions):
        matches = [
            form
            for form in self.forms
            if form.definition.mnemonic == mnemonic
            and form.operand_size == operand_size
            and all(
                getattr(form.definition, name) == value
                if hasattr(form.definition, name)
                else form.definition.filters.get(name) == value
                for name, value in conditions.items()
            )
        ]
        self.assertEqual(len(matches), 1, f"{mnemonic} o{operand_size} {conditions} matched {len(matches)} forms")
        return matches[0]

    def assert_encodes(self, cases):
        instances = []
        for form, operands, prefixes, expected in cases:
            with self.subTest(form=form.key, operands=operands, prefixes=prefixes):
                encoded = encoding.encode(form, operands, prefixes)
                self.assertEqual(encoded, expected)
                instances.append((form, operands, prefixes, encoded))
        encoding.verify_all(self.decoder, instances)

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

    def test_default_operands_leave_a_siblings_bytes(self):
        nop = self.form("nop", 32, opcode=0x1C, modrm_mod="!3")
        self.assertEqual(encoding.default_operands(nop), [Mem("rbx"), Reg("ecx")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{nop.key}: .*modrm.reg 0"):
            encoding.encode(nop, [Mem("rbx"), Reg("eax")])

    def test_verify_names_the_form(self):
        add = self.form("add", 64, opcode=0x01, modrm_mod="3")
        operands = [Reg("rax"), Reg("rbx")]
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{add.key}: .*rbx"):
            encoding.verify(self.decoder, add, operands, (), [bytes.fromhex("4801c3")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{add.key}: "):
            encoding.verify(self.decoder, add, operands, (), [b"\x06"])

    def test_unknown_value_names_the_form(self):
        addps = self.form("addps", modrm_mod="3")
        broken = dataclasses.replace(addps.definition, filters={**addps.definition.filters, "modrm_mod": "4"})
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{addps.key}: .*'4'"):
            encoding.encode(database.Form(broken, None, addps.key), [Reg("xmm0"), Reg("xmm1")])

    def verify_defaults(self, forms):
        instances = []
        for form in forms:
            operands = encoding.default_operands(form)
            instances.append((form, operands, (), encoding.encode(form, operands)))
        encoding.verify_all(self.decoder, instances)

    def test_every_set_form_encodes(self):
        self.verify_defaults([form for form in self.forms if database.classify(form.definition) is Group.SET])

    def test_every_outside_definition_encodes(self):
        outside = [definition for definition in self.definitions if database.classify(definition) is not Group.SET]
        self.verify_defaults([database.forms([definition])[0] for definition in outside])


if __name__ == "__main__":
    unittest.main()
