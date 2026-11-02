import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from instructions import records
from instructions.records import CaseInput, Header, Outcome, Result

FLAGS = ("CF", "PF", "AF", "ZF", "SF", "OF", "DF")


def _header():
    return Header(bytes(range(32)), bytes(range(100, 132)))


def _slot(fill):
    return bytes([fill]) * records.SLOT_SIZE


class RecordsTest(unittest.TestCase):
    def test_layout_offsets(self):
        self.assertEqual(records.SLOT_SIZE, 1168)
        self.assertEqual(records.FX_OFFSET, 0)
        self.assertEqual(records.FX_SIZE, 512)
        self.assertEqual(records.ymm_offset(0), 512)
        self.assertEqual(records.ymm_offset(15), 512 + 32 * 15)
        self.assertEqual(records.gpr_offset(0), 1024)
        self.assertEqual(records.gpr_offset(15), 1024 + 8 * 15)
        self.assertEqual(records.GPR_NAMES[:8], ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"))
        self.assertEqual(records.GPR_NAMES[8:], tuple(f"r{n}" for n in range(8, 16)))
        self.assertEqual(
            (records.LOAD_AH, records.LOAD_OF_LEFT, records.LOAD_OF_RIGHT, records.LOAD_DF),
            (1152, 1153, 1154, 1155),
        )
        self.assertEqual((records.STORE_AX, records.STORE_DF, records.PATH), (1156, 1158, 1159))
        self.assertEqual(records.fx_st_offset(3), 32 + 48)
        self.assertEqual((records.FX_FCW, records.FX_FSW, records.FX_FTW, records.FX_MXCSR), (0, 2, 4, 24))
        self.assertEqual(records.BLOCK_SIZE, 4096)

    def test_outcome_values(self):
        self.assertEqual(Outcome.COMPLETED, 0)
        self.assertEqual(Outcome.PAGE_FAULT, 5)
        self.assertEqual(Outcome.TIMED_OUT, 15)
        self.assertEqual(len(Outcome), 16)

    def test_load_bytes(self):
        flags = {name: 1 for name in FLAGS}
        self.assertEqual(records.load_bytes(flags), (0xD7, 0x7F, 0x01, 1))
        flags = {name: 0 for name in FLAGS}
        self.assertEqual(records.load_bytes(flags), (0x02, 0, 0, 0))

    def test_store_flags_inverts_load_bytes(self):
        for bits in range(1 << len(FLAGS)):
            flags = {name: bits >> position & 1 for position, name in enumerate(FLAGS)}
            ah, of_left, of_right, df = records.load_bytes(flags)
            self.assertEqual(records.store_flags(ah << 8 | flags["OF"], df), flags)

    def test_inputs_round_trip(self):
        inputs = [
            CaseInput(0, 0, 1, False, _slot(1), ()),
            CaseInput(7, 3, 2**64 - 1, True, _slot(2), ((0, b"\x01"), (4090, b"abcdef"))),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "inputs.bin"
            records.write_inputs(path, _header(), inputs)
            self.assertEqual(records.read_inputs(path), (_header(), inputs))

    def test_input_file_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "inputs.bin"
            records.write_inputs(path, _header(), [CaseInput(1, 2, 3, True, _slot(0), ((5, b"xy"),))])
            data = path.read_bytes()
        self.assertEqual(data[:8], b"PS5LIN01")
        self.assertEqual(int.from_bytes(data[72:76], "little"), 1)
        self.assertEqual(len(data), 76 + 20 + records.SLOT_SIZE + 2 + 4 + 2)

    def test_results_round_trip(self):
        results = [
            Result(0, Outcome.COMPLETED, 0, 0, 0, {0: 5, 1024: 2**64 - 1}, {16: 9}, None),
            Result(1, Outcome.PAGE_FAULT, 2, 0x1000, 0x20, {}, {}, None),
            Result(2, Outcome.COMPLETED, 0, 0, 0, {8: 1}, {}, (1, 2**64 - 1, 3)),
        ]
        data = records.write_results(_header(), results)
        self.assertEqual(data[:8], b"PS5LRES1")
        self.assertEqual(records.read_results(data), (_header(), results))

    def test_results_reject_truncation(self):
        data = records.write_results(_header(), [Result(0, Outcome.COMPLETED, 0, 0, 0, {8: 1}, {}, None)])
        with self.assertRaises(records.RecordError):
            records.read_results(data[:-1])
        with self.assertRaises(records.RecordError):
            records.read_results(data + b"\0")

    def test_fill_block(self):
        block = records.fill_block(1)
        self.assertEqual(len(block), records.BLOCK_SIZE)
        self.assertEqual(records.fill_block(0), records.fill_block(0x9E3779B97F4A7C15))
        self.assertNotEqual(block, records.fill_block(2))

    def test_fill_matches_c(self):
        compiler = os.environ.get("PS5LIFT_CLANG")
        self.assertIsNotNone(compiler, "PS5LIFT_CLANG is not set: it names the clang executable")
        seeds = (0, 1, 2**64 - 1)
        source = (
            records.header_text()
            + "\n#include <stdio.h>\nint main(void) {\n"
            + "    uint64_t seeds[] = {0, 1, UINT64_MAX};\n"
            + "    for (int i = 0; i < 3; ++i) {\n"
            + "        uint8_t block[PS5LIFT_INSTRUCTION_BLOCK_SIZE];\n"
            + "        Ps5LiftFillBlock(block, seeds[i]);\n"
            + "        for (int j = 0; j < PS5LIFT_INSTRUCTION_BLOCK_SIZE; ++j) printf(\"%02x\", block[j]);\n"
            + "        printf(\"\\n\");\n"
            + "    }\n    return 0;\n}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "fill.c"
            program.write_text(source)
            executable = Path(directory) / "fill"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", "-o", executable, program], check=True)
            lines = subprocess.run([executable], check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual(lines, [records.fill_block(seed).hex() for seed in seeds])


if __name__ == "__main__":
    unittest.main()
