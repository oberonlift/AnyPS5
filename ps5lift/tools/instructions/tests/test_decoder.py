import unittest

from instructions import decoder


class DecoderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decoder = decoder.from_environment()

    def decode_one(self, encoding):
        return self.decoder.decode([bytes.fromhex(encoding)])[0]

    def test_amd_branch_mode(self):
        decoded = self.decode_one("66e91000")
        self.assertEqual(decoded.length, 4)
        self.assertEqual(decoded.mnemonic, "jmp")
        self.assertEqual(decoded.operand_width, 16)

    def test_endbr64(self):
        decoded = self.decode_one("f30f1efa")
        self.assertEqual(decoded.mnemonic, "endbr64")
        self.assertEqual(decoded.length, 4)

    def test_add(self):
        decoded = self.decode_one("4801d8")
        self.assertEqual(decoded.mnemonic, "add")
        self.assertEqual([operand.register for operand in decoded.operands[:2]], ["rax", "rbx"])
        self.assertEqual([operand.size for operand in decoded.operands[:2]], [64, 64])

    def test_privileged(self):
        self.assertTrue(self.decode_one("0f01f8").privileged)
        self.assertFalse(self.decode_one("4801d8").privileged)

    def test_batch_keeps_order(self):
        decoded = self.decoder.decode([bytes.fromhex("90"), bytes.fromhex("4801d8"), bytes.fromhex("f30f1efa")])
        self.assertEqual([item.mnemonic for item in decoded], ["nop", "add", "endbr64"])

    def test_invalid_encoding_is_an_error(self):
        with self.assertRaises(decoder.DecodeError):
            self.decode_one("06")

    def test_memory_operand(self):
        decoded = self.decode_one("488b4c9810")
        memory = decoded.operands[1]
        self.assertEqual((memory.type, memory.base, memory.index, memory.scale, memory.displacement), ("memory", "rax", "rbx", 4, 16))

    def test_text(self):
        self.assertEqual(self.decode_one("4801d8").text, "add rax, rbx")

    def test_mnemonic_count(self):
        self.assertEqual(len(self.decoder.mnemonics()), 1792)


if __name__ == "__main__":
    unittest.main()
