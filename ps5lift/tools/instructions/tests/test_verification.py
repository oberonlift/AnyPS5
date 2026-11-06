import unittest

from instructions import database, decoder, encoding, verification
from instructions.database import Group
from instructions.encoding import Imm, Mem, Reg, Target
from instructions.tests.support import find_form


class VerificationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decoder = decoder.from_environment()
        cls.definitions = database.load()
        cls.forms = database.forms(cls.definitions)

    def form(self, mnemonic, operand_size=None, **conditions):
        return find_form(self, self.forms, mnemonic, operand_size, **conditions)

    def instance(self, form, operands, prefixes=()):
        return form, operands, prefixes, encoding.encode(form, operands, prefixes)

    def test_accepts_what_it_encodes(self):
        mov_load = self.form("mov", 64, opcode=0x8B, modrm_mod="!3")
        verification.verify_all(self.decoder, [
            self.instance(self.form("add", 64, opcode=0x01, modrm_mod="3"), [Reg("rax"), Reg("rbx")]),
            self.instance(self.form("add", 16, opcode=0x83, modrm_mod="3"), [Reg("ax"), Imm(-1)]),
            self.instance(self.form("movaps", encoding=None, opcode=0x28, modrm_mod="!3"), [Reg("xmm8"), Mem("r12")]),
            self.instance(mov_load, [Reg("rax"), Mem("rip", displacement="slot", displacement_size=32)]),
            self.instance(mov_load, [Reg("r9"), Mem("rsp", "r13", 8, -2, 32)]),
            self.instance(mov_load, [Reg("rax"), Mem("rbp", displacement=-128, displacement_size=8)]),
            self.instance(self.form("mov", 64, opcode=0xA1), [Mem(None, displacement="block", displacement_size=64)]),
            self.instance(self.form("add", 64, opcode=0x01, modrm_mod="!3"), [Mem("rbx"), Reg("rax")], ("lock",)),
            self.instance(self.form("jmp", 64, opcode=0xE9, feature_amd="1"), [Target("taken")], ("bnd",)),
            self.instance(self.form("movsb", mandatory_prefix="f3"), []),
            self.instance(self.form("vaddps", encoding="mvex", modrm_mod="3", mvex_e="0"), [Reg("zmm17"), Reg("k1"), Reg("zmm2"), Reg("zmm31")]),
            self.instance(self.form("vaddps", encoding="evex", vector_length="512", modrm_mod="3", evex_b="0"), [Reg("zmm17"), Reg("k1"), Reg("zmm20"), Reg("zmm31")]),
            self.instance(self.form("jkzd", opcode=0x74), [Reg("k1"), Target("taken")]),
        ])

    def test_names_the_form(self):
        add = self.form("add", 64, opcode=0x01, modrm_mod="3")
        operands = [Reg("rax"), Reg("rbx")]
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{add.key}: .*rbx"):
            verification.verify(self.decoder, add, operands, (), [bytes.fromhex("4801c3")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{add.key}: "):
            verification.verify(self.decoder, add, operands, (), [b"\x06"])

    def test_displacement_size_must_match(self):
        add = self.form("add", 64, opcode=0x03, modrm_mod="!3")
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{add.key}: .*displacement size 8, not 0"):
            verification.verify(self.decoder, add, [Reg("rax"), Mem("rbx")], (), [bytes.fromhex("48034300")])
        with self.assertRaisesRegex(encoding.EncodingError, rf"^{add.key}: .*displacement size 0, not 32"):
            verification.verify(self.decoder, add, [Reg("rax"), Mem("rbx", displacement_size=32)], (), [bytes.fromhex("480303")])

    def verify_defaults(self, forms):
        instances = []
        for form in forms:
            operands = encoding.default_operands(form)
            instances.append((form, operands, (), encoding.encode(form, operands)))
        verification.verify_all(self.decoder, instances)

    def test_every_set_form_encodes(self):
        self.verify_defaults([form for form in self.forms if database.classify(form.definition) is Group.SET])

    def test_every_outside_definition_encodes(self):
        outside = [definition for definition in self.definitions if database.classify(definition) is not Group.SET]
        self.verify_defaults([database.forms([definition])[0] for definition in outside])


if __name__ == "__main__":
    unittest.main()
