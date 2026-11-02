"""Read Zydis's instruction database and classify every definition into the PS5 set or one of four groups outside it.

It keeps the rows that the lifter's Zydis decoder modes select, and gives each definition the forms that its own row
decodes: an operand size that no prefix and W reach, or whose bytes Zydis gives to another row, is not a form of this
one. It is the one source of the operand-size and address-size rules, which Zydis's tables for 64-bit mode define."""
import dataclasses
import enum
import json
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

DATABASE = Path(__file__).resolve().parents[3] / "3rdparty" / "zydis-db" / "Data" / "instructions.json"

PS5_EXTENSIONS = frozenset({
    "BASE", "X87", "MMX", "SSE", "SSE2", "SSE3", "SSSE3", "SSE4", "SSE4A", "AVX", "AVX2", "AVX2GATHER", "AVXAES", "FMA",
    "F16C", "AES", "PCLMULQDQ", "SHA", "BMI1", "BMI2", "LZCNT", "ADOX_ADCX", "MOVBE", "RDRAND", "RDSEED", "CLFSH",
    "CLFLUSHOPT", "CLWB", "XSAVE", "XSAVEOPT", "XSAVEC", "XSAVES", "RDPID", "RDTSCP", "PAUSE", "LONGMODE",
    "AMD3DNOW_PREFETCH",
})

KNC_EXTENSIONS = frozenset({"KNC", "KNCE", "KNCV"})
DECODER_MODES = {
    "amd": True,
    "cet": True,
    "cldemote": True,
    "iprefetch": True,
    "knc": False,
    "lzcnt": True,
    "mpx": True,
    "tzcnt": True,
    "ud0_compat": False,
}
FEATURE_FILTER = "feature_"
MODRM_FIELD_VALUES = frozenset("01234567")
SPECIFIC_MANDATORY_PREFIXES = frozenset({"none", "66", "f3", "f2"})
PREFIXES_WITHOUT_OPERAND_SIZE = frozenset({"66", "none"})
OPERAND_SIZE_FILTER_VALUES = {(False, 0): "32", (True, 0): "16", (False, 1): "64", (True, 1): "64"}
HINT_OPCODES = frozenset({0x0D, *range(0x18, 0x20)})
UNMARKED_CPL0_MNEMONICS = frozenset({"lgdt"})
IO_MNEMONICS = frozenset("in out insb insw insd outsb outsw outsd cli sti rdpmc".split())
SEGMENT_TABLE_MNEMONICS = frozenset("lar lsl verr verw sgdt sidt sldt smsw str iret iretd iretq lfs lgs lss".split())
KERNEL_MNEMONICS = frozenset({"syscall", "int", "int1"})
FAR_INDIRECT = {"call": "3", "jmp": "5"}
FAR_RET_OPCODES = frozenset({0xCA, 0xCB})
SEGMENT_REGISTERS_PUSHED = frozenset({"fs", "gs"})

OPERAND_TYPE_SIZES = {
    "gpr16_32_64": {16, 32, 64},
    "gpr16_32_32": {16, 32},
    "gpr32_32_64": {32, 64},
}


def _size_map(plain, prefixed, wide, wide_prefixed):
    return {(False, 0): plain, (True, 0): prefixed, (False, 1): wide, (True, 1): wide_prefixed}


OPERAND_SIZE_MAPS = {
    None: _size_map(32, 16, 64, 64),
    "byteop": _size_map(32, 16, 64, 64),
    "ignore66": _size_map(32, 32, 64, 64),
    "rexw32": _size_map(32, 16, 32, 32),
    "default64": _size_map(64, 16, 64, 64),
    "force64": _size_map(64, 64, 64, 64),
    "force32_or_rexw": _size_map(32, 32, 64, 64),
    "force32_or_64": _size_map(64, 64, 64, 64),
}
ADDRESS_SIZE_MAPS = {
    None: {False: 64, True: 32},
    "ignored": {False: 64, True: 64},
    "force32_or_64": {False: 64, True: 64},
}


@dataclass(frozen=True)
class Definition:
    index: int
    mnemonic: str
    encoding: Optional[str]
    opcode_map: Optional[str]
    opcode: int
    filters: dict
    operands: tuple
    extension: str
    isa_set: str
    category: str
    cpl: Optional[int]
    opsize_map: Optional[str]
    adsize_map: Optional[str]
    undefined_flags: frozenset
    prefix_flags: frozenset
    flags: frozenset
    mask_mode: Optional[str]
    sibling_claims: frozenset


class Group(enum.Enum):
    SET = enum.auto()
    NOT_ON_PS5 = enum.auto()
    PRIVILEGED = enum.auto()
    SEGMENT_TABLES = enum.auto()
    CONSOLE_KERNEL = enum.auto()


@dataclass(frozen=True)
class Form:
    definition: Definition
    operand_size: Optional[int]
    key: str


@dataclass(frozen=True)
class OperandSizeEncoding:
    size: int
    operand_size_prefix: bool
    w: int


def _definition(index, row):
    meta = row["meta_info"]
    flags = row.get("affected_flags", {})
    return Definition(
        index=index,
        mnemonic=row["mnemonic"],
        encoding=row.get("encoding"),
        opcode_map=row.get("opcode_map"),
        opcode=int(row["opcode"], 16),
        filters=row.get("filters", {}),
        operands=tuple(row.get("operands", ())),
        extension=meta["extension"],
        isa_set=meta["isa_set"],
        category=meta["category"],
        cpl=row.get("cpl"),
        opsize_map=row.get("opsize_map"),
        adsize_map=row.get("adsize_map"),
        undefined_flags=frozenset(flag for flag, effect in flags.items() if effect == "u"),
        prefix_flags=frozenset(row.get("prefix_flags", ())),
        flags=frozenset(row.get("flags", ())),
        mask_mode=row.get("evex", {}).get("mask_mode"),
        sibling_claims=frozenset(),
    )


def _selected_by_decoder_modes(definition):
    for name, value in definition.filters.items():
        if not name.startswith(FEATURE_FILTER):
            continue
        mode = name[len(FEATURE_FILTER):]
        if mode not in DECODER_MODES or value not in ("0", "1"):
            raise ValueError(f"{definition.mnemonic}.{definition.index}: no decoder mode for filter {name} {value!r}")
        if (value == "1") != DECODER_MODES[mode]:
            return False
    return True


def _kept(definition):
    return (
        definition.filters.get("mode") != "!64"
        and definition.encoding != "mvex"
        and definition.extension not in KNC_EXTENSIONS
        and _selected_by_decoder_modes(definition)
    )


def _always_satisfied(name):
    return name in ("mandatory_prefix", "mode") or name.startswith(FEATURE_FILTER)


def _claim(definition, sibling):
    prefix = sibling.filters.get("mandatory_prefix")
    if sibling is definition or prefix not in SPECIFIC_MANDATORY_PREFIXES:
        return None
    required = {name: value for name, value in sibling.filters.items() if not _always_satisfied(name)}
    register = None
    if "modrm_reg" in required and "modrm_reg" not in definition.filters:
        register = required.pop("modrm_reg")
        if register not in MODRM_FIELD_VALUES:
            raise ValueError(f"{sibling.mnemonic}.{sibling.index}: no rule for a claim on modrm_reg {register!r}")
    if all(definition.filters.get(name) == value for name, value in required.items()):
        return prefix, register
    return None


def _sibling_claims(definition, siblings):
    if definition.encoding is not None or definition.filters.get("mandatory_prefix") != "ignore":
        return frozenset()
    claims = (_claim(definition, sibling) for sibling in siblings)
    return frozenset(claim for claim in claims if claim is not None)


def _family(definition):
    return definition.encoding, definition.opcode_map, definition.opcode


def _with_sibling_claims(definitions):
    families = defaultdict(list)
    for definition in definitions:
        families[_family(definition)].append(definition)
    return [
        dataclasses.replace(definition, sibling_claims=_sibling_claims(definition, families[_family(definition)]))
        for definition in definitions
    ]


def claimed_by_sibling(definition, prefix, modrm_reg):
    return (prefix, None) in definition.sibling_claims or (prefix, modrm_reg) in definition.sibling_claims


def filter_matches(condition, value):
    return condition[1:] != value if condition.startswith("!") else condition == value


def _takes_operand_size_prefix(definition):
    mandatory_prefix = definition.filters.get("mandatory_prefix")
    return definition.encoding is None and mandatory_prefix not in PREFIXES_WITHOUT_OPERAND_SIZE


def _size_rule(definition, table, name):
    value = getattr(definition, name)
    if value not in table:
        raise ValueError(f"{definition.mnemonic}.{definition.index}: no rule for {name} {value!r}")
    return table[value]


def operand_size_encodings(definition):
    filters = definition.filters
    size_map = _size_rule(definition, OPERAND_SIZE_MAPS, "opsize_map")
    result = []
    for w in (0, 1):
        for prefix in (False, True):
            if prefix and not _takes_operand_size_prefix(definition):
                continue
            if "rex_w" in filters and int(filters["rex_w"]) != w:
                continue
            size_filter_value = OPERAND_SIZE_FILTER_VALUES[(prefix, w)]
            if "operand_size" in filters and not filter_matches(filters["operand_size"], size_filter_value):
                continue
            result.append(OperandSizeEncoding(size_map[(prefix, w)], prefix, w))
    return result


def operand_size_encoding(definition, operand_size):
    choices = operand_size_encodings(definition)
    return next((choice for choice in choices if operand_size is None or choice.size == operand_size), None)


def address_size_prefix(definition, address_size):
    sizes = _size_rule(definition, ADDRESS_SIZE_MAPS, "adsize_map")
    return next((prefix for prefix in (False, True) if sizes[prefix] == address_size), None)


def load(path=DATABASE):
    with open(path) as file:
        rows = json.load(file)
    definitions = (_definition(index, row) for index, row in enumerate(rows))
    return _with_sibling_claims([definition for definition in definitions if _kept(definition)])


def _is_hint_space(definition):
    return definition.encoding is None and definition.opcode_map == "0f" and definition.opcode in HINT_OPCODES


def _pushes_segment_register(definition):
    return definition.mnemonic in ("push", "pop") and any(
        operand.get("register") in SEGMENT_REGISTERS_PUSHED for operand in definition.operands
    )


def _is_segment_table(definition):
    mnemonic = definition.mnemonic
    return (
        mnemonic in SEGMENT_TABLE_MNEMONICS
        or any(operand["operand_type"] == "sreg" for operand in definition.operands)
        or _pushes_segment_register(definition)
        or (mnemonic == "ret" and definition.opcode in FAR_RET_OPCODES)
        or (
            mnemonic in FAR_INDIRECT
            and definition.opcode == 0xFF
            and definition.filters.get("modrm_reg") == FAR_INDIRECT[mnemonic]
        )
    )


def classify(definition):
    if _is_hint_space(definition):
        return Group.SET
    if definition.extension not in PS5_EXTENSIONS or definition.mnemonic == "sysenter":
        return Group.NOT_ON_PS5
    if definition.cpl == 0 or definition.mnemonic in UNMARKED_CPL0_MNEMONICS or definition.mnemonic in IO_MNEMONICS:
        return Group.PRIVILEGED
    if _is_segment_table(definition):
        return Group.SEGMENT_TABLES
    if definition.mnemonic in KERNEL_MNEMONICS:
        return Group.CONSOLE_KERNEL
    return Group.SET


def _depends_on_operand_size(definition):
    return any(
        operand["operand_type"] in OPERAND_TYPE_SIZES
        or len({operand.get("width16"), operand.get("width32"), operand.get("width64")}) > 1
        for operand in definition.operands
    )


def _form_sizes(definition):
    if not _depends_on_operand_size(definition):
        return [None] if operand_size_encoding(definition, None) is not None else []
    sizes = {choice.size for choice in operand_size_encodings(definition)}
    for operand in definition.operands:
        sizes &= OPERAND_TYPE_SIZES.get(operand["operand_type"], sizes)
    return sorted(sizes)


def _decodes_from_own_row(definition, operand_size):
    prefix = "66" if operand_size_encoding(definition, operand_size).operand_size_prefix else "none"
    return not claimed_by_sibling(definition, prefix, None)


def forms(definitions):
    result = []
    for definition in definitions:
        base = f"{definition.mnemonic}.{definition.index}"
        sizes = _form_sizes(definition)
        for size in sizes:
            if _decodes_from_own_row(definition, size):
                result.append(Form(definition, size, f"{base}.o{size}" if len(sizes) > 1 else base))
    return result


def test_group(form):
    group = classify(form.definition)
    if group is Group.SET:
        extension = form.definition.extension
        return extension.lower() if extension in PS5_EXTENSIONS else "hint"
    return "outside-" + group.name.lower().replace("_", "-")
