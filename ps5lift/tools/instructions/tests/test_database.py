import json
import unittest

from instructions import database, decoder
from instructions.database import Group


class DatabaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.definitions = database.load()

    def find(self, mnemonic, **conditions):
        matches = [
            definition
            for definition in self.definitions
            if definition.mnemonic == mnemonic
            and all(
                getattr(definition, name) == value if hasattr(definition, name) else definition.filters.get(name) == value
                for name, value in conditions.items()
            )
        ]
        self.assertEqual(len(matches), 1, f"{mnemonic} {conditions} matched {len(matches)} definitions")
        return matches[0]

    def sizes(self, mnemonic, **conditions):
        return {form.operand_size for form in database.forms([self.find(mnemonic, **conditions)])}

    def test_counts(self):
        in_set = [definition for definition in self.definitions if database.classify(definition) is Group.SET]
        self.assertEqual(len(in_set), 2889)
        self.assertEqual(len({definition.mnemonic for definition in in_set}), 923)

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
