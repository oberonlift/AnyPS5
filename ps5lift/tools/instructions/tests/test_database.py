import json
import unittest

from instructions import database, decoder
from instructions.database import Group


class DatabaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.definitions = database.load()

    def matching(self, mnemonic, **conditions):
        return [
            definition
            for definition in self.definitions
            if definition.mnemonic == mnemonic
            and all(
                getattr(definition, name) == value if hasattr(definition, name) else definition.filters.get(name) == value
                for name, value in conditions.items()
            )
        ]

    def find(self, mnemonic, **conditions):
        matches = self.matching(mnemonic, **conditions)
        self.assertEqual(len(matches), 1, f"{mnemonic} {conditions} matched {len(matches)} definitions")
        return matches[0]

    def sizes(self, mnemonic, **conditions):
        return {form.operand_size for form in database.forms([self.find(mnemonic, **conditions)])}

    def test_counts(self):
        in_set = [definition for definition in self.definitions if database.classify(definition) is Group.SET]
        self.assertEqual(len(self.definitions), 7260)
        self.assertEqual(len(in_set), 2860)
        self.assertEqual(len({definition.mnemonic for definition in in_set}), 922)
        self.assertEqual(len(database.forms(in_set)), 3522)

    def test_decoder_modes_match_ps5lift_decode(self):
        probes = {
            "amd": ("66e800000000", ("call", 4), ("call", 6)),
            "cet": ("f30f1efa", ("endbr64", 4), ("nop", 4)),
            "cldemote": ("0f1c03", ("cldemote", 3), ("nop", 3)),
            "iprefetch": ("0f183d00000000", ("prefetchit0", 7), ("nop", 7)),
            "knc": ("c5f890c1", ("kmov", 4), ("kmovw", 4)),
            "lzcnt": ("f30fbdc0", ("lzcnt", 4), ("bsr", 4)),
            "mpx": ("0f1a03", ("bndldx", 3), ("nop", 3)),
            "tzcnt": ("f30fbcc0", ("tzcnt", 4), ("bsf", 4)),
            "ud0_compat": ("0fffc0", ("ud0", 2), ("ud0", 3)),
        }
        self.assertEqual(set(probes), set(database.DECODER_MODES))
        decoded = decoder.from_environment().decode([bytes.fromhex(probe) for probe, _, _ in probes.values()])
        for (mode, (probe, enabled, disabled)), result in zip(probes.items(), decoded):
            with self.subTest(mode=mode, probe=probe):
                expected = enabled if database.DECODER_MODES[mode] else disabled
                self.assertEqual((result.mnemonic, result.length), expected)

    def test_rows_for_other_decoder_modes_dropped(self):
        for definition in self.definitions:
            for name, value in definition.filters.items():
                if name.startswith(database.FEATURE_FILTER):
                    self.assertEqual(value == "1", database.DECODER_MODES[name[len(database.FEATURE_FILTER):]], definition)
        self.assertEqual(self.find("call", opcode=0xE8).opsize_map, "default64")
        self.assertFalse(self.matching("nop", opcode=0x1A, modrm_mod="!3"))
        self.assertFalse(self.matching("nop", opcode=0x1E, modrm_reg="7", modrm_rm="2"))
        self.assertFalse(self.matching("ud0", modrm_mod=None))
        self.assertIs(database.classify(self.find("nop", opcode=0x1F, modrm_mod="!3", modrm_reg="0")), Group.SET)

    def test_no_16_bit_form_where_66_is_mandatory(self):
        self.assertEqual(self.sizes("bsf", modrm_mod="3", mandatory_prefix="none"), {32, 64})
        self.assertEqual(self.sizes("nop", opcode=0x1A, modrm_mod="3", mandatory_prefix="none"), {32, 64})
        self.assertEqual(self.sizes("nop", opcode=0x1E, modrm_mod="3", mandatory_prefix="66"), {32, 64})

    def test_sibling_claims(self):
        self.assertEqual(self.sizes("bsf", modrm_mod="3", mandatory_prefix="ignore"), {16})
        self.assertEqual(self.sizes("bsr", modrm_mod="!3", mandatory_prefix="ignore"), {16})
        self.assertEqual(self.sizes("nop", opcode=0x1E, modrm_mod="3", mandatory_prefix="ignore"), set())
        cldemote_space = self.find("nop", opcode=0x1C, modrm_mod="!3")
        self.assertEqual(self.sizes("nop", opcode=0x1C, modrm_mod="!3"), {16, 32, 64})
        self.assertTrue(database.claimed_by_sibling(cldemote_space, "none", "0"))
        self.assertFalse(database.claimed_by_sibling(cldemote_space, "none", "1"))
        self.assertFalse(database.claimed_by_sibling(cldemote_space, "66", "0"))
        movsb = self.find("movsb", mandatory_prefix="ignore")
        self.assertEqual({prefix for prefix, _ in movsb.sibling_claims}, {"f2", "f3"})

    def test_classify_examples(self):
        examples = [
            (Group.SET, "add", {"opcode": 0x01, "modrm_mod": "3"}),
            (Group.SET, "lzcnt", {"extension": "LZCNT", "modrm_mod": "3"}),
            (Group.SET, "popcnt", {"extension": "SSE4", "modrm_mod": "3"}),
            (Group.SET, "endbr64", {}),
            (Group.SET, "bndmov", {"opcode": 0x1A, "modrm_mod": "3"}),
            (Group.SET, "rdsspq", {}),
            (Group.SET, "prefetchwt1", {}),
            (Group.SET, "int3", {}),
            (Group.SET, "ud2", {}),
            (Group.NOT_ON_PS5, "vaddps", {"encoding": "evex", "vector_length": "128", "modrm_mod": "3"}),
            (Group.NOT_ON_PS5, "monitor", {}),
            (Group.NOT_ON_PS5, "clzero", {}),
            (Group.NOT_ON_PS5, "rdfsbase", {}),
            (Group.NOT_ON_PS5, "sysenter", {}),
            (Group.PRIVILEGED, "hlt", {}),
            (Group.PRIVILEGED, "wrmsr", {}),
            (Group.PRIVILEGED, "in", {"opcode": 0xE4}),
            (Group.PRIVILEGED, "rdpmc", {}),
            (Group.PRIVILEGED, "lgdt", {}),
            (Group.PRIVILEGED, "xsaves", {"rex_w": "0"}),
            (Group.SEGMENT_TABLES, "lar", {"modrm_mod": "3"}),
            (Group.SEGMENT_TABLES, "mov", {"opcode": 0x8E, "modrm_mod": "3"}),
            (Group.SEGMENT_TABLES, "mov", {"opcode": 0x8C, "modrm_mod": "3"}),
            (Group.SEGMENT_TABLES, "push", {"opcode_map": "0f", "opcode": 0xA0}),
            (Group.SEGMENT_TABLES, "ret", {"opcode": 0xCB}),
            (Group.CONSOLE_KERNEL, "syscall", {}),
            (Group.CONSOLE_KERNEL, "int", {}),
            (Group.CONSOLE_KERNEL, "int1", {}),
        ]
        for group, mnemonic, conditions in examples:
            with self.subTest(mnemonic=mnemonic, conditions=conditions):
                self.assertIs(database.classify(self.find(mnemonic, **conditions)), group)

    def test_far_call_and_jmp_are_segment_tables(self):
        far = [
            definition
            for definition in self.definitions
            if definition.mnemonic in ("call", "jmp") and definition.opcode == 0xFF and definition.filters["modrm_reg"] in ("3", "5")
        ]
        self.assertEqual({definition.mnemonic for definition in far}, {"call", "jmp"})
        for definition in far:
            self.assertIs(database.classify(definition), Group.SEGMENT_TABLES)

    def test_knc_rows_dropped(self):
        self.assertFalse([definition for definition in self.definitions if definition.mnemonic == "popcnt" and definition.extension == "KNC"])

    def test_operand_size_encodings(self):
        def encodings(mnemonic, **conditions):
            return [
                (choice.size, choice.operand_size_prefix, choice.w)
                for choice in database.operand_size_encodings(self.find(mnemonic, **conditions))
            ]

        self.assertEqual(encodings("add", opcode=0x01, modrm_mod="3"), [(32, False, 0), (16, True, 0), (64, False, 1), (64, True, 1)])
        self.assertEqual(encodings("push", opcode=0x50), [(64, False, 0), (16, True, 0), (64, False, 1), (64, True, 1)])
        self.assertEqual(encodings("mov", opcode=0x22), [(64, False, 0), (64, True, 0), (64, False, 1), (64, True, 1)])
        self.assertEqual(encodings("in", opcode=0xE5, operand_size="64"), [(32, False, 1), (32, True, 1)])
        self.assertEqual(encodings("bsf", modrm_mod="3", mandatory_prefix="none"), [(32, False, 0), (64, False, 1)])
        self.assertEqual(encodings("vaddps", encoding="vex", vector_length="256", modrm_mod="3"), [(32, False, 0), (64, False, 1)])

    def test_undefined_flags(self):
        bsf = self.find("bsf", modrm_mod="3", mandatory_prefix="ignore")
        self.assertEqual(bsf.undefined_flags, frozenset({"cf", "pf", "af", "sf", "of"}))

    def test_form_sizes(self):
        self.assertEqual(self.sizes("add", opcode=0x01, modrm_mod="3"), {16, 32, 64})
        self.assertEqual(self.sizes("push", opcode=0x50), {16, 64})
        self.assertEqual(self.sizes("addps", modrm_mod="3"), {None})

    def test_form_keys(self):
        add = self.find("add", opcode=0x01, modrm_mod="3")
        self.assertEqual([form.key for form in database.forms([add])], [f"add.{add.index}.o{size}" for size in (16, 32, 64)])
        addps = self.find("addps", modrm_mod="3")
        self.assertEqual([form.key for form in database.forms([addps])], [f"addps.{addps.index}"])

    def test_test_group(self):
        def groups(mnemonic, **conditions):
            return {database.test_group(form) for form in database.forms([self.find(mnemonic, **conditions)])}

        self.assertEqual(groups("add", opcode=0x01, modrm_mod="3"), {"base"})
        self.assertEqual(groups("endbr64"), {"hint"})
        self.assertEqual(groups("prefetchwt1"), {"hint"})
        self.assertEqual(groups("vaddps", encoding="evex", vector_length="128", modrm_mod="3"), {"outside-not-on-ps5"})
        self.assertEqual(groups("hlt"), {"outside-privileged"})
        self.assertEqual(groups("lar", modrm_mod="3"), {"outside-segment-tables"})
        self.assertEqual(groups("syscall"), {"outside-console-kernel"})

    def test_database_matches_zydis(self):
        with open(database.DATABASE) as file:
            raw = {definition["mnemonic"] for definition in json.load(file)}
        self.assertEqual(raw, decoder.from_environment().mnemonics())


if __name__ == "__main__":
    unittest.main()
