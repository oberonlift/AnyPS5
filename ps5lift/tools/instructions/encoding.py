"""Encode instances of Zydis database definitions as x86-64 bytes, and check each encoding by decoding it with Zydis.

An instance is a form plus one operand choice for each of its definition's operands that has an `encoding`, in order;
implicit and hidden operands follow from the definition. `Reg` names a register by its Zydis name. `Mem` is a memory
operand: `base` may be "rip", `displacement` an int or an assembler expression, and `displacement_size` is in bits (0, 8
or 32, or the address size for an absolute `moffs`). `Imm` is an immediate and `Target` the label a relative branch
reaches.

An encoding is a list of byte strings and `Field`s. A field is `size` bytes that the assembler computes from
`expression`, less the address of the instruction's end when it is relative, so branch targets, RIP-relative
displacements and absolute addresses stay symbolic. A database value without a rule here raises `EncodingError` naming
the form and the value.

`default_operands` picks the first legal register of each class, [rbx] for memory (or the base a `modrm_rm` filter
fixes), 0 for immediates and moffs addresses, and the label "target" for branches. `verify_all` decodes a batch of
(form, operands, prefixes, encoded) instances with the lifter's Zydis settings in one call, and raises `EncodingError`
naming each form whose encoding does not decode to that form with those operands and prefixes."""
from dataclasses import dataclass, field
from typing import List, NamedTuple, Optional, Union

from .database import (
    DECODER_MODES, FEATURE_FILTER, MODRM_FIELD_VALUES, claimed_by_sibling, takes_operand_size_prefix,
)
from .decoder import RejectedEncoding


class EncodingError(Exception):
    pass


@dataclass(frozen=True)
class Reg:
    name: str


@dataclass(frozen=True)
class Mem:
    base: Optional[str]
    index: Optional[str] = None
    scale: int = 1
    displacement: Union[int, str] = 0
    displacement_size: int = 0


@dataclass(frozen=True)
class Imm:
    value: int


@dataclass(frozen=True)
class Target:
    label: str


@dataclass(frozen=True)
class Field:
    size: int
    expression: str
    relative: bool


Encoded = List[Union[bytes, Field]]


def _numbered(prefix, count):
    return [f"{prefix}{number}" for number in range(count)]


_LEGACY_GPRS = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
_REGISTER_FILES = {
    "gpr8": ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"] + [f"r{number}b" for number in range(8, 16)],
    "gpr16": _LEGACY_GPRS + [f"r{number}w" for number in range(8, 16)],
    "gpr32": ["e" + name for name in _LEGACY_GPRS] + [f"r{number}d" for number in range(8, 16)],
    "gpr64": ["r" + name for name in _LEGACY_GPRS] + [f"r{number}" for number in range(8, 16)],
    "xmm": _numbered("xmm", 32),
    "ymm": _numbered("ymm", 32),
    "zmm": _numbered("zmm", 32),
    "mmx": _numbered("mm", 8),
    "mask": _numbered("k", 8),
    "bnd": _numbered("bnd", 4),
    "fpr": _numbered("st", 8),
    "sreg": ["es", "cs", "ss", "ds", "fs", "gs"],
    "cr": _numbered("cr", 16),
    "dr": _numbered("dr", 16),
    "tmm": _numbered("tmm", 8),
}
_HIGH_BYTE_REGISTERS = ["ah", "ch", "dh", "bh"]
_REX_BYTE_REGISTERS = frozenset({"spl", "bpl", "sil", "dil"})
_DISTINCT_KINDS = frozenset({"xmm", "ymm", "zmm", "tmm"})
_DISTINCT_REGISTER_FLAGS = frozenset({"is_gather", "no_source_dest_match", "no_source_source_match"})


@dataclass(frozen=True)
class _Register:
    name: str
    kind: str
    number: int


_REGISTERS = {
    name: _Register(name, kind, number) for kind, names in _REGISTER_FILES.items() for number, name in enumerate(names)
}
_REGISTERS.update({name: _Register(name, "gpr8", 4 + offset) for offset, name in enumerate(_HIGH_BYTE_REGISTERS)})
_CANDIDATES = {kind: [_REGISTERS[name] for name in names] for kind, names in _REGISTER_FILES.items()}
_CANDIDATES["gpr8"] = [
    _REGISTERS[name] for name in _REGISTER_FILES["gpr8"][:4] + _HIGH_BYTE_REGISTERS + _REGISTER_FILES["gpr8"][4:]
]

_FILTERS = {
    "mode": {"64"},
    "modrm_mod": {"3", "!3"},
    "modrm_reg": MODRM_FIELD_VALUES | {"!0"},
    "modrm_rm": MODRM_FIELD_VALUES,
    "mandatory_prefix": {"none", "66", "f3", "f2", "ignore"},
    "rex_w": {"0", "1"},
    "rex_b": {"0", "1"},
    "vector_length": {"128", "256", "512"},
    "evex_b": {"0", "1"},
    "operand_size": {"16", "32", "64", "!16", "!64"},
    "address_size": {"32", "64", "!16"},
    "prefix_group1": {"0", "1"},
    "force_modrm_reg": {True},
    "force_modrm_rm": {True},
    **{FEATURE_FILTER + mode: {"1" if enabled else "0"} for mode, enabled in DECODER_MODES.items()},
}


def _size_map(plain, prefixed, wide, wide_prefixed):
    return {(False, 0): plain, (True, 0): prefixed, (False, 1): wide, (True, 1): wide_prefixed}


_OPERAND_SIZE_MAPS = {
    None: _size_map(32, 16, 64, 64),
    "byteop": _size_map(32, 16, 64, 64),
    "ignore66": _size_map(32, 32, 64, 64),
    "rexw32": _size_map(32, 16, 32, 32),
    "default64": _size_map(64, 16, 64, 64),
    "force64": _size_map(64, 64, 64, 64),
    "force32_or_rexw": _size_map(32, 32, 64, 64),
    "force32_or_64": _size_map(64, 64, 64, 64),
}

_SIZED_REGISTER_TYPES = {
    "gpr16_32_64": {16: "gpr16", 32: "gpr32", 64: "gpr64"},
    "gpr16_32_32": {16: "gpr16", 32: "gpr32", 64: "gpr32"},
    "gpr32_32_64": {16: "gpr32", 32: "gpr32", 64: "gpr64"},
}
_ADDRESS_REGISTER_TYPES = {"gpr_asz": {32: "gpr32", 64: "gpr64"}}
_ADDRESS_KINDS = {"gpr32": 32, "gpr64": 64}
_MEMORY_TYPES = frozenset({"mem", "agen", "agen_norel", "mib"})
_VSIB_INDEX_KINDS = {"mem_vsibx": "xmm", "mem_vsiby": "ymm", "mem_vsibz": "zmm"}
_IMPLICIT_TYPES = frozenset({"implicit_reg", "implicit_mem", "implicit_imm1"})
_REGISTER_TYPES = frozenset(_REGISTER_FILES) | frozenset(_SIZED_REGISTER_TYPES) | frozenset(_ADDRESS_REGISTER_TYPES)
_REGISTER_ENCODINGS = frozenset({"modrm_reg", "modrm_rm", "ndsndd", "mask", "opcode", "is4"})
_IMMEDIATE_ENCODINGS = {
    "uimm8": ({16: 1, 32: 1, 64: 1}, False),
    "uimm16": ({16: 2, 32: 2, 64: 2}, False),
    "uimm32": ({16: 4, 32: 4, 64: 4}, False),
    "simm8": ({16: 1, 32: 1, 64: 1}, True),
    "simm16_32_32": ({16: 2, 32: 4, 64: 4}, True),
    "simm16_32_64": ({16: 2, 32: 4, 64: 8}, True),
}
_BRANCH_ENCODINGS = {
    "jimm8": {16: 1, 32: 1, 64: 1},
    "jimm32": {16: 4, 32: 4, 64: 4},
    "jimm16_32_32": {16: 2, 32: 4, 64: 4},
}
_SIZED_IMPLICIT_PREFIXES = ("osz_", "asz_", "ssz_")
_SCALES = {1: 0, 2: 1, 4: 2, 8: 3}
_MODS = {0: 0, 8: 1, 32: 2}

_LEGACY_MAPS = {None: b"", "0f": b"\x0f", "0f38": b"\x0f\x38", "0f3a": b"\x0f\x3a"}
_THREE_DNOW_MAPS = {"0f0f": b"\x0f\x0f"}
_VEX_MAPS = {"0f": 1, "0f38": 2, "0f3a": 3}
_XOP_MAPS = {"xop8": 8, "xop9": 9, "xopa": 10}
_EVEX_MAPS = {"0f": 1, "0f38": 2, "0f3a": 3, "map5": 5, "map6": 6}
_LEGACY_MANDATORY_PREFIXES = {None: b"", "none": b"", "ignore": b"", "66": b"\x66", "f3": b"\xf3", "f2": b"\xf2"}
_VECTOR_PREFIXES = {None: 0, "none": 0, "66": 1, "f3": 2, "f2": 3}
_VEX_LENGTHS = {None: 0, "128": 0, "256": 1}
_EVEX_LENGTHS = {None: 0, "128": 0, "256": 1, "512": 2}

_PREFIXES = {
    "lock": (0xF0, frozenset({"accepts_lock"})),
    "rep": (0xF3, frozenset({"accepts_rep", "accepts_reperepz"})),
    "repne": (0xF2, frozenset({"accepts_repnerepnz"})),
    "bnd": (0xF2, frozenset({"accepts_bound"})),
}
_GROUP1_MANDATORY_PREFIXES = {"f2": 0xF2, "f3": 0xF3}
_GROUP1_PREFIX_NAMES = {0xF2: "f2", 0xF3: "f3"}
_GROUP1_WORDS = {
    0xF2: (
        ("accepts_repnerepnz", "repne", False),
        ("accepts_xacquire", "xacquire", True),
        ("accepts_bound", "bnd", False),
    ),
    0xF3: (
        ("accepts_rep", "rep", False),
        ("accepts_reperepz", "repe", False),
        ("accepts_xrelease", "xrelease", True),
    ),
}
_PRINTED_PREFIXES = frozenset({"xacquire", "xrelease", "lock", "bnd", "notrack", "rep", "repe", "repne"})
_DEFAULT_TARGET = "target"


@dataclass(frozen=True)
class _Sizes:
    operand: int
    operand_prefix: bool
    w: int
    address: int


@dataclass(frozen=True)
class _Memory:
    mod: int
    rm: int
    sib: Optional[int]
    displacement: list
    base_high: int
    index_high: int
    index_top: int


@dataclass
class _Parts:
    reg: Optional[int] = None
    rm: Optional[int] = None
    memory: Optional[_Memory] = None
    vvvv: Optional[int] = None
    opcode_register: Optional[int] = None
    is4: Optional[int] = None
    is4_low: Optional[int] = None
    aaa: int = 0
    displacement: list = field(default_factory=list)
    immediates: list = field(default_factory=list)
    needs_rex: bool = False
    forbids_rex: bool = False


class _Extensions(NamedTuple):
    r: int
    x: int
    b: int
    r2: int
    v2: int
    vvvv: int


def _rule(form, table, value, what):
    if value not in table:
        raise EncodingError(f"{form.key}: no rule for {what} {value!r}")
    return table[value]


def _matches(condition, value):
    return condition[1:] != value if condition.startswith("!") else condition == value


def _bit(number, position):
    return (number >> position) & 1


def _register(form, name):
    return _rule(form, _REGISTERS, name, "register")


def _category(form, operand):
    operand_type = operand["operand_type"]
    encoding = operand.get("encoding")
    if encoding is None:
        if operand_type in _IMPLICIT_TYPES:
            return "implicit"
    elif operand_type in _REGISTER_TYPES:
        if encoding in _REGISTER_ENCODINGS:
            return "register"
    elif operand_type in _MEMORY_TYPES or operand_type in _VSIB_INDEX_KINDS:
        if encoding == "modrm_rm":
            return "memory"
    elif operand_type == "moffs":
        if encoding == "disp16_32_64":
            return "moffs"
    elif operand_type == "imm":
        if encoding in _IMMEDIATE_ENCODINGS:
            return "immediate"
        if encoding == "is4":
            return "is4"
    elif operand_type == "rel":
        if encoding in _BRANCH_ENCODINGS:
            return "branch"
    raise EncodingError(f"{form.key}: no rule for operand type {operand_type!r} encoded as {encoding!r}")


def _check_definition(form):
    filters = form.definition.filters
    for name, value in filters.items():
        if value not in _rule(form, _FILTERS, name, "filter"):
            raise EncodingError(f"{form.key}: no rule for filter {name} {value!r}")
    for forced, field_name in (("force_modrm_reg", "modrm_reg"), ("force_modrm_rm", "modrm_rm")):
        if forced in filters and filters.get(field_name) not in MODRM_FIELD_VALUES:
            raise EncodingError(f"{form.key}: {forced} needs a fixed {field_name}, not {filters.get(field_name)!r}")
    for operand in form.definition.operands:
        _category(form, operand)


def _explicit_operands(definition):
    return [operand for operand in definition.operands if "encoding" in operand]


def _pairs(form, operands):
    explicit = _explicit_operands(form.definition)
    if len(operands) != len(explicit):
        raise EncodingError(f"{form.key}: takes {len(explicit)} operands, not {len(operands)}")
    return [(operand, _category(form, operand), choice) for operand, choice in zip(explicit, operands)]


def _operand_sizes(form):
    definition = form.definition
    filters = definition.filters
    size_map = _rule(form, _OPERAND_SIZE_MAPS, definition.opsize_map, "opsize_map")
    for w in (0, 1):
        for prefix in (False, True):
            if form.operand_size is not None and size_map[(prefix, w)] != form.operand_size:
                continue
            if prefix and not takes_operand_size_prefix(definition):
                continue
            if "rex_w" in filters and int(filters["rex_w"]) != w:
                continue
            size_index = "64" if w else "16" if prefix else "32"
            if "operand_size" in filters and not _matches(filters["operand_size"], size_index):
                continue
            return size_map[(prefix, w)], prefix, w
    raise EncodingError(f"{form.key}: no operand-size prefix and W give operand size {form.operand_size} "
                        "under its filters")


def _address_size(form, pairs):
    sizes = set()
    required = form.definition.filters.get("address_size")
    if required in ("32", "64"):
        sizes.add(int(required))
    for operand, category, choice in pairs:
        names = []
        if category == "memory" and isinstance(choice, Mem):
            names = [name for name in (choice.base, choice.index) if name is not None]
        elif category == "moffs" and isinstance(choice, Mem):
            sizes.add(choice.displacement_size)
        elif category == "register" and operand["operand_type"] in _ADDRESS_REGISTER_TYPES and isinstance(choice, Reg):
            names = [choice.name]
        for name in names:
            if name == "rip":
                sizes.add(64)
            elif _register(form, name).kind in _ADDRESS_KINDS:
                sizes.add(_ADDRESS_KINDS[_register(form, name).kind])
    if len(sizes) > 1:
        raise EncodingError(f"{form.key}: mixes address sizes {sorted(sizes)}")
    return sizes.pop() if sizes else 64


def _sizes(form, pairs):
    operand, prefix, w = _operand_sizes(form)
    return _Sizes(operand, prefix, w, _address_size(form, pairs))


def _register_kind(form, operand, operand_size, address_size):
    operand_type = operand["operand_type"]
    if operand_type in _SIZED_REGISTER_TYPES:
        return _SIZED_REGISTER_TYPES[operand_type][operand_size]
    if operand_type in _ADDRESS_REGISTER_TYPES:
        return _rule(form, _ADDRESS_REGISTER_TYPES[operand_type], address_size, f"{operand_type} at address size")
    return operand_type


def _expect(form, choice, kind):
    if not isinstance(choice, kind):
        raise EncodingError(f"{form.key}: {choice!r} is not a {kind.__name__} operand")


def _displacement(form, value, size, relative):
    if isinstance(value, str):
        return [Field(size, value, relative)]
    limit = 1 << (8 * size - 1)
    if not -limit <= value < limit:
        raise EncodingError(f"{form.key}: displacement {value} does not fit {size} bytes")
    return [value.to_bytes(size, "little", signed=True)]


def _address_register(form, name, address_size):
    register = _register(form, name)
    if register.kind != f"gpr{address_size}":
        raise EncodingError(f"{form.key}: {name} is not a {address_size}-bit address register")
    return register


def _index(form, operand, choice, address_size):
    vsib_kind = _VSIB_INDEX_KINDS.get(operand["operand_type"])
    if vsib_kind:
        index = _register(form, choice.index) if choice.index is not None else None
        if index is None or index.kind != vsib_kind:
            raise EncodingError(f"{form.key}: a VSIB operand needs a {vsib_kind} index, not {choice.index!r}")
        return index
    if choice.index is None:
        return None
    index = _address_register(form, choice.index, address_size)
    if index.number == 4:
        raise EncodingError(f"{form.key}: {choice.index} cannot be an index")
    return index


def _memory(form, operand, choice, address_size):
    definition = form.definition
    vsib = operand["operand_type"] in _VSIB_INDEX_KINDS
    sib_forced = definition.filters.get("modrm_rm") == "4"
    size = choice.displacement_size
    scale = _rule(form, _SCALES, choice.scale, "scale")
    if choice.index is None and choice.scale != 1:
        raise EncodingError(f"{form.key}: scale {choice.scale} without an index")
    if size == 0 and choice.displacement != 0:
        raise EncodingError(f"{form.key}: displacement {choice.displacement!r} needs a displacement size")
    if size == 8 and definition.encoding == "evex":
        raise EncodingError(f"{form.key}: no rule for EVEX's 8-bit displacement, which the tuple type scales")
    if choice.base == "rip":
        if choice.index is not None or size != 32 or sib_forced or vsib:
            raise EncodingError(f"{form.key}: RIP-relative addressing takes a 32-bit displacement and no index")
        return _Memory(0, 5, None, _displacement(form, choice.displacement, 4, True), 0, 0, 0)
    index = _index(form, operand, choice, address_size)
    index_number = 4 if index is None else index.number
    index_high, index_top = (0, 0) if index is None else (_bit(index_number, 3), _bit(index_number, 4))
    if choice.base is None:
        if size != 32:
            raise EncodingError(f"{form.key}: an address without a base takes a 32-bit displacement")
        sib = scale << 6 | (index_number & 7) << 3 | 5
        return _Memory(0, 4, sib, _displacement(form, choice.displacement, 4, False), 0, index_high, index_top)
    base = _address_register(form, choice.base, address_size)
    mod = _rule(form, _MODS, size, "displacement size")
    if mod == 0 and base.number & 7 == 5:
        raise EncodingError(f"{form.key}: [{base.name}] needs a displacement")
    if index is not None or base.number & 7 == 4 or sib_forced or vsib:
        rm, sib = 4, scale << 6 | (index_number & 7) << 3 | (base.number & 7)
    else:
        rm, sib = base.number & 7, None
    displacement = _displacement(form, choice.displacement, size // 8, False) if size else []
    return _Memory(mod, rm, sib, displacement, _bit(base.number, 3), index_high, index_top)


def _set_slot(form, parts, slot, number):
    if getattr(parts, slot) is not None:
        raise EncodingError(f"{form.key}: two operands encode in {slot}")
    setattr(parts, slot, number)


def _place_register(form, operand, choice, sizes, parts):
    opcode = form.definition.opcode
    _expect(form, choice, Reg)
    register = _register(form, choice.name)
    kind = _register_kind(form, operand, sizes.operand, sizes.address)
    if register.kind != kind:
        raise EncodingError(f"{form.key}: {choice.name} is not a {kind} register")
    parts.needs_rex |= choice.name in _REX_BYTE_REGISTERS
    parts.forbids_rex |= choice.name in _HIGH_BYTE_REGISTERS
    encoding = operand["encoding"]
    if encoding == "modrm_reg":
        _set_slot(form, parts, "reg", register.number)
    elif encoding == "modrm_rm":
        _set_slot(form, parts, "rm", register.number)
    elif encoding == "ndsndd":
        _set_slot(form, parts, "vvvv", register.number)
    elif encoding == "mask":
        parts.aaa = register.number
    elif encoding == "opcode":
        if register.number & 7 != opcode & 7:
            raise EncodingError(f"{form.key}: opcode {opcode:#04x} encodes registers {opcode & 7} modulo 8, "
                                f"not {choice.name}")
        _set_slot(form, parts, "opcode_register", register.number)
    else:
        if register.number >= 16:
            raise EncodingError(f"{form.key}: an is4 operand cannot be {choice.name}")
        _set_slot(form, parts, "is4", register.number)


def _place_immediate(form, operand, choice, sizes, parts):
    _expect(form, choice, Imm)
    encoding = operand["encoding"]
    sizes_by_operand_size, signed = _IMMEDIATE_ENCODINGS[encoding]
    size = sizes_by_operand_size[sizes.operand]
    low, high = (-(1 << (8 * size - 1)), 1 << (8 * size - 1)) if signed else (0, 1 << (8 * size))
    if not low <= choice.value < high:
        raise EncodingError(f"{form.key}: immediate {choice.value} does not fit {encoding} "
                            f"at operand size {sizes.operand}")
    parts.immediates.append(choice.value.to_bytes(size, "little", signed=signed))


def _place(form, operand, category, choice, sizes, parts):
    if category == "register":
        _place_register(form, operand, choice, sizes, parts)
    elif category == "memory":
        _expect(form, choice, Mem)
        if parts.memory is not None or parts.rm is not None:
            raise EncodingError(f"{form.key}: two operands encode in rm")
        parts.memory = _memory(form, operand, choice, sizes.address)
    elif category == "moffs":
        _expect(form, choice, Mem)
        if choice.base is not None or choice.index is not None or choice.displacement_size != sizes.address:
            raise EncodingError(f"{form.key}: a moffs operand is an absolute {sizes.address}-bit address, "
                                f"not {choice!r}")
        parts.displacement = _displacement(form, choice.displacement, sizes.address // 8, False)
    elif category == "immediate":
        _place_immediate(form, operand, choice, sizes, parts)
    elif category == "is4":
        _expect(form, choice, Imm)
        if not 0 <= choice.value < 16:
            raise EncodingError(f"{form.key}: immediate {choice.value} does not fit the low half of the is4 byte")
        _set_slot(form, parts, "is4_low", choice.value)
    elif category == "branch":
        _expect(form, choice, Target)
        parts.immediates.append(Field(_BRANCH_ENCODINGS[operand["encoding"]][sizes.operand], choice.label, True))


def _mandatory_candidate(group1, sizes):
    if group1 is not None:
        return _GROUP1_PREFIX_NAMES[group1]
    return "66" if sizes.operand_prefix else "none"


def _fixed_field(form, name):
    value = form.definition.filters.get(name)
    if value is None:
        return 0
    if value not in MODRM_FIELD_VALUES:
        raise EncodingError(f"{form.key}: no rule for {name} {value!r} without an operand")
    return int(value)


def _reg_field(form, parts):
    return parts.reg if parts.reg is not None else _fixed_field(form, "modrm_reg")


def _modrm(form, parts):
    filters = form.definition.filters
    has_operand = parts.reg is not None or parts.rm is not None or parts.memory is not None
    if not has_operand and not {"modrm_mod", "modrm_reg", "modrm_rm"} & filters.keys():
        return []
    reg = _reg_field(form, parts)
    if parts.memory is not None:
        mod, rm = parts.memory.mod, parts.memory.rm
    elif parts.rm is not None:
        mod, rm = 3, parts.rm
    elif filters.get("modrm_mod") == "3":
        mod, rm = 3, _fixed_field(form, "modrm_rm")
    else:
        raise EncodingError(f"{form.key}: no rule for a ModRM without an r/m operand "
                            f"under modrm_mod {filters.get('modrm_mod')!r}")
    for name, value in (("modrm_mod", mod), ("modrm_reg", reg & 7), ("modrm_rm", rm & 7)):
        if name in filters and not _matches(filters[name], str(value)):
            raise EncodingError(f"{form.key}: {name} {value} contradicts its filter {filters[name]!r}")
    result = [bytes([mod << 6 | (reg & 7) << 3 | (rm & 7)])]
    if parts.memory is not None:
        if parts.memory.sib is not None:
            result.append(bytes([parts.memory.sib]))
        result.extend(parts.memory.displacement)
    return result


def _is4(form, parts):
    if parts.is4 is None:
        if parts.is4_low is not None:
            raise EncodingError(f"{form.key}: an is4 immediate needs an is4 register")
        return []
    if parts.immediates:
        raise EncodingError(f"{form.key}: no rule for an is4 byte beside other immediates")
    low = 0 if parts.is4_low is None else parts.is4_low
    return [bytes([parts.is4 << 4 | low])]


def _requested_prefixes(form, prefixes):
    lock = False
    group1 = []
    for name in prefixes:
        byte, accepting = _rule(form, _PREFIXES, name, "prefix")
        if not accepting & form.definition.prefix_flags:
            raise EncodingError(f"{form.key}: does not accept the {name} prefix")
        if name == "lock":
            if lock:
                raise EncodingError(f"{form.key}: lock is requested twice")
            lock = True
        else:
            group1.append(byte)
    if len(group1) > 1:
        raise EncodingError(f"{form.key}: takes at most one of rep, repne and bnd, not {list(prefixes)}")
    return lock, group1[0] if group1 else None


def _extensions(form, parts):
    rm_sources = [parts.memory, parts.rm, parts.opcode_register]
    if sum(source is not None for source in rm_sources) > 1:
        raise EncodingError(f"{form.key}: two operands extend REX.B")
    reg = 0 if parts.reg is None else parts.reg
    vvvv = 0 if parts.vvvv is None else parts.vvvv
    if parts.memory is not None:
        x, b, index_top = parts.memory.index_high, parts.memory.base_high, parts.memory.index_top
    else:
        rm = next((number for number in (parts.rm, parts.opcode_register) if number is not None), 0)
        x, b, index_top = _bit(rm, 4), _bit(rm, 3), 0
    if index_top and parts.vvvv is not None:
        raise EncodingError(f"{form.key}: EVEX.V' cannot extend both the VSIB index and vvvv")
    extensions = _Extensions(_bit(reg, 3), x, b, _bit(reg, 4), _bit(vvvv, 4) | index_top, vvvv)
    extended = extensions.r2 or extensions.v2 or parts.aaa or (parts.memory is None and x)
    if form.definition.encoding != "evex" and extended:
        raise EncodingError(f"{form.key}: only EVEX encodes registers numbered 16 and up, or a mask")
    return extensions


def _address_override(sizes):
    return b"\x67" if sizes.address == 32 else b""


def _legacy(form, parts, sizes, prefixes, tail):
    definition = form.definition
    filters = definition.filters
    if parts.vvvv is not None:
        raise EncodingError(f"{form.key}: a {definition.encoding or 'legacy'} encoding has no vvvv")
    lock, group1 = _requested_prefixes(form, prefixes)
    mandatory = filters.get("mandatory_prefix")
    if group1 is not None and mandatory in ("66", "f2", "f3"):
        raise EncodingError(f"{form.key}: a requested prefix would replace the mandatory {mandatory} prefix")
    group1_present = group1 is not None or mandatory in _GROUP1_MANDATORY_PREFIXES
    if "prefix_group1" in filters and group1_present != (filters["prefix_group1"] == "1"):
        raise EncodingError(f"{form.key}: prefix_group1 {filters['prefix_group1']!r} "
                            f"contradicts the prefixes {list(prefixes)}")
    candidate, register = _mandatory_candidate(group1, sizes), str(_reg_field(form, parts) & 7)
    if claimed_by_sibling(definition, candidate, register):
        raise EncodingError(f"{form.key}: with mandatory prefix {candidate} and modrm.reg {register}, "
                            "the bytes decode from a more specific row")
    extensions = _extensions(form, parts)
    rex = sizes.w << 3 | extensions.r << 2 | extensions.x << 1 | extensions.b
    if "rex_b" in filters and extensions.b != int(filters["rex_b"]):
        raise EncodingError(f"{form.key}: REX.B {extensions.b} contradicts its filter rex_b {filters['rex_b']!r}")
    prefix_bytes = bytearray()
    if lock:
        prefix_bytes.append(0xF0)
    if group1 is not None:
        prefix_bytes.append(group1)
    prefix_bytes += _address_override(sizes)
    if sizes.operand_prefix:
        prefix_bytes.append(0x66)
    prefix_bytes += _rule(form, _LEGACY_MANDATORY_PREFIXES, mandatory, "mandatory_prefix")
    if rex or parts.needs_rex:
        if parts.forbids_rex:
            raise EncodingError(f"{form.key}: a high byte register cannot be encoded with a REX prefix")
        prefix_bytes.append(0x40 | rex)
    if definition.encoding == "3dnow":
        escape = _rule(form, _THREE_DNOW_MAPS, definition.opcode_map, "3dnow opcode_map")
        return [bytes(prefix_bytes) + escape, *tail, bytes([definition.opcode])]
    escape = _rule(form, _LEGACY_MAPS, definition.opcode_map, "opcode_map")
    return [bytes(prefix_bytes) + escape + bytes([definition.opcode]), *tail]


def _reject_prefixes(form, prefixes):
    if prefixes:
        raise EncodingError(f"{form.key}: a {form.definition.encoding} encoding takes no prefixes, "
                            f"not {list(prefixes)}")


def _vex_like(form, parts, sizes, prefixes, tail, escape, maps):
    definition = form.definition
    filters = definition.filters
    _reject_prefixes(form, prefixes)
    selected_map = _rule(form, maps, definition.opcode_map, f"{definition.encoding} opcode_map")
    pp = _rule(form, _VECTOR_PREFIXES, filters.get("mandatory_prefix"), f"{definition.encoding} mandatory_prefix")
    length = _rule(form, _VEX_LENGTHS, filters.get("vector_length"), f"{definition.encoding} vector_length")
    r, x, b, _, _, vvvv = _extensions(form, parts)
    last = (~vvvv & 0xF) << 3 | length << 2 | pp
    if escape == 0xC4 and selected_map == 1 and sizes.w == 0 and not x and not b:
        prefix = bytes([0xC5, (1 - r) << 7 | last])
    else:
        prefix = bytes([escape, (1 - r) << 7 | (1 - x) << 6 | (1 - b) << 5 | selected_map, sizes.w << 7 | last])
    return [_address_override(sizes) + prefix + bytes([definition.opcode]), *tail]


def _vex(form, parts, sizes, prefixes, tail):
    return _vex_like(form, parts, sizes, prefixes, tail, 0xC4, _VEX_MAPS)


def _xop(form, parts, sizes, prefixes, tail):
    return _vex_like(form, parts, sizes, prefixes, tail, 0x8F, _XOP_MAPS)


def _evex(form, parts, sizes, prefixes, tail):
    definition = form.definition
    filters = definition.filters
    _reject_prefixes(form, prefixes)
    selected_map = _rule(form, _EVEX_MAPS, definition.opcode_map, "evex opcode_map")
    pp = _rule(form, _VECTOR_PREFIXES, filters.get("mandatory_prefix"), "evex mandatory_prefix")
    broadcast = int(filters.get("evex_b", "0"))
    if broadcast and parts.memory is None:
        length = 0
    else:
        length = _rule(form, _EVEX_LENGTHS, filters.get("vector_length"), "evex vector_length")
    r, x, b, r2, v2, vvvv = _extensions(form, parts)
    prefix = bytes([
        0x62,
        (1 - r) << 7 | (1 - x) << 6 | (1 - b) << 5 | (1 - r2) << 4 | selected_map,
        sizes.w << 7 | (~vvvv & 0xF) << 3 | 1 << 2 | pp,
        length << 5 | broadcast << 4 | (1 - v2) << 3 | parts.aaa,
    ])
    return [_address_override(sizes) + prefix + bytes([definition.opcode]), *tail]


_ASSEMBLERS = {None: _legacy, "3dnow": _legacy, "vex": _vex, "xop": _xop, "evex": _evex}


def _joined(pieces):
    result = []
    for piece in pieces:
        if isinstance(piece, bytes) and result and isinstance(result[-1], bytes):
            result[-1] += piece
        elif not isinstance(piece, bytes) or piece:
            result.append(piece)
    return result


def encode(form, operands, prefixes=()):
    _check_definition(form)
    assemble = _rule(form, _ASSEMBLERS, form.definition.encoding, "encoding")
    pairs = _pairs(form, operands)
    sizes = _sizes(form, pairs)
    parts = _Parts()
    for operand, category, choice in pairs:
        _place(form, operand, category, choice, sizes, parts)
    tail = _modrm(form, parts) + parts.displacement + _is4(form, parts) + parts.immediates
    return _joined(assemble(form, parts, sizes, tuple(prefixes), tail))


def placeholder(encoded):
    return b"".join(piece if isinstance(piece, bytes) else bytes(piece.size) for piece in encoded)


def _first_register(form, kind, encoding, used, candidate):
    definition = form.definition
    filters = definition.filters
    for register in _CANDIDATES[kind]:
        if used is not None and kind in _DISTINCT_KINDS and register.number in used:
            continue
        if encoding == "modrm_reg" and claimed_by_sibling(definition, candidate, str(register.number & 7)):
            continue
        if encoding == "opcode" and register.number & 7 != definition.opcode & 7:
            continue
        if encoding == "opcode" and "rex_b" in filters and _bit(register.number, 3) != int(filters["rex_b"]):
            continue
        if encoding in filters and not _matches(filters[encoding], str(register.number & 7)):
            continue
        if encoding == "mask" and definition.mask_mode == "required" and register.number == 0:
            continue
        if used is not None and kind in _DISTINCT_KINDS:
            used.add(register.number)
        return register
    raise EncodingError(f"{form.key}: no {kind} register satisfies its filters")


def _default_memory(form, operand, address_size, used, candidate):
    rm = form.definition.filters.get("modrm_rm")
    number = 3 if rm in (None, "4") else int(rm)
    vsib_kind = _VSIB_INDEX_KINDS.get(operand["operand_type"])
    index = _first_register(form, vsib_kind, None, used, candidate).name if vsib_kind else None
    return Mem(_REGISTER_FILES[f"gpr{address_size}"][number], index, displacement_size=8 if number == 5 else 0)


def default_operands(form):
    definition = form.definition
    _check_definition(form)
    operand_size, operand_prefix, _ = _operand_sizes(form)
    candidate = "66" if operand_prefix else "none"
    address_size = 32 if definition.filters.get("address_size") == "32" else 64
    used = set() if definition.flags & _DISTINCT_REGISTER_FLAGS else None
    choices = []
    for operand in _explicit_operands(definition):
        category = _category(form, operand)
        if category == "register":
            kind = _register_kind(form, operand, operand_size, address_size)
            choices.append(Reg(_first_register(form, kind, operand["encoding"], used, candidate).name))
        elif category == "memory":
            choices.append(_default_memory(form, operand, address_size, used, candidate))
        elif category == "moffs":
            choices.append(Mem(None, displacement_size=address_size))
        elif category in ("immediate", "is4"):
            choices.append(Imm(0))
        else:
            choices.append(Target(_DEFAULT_TARGET))
    return choices


def _implicit_choice(form, operand, sizes):
    operand_type = operand["operand_type"]
    if operand_type == "implicit_imm1":
        return Imm(1)
    if operand_type != "implicit_reg":
        raise EncodingError(f"{form.key}: no rule for a visible {operand_type} operand")
    name = operand["register"]
    for prefix, size in zip(_SIZED_IMPLICIT_PREFIXES, (sizes.operand, sizes.address, 64)):
        if name.startswith(prefix):
            base = name[len(prefix):]
            if base not in _LEGACY_GPRS:
                raise EncodingError(f"{form.key}: no rule for implicit register {name!r}")
            return Reg(_REGISTER_FILES[f"gpr{size}"][_LEGACY_GPRS.index(base)])
    return Reg(name)


def _expected_operands(form, operands):
    sizes = _sizes(form, _pairs(form, operands))
    choices = iter(operands)
    expected = []
    for operand in form.definition.operands:
        if "encoding" in operand:
            expected.append(next(choices))
        elif operand.get("visible", True):
            expected.append(_implicit_choice(form, operand, sizes))
    return expected


def _operand_matches(expected, decoded):
    if isinstance(expected, Reg):
        return decoded.type == "register" and decoded.register == expected.name
    if isinstance(expected, Mem):
        return (
            decoded.type == "memory"
            and decoded.base == (expected.base or "none")
            and decoded.index == (expected.index or "none")
            and decoded.scale == (expected.scale if expected.index else 0)
            and decoded.displacement == (expected.displacement if isinstance(expected.displacement, int) else 0)
            and decoded.displacement_size == expected.displacement_size
        )
    if isinstance(expected, Imm):
        return decoded.type == "immediate" and decoded.immediate == expected.value
    return decoded.type == "immediate" and decoded.immediate == 0


def _described(decoded):
    if decoded.type == "register":
        return decoded.register
    if decoded.type == "memory":
        return (f"[{decoded.base} + {decoded.index}*{decoded.scale} + {decoded.displacement}] "
                f"with displacement size {decoded.displacement_size}")
    if decoded.type == "immediate":
        return f"immediate {decoded.immediate}"
    return decoded.type


def _expected_prefixes(form, prefixes):
    definition = form.definition
    lock, group1 = _requested_prefixes(form, prefixes)
    if group1 is None:
        group1 = _GROUP1_MANDATORY_PREFIXES.get(definition.filters.get("mandatory_prefix"))
    words = {"lock"} if lock else set()
    hle = lock or "accepts_lockless_hle" in definition.prefix_flags
    for flag, word, needs_hle in _GROUP1_WORDS.get(group1, ()):
        if flag in definition.prefix_flags and (hle or not needs_hle):
            words.add(word)
            break
    return words


def _printed_prefixes(text):
    words = set()
    for word in text.split():
        if word not in _PRINTED_PREFIXES:
            break
        words.add(word)
    return words


def _mismatch(form, operands, prefixes, encoding, decoded):
    problems = []
    if decoded.length != len(encoding):
        problems.append(f"length {decoded.length}, not {len(encoding)}")
    if decoded.mnemonic != form.definition.mnemonic:
        problems.append(f"mnemonic {decoded.mnemonic}, not {form.definition.mnemonic}")
    expected = _expected_operands(form, operands)
    visible = [operand for operand in decoded.operands if operand.visibility != "hidden"]
    if len(visible) != len(expected):
        problems.append(f"{len(visible)} visible operands, not {len(expected)}")
    else:
        for position, (want, got) in enumerate(zip(expected, visible)):
            if not _operand_matches(want, got):
                problems.append(f"operand {position} is {_described(got)}, not {want}")
    if form.operand_size is not None and decoded.operand_width != form.operand_size:
        problems.append(f"operand width {decoded.operand_width}, not {form.operand_size}")
    printed, wanted = _printed_prefixes(decoded.text), _expected_prefixes(form, prefixes)
    if printed != wanted:
        problems.append(f"prefixes {sorted(printed)}, not {sorted(wanted)}")
    if not problems:
        return None
    return f"{form.key}: {encoding.hex(' ')} decodes as '{decoded.text}': {'; '.join(problems)}"


def verify_all(decoder, instances):
    instances = list(instances)
    encodings = [placeholder(encoded) for _, _, _, encoded in instances]
    try:
        results = decoder.decode(encodings)
    except RejectedEncoding as error:
        raise EncodingError(f"{instances[error.index][0].key}: {error}") from error
    mismatches = [
        _mismatch(form, operands, prefixes, encoding, decoded)
        for (form, operands, prefixes, _), encoding, decoded in zip(instances, encodings, results)
    ]
    mismatches = [mismatch for mismatch in mismatches if mismatch is not None]
    if mismatches:
        raise EncodingError("\n".join(mismatches))


def verify(decoder, form, operands, prefixes, encoded):
    verify_all(decoder, [(form, operands, prefixes, encoded)])
