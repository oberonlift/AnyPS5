"""What a form's operands are and the inputs its cases give them.

A `Model` describes one form: its operands with their categories, register kinds and whether they are read or written;
its shape; whether it is an SSE or AVX floating-point form, an x87 form or an MMX form; whether it accesses memory or
the stack; and its `Input`s. An input is one value a case chooses, with the `Values` it is drawn from and how it is set
in a case. Every read operand is an input, and so are the flags (which count toward the number of base cases only when
the form tests them). A `Rule` adds what one mnemonic needs: registers that hold addresses, inputs of its own, a step
that runs after the inputs are set, extra cases, and the width of its memory access where the database's differs."""
import enum
from dataclasses import dataclass
from typing import Optional

from . import records
from .database import operand_size_encoding
from .encoding import (
    HIGH_BYTE_REGISTERS,
    REGISTER_FILES,
    explicit_operands,
    immediate_layout,
    implicit_register,
    operand_category,
    operand_register_kind,
)
from .values import (
    BCD_VALUES,
    Values,
    choice_values,
    count_edges,
    count_values,
    float_values,
    integer_edges,
    integer_limit_edges,
    integer_values,
    narrowing_edges,
    random_bits,
    text_key,
)

FLOAT_BITS = {"float16": 16, "float32": 32, "float64": 64}
INTEGER_LANE_BITS = {
    "int8": 8, "uint8": 8, "int16": 16, "uint16": 16, "int32": 32, "uint32": 32, "int64": 64, "uint64": 64,
    "uint128": 64, "uint256": 64,
}
REGISTER_BITS = {"gpr8": 8, "gpr16": 16, "gpr32": 32, "gpr64": 64}
VSIB_KINDS = {"mem_vsibx": "xmm", "mem_vsiby": "ymm"}
IMPLICIT_BASES = {"sp": 4, "bp": 5, "si": 6, "di": 7, "bx": 3}
FLAG_BITS = {name: bit for bit, name in enumerate(records.FLAG_NAMES)}
FLAG_PATTERNS = tuple(
    sum(1 << FLAG_BITS[name] for name in pattern)
    for pattern in (
        (), records.FLAG_NAMES, ("CF",), ("PF",), ("AF",), ("ZF",), ("SF",), ("OF",), ("SF", "OF"), ("ZF", "SF"),
        ("ZF", "OF"), ("CF", "ZF"), ("DF",), ("DF", "CF", "ZF"),
    )
)

_X87_ELEMENTS = frozenset({"float80", "bcd80"})
_SIZED_INTEGERS = frozenset({None, "int", "uint"})
_VECTOR_BYTES = {"xmm": 16, "ymm": 32}
_MEMORY_TYPES = frozenset({"mem", "mem_vsibx", "mem_vsiby", "moffs", "implicit_mem"})
_NO_ACCESS_CATEGORIES = frozenset({"PREFETCH", "WIDENOP", "NOP", "CLDEMOTE", "PREFETCHWT1", "MPX"})
_X87_STATE_FLAGS = frozenset({"fpu_state_cr", "fpu_state_cw"})
_X87_PUSHES = frozenset("fld fild fbld fld1 fldl2t fldl2e fldpi fldlg2 fldln2 fldz fptan fsincos fxtract".split())
_TESTED_FLAGS = frozenset(name.lower() for name in records.FLAG_NAMES)
_X87_BITS = 80
_FAMILY_BITS = {"vector": 256, "mmx": 64, "st": _X87_BITS}
_BYTE_SHIFT_WIDTH = 16

_BYTE_IMMEDIATES = (0x00, 0x01, 0x1B, 0x4E, 0x55, 0x7F, 0x80, 0xAA, 0xE4, 0xFF)
_STRING_COMPARE_CONTROLS = tuple(
    source | aggregation << 2 | (source ^ aggregation) << 4 | (aggregation & 1) << 6
    for aggregation in range(4) for source in range(4)
)
_ROUNDING_CONTROLS = tuple(range(16))
_IMMEDIATE_EDGES = {
    "roundps": (_ROUNDING_CONTROLS,),
    "roundpd": (_ROUNDING_CONTROLS,),
    "roundss": (_ROUNDING_CONTROLS,),
    "roundsd": (_ROUNDING_CONTROLS,),
    "pclmulqdq": ((0x00, 0x01, 0x10, 0x11),),
    "pcmpestri": (_STRING_COMPARE_CONTROLS,),
    "pcmpestrm": (_STRING_COMPARE_CONTROLS,),
    "pcmpistri": (_STRING_COMPARE_CONTROLS,),
    "pcmpistrm": (_STRING_COMPARE_CONTROLS,),
    "vcvtps2ph": (tuple(range(8)),),
    "sha1rnds4": (tuple(range(4)),),
    "enter": ((0, 1, 8, 0x100, 0x7FFF, 0xFFFF), (0, 1, 2, 31, 32, 255)),
    "ret": ((0, 1, 8, 0x100, 0x7FFE, 0xFFFF),),
}
_COMPARISON_PREDICATES = {"cc3": tuple(range(8)), "cc5": tuple(range(32))}
_SSE_SHIFTS = frozenset(
    f"{prefix}{kind}" for prefix in ("ps", "vps") for kind in "llw lld llq rlw rld rlq raw rad".split()
)
_VARIABLE_SHIFTS = frozenset("vpsllvd vpsllvq vpsrlvd vpsrlvq vpsravd".split())
_SHIFT_ELEMENT_BITS = {"w": 16, "d": 32, "q": 64}
_BYTE_SHIFTS = frozenset("pslldq psrldq vpslldq vpsrldq palignr vpalignr".split())
_REGISTER_COUNTS = frozenset("shlx shrx sarx bzhi".split())
BIT_TESTS = frozenset("bt btc btr bts".split())
_SCALAR_INTEGER_SOURCES = frozenset("cvtsi2ss cvtsi2sd vcvtsi2ss vcvtsi2sd".split())


class CaseError(Exception):
    pass


class Shape(enum.Enum):
    STRAIGHT = "straight"
    BRANCH = "branch"
    CALL = "call"
    RETURN = "return"
    INDIRECT_JUMP = "indirect jump"


def _locations():
    locations = {}
    for kind, bits in REGISTER_BITS.items():
        for number, name in enumerate(REGISTER_FILES[kind]):
            locations[name] = ("gpr", number, 0, bits)
    for number, name in enumerate(HIGH_BYTE_REGISTERS):
        locations[name] = ("gpr", number, 8, 8)
    for kind, size in _VECTOR_BYTES.items():
        for number, name in enumerate(REGISTER_FILES[kind]):
            locations[name] = ("vector", number, size)
    for kind, family in (("mmx", "mmx"), ("fpr", "st"), ("bnd", "bnd")):
        for number, name in enumerate(REGISTER_FILES[kind]):
            locations[name] = (family, number)
    return locations


LOCATIONS = _locations()


def register_key(name):
    location = LOCATIONS.get(name)
    return None if location is None else location[:2]


def family(kind):
    if kind in REGISTER_BITS:
        return "gpr"
    if kind in _VECTOR_BYTES:
        return "vector"
    return "st" if kind == "fpr" else kind


def element_bits(element):
    return FLOAT_BITS.get(element) or INTEGER_LANE_BITS[element]


def signed(value, bits):
    return value - (1 << bits) if value >> bits - 1 & 1 else value


def set_operand(position):
    return lambda case, value: case.set_operand(position, value)


def keep(name):
    return lambda case, value: case.scratch.__setitem__(name, value)


@dataclass(frozen=True)
class Operand:
    position: int
    spec: dict
    choice: Optional[int]
    category: str
    kind: Optional[str]
    reads: bool
    writes: bool


@dataclass(frozen=True)
class Input:
    name: str
    values: Values
    apply: object
    counts: bool
    immediate: bool = False


def _no_extras(model):
    return ()


@dataclass(frozen=True)
class Rule:
    pins: tuple = ()
    inputs: object = None
    case: object = None
    extras: object = _no_extras
    access_width: Optional[int] = None


class Model:
    def __init__(self, form, rule):
        definition = form.definition
        self.form = form
        self.definition = definition
        self.mnemonic = definition.mnemonic
        self.rule = rule
        self.key = text_key(form.key)
        self.operand_size = operand_size_encoding(definition, form.operand_size).size
        self.address_size = 32 if definition.filters.get("address_size") == "32" else 64
        self.shape = _shape(definition)
        self.operands = tuple(self._operands())
        self.explicit = tuple(operand for operand in self.operands if operand.choice is not None)
        types = {operand.spec["operand_type"] for operand in self.operands}
        elements = {operand.spec.get("element_type") for operand in self.operands}
        self.mmx = definition.extension == "MMX" or "mmx" in types
        self.x87 = not self.mmx and (
            definition.extension == "X87" or bool(elements & _X87_ELEMENTS) or bool(definition.flags & _X87_STATE_FLAGS)
        )
        self.floating = not self.x87 and bool(elements & set(FLOAT_BITS))
        self.vex = definition.encoding == "vex"
        self.accesses_memory = definition.category not in _NO_ACCESS_CATEGORIES and bool(types & _MEMORY_TYPES)
        self.stack = any(operand.spec.get("mem_base") == "sp" for operand in self.operands) or any(
            self._implicit_gpr(operand) == 4 for operand in self.operands
        )
        self.pushes = self.mnemonic in _X87_PUSHES
        mandatory = definition.filters.get("mandatory_prefix")
        self.rep = mandatory if definition.category == "STRINGOP" and mandatory in ("f2", "f3") else None
        self.pins = frozenset(self._pins())
        self.reserved = frozenset(self._reserved())
        self.supplements = self._supplements()
        inputs = [inp for operand in self.operands for inp in _operand_inputs(self, operand)]
        inputs.append(_flags_input(self))
        if rule.inputs is not None:
            inputs = rule.inputs(self, inputs)
        self.inputs = tuple(inputs)
        self.base_cases = max((len(inp.values) for inp in self.inputs if inp.counts), default=1)
        self.immediates = tuple((number, inp.values) for number, inp in enumerate(self.inputs) if inp.immediate)

    def _operands(self):
        explicit = iter(range(len(explicit_operands(self.definition))))
        for position, spec in enumerate(self.definition.operands):
            action = spec.get("action") or "read"
            choice = next(explicit) if "encoding" in spec else None
            category = operand_category(self.form, spec) if choice is not None else spec["operand_type"]
            kind = None
            if category == "register":
                kind = operand_register_kind(self.form, spec, self.operand_size, self.address_size)
            yield Operand(position, spec, choice, category, kind, "read" in action, "write" in action)

    def implicit_name(self, operand):
        return implicit_register(self.form, operand.spec["register"], self.operand_size, self.address_size)

    def _implicit_gpr(self, operand):
        if operand.category != "implicit_reg":
            return None
        location = LOCATIONS.get(self.implicit_name(operand))
        return location[1] if location is not None and location[0] == "gpr" else None

    def _pins(self):
        if self.stack:
            yield 4
        for operand in self.operands:
            if operand.category == "implicit_mem":
                yield IMPLICIT_BASES[operand.spec["mem_base"]]
        yield from self.rule.pins

    def _reserved(self):
        for number in self.pins:
            yield ("gpr", number)
        for operand in self.operands:
            if operand.category == "implicit_reg":
                key = register_key(self.implicit_name(operand))
                if key is not None:
                    yield key

    def _supplements(self):
        if "cvt" not in self.mnemonic and not self.mnemonic.startswith(("fist", "fst")):
            return {}
        read_floats = {_float_bits(operand) for operand in self.operands if operand.reads} - {None}
        supplements = {bits: () for bits in sorted(read_floats)}
        for operand in self.operands:
            if not operand.writes:
                continue
            written_float = _float_bits(operand)
            written_integer = None if written_float else _integer_bits(self, operand)
            for bits in supplements:
                if written_float is not None and written_float < bits:
                    supplements[bits] += narrowing_edges(bits, written_float)
                elif written_integer is not None:
                    supplements[bits] += integer_limit_edges(bits, written_integer)
        return {bits: tuple(dict.fromkeys(values)) for bits, values in supplements.items()}

    def position(self, choice):
        return self.explicit[choice].position

    def operand_bits(self, operand):
        if operand.kind in REGISTER_BITS:
            return REGISTER_BITS[operand.kind]
        return 8 * operand.spec[f"width{self.operand_size}"]

    def memory_width(self, operand):
        spec = operand.spec
        if operand.category == "implicit_mem" and spec["mem_base"] == "bx":
            return 1
        if spec["operand_type"] in VSIB_KINDS:
            return element_bits(spec["element_type"]) // 8
        if operand.choice is not None and self.rule.access_width is not None:
            return self.rule.access_width
        return spec[f"width{self.operand_size}"]

    def immediates_for(self, case):
        return tuple(values.edges[(case + number) % len(values)] for number, values in self.immediates)

    def lane_values(self, operand, total_bits, scalar=False):
        element = operand.spec.get("element_type")
        count = _count_width(self, operand)
        float_bits = _float_bits(operand)
        if float_bits is not None and not scalar:
            return float_values(float_bits, total_bits // float_bits, self.supplements.get(float_bits, ()))
        if element == "bcd80":
            return BCD_VALUES
        if scalar or element in _SIZED_INTEGERS or self.mnemonic in _SCALAR_INTEGER_SOURCES:
            bits = total_bits
        elif element in INTEGER_LANE_BITS:
            bits = min(INTEGER_LANE_BITS[element], total_bits)
        else:
            raise CaseError(f"{self.form.key}: no rule for the values of a {element} operand")
        if count is not None:
            return count_values(count, bits, total_bits // bits)
        return integer_values(bits, total_bits // bits)


def _float_bits(operand):
    element = operand.spec.get("element_type")
    return _X87_BITS if element == "float80" else FLOAT_BITS.get(element)


def _integer_bits(model, operand):
    element = operand.spec.get("element_type")
    if operand.kind in REGISTER_BITS:
        return REGISTER_BITS[operand.kind]
    if element in INTEGER_LANE_BITS:
        return INTEGER_LANE_BITS[element]
    if operand.spec["operand_type"] == "mem" and element in _SIZED_INTEGERS:
        return model.operand_bits(operand)
    return None


def _shape(definition):
    category = definition.category
    if category == "COND_BR":
        return Shape.BRANCH
    if category == "UNCOND_BR":
        relative = any(operand["operand_type"] == "rel" for operand in definition.operands)
        return Shape.BRANCH if relative else Shape.INDIRECT_JUMP
    if category == "CALL":
        return Shape.CALL
    if category == "RET":
        return Shape.RETURN
    return Shape.STRAIGHT


def _count_width(model, operand):
    mnemonic = model.mnemonic
    last = model.explicit[-1].position if model.explicit else None
    if model.definition.category in ("SHIFT", "ROTATE"):
        cl = operand.category == "implicit_reg" and model.implicit_name(operand) == "cl"
        return model.operand_bits(model.explicit[0]) if operand.category == "immediate" or cl else None
    if mnemonic in _REGISTER_COUNTS:
        return model.operand_size if operand.choice == 2 else None
    if mnemonic == "rorx":
        return model.operand_size if operand.category == "immediate" else None
    if mnemonic in _SSE_SHIFTS or mnemonic in _VARIABLE_SHIFTS:
        return _SHIFT_ELEMENT_BITS[mnemonic[-1]] if operand.position == last else None
    if mnemonic in _BYTE_SHIFTS:
        return _BYTE_SHIFT_WIDTH if operand.category == "immediate" else None
    if mnemonic in BIT_TESTS and operand.choice == 1:
        return model.operand_size
    return None


def _immediate_values(model, operand, number):
    size, is_signed = immediate_layout(operand.spec, model.operand_size)
    bits = 8 * size
    mnemonic = model.mnemonic
    table = _IMMEDIATE_EDGES.get(mnemonic, _IMMEDIATE_EDGES.get(mnemonic[1:]) if mnemonic.startswith("v") else None)
    count = _count_width(model, operand)
    if operand.spec.get("element_type") in _COMPARISON_PREDICATES:
        edges = _COMPARISON_PREDICATES[operand.spec["element_type"]]
    elif table is not None:
        edges = table[number]
    elif count is not None:
        edges = count_edges(count)
    elif is_signed:
        edges = tuple(signed(value, bits) for value in integer_edges(bits))
    elif bits == 8:
        edges = _BYTE_IMMEDIATES
    else:
        edges = integer_edges(bits)
    return choice_values(edges)


def _register_values(model, operand, register_family, gpr_bits):
    if register_family == "gpr":
        return model.lane_values(operand, gpr_bits, scalar=True)
    bits = _FAMILY_BITS.get(register_family)
    return None if bits is None else model.lane_values(operand, bits)


def _set_address_register(choice, part):
    def apply(case, value):
        memory = case.instance.operands[choice]
        name = memory.base if part == "base" else memory.index
        if name not in (None, "rip"):
            case.set_register(name, value)

    return apply


def _operand_inputs(model, operand):
    category = operand.category
    spec = operand.spec
    if category == "immediate":
        immediates = [other.position for other in model.explicit if other.category == "immediate"]
        number = immediates.index(operand.position)
        return [Input(f"immediate {operand.choice}", _immediate_values(model, operand, number), None, True, True)]
    if not operand.reads or category in ("branch", "implicit_imm1"):
        return []
    if model.shape in (Shape.CALL, Shape.INDIRECT_JUMP) and operand.choice is not None:
        return []
    if category == "register":
        values = _register_values(model, operand, family(operand.kind), REGISTER_BITS.get(operand.kind))
        if values is None:
            return []
        return [Input(f"operand {operand.choice}", values, set_operand(operand.position), True)]
    if category == "memory":
        if model.mnemonic == "lea":
            values = integer_values(model.address_size)
            return [Input(f"operand {operand.choice} {part}", values, _set_address_register(operand.choice, part), True)
                    for part in ("base", "index")]
        if not model.accesses_memory or spec["operand_type"] in VSIB_KINDS or spec.get("element_type") == "struct":
            return []
        values = model.lane_values(operand, model.operand_bits(operand))
        return [Input(f"operand {operand.choice}", values, set_operand(operand.position), True)]
    if category == "moffs":
        values = integer_values(model.operand_bits(operand))
        return [Input(f"operand {operand.choice}", values, set_operand(operand.position), True)]
    if category == "implicit_reg":
        name = model.implicit_name(operand)
        location = LOCATIONS.get(name)
        if location is None or (location[0] == "gpr" and location[1] in model.pins):
            return []
        gpr_bits = location[3] if location[0] == "gpr" else None
        values = _register_values(model, operand, location[0], gpr_bits)
        return [] if values is None else [Input(name, values, set_operand(operand.position), True)]
    if category == "implicit_mem":
        if spec["mem_base"] not in ("sp", "bp") or model.shape is Shape.RETURN:
            return []
        values = integer_values(8 * spec[f"width{model.operand_size}"])
        return [Input(f"[{spec['mem_base']}]", values, set_operand(operand.position), True)]
    raise CaseError(f"{model.form.key}: no rule for the values of a {category} operand")


def _flags_input(model):
    tested = bool(model.definition.tested_flags & _TESTED_FLAGS)
    values = Values(FLAG_PATTERNS, random_bits(len(FLAG_BITS)))
    return Input("flags", values, lambda case, value: case.set_flags(value), tested)
