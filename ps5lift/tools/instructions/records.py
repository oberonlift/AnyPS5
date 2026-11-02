"""The binary formats the instruction tests share, and the C header generated from them.

The slot is the guest memory image that generated code loads its initial state from and stores its final state into. The
input file lists the cases, and the result file records what each one did. Everything is little-endian. `header_text`
emits `records.h` for the drivers, so the C and Python sides cannot drift apart."""
import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import Optional

GPR_NAMES = (
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
)

FX_OFFSET = 0
FX_SIZE = 512
FX_FCW = 0
FX_FSW = 2
FX_FTW = 4
FX_MXCSR = 24
FX_ST = 32
FX_ST_STRIDE = 16
YMM_OFFSET = 512
YMM_STRIDE = 32
YMM_COUNT = 16
GPR_OFFSET = 1024
GPR_STRIDE = 8
LOAD_AH = 1152
LOAD_OF_LEFT = 1153
LOAD_OF_RIGHT = 1154
LOAD_DF = 1155
STORE_AX = 1156
STORE_DF = 1158
PATH = 1159
PATH_UNSET = 0xFF
SLOT_SIZE = 1168
SLOT_ALIGNMENT = 64

BLOCK_SIZE = 4096
BLOCK_END_ALIGNMENT = 64 * 1024
GUARD_SIZE = 64 * 1024

INPUT_MAGIC = b"PS5LIN01"
RESULT_MAGIC = b"PS5LRES1"
FINGERPRINT_SIZE = 32

FLAG_NAMES = ("CF", "PF", "AF", "ZF", "SF", "OF", "DF")
_AH_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7}
_AH_ALWAYS_SET = 0x02

_FILL_MULTIPLIER = 0x2545F4914F6CDD1D
_FILL_ZERO_SEED = 0x9E3779B97F4A7C15
_MASK64 = 2**64 - 1


@dataclass(frozen=True)
class Field:
    name: str
    c_type: str
    code: str
    count: Optional[int] = None

    def declaration(self):
        suffix = "" if self.count is None else f"[{self.count}]"
        return f"{self.c_type} {self.name}{suffix};"

    def format(self):
        return self.code if self.count is None else f"{self.count}{self.code}"

    def size(self):
        return struct.calcsize("<" + self.format())


@dataclass(frozen=True)
class Record:
    c_name: str
    fields: tuple

    def struct(self):
        return struct.Struct("<" + "".join(field.format() for field in self.fields))

    def c_text(self):
        body = "\n".join(f"    {field.declaration()}" for field in self.fields)
        lines = [f"typedef struct {self.c_name} {{", body, f"}} {self.c_name};"]
        lines.append(f'_Static_assert(sizeof({self.c_name}) == {self.struct().size}, "{self.c_name} size");')
        offset = 0
        for field in self.fields:
            lines.append(
                f'_Static_assert(offsetof({self.c_name}, {field.name}) == {offset}, "{self.c_name}.{field.name} offset");'
            )
            offset += field.size()
        return "\n".join(lines)


FILE_HEADER = Record("Ps5LiftInstructionFileHeader", (
    Field("Magic", "char", "s", 8),
    Field("Program", "uint8_t", "s", FINGERPRINT_SIZE),
    Field("Inputs", "uint8_t", "s", FINGERPRINT_SIZE),
    Field("Count", "uint32_t", "I"),
))
INPUT_HEAD = Record("Ps5LiftInstructionInputHead", (
    Field("CaseIndex", "uint32_t", "I"),
    Field("Instance", "uint32_t", "I"),
    Field("FillSeed", "uint64_t", "Q"),
    Field("ConsoleDefined", "uint8_t", "B"),
    Field("Pad", "uint8_t", "x", 3),
))
PATCH_COUNT = Record("Ps5LiftInstructionPatchCount", (
    Field("Count", "uint16_t", "H"),
))
PATCH_HEAD = Record("Ps5LiftInstructionPatchHead", (
    Field("Offset", "uint16_t", "H"),
    Field("Length", "uint16_t", "H"),
))
RESULT_HEAD = Record("Ps5LiftInstructionResultHead", (
    Field("CaseIndex", "uint32_t", "I"),
    Field("Outcome", "uint8_t", "B"),
    Field("ConsoleDefined", "uint8_t", "B"),
    Field("Detail", "uint16_t", "H"),
    Field("SlotWords", "uint16_t", "H"),
    Field("MemoryWords", "uint16_t", "H"),
    Field("Address", "uint64_t", "Q"),
    Field("Data", "uint64_t", "Q"),
))
WORD = Record("Ps5LiftInstructionWord", (
    Field("Offset", "uint16_t", "H"),
    Field("Value", "uint64_t", "Q"),
))
EXPECTED_COUNT = Record("Ps5LiftInstructionExpectedCount", (
    Field("Count", "uint8_t", "B"),
))
RECORDS = (FILE_HEADER, INPUT_HEAD, PATCH_COUNT, PATCH_HEAD, RESULT_HEAD, WORD, EXPECTED_COUNT)

_FILE_HEADER = FILE_HEADER.struct()
_INPUT_HEAD = INPUT_HEAD.struct()
_PATCH_COUNT = PATCH_COUNT.struct()
_PATCH_HEAD = PATCH_HEAD.struct()
_RESULT_HEAD = RESULT_HEAD.struct()
_WORD = WORD.struct()
_EXPECTED_COUNT = EXPECTED_COUNT.struct()


class Outcome(IntEnum):
    COMPLETED = 0
    DIVIDE_ERROR = 1
    BREAKPOINT = 2
    INVALID_OPCODE = 3
    GENERAL_PROTECTION = 4
    PAGE_FAULT = 5
    ALIGNMENT_CHECK = 6
    X87_FLOATING_POINT = 7
    SIMD_FLOATING_POINT = 8
    SEGMENT_TABLES = 9
    CONSOLE_KERNEL = 10
    NEEDS_EXACT_FLOAT = 11
    MISSING_TRANSLATION = 12
    HOST_SIGNAL = 13
    CRASHED = 14
    TIMED_OUT = 15


class RecordError(Exception):
    pass


@dataclass(frozen=True)
class Header:
    program: bytes
    inputs: bytes


@dataclass(frozen=True)
class CaseInput:
    case: int
    instance: int
    fill_seed: int
    console_defined: bool
    slot: bytes
    patches: tuple


@dataclass(frozen=True)
class Result:
    case: int
    outcome: Outcome
    detail: int
    address: int
    data: int
    slot_words: dict
    memory_words: dict
    expected: Optional[tuple]


def fx_st_offset(index):
    return FX_OFFSET + FX_ST + FX_ST_STRIDE * index


def ymm_offset(index):
    return YMM_OFFSET + YMM_STRIDE * index


def gpr_offset(index):
    return GPR_OFFSET + GPR_STRIDE * index


def load_bytes(flags):
    _check_flags(flags)
    ah = _AH_ALWAYS_SET
    for name, bit in _AH_BITS.items():
        ah |= flags[name] << bit
    of_left, of_right = (0x7F, 0x01) if flags["OF"] else (0, 0)
    return ah, of_left, of_right, flags["DF"]


def store_flags(ax, df):
    ah = ax >> 8
    flags = {name: ah >> bit & 1 for name, bit in _AH_BITS.items()}
    flags["OF"] = ax & 1
    flags["DF"] = df
    return flags


def _check_flags(flags):
    if set(flags) != set(FLAG_NAMES) or any(value not in (0, 1) for value in flags.values()):
        raise RecordError(f"flags must map exactly {FLAG_NAMES} to 0 or 1, not {flags}")


def fill_block(seed):
    x = seed or _FILL_ZERO_SEED
    out = bytearray()
    for _ in range(BLOCK_SIZE // 8):
        x ^= x >> 12
        x ^= x << 25 & _MASK64
        x ^= x >> 27
        out += (x * _FILL_MULTIPLIER & _MASK64).to_bytes(8, "little")
    return bytes(out)


def _pack_header(magic, header, count):
    for name, fingerprint in (("program", header.program), ("inputs", header.inputs)):
        if len(fingerprint) != FINGERPRINT_SIZE:
            raise RecordError(f"the {name} fingerprint is {len(fingerprint)} bytes, not {FINGERPRINT_SIZE}")
    return _FILE_HEADER.pack(magic, header.program, header.inputs, count)


class _Reader:
    def __init__(self, data):
        self.data = data
        self.position = 0

    def take(self, size):
        end = self.position + size
        if end > len(self.data):
            raise RecordError(f"the file ends at {len(self.data)}, {end - len(self.data)} bytes short")
        chunk = self.data[self.position:end]
        self.position = end
        return chunk

    def unpack(self, layout):
        return layout.unpack(self.take(layout.size))

    def finish(self):
        if self.position != len(self.data):
            raise RecordError(f"{len(self.data) - self.position} bytes follow the last record")


def _unpack_header(reader, magic, count_name):
    found, program, inputs, count = reader.unpack(_FILE_HEADER)
    if found != magic:
        raise RecordError(f"the {count_name} file starts with {found!r}, not {magic!r}")
    return Header(program, inputs), count


def write_inputs(path, header, inputs):
    out = bytearray(_pack_header(INPUT_MAGIC, header, len(inputs)))
    for case in inputs:
        if len(case.slot) != SLOT_SIZE:
            raise RecordError(f"case {case.case}: the slot is {len(case.slot)} bytes, not {SLOT_SIZE}")
        out += _INPUT_HEAD.pack(case.case, case.instance, case.fill_seed, case.console_defined)
        out += case.slot
        out += _PATCH_COUNT.pack(len(case.patches))
        for offset, data in case.patches:
            out += _PATCH_HEAD.pack(offset, len(data)) + data
    with open(path, "wb") as file:
        file.write(out)


def read_inputs(path):
    with open(path, "rb") as file:
        reader = _Reader(file.read())
    header, count = _unpack_header(reader, INPUT_MAGIC, "input")
    inputs = []
    for _ in range(count):
        case, instance, fill_seed, console_defined = reader.unpack(_INPUT_HEAD)
        slot = reader.take(SLOT_SIZE)
        (patch_count,) = reader.unpack(_PATCH_COUNT)
        patches = []
        for _ in range(patch_count):
            offset, length = reader.unpack(_PATCH_HEAD)
            patches.append((offset, reader.take(length)))
        inputs.append(CaseInput(case, instance, fill_seed, bool(console_defined), slot, tuple(patches)))
    reader.finish()
    return header, inputs


def _pack_words(words):
    return b"".join(_WORD.pack(offset, value) for offset, value in sorted(words.items()))


def write_results(header, results):
    out = bytearray(_pack_header(RESULT_MAGIC, header, len(results)))
    for result in results:
        out += _RESULT_HEAD.pack(
            result.case,
            result.outcome,
            result.expected is not None,
            result.detail,
            len(result.slot_words),
            len(result.memory_words),
            result.address,
            result.data,
        )
        out += _pack_words(result.slot_words) + _pack_words(result.memory_words)
        if result.expected is not None:
            out += _EXPECTED_COUNT.pack(len(result.expected))
            out += b"".join(struct.pack("<Q", value) for value in result.expected)
    return bytes(out)


def _read_words(reader, count):
    return dict(reader.unpack(_WORD) for _ in range(count))


def read_results(data):
    reader = _Reader(data)
    header, count = _unpack_header(reader, RESULT_MAGIC, "result")
    results = []
    for _ in range(count):
        case, outcome, console_defined, detail, slot_count, memory_count, address, value = reader.unpack(_RESULT_HEAD)
        slot_words = _read_words(reader, slot_count)
        memory_words = _read_words(reader, memory_count)
        expected = None
        if console_defined:
            (expected_count,) = reader.unpack(_EXPECTED_COUNT)
            expected = struct.unpack(f"<{expected_count}Q", reader.take(8 * expected_count))
        results.append(Result(case, Outcome(outcome), detail, address, value, slot_words, memory_words, expected))
    reader.finish()
    return header, results


_C_TEMPLATE = """#ifndef PS5LIFT_INSTRUCTION_RECORDS_H
#define PS5LIFT_INSTRUCTION_RECORDS_H

#include <stddef.h>
#include <stdint.h>

{defines}

#pragma pack(push, 1)

{structs}

#pragma pack(pop)

static inline void Ps5LiftFillBlock(uint8_t* block, uint64_t seed)
{{
    uint64_t x = seed ? seed : 0x{zero_seed:X}ull;
    for (int i = 0; i < PS5LIFT_INSTRUCTION_BLOCK_SIZE / 8; ++i) {{
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        uint64_t word = x * 0x{multiplier:X}ull;
        for (int j = 0; j < 8; ++j) {{
            block[i * 8 + j] = (uint8_t)(word >> (8 * j));
        }}
    }}
}}

#endif
"""


def _defines():
    values = {
        "FX": FX_OFFSET,
        "FX_SIZE": FX_SIZE,
        "FX_FCW": FX_FCW,
        "FX_FSW": FX_FSW,
        "FX_FTW": FX_FTW,
        "FX_MXCSR": FX_MXCSR,
        "FX_ST": FX_ST,
        "FX_ST_STRIDE": FX_ST_STRIDE,
        "YMM": YMM_OFFSET,
        "YMM_STRIDE": YMM_STRIDE,
        "YMM_COUNT": YMM_COUNT,
        "GPR": GPR_OFFSET,
        "GPR_STRIDE": GPR_STRIDE,
        "GPR_COUNT": len(GPR_NAMES),
        "LOAD_AH": LOAD_AH,
        "LOAD_OF_LEFT": LOAD_OF_LEFT,
        "LOAD_OF_RIGHT": LOAD_OF_RIGHT,
        "LOAD_DF": LOAD_DF,
        "STORE_AX": STORE_AX,
        "STORE_DF": STORE_DF,
        "PATH": PATH,
        "PATH_UNSET": PATH_UNSET,
        "SLOT_SIZE": SLOT_SIZE,
        "SLOT_ALIGNMENT": SLOT_ALIGNMENT,
        "BLOCK_SIZE": BLOCK_SIZE,
        "BLOCK_END_ALIGNMENT": BLOCK_END_ALIGNMENT,
        "GUARD_SIZE": GUARD_SIZE,
        "FINGERPRINT_SIZE": FINGERPRINT_SIZE,
    }
    lines = [f"#define PS5LIFT_INSTRUCTION_{name} {value}" for name, value in values.items()]
    lines.append(f'#define PS5LIFT_INSTRUCTION_INPUT_MAGIC "{INPUT_MAGIC.decode()}"')
    lines.append(f'#define PS5LIFT_INSTRUCTION_RESULT_MAGIC "{RESULT_MAGIC.decode()}"')
    lines += [f"#define PS5LIFT_INSTRUCTION_OUTCOME_{outcome.name} {outcome.value}" for outcome in Outcome]
    return "\n".join(lines)


def header_text():
    return _C_TEMPLATE.format(
        defines=_defines(),
        structs="\n\n".join(record.c_text() for record in RECORDS),
        zero_seed=_FILL_ZERO_SEED,
        multiplier=_FILL_MULTIPLIER,
    )
