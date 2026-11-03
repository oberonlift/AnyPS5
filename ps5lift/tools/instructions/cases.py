"""Choose the instances of the PS5 set's forms and schedule the cases each instance runs.

Instances. A form's primary instance alternates low and high registers through its operands (rax then r15, xmm0 then
xmm15 or ymm15). Its variants put in its operands each special register they allow (rsp, rbp, r12, r13, the high byte
registers, the REX byte registers, the same register as source and destination, and xmm8 to xmm15 or ymm8 to ymm15),
each addressing form of a memory operand (base only, base and index at each scale, an 8-bit and a 32-bit displacement,
RIP-relative, and rsp, rbp, r12 and r13 as base; an indirect jump's rsp base takes a displacement of 8, past the return
address that the code calling the jump leaves at rsp), and the bnd and lock prefixes the form takes. Immediates are
part of the bytes, so a form has one primary instance for each set of immediates its base cases take; an absolute
address is too, so a moffs form's memory cases are instances of their own. `instances` verifies every instance with one
batched decode, and numbers the instances of each test group from 0.

Cases. The primary's base cases number E, the longest edge list among its inputs (`model`). In case i, input k
takes edge (i + k) mod n_k, and lane j of a vector input takes edge (i + k + j) mod n. Four seeded random cases follow.
SSE and AVX floating-point instances, and those that read MXCSR, run their base cases under each of the 13
`MXCSR_STATES`, and an FTZ-only or DAZ-only case names the console-state case with the same inputs as its twin. x87
instances cycle TOP, the tags, precision and rounding, the exception masks and pending exceptions through their base
cases, of which they have at least as many as the longest of those cycles, and add an all-empty, an all-valid and a
pending-unmasked case. A form that accesses memory adds, for each memory operand, a case misaligned by one, one that
ends at the block's end and one that crosses into the guard by one byte; for an operand that must be aligned (XED's
REQUIRES_ALIGNMENT), which cannot cross a page, that last case is an aligned access at the guard's first byte. A return
gets none of these: the code that calls it writes the stack slot it reads first. Each variant runs base case 0 and one
random case. `_RULES` is the table of what particular mnemonics need of their inputs, including extra cases built on
given base cases.

Values are symbolic where they are addresses: a `BlockOffset` is an offset in the memory block, displacements name the
block as `BLOCK`, and a fixup names where the address of `TARGET`, the instance's branch target, goes: a register, which
holds 0 until the guest program's code loads the address (`fixup_register` names it), or a place in the block, which
the case's inputs patch. `x87` is the FXSAVE image up to the XMM registers, with MXCSR in `mxcsr` instead. A case's
`changeable_words` counts the 8-byte words of the result slot and of the block that it can change, which
`projected_recording_bytes` turns into the size of its result in a recording. Every random value derives from `SEED`,
the form's key, the instance's place among its form's instances and the case's place in its schedule.

`expectation_written` names the forms whose expectations are written from AMD's manual instead of recorded on Intel
hardware, because Intel runs their bytes as other instructions: SSE4A, which Intel lacks, and the 16-bit near branches
of AMD's branch mode, which Intel runs as 64-bit ones. Their cases are left out of the recording.

`xrstor` loads MXCSR from its image with SSE or AVX in the standard format, and with SSE alone in the compacted one,
where an image whose XSTATE_BV leaves SSE out sets it to 0x1f80; `loads_mxcsr` says so. Each `xrstor` form adds a case
that loads MXCSR with its misaligned-SSE bit (MM) in the standard format with every component, which Zen 2 accepts and
Intel refuses."""
import re
import struct
from dataclasses import dataclass, replace
from typing import Optional, Union

from . import records
from .database import Form, operand_size_encoding, test_group
from .encoding import (
    HIGH_BYTE_REGISTERS,
    REGISTER_FILES,
    Imm,
    Mem,
    Reg,
    Target,
    encode,
    mandatory_candidate,
    register_candidates,
)
from .model import (
    BIT_TESTS,
    FLAG_BITS,
    FLAG_PATTERNS,
    IMPLICIT_BASES,
    LOCATIONS,
    VSIB_KINDS,
    CaseError,
    Input,
    Model,
    Rule,
    Shape,
    element_bits,
    family,
    keep,
    register_key,
    register_location,
    set_operand,
    signed,
)
from .values import (
    MASK64,
    Random,
    Values,
    choice_values,
    float_edges,
    integer_edges,
    integer_values,
    random_bits,
    random_float80,
)
from .verification import verify_all

SEED = 0x5053354C49465436
BLOCK = "ps5lift_instruction_block"
TARGET = "target"
RETURN_ADDRESS_SIZE = 8

CONSOLE_MXCSR = 0x9FC0
_FTZ = 1 << 15
_DAZ = 1 << 6
_FTZ_ALONE = CONSOLE_MXCSR & ~_DAZ
_DAZ_ALONE = CONSOLE_MXCSR & ~_FTZ
MXCSR_STATES = (
    CONSOLE_MXCSR,
    *(0x1F80 | rounding << 13 for rounding in range(4)),
    _FTZ_ALONE,
    _DAZ_ALONE,
    *(0x1F80 & ~(1 << 7 + exception) for exception in range(6)),
)
_MISALIGNED_SSE = 1 << 17
_RESERVED_MXCSR = 1 << 31
_MISALIGNED_SSE_LOAD = CONSOLE_MXCSR | _MISALIGNED_SSE
_MXCSR_LOADS = MXCSR_STATES + (CONSOLE_MXCSR | 0x3F, _MISALIGNED_SSE_LOAD, 0x1F80 | _RESERVED_MXCSR)
_MXCSR_INITIAL = 0x1F80

_RANDOM_CASES = 4

_MEMORY = 1024
_GATHER_BASE = 1536
_SOURCE = 1536
_DESTINATION = 2560
_STACK = 3072
_LEAVE_FRAME = _STACK + 64
_ENTER_FRAME = _STACK + 256

_GATHER_SPACING = 8
_GATHER_LANES = 8
_GATHER_SLOTS = 16
_BIT_OFFSET_REACH = (records.BLOCK_SIZE - _MEMORY) * 8
_FAR_BIT_OFFSETS = (8 * 1000 + 3, -8 * 1000 + 5)
_XSAVE_LEGACY = 512
_XSAVE_HEADER = 64
_XSAVE_AVX = 256
_XSAVE_FULL = _XSAVE_LEGACY + _XSAVE_HEADER + _XSAVE_AVX
_XSAVE_COMPONENTS = 7
_XSAVE_SSE_COMPONENT = 2
_XSAVE_AVX_COMPONENT = 4
_XSAVE_MXCSR_COMPONENTS = 6
_COMPACTED = 1 << 63

_X87_IMAGE_SIZE = records.FX_XMM
_X87_DEFAULT_CONTROL = 0x037F
_X87_RESERVED_CONTROL = 0x40
_X87_CONTROLS = tuple((precision, rounding) for precision in (0, 2, 3) for rounding in range(4))
_X87_MASKS = (0x3F, *(0x3F & ~(1 << exception) for exception in range(6)))
_X87_PENDING = (0, 0x20, 0x30, 0x01, 0x02)
_X87_SUMMARY = 0x80 | 0x8000
_X87_INVALID = 0x01
_X87_CONDITION_BITS = (8, 9, 10, 14)
_X87_LOW_HALF = 0x0F
_X87_OVERRIDES = ("empty", "full", "pending")
_X87_CYCLE = max(8, len(_X87_CONTROLS), len(_X87_MASKS), len(_X87_PENDING))
_REQUIRES_ALIGNMENT = "REQUIRES_ALIGNMENT"
_MMX_EXPONENT = 0xFFFF << 64
_ALL_LANES = bytes([0xFF]) * 32

_FLAG_DICTS = tuple({name: mask >> bit & 1 for name, bit in FLAG_BITS.items()} for mask in range(1 << len(FLAG_BITS)))
_RFLAGS_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7, "DF": 10, "OF": 11}
_RFLAGS_USER = 0x202
_FLAGS_WORD = records.LOAD_AH & ~7

_RESULT_BYTES = records.RESULT_HEAD.struct().size
_WORD_BYTES = records.WORD.struct().size
_CONSOLE_DEFINED_BYTES = records.EXPECTED_COUNT.struct().size + 4 * 8

_GPR_ORDER = (0, 15, 1, 14, 2, 13, 3, 12, 6, 11, 7, 10, 8, 9, 5, 4)
_GPR8_ORDER = (0, 15, 1, 14, 2, 13, 3, 12, 11, 10, 8, 9, 6, 7, 5, 4)
_ADDRESS_ORDER = _GPR_ORDER[:-2]
_BASE_ORDER = (3, 6, 7, 1, 2, 0, 8, 9, 10, 11, 14, 15)
_INDEX_ORDER = (6, 7, 1, 2, 3, 0, 8, 9, 10, 11, 14, 15)
_VECTOR_ORDER = (0, 15, 1, 14, 2, 13, 3, 12, 4, 11, 5, 10, 6, 9, 7, 8)
_HIGH_VECTOR_ORDER = tuple(range(8, 16))
_SMALL_ORDER = (0, 7, 1, 6, 2, 5, 3, 4)
_BND_ORDER = (0, 3, 1, 2)
_SPECIAL_GPRS = (("rsp", 4), ("rbp", 5), ("r12", 12), ("r13", 13))
_REX_BYTE_REGISTERS = ("spl", "bpl", "sil", "dil")
_INDEX_VALUES = {1: 3, 2: -2, 4: 5, 8: -1}
_DISPLACEMENT8 = -0x40
_DISPLACEMENT32 = -0x7FFFF000
_NO_RIP_TYPES = frozenset({*VSIB_KINDS, "mib", "agen_norel"})
_NO_ACCESS_TYPES = frozenset({"agen", "agen_norel", "mib"})
_DISTINCT_REGISTER_FLAGS = frozenset({"is_gather", "no_source_dest_match", "no_source_source_match"})
_MEMORY_SPECIAL_ROLES = ("misaligned", "block-end", "crossing")

_GATHERS = frozenset("vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd vpgatherdq vpgatherqd vpgatherqq".split())
_XSAVE_FAMILY = frozenset("xsave xsave64 xsavec xsavec64 xsaveopt xsaveopt64 xrstor xrstor64".split())
_XRSTOR = frozenset({"xrstor", "xrstor64"})
_MXCSR_LOADERS = frozenset({"ldmxcsr", "vldmxcsr", "fxrstor", "fxrstor64", *_XRSTOR})
_MASKED_STORES = frozenset("maskmovq maskmovdqu vmaskmovdqu".split())
_MASKED_MOVES = frozenset("vmaskmovps vmaskmovpd vpmaskmovd vpmaskmovq".split())
_CONSOLE_DEFINED = frozenset({"cpuid", "xgetbv"})
_CPUID_QUERIES = (
    (0x0, None), (0x1, None), (0x2, None), (0x3, None), (0x4, None), (0x7, 0), (0x7, 1), (0x8, None), (0x9, None),
    (0xA, None), (0xB, None), (0xC, None), (0xD, 0), (0xD, 1), (0xD, 2), (0xD, 3), (0x80000000, None),
    (0x80000001, None),
)
_XGETBV_REGISTERS = (0, 1)
_LOOP_COUNTS = (0, 1, 2, 1 << 32)
_REP_COUNTS = (0, 1, 2, 7, 64)
_REP_LIMIT = 64
_WRITTEN_GROUPS = frozenset({"sse4a"})
_NEAR_BRANCH_CATEGORIES = frozenset({"COND_BR", "UNCOND_BR", "CALL", "RET"})
_SHORT_BRANCH_SIZE = 16
_BASE, _RANDOM, _MEMORY_CASE, _X87_CASE, _EXTRA_CASE, _VARIANT, _STATE = range(1, 8)


@dataclass(frozen=True)
class BlockOffset:
    offset: int


@dataclass(frozen=True)
class Instance:
    index: int
    form: Form
    operands: tuple
    prefixes: tuple
    shape: Shape
    encoded: tuple
    text: str
    role: str


@dataclass(frozen=True)
class CaseSpec:
    index: int
    instance: int
    gprs: tuple
    vectors: tuple
    flags: dict
    mxcsr: int
    x87: bytes
    patches: tuple
    fixups: tuple
    fill_seed: int
    twin: Optional[int]
    loads_mxcsr: Optional[int]
    console_defined: bool
    changeable_words: int


def _block_expression(offset):
    return f"{BLOCK}+{offset}"


def expression_offset(expression):
    match = re.fullmatch(rf"{BLOCK}\+(\d+)", expression)
    if match is None:
        raise CaseError(f"{expression!r} names no block offset")
    return int(match.group(1))


def expectation_written(form):
    if test_group(form) in _WRITTEN_GROUPS:
        return True
    return form.definition.category in _NEAR_BRANCH_CATEGORIES and form.operand_size == _SHORT_BRANCH_SIZE


def form_model(form):
    definition = form.definition
    rule = _STRING_RULE if definition.category == "STRINGOP" else _RULES.get(definition.mnemonic, _NO_RULE)
    return Model(form, rule)


@dataclass(frozen=True)
class _Address:
    base: Union[None, int, str] = None
    index: bool = False
    scale: Optional[int] = None
    displacement: int = 0
    displacement_size: int = 0
    order: tuple = _ADDRESS_ORDER
    alternate: bool = True


@dataclass(frozen=True)
class _Options:
    low: bool = False
    vectors: tuple = _VECTOR_ORDER
    address: _Address = _Address()
    prefixes: tuple = ()
    alternate: bool = True


def _high_first(order):
    pairs = [order[number:number + 2][::-1] for number in range(0, len(order), 2)]
    return tuple(number for pair in pairs for number in pair)


def _register_order(model, operand, options, high):
    register_family = family(operand.kind)
    if register_family == "gpr":
        order = _GPR8_ORDER if operand.kind == "gpr8" else _GPR_ORDER
    elif register_family == "vector":
        order = options.vectors
    elif register_family == "st":
        order = tuple(number for number in _SMALL_ORDER if not (model.pushes and number == 7))
    elif register_family == "mmx":
        order = _SMALL_ORDER
    elif register_family == "bnd":
        order = _BND_ORDER
    else:
        raise CaseError(f"{model.form.key}: no register order for {operand.kind} operands")
    if options.low:
        order = tuple(number for number in order if number < 8)
    return _high_first(order) if high else order


def _pick_register(model, operand, used, candidate, options, high):
    allowed = set(register_candidates(model.form, operand.spec, operand.kind, candidate))
    names = REGISTER_FILES[operand.kind]
    for number in _register_order(model, operand, options, high):
        name = names[number]
        if name in allowed and register_key(name) not in used:
            used.add(register_key(name))
            return Reg(name)
    return None


def _first_free(model, order, register_family, used, options):
    for number in order:
        if (register_family, number) not in used and (not options.low or number < 8):
            used.add((register_family, number))
            return number
    raise CaseError(f"{model.form.key}: no free {register_family} register for an address")


def _pick_address(model, operand, used, candidate, options, high):
    address = options.address
    vsib = VSIB_KINDS.get(operand.spec["operand_type"])
    filtered = model.definition.filters.get("modrm_rm")
    rm = None if filtered is None else int(filtered)
    if address.base == "rip":
        if operand.spec["operand_type"] in _NO_RIP_TYPES or rm is not None:
            return None
        return Mem("rip", displacement=_block_expression(_MEMORY), displacement_size=32)
    if isinstance(address.base, int):
        base = address.base
        if ("gpr", base) in used and not (base == 4 and model.stack):
            return None
    else:
        order = _high_first(address.order) if high and address.alternate else address.order
        free = [number for number in order if ("gpr", number) not in used and (not options.low or number < 8)]
        free = [number for number in free if rm in (None, 4) or number & 7 == rm]
        if not free:
            raise CaseError(f"{model.form.key}: no free base register for operand {operand.choice}")
        base = free[0]
    if rm not in (None, 4) and base & 7 != rm:
        return None
    used.add(("gpr", base))
    names = REGISTER_FILES[f"gpr{model.address_size}"]
    index = None
    scale = address.scale or 1
    if vsib:
        order = _high_first(options.vectors) if high else options.vectors
        index = REGISTER_FILES[vsib][_first_free(model, order, "vector", used, options)]
        scale = address.scale or element_bits(operand.spec["element_type"]) // 8
    elif address.index:
        index = names[_first_free(model, _INDEX_ORDER, "gpr", used, options)]
    size = address.displacement_size or (8 if base & 7 == 5 else 0)
    return Mem(names[base], index, scale, address.displacement, size)


def _address_registers(memory):
    return [name for name in (memory.base, memory.index) if name not in (None, "rip")]


def _keys_of(choice):
    if isinstance(choice, Reg):
        return {register_key(choice.name)}
    if isinstance(choice, Mem):
        return {register_key(name) for name in _address_registers(choice)}
    return set()


def _assign(model, forced, immediates, options):
    candidate = mandatory_candidate(model.form, options.prefixes)
    used = set(model.reserved)
    for choice in forced.values():
        used |= _keys_of(choice)
    values = iter(immediates)
    choices = []
    high = False
    for operand in model.explicit:
        category = operand.category
        if operand.choice in forced:
            choice = forced[operand.choice]
        elif category == "register":
            choice = _pick_register(model, operand, used, candidate, options, high)
        elif category == "memory":
            choice = _pick_address(model, operand, used, candidate, options, high)
        elif category == "moffs":
            choice = Mem(None, displacement=_block_expression(_MEMORY), displacement_size=model.address_size)
        elif category == "immediate":
            choice = Imm(next(values))
        elif category == "branch":
            choice = Target(TARGET)
        else:
            raise CaseError(f"{model.form.key}: no rule for choosing a {category} operand")
        if choice is None:
            return None
        choices.append(choice)
        if category in ("register", "memory"):
            high = options.alternate and not high
    return tuple(choices)


def _register_operands(model, register_family):
    registers = [operand for operand in model.explicit if operand.category == "register"]
    return [operand for operand in registers if family(operand.kind) == register_family]


def _admits(model, operand, name):
    return name in register_candidates(model.form, operand.spec, operand.kind, mandatory_candidate(model.form))


def _special_register_variants(model, immediates):
    for role, number in _SPECIAL_GPRS:
        stack_pointer = number == 4 and model.stack
        if ("gpr", number) in model.reserved and not stack_pointer:
            continue
        if number == 4 and model.shape in (Shape.CALL, Shape.INDIRECT_JUMP):
            continue
        for operand in _register_operands(model, "gpr"):
            name = REGISTER_FILES[operand.kind][number]
            if operand.kind != "gpr8" and _admits(model, operand, name):
                yield role, _assign(model, {operand.choice: Reg(name)}, immediates, _Options())
                break


def _byte_register_variants(model, immediates):
    operands = [operand for operand in _register_operands(model, "gpr") if operand.kind == "gpr8"]
    if not operands:
        return
    sets = []
    if operand_size_encoding(model.definition, model.form.operand_size).w == 0 and model.definition.encoding is None:
        sets.append(("high-byte", HIGH_BYTE_REGISTERS, _Options(low=True)))
    sets.append(("rex-byte", _REX_BYTE_REGISTERS, _Options()))
    for role, names, options in sets:
        free = [name for name in names if register_key(name) not in model.reserved]
        forced = {}
        for operand in operands:
            name = next((name for name in free if _admits(model, operand, name)), None)
            if name is None:
                break
            free.remove(name)
            forced[operand.choice] = Reg(name)
        else:
            yield role, _assign(model, forced, immediates, options)


def _same_register_variant(model, primary, immediates):
    if model.definition.flags & _DISTINCT_REGISTER_FLAGS:
        return
    for register_family in ("gpr", "vector", "mmx"):
        operands = _register_operands(model, register_family)
        if len(operands) < 2 or not any(op.writes for op in operands) or not any(op.reads for op in operands):
            continue
        number = LOCATIONS[primary[operands[0].choice].name][1]
        forced = {operand.choice: Reg(REGISTER_FILES[operand.kind][number]) for operand in operands}
        if all(_admits(model, operand, forced[operand.choice].name) for operand in operands):
            yield "same-register", _assign(model, forced, immediates, _Options())
        return


def _addressing_variants(model, immediates):
    memories = [operand for operand in model.explicit if operand.category == "memory"]
    if not memories:
        return
    fixed = {"order": _BASE_ORDER, "alternate": False}
    addresses = [] if memories[0].spec["operand_type"] in VSIB_KINDS else [("base", _Address(**fixed))]
    addresses += [(f"index*{scale}", _Address(index=True, scale=scale, **fixed)) for scale in (1, 2, 4, 8)]
    addresses += [
        ("disp8", _Address(displacement=_DISPLACEMENT8, displacement_size=8, **fixed)),
        ("disp32", _Address(displacement=_DISPLACEMENT32, displacement_size=32, **fixed)),
        ("rip", _Address(base="rip")),
    ]
    for role, number in _SPECIAL_GPRS:
        if number == 4 and model.shape is Shape.INDIRECT_JUMP:
            address = _Address(base=number, displacement=RETURN_ADDRESS_SIZE, displacement_size=8)
        else:
            address = _Address(base=number)
        addresses.append((f"base={role}", address))
    for role, address in addresses:
        yield role, _assign(model, {}, immediates, _Options(address=address))


def _variants(model, primary, immediates):
    yield from _special_register_variants(model, immediates)
    yield from _byte_register_variants(model, immediates)
    yield from _same_register_variant(model, primary, immediates)
    vectors = [operand for operand in model.explicit if operand.kind and family(operand.kind) == "vector"]
    if vectors or any(operand.spec["operand_type"] in VSIB_KINDS for operand in model.explicit):
        yield "high-vector", _assign(model, {}, immediates, _Options(vectors=_HIGH_VECTOR_ORDER, alternate=False))
    yield from _addressing_variants(model, immediates)


def _memory_specials(width, aligned):
    misaligned, block_end, crossing = _MEMORY_SPECIAL_ROLES
    specials = [(misaligned, None)] if width > 1 else []
    crossing_offset = records.BLOCK_SIZE if aligned else records.BLOCK_SIZE - width + 1
    return specials + [(block_end, records.BLOCK_SIZE - width), (crossing, crossing_offset)]


def _requires_alignment(model, operand):
    attributes = model.definition.xed_attributes
    if attributes is None:
        raise CaseError(f"{model.form.key}: its row has no XED record to say whether it requires alignment")
    return operand.choice is not None and _REQUIRES_ALIGNMENT in attributes


def _base_case_count(model):
    return max(model.base_cases, _X87_CYCLE) if model.x87 else model.base_cases


def _moffs_plans(model, primary):
    moffs = [operand for operand in model.explicit if operand.category == "moffs"]
    if not moffs or not model.accesses_memory:
        return
    for role, offset in _memory_specials(model.memory_width(moffs[0]), _requires_alignment(model, moffs[0])):
        address = Mem(None, displacement=_block_expression(_MEMORY + 1 if offset is None else offset),
                      displacement_size=model.address_size)
        operands = tuple(address if op.category == "moffs" else choice for op, choice in zip(model.explicit, primary))
        yield role, operands, ()


def _plans(model):
    cases = range(_base_case_count(model))
    family_immediates = list(dict.fromkeys(model.immediates_for(case) for case in cases))
    plans = [("primary", _assign(model, {}, immediates, _Options()), ()) for immediates in family_immediates]
    if any(operands is None for _, operands, _ in plans):
        raise CaseError(f"{model.form.key}: its filters leave the primary instance no free register")
    primary = plans[0][1]
    for role, operands in _variants(model, primary, family_immediates[0]):
        if operands is not None:
            plans.append((role, operands, ()))
    for flag, prefix in (("accepts_bound", "bnd"), ("accepts_lock", "lock")):
        if flag in model.definition.prefix_flags:
            plans.append((prefix, primary, (prefix,)))
    plans.extend(_moffs_plans(model, primary))
    seen = set()
    for role, operands, prefixes in plans:
        if (operands, prefixes) not in seen:
            seen.add((operands, prefixes))
            yield role, operands, prefixes


def instances(forms, decoder):
    planned = []
    for form in forms:
        model = form_model(form)
        for role, operands, prefixes in _plans(model):
            planned.append((form, model.shape, role, operands, prefixes, tuple(encode(form, operands, prefixes))))
    batch = [(form, operands, prefixes, encoded) for form, _, _, operands, prefixes, encoded in planned]
    decoded = verify_all(decoder, batch)
    counts = {}
    result = []
    for (form, shape, role, operands, prefixes, encoded), decoding in zip(planned, decoded):
        group = test_group(form)
        index = counts.get(group, 0)
        counts[group] = index + 1
        result.append(Instance(index, form, operands, prefixes, shape, encoded, decoding.text, role))
    return result


class _Layout:
    def __init__(self, model, instance, ordinal, base_state):
        self.model = model
        self.instance = instance
        self.ordinal = ordinal
        self.locations = {}
        self.widths = {}
        self.accesses = {}
        self.targets = {}
        self.pins = set(model.pins)
        for operand in model.operands:
            self._locate(operand)
        gprs, self.vectors, self.st = base_state
        self.gprs = list(gprs)
        if 4 in self.pins or 4 not in self._data_registers():
            self.gprs[4] = BlockOffset(_STACK)
        self.stack_operands = {
            position for position, location in self.locations.items()
            if model.stack and location is not None and location[:2] == ("gpr", 4)
        }
        self.st_used = self._st_used()
        self.slot_words = frozenset(self._slot_words())

    def _data_registers(self):
        registers = {location[1] for location in self.locations.values() if location and location[0] == "gpr"}
        if self.model.mnemonic == "lea":
            for choice in self.instance.operands:
                if isinstance(choice, Mem):
                    registers |= {register_key(name)[1] for name in _address_registers(choice)}
        return registers

    def _locate(self, operand):
        if operand.choice is not None:
            choice = self.instance.operands[operand.choice]
            if isinstance(choice, Reg):
                self.locations[operand.position] = register_location(self.model.form, choice.name)
            elif isinstance(choice, Mem) and self.model.mnemonic != "lea":
                self._locate_memory(operand, choice)
        elif operand.category == "implicit_reg":
            self.locations[operand.position] = self.model.implicit_location(operand)
        elif operand.category == "implicit_mem":
            self._locate_implicit_memory(operand)

    def _locate_implicit_memory(self, operand):
        position = operand.position
        base = operand.spec["mem_base"]
        width = self.model.memory_width(operand)
        self.locations[position] = ("memory", position)
        self.widths[position] = width
        if base == "sp" and operand.writes:
            self.accesses[position], self.targets[position] = "push", _STACK - width
        elif base == "sp":
            self.accesses[position], self.targets[position] = "pop", _STACK
        elif base == "bx":
            self.accesses[position], self.targets[position] = "xlat", _MEMORY
        else:
            self.accesses[position] = "register"
            self.targets[position] = {"si": _SOURCE, "di": _DESTINATION, "bp": _LEAVE_FRAME}[base]

    def _locate_memory(self, operand, memory):
        model = self.model
        position = operand.position
        operand_type = operand.spec["operand_type"]
        self.locations[position] = ("memory", position)
        accesses = model.accesses_memory and operand_type not in _NO_ACCESS_TYPES
        self.widths[position] = model.memory_width(operand) if accesses else 0
        if memory.base in ("rip", None):
            self.accesses[position], self.targets[position] = "fixed", expression_offset(memory.displacement)
        elif operand_type in VSIB_KINDS:
            self.accesses[position], self.targets[position] = "vsib", _GATHER_BASE
        else:
            self.accesses[position] = "explicit"
            self.targets[position] = _STACK if memory.base == "rsp" and model.stack else _MEMORY
        for name in _address_registers(memory):
            if LOCATIONS[name][0] == "gpr":
                self.pins.add(LOCATIONS[name][1])

    def _st_used(self):
        used = 0
        for operand in self.model.operands:
            location = self.locations.get(operand.position)
            if location and location[0] == "st" and operand.reads:
                used |= 1 << location[1]
        return used

    def _slot_words(self):
        model = self.model
        yield _FLAGS_WORD
        for operand in model.operands:
            location = self.locations.get(operand.position)
            if not operand.writes or location is None:
                continue
            if location[0] == "gpr":
                yield records.gpr_offset(location[1])
            elif location[0] == "vector":
                for word in range(4 if model.vex or location[2] == 32 else 2):
                    yield records.ymm_offset(location[1]) + 8 * word
            elif location[0] == "mmx":
                yield records.fx_st_offset(location[1])
        if model.stack:
            yield records.gpr_offset(4)
        if model.mmx:
            yield records.FX_OFFSET
        if model.x87:
            yield from (records.FX_OFFSET, records.FX_OFFSET + 8, records.FX_OFFSET + 16)
            for number in range(8):
                yield from (records.fx_st_offset(number), records.fx_st_offset(number) + 8)
        if model.floating or model.mnemonic in _MXCSR_LOADERS:
            yield records.FX_OFFSET + records.FX_MXCSR
        if "xmm_state_cw" in model.definition.flags:
            for number in range(records.YMM_COUNT):
                for word in range(4 if model.mnemonic in _XRSTOR else 2):
                    yield records.ymm_offset(number) + 8 * word


class _Case:
    __slots__ = (
        "layout", "model", "instance", "random", "fill_seed", "base", "special", "gprs", "vectors", "st", "flags",
        "mxcsr", "fcw", "fsw", "valid", "top", "patches", "fixups", "writes", "loads_mxcsr", "targets", "values",
        "scratch",
    )

    def __init__(self, layout, random, base, special, mxcsr):
        self.layout = layout
        self.model = layout.model
        self.instance = layout.instance
        self.random = random
        self.fill_seed = random.word()
        self.base = base
        self.special = special
        self.gprs = list(layout.gprs)
        self.vectors = list(layout.vectors)
        self.st = list(layout.st)
        self.flags = 0
        self.mxcsr = mxcsr
        self.fcw = _X87_DEFAULT_CONTROL
        self.fsw = 0
        self.valid = 0
        self.top = 0
        self.patches = []
        self.fixups = []
        self.writes = []
        self.loads_mxcsr = None
        self.targets = dict(layout.targets)
        self.values = {}
        self.scratch = {}

    def set_gpr(self, number, shift, bits, value):
        current = self.gprs[number]
        if isinstance(current, BlockOffset):
            raise CaseError(f"{self.model.form.key}: {records.GPR_NAMES[number]} holds an address, not a value")
        mask = (1 << bits) - 1
        self.gprs[number] = current & ~(mask << shift) | (value & mask) << shift

    def set_address(self, number, offset):
        self.gprs[number] = BlockOffset(offset)

    def set_register(self, name, value):
        location = LOCATIONS[name]
        self.set_gpr(location[1], location[2], location[3], value)

    def set_flags(self, mask):
        self.flags = mask

    def set_operand(self, position, value):
        layout = self.layout
        if position in layout.stack_operands:
            return
        location = layout.locations[position]
        self.values[position] = value
        kind = location[0]
        if kind == "gpr":
            self.set_gpr(location[1], location[2], location[3], value)
        elif kind == "vector":
            self.vectors[location[1]] = value.to_bytes(32, "little")
        elif kind == "mmx":
            self.st[location[1]] = _MMX_EXPONENT | value
        elif kind == "st":
            self.st[location[1]] = value
        elif kind == "memory":
            self.store(self.targets[position], value.to_bytes(layout.widths[position], "little"))
        else:
            raise CaseError(f"{self.model.form.key}: no rule for setting a {kind} operand")

    def value_of(self, position, bits):
        return self.values[position] & (1 << bits) - 1

    def store(self, offset, data):
        start, end = max(offset, 0), min(offset + len(data), records.BLOCK_SIZE)
        if start < end:
            self.patches.append((start, data[start - offset:end - offset]))

    def write(self, offset, length):
        self.writes.append((offset, length))


def _rflags(mask):
    value = _RFLAGS_USER
    for name, bit in FLAG_BITS.items():
        if mask >> bit & 1:
            value |= 1 << _RFLAGS_BITS[name]
    return value


def _ordered_patches(patches):
    image = {}
    for offset, data in patches:
        for position, byte in enumerate(data):
            image[offset + position] = byte
    runs = []
    for position in sorted(image):
        if runs and runs[-1][0] + len(runs[-1][1]) == position:
            runs[-1][1].append(image[position])
        else:
            runs.append((position, bytearray([image[position]])))
    return tuple((offset, bytes(data)) for offset, data in runs)


def _place(case):
    layout = case.layout
    for position, kind in layout.accesses.items():
        target = case.targets[position]
        operand = case.model.operands[position]
        if kind == "explicit":
            memory = case.instance.operands[operand.choice]
            index = _INDEX_VALUES[memory.scale] if memory.index is not None else 0
            if memory.index is not None:
                case.gprs[LOCATIONS[memory.index][1]] = index & MASK64
            case.set_address(LOCATIONS[memory.base][1], target - memory.displacement - index * memory.scale)
        elif kind == "push":
            case.set_address(4, target + layout.widths[position])
        elif kind == "pop":
            case.set_address(4, target)
        elif kind == "register":
            case.set_address(IMPLICIT_BASES[operand.spec["mem_base"]], target)


def _x87_control_word(number):
    precision, rounding = _X87_CONTROLS[number % len(_X87_CONTROLS)]
    return _X87_RESERVED_CONTROL | _X87_MASKS[number % len(_X87_MASKS)] | precision << 8 | rounding << 10


def _x87_state(case, state, override):
    model = case.model
    if model.mmx:
        case.valid = 0xFF if state % 2 else 0
        return
    if not model.x87:
        return
    masks = _X87_MASKS[state % len(_X87_MASKS)]
    pending = _X87_PENDING[state % len(_X87_PENDING)] & masks
    valid = 0xFF if state % 2 == 0 else case.layout.st_used | _X87_LOW_HALF
    if model.pushes:
        valid &= 0x7F
    if override == "empty":
        valid = 0
    elif override == "full":
        valid = 0xFF
    elif override == "pending":
        masks, pending = 0x3F & ~_X87_INVALID, _X87_INVALID | _X87_SUMMARY
    condition = sum((state >> bit & 1) << position for bit, position in enumerate(_X87_CONDITION_BITS))
    case.top = state % 8
    case.valid = valid
    case.fcw = _x87_control_word(state) & ~0x3F | masks
    case.fsw = case.top << 11 | pending | condition


def _abridged_tags(top, valid):
    return sum(1 << (top + number) % 8 for number in range(8) if valid >> number & 1)


def _x87_image(case):
    if not case.model.x87 and not case.model.mmx:
        return DEFAULT_X87_IMAGE
    image = bytearray(_X87_IMAGE_SIZE)
    struct.pack_into("<HHB", image, records.FX_FCW, case.fcw, case.fsw, _abridged_tags(case.top, case.valid))
    for number, value in enumerate(case.st):
        offset = records.fx_st_offset(number)
        image[offset:offset + 10] = value.to_bytes(10, "little")
    return bytes(image)


def _default_x87_image():
    image = bytearray(_X87_IMAGE_SIZE)
    struct.pack_into("<H", image, records.FX_FCW, _X87_DEFAULT_CONTROL)
    return bytes(image)


DEFAULT_X87_IMAGE = _default_x87_image()


def _target_choice(instance):
    if instance.shape in (Shape.CALL, Shape.INDIRECT_JUMP) and instance.operands:
        return instance.operands[0]
    return None


def fixup_register(instance):
    choice = _target_choice(instance)
    return choice.name if isinstance(choice, Reg) else None


def _fixups(case):
    choice = _target_choice(case.instance)
    if isinstance(choice, Reg):
        case.gprs[LOCATIONS[choice.name][1]] = 0
        case.fixups.append((choice.name, TARGET))
    elif isinstance(choice, Mem):
        offset = case.targets[case.model.explicit[0].position]
        if 0 <= offset and offset + 8 <= records.BLOCK_SIZE:
            case.fixups.append((BlockOffset(offset), TARGET))


def _memory_writes(case):
    layout = case.layout
    for operand in case.model.operands:
        if operand.writes and operand.position in layout.accesses and layout.widths[operand.position]:
            case.write(case.targets[operand.position], layout.widths[operand.position])


def _changeable_words(case):
    words = set()
    for offset, length in case.writes:
        start, end = max(offset, 0), min(offset + length, records.BLOCK_SIZE)
        words.update(range(start // 8, (end + 7) // 8))
    return len(case.layout.slot_words) + len(words)


def _build(layout, identifiers, *, base=None, mxcsr=CONSOLE_MXCSR, state=0, x87=None, special=None, extra=None):
    model = layout.model
    case = _Case(layout, Random(SEED, model.key, layout.ordinal, *identifiers), base, special, mxcsr)
    if special is not None:
        position, offset = special
        case.targets[position] = offset
    _place(case)
    _x87_state(case, state, x87)
    for number, inp in enumerate(model.inputs):
        if not inp.immediate:
            inp.apply(case, inp.values.value(base + number) if base is not None else inp.values.random(case.random))
    _memory_writes(case)
    if model.rule.case is not None:
        model.rule.case(case)
    if extra is not None:
        extra(case)
    _fixups(case)
    return case


def _spec(case, index, twin):
    return CaseSpec(
        index=index,
        instance=case.instance.index,
        gprs=tuple(value if isinstance(value, BlockOffset) else value & MASK64 for value in case.gprs),
        vectors=tuple(case.vectors),
        flags=_FLAG_DICTS[case.flags],
        mxcsr=case.mxcsr,
        x87=_x87_image(case),
        patches=_ordered_patches(case.patches),
        fixups=tuple(case.fixups),
        fill_seed=case.fill_seed,
        twin=twin,
        loads_mxcsr=case.loads_mxcsr,
        console_defined=case.model.mnemonic in _CONSOLE_DEFINED,
        changeable_words=_changeable_words(case),
    )


def _base_state(model):
    random = Random(SEED, model.key, _STATE)
    gprs = [random.word() for _ in records.GPR_NAMES]
    vectors = [random.bits(256).to_bytes(32, "little") for _ in range(records.YMM_COUNT)]
    if model.mmx:
        st = [_MMX_EXPONENT | random.word() for _ in range(8)]
    else:
        st = [random_float80(random) for _ in range(8)]
    return gprs, vectors, st


def _immediates_of(instance):
    return tuple(choice.value for choice in instance.operands if isinstance(choice, Imm))


def _form_specs(model, members, first_index):
    base_state = _base_state(model)
    layouts = [_Layout(model, instance, ordinal, base_state) for ordinal, instance in enumerate(members)]
    primaries = [layout for layout in layouts if layout.instance.role == "primary"]
    by_immediates = {_immediates_of(layout.instance): layout for layout in primaries}
    first = by_immediates[model.immediates_for(0)]
    specs = []

    def add(case, twin=None):
        specs.append(_spec(case, first_index + len(specs), twin))
        return specs[-1].index

    count = _base_case_count(model)
    console = {}
    for mxcsr in MXCSR_STATES if model.floating else (CONSOLE_MXCSR,):
        for case in range(count):
            layout = by_immediates[model.immediates_for(case)]
            twin = console[case] if mxcsr in (_FTZ_ALONE, _DAZ_ALONE) else None
            index = add(_build(layout, (_BASE, case), base=case, mxcsr=mxcsr, state=case), twin)
            if mxcsr == CONSOLE_MXCSR:
                console[case] = index
    if model.x87:
        for number, override in enumerate(_X87_OVERRIDES):
            add(_build(first, (_X87_CASE, number), base=0, x87=override))
    for number in range(_RANDOM_CASES):
        add(_build(primaries[number % len(primaries)], (_RANDOM, number), state=count + number))
    if model.accesses_memory and model.shape is not Shape.RETURN:
        for position, kind in first.accesses.items():
            width = first.widths[position]
            if kind == "fixed" or not width:
                continue
            specials = _memory_specials(width, _requires_alignment(model, model.operands[position]))
            for number, (_, offset) in enumerate(specials):
                offset = first.targets[position] + 1 if offset is None else offset
                add(_build(first, (_MEMORY_CASE, position, number), base=0, special=(position, offset)))
    for number, (case, extra) in enumerate(model.rule.extras(model)):
        layout = by_immediates[model.immediates_for(case)]
        add(_build(layout, (_EXTRA_CASE, number), base=case, state=case, extra=extra))
    for layout in layouts:
        if layout.instance.role == "primary":
            continue
        add(_build(layout, (_VARIANT, 0), base=0))
        if layout.instance.role not in _MEMORY_SPECIAL_ROLES:
            add(_build(layout, (_VARIANT, 1), state=count))
    return specs


def case_specs(group_instances):
    groups = {test_group(instance.form) for instance in group_instances}
    if len(groups) > 1:
        raise CaseError(f"case_specs takes one test group's instances, not {sorted(groups)}")
    for position, instance in enumerate(group_instances):
        if instance.index != position:
            raise CaseError(f"instance {instance.index} of {instance.form.key} is at position {position}")
    specs = []
    start = 0
    while start < len(group_instances):
        form = group_instances[start].form
        end = start
        while end < len(group_instances) and group_instances[end].form.key == form.key:
            end += 1
        specs += _form_specs(form_model(form), group_instances[start:end], len(specs))
        start = end
    return specs


def projected_recording_bytes(specs):
    total = 0
    for spec in specs:
        total += _RESULT_BYTES + _WORD_BYTES * spec.changeable_words
        if spec.console_defined:
            total += _CONSOLE_DEFINED_BYTES
    return total


def _without(inputs, names):
    return [inp for inp in inputs if inp.name not in names]


def _replacing(inputs, name, **changes):
    if not any(inp.name == name for inp in inputs):
        raise CaseError(f"no input {name!r} to replace among {[inp.name for inp in inputs]}")
    return [replace(inp, **changes) if inp.name == name else inp for inp in inputs]


def _counter(model):
    return next(
        operand for operand in model.operands
        if operand.category == "implicit_reg" and model.implicit_name(operand) in ("rcx", "ecx")
    )


def _string_inputs(model, inputs):
    if model.rep is None:
        return inputs
    name = model.implicit_name(_counter(model))
    values = Values(_REP_COUNTS, lambda random: random.below(_REP_LIMIT + 1))
    return _replacing(inputs, name, values=values)


def _string_case(case):
    model = case.model
    layout = case.layout
    accesses = {model.operands[position].spec["mem_base"]: position for position in layout.accesses}
    width = layout.widths[next(iter(layout.accesses))]
    bits = 8 * width
    if case.special is not None:
        if model.rep is not None:
            case.set_register("rcx", 1)
        case.flags &= ~(1 << FLAG_BITS["DF"])
    count = case.gprs[1] if model.rep is not None else 1
    step = -width if case.flags >> FLAG_BITS["DF"] & 1 else width
    edges = integer_edges(bits)
    random = case.random
    if case.base is None:
        source = [random.bits(bits) for _ in range(max(count, 1))]
        stop = random.below(count + 1)
    else:
        source = [edges[(case.base + number) % len(edges)] for number in range(max(count, 1))]
        stop = case.base % (count + 1)
    case.writes = []
    if "si" in accesses:
        for number, value in enumerate(source):
            case.store(case.targets[accesses["si"]] + number * step, value.to_bytes(width, "little"))
    if "di" not in accesses:
        return
    position = accesses["di"]
    if model.mnemonic.startswith(("cmps", "scas")):
        accumulator = case.gprs[0] & (1 << bits) - 1
        reference = source if model.mnemonic.startswith("cmps") else [accumulator] * len(source)
        repeats_while_equal = model.rep != "f2"
        for number, value in enumerate(reference):
            differs = (number == stop) == repeats_while_equal
            stored = value ^ number + 1 if differs else value
            case.store(case.targets[position] + number * step, stored.to_bytes(width, "little"))
    if model.operands[position].writes:
        for number in range(count):
            case.write(case.targets[position] + number * step, width)


def _xlat_inputs(model, inputs):
    return [Input("al", integer_values(8), lambda case, value: case.set_register("al", value), True)] + inputs


def _xlat_case(case):
    position = next(iter(case.layout.accesses))
    index = case.gprs[0] & 0xFF
    table = case.targets[position] - index if case.special is not None else _MEMORY
    case.set_address(IMPLICIT_BASES["bx"], table)
    edges = integer_edges(8)
    start = case.base if case.base is not None else case.random.below(len(edges))
    case.store(table, bytes(edges[(start + number) % len(edges)] for number in range(256)))


def _enter_case(case):
    case.set_address(IMPLICIT_BASES["bp"], _ENTER_FRAME)
    level = _immediates_of(case.instance)[1] % 32
    position = next(iter(case.layout.accesses))
    width = case.layout.widths[position]
    pushes = 1 + max(level - 1, 0) + (1 if level else 0)
    case.writes = [(case.targets[position] - width * (pushes - 1), width * pushes)]


def _popf_inputs(model, inputs):
    operand = next(operand for operand in model.operands if operand.category == "implicit_mem")
    mask = (1 << model.operand_bits(operand)) - 1
    values = Values(
        tuple(_rflags(flags) & mask for flags in FLAG_PATTERNS),
        lambda random: _rflags(random.bits(len(FLAG_BITS))) & mask,
    )
    return _replacing(inputs, "[sp]", values=values)


def _counter_inputs(model, inputs):
    name = model.implicit_name(_counter(model))
    bits = LOCATIONS[name][3]
    edges = tuple(dict.fromkeys(value & (1 << bits) - 1 for value in _LOOP_COUNTS))
    return _replacing(inputs, name, values=Values(edges, random_bits(bits)))


def _divisor_bits(model):
    return model.operand_bits(model.explicit[0])


def _division_inputs(model, inputs):
    dividend = {model.implicit_name(operand) for operand in model.operands if operand.category == "implicit_reg"}
    values = integer_values(_divisor_bits(model))
    return _without(inputs, dividend) + [Input(name, values, keep(name), True) for name in ("quotient", "remainder")]


def _set_dividend(case, dividend):
    bits = _divisor_bits(case.model)
    dividend &= (1 << 2 * bits) - 1
    if bits == 8:
        case.set_gpr(0, 0, 16, dividend)
    else:
        case.set_gpr(0, 0, bits, dividend)
        case.set_gpr(2, 0, bits, dividend >> bits)


def _division_case(case):
    model = case.model
    bits = _divisor_bits(model)
    divisor = case.value_of(model.position(0), bits)
    quotient, remainder = case.scratch["quotient"], case.scratch["remainder"]
    if model.mnemonic == "idiv":
        divisor, quotient = signed(divisor, bits), signed(quotient, bits)
        if divisor == 0:
            dividend = quotient
        else:
            product = quotient * divisor
            magnitude = remainder % abs(divisor)
            dividend = product + (magnitude if product >= 0 else -magnitude)
    else:
        dividend = quotient * divisor + remainder % divisor if divisor else quotient
    _set_dividend(case, dividend)


def _division_overflow(case):
    model = case.model
    bits = _divisor_bits(model)
    if model.mnemonic == "idiv":
        case.set_operand(model.position(0), (1 << bits) - 1)
        _set_dividend(case, -(1 << bits - 1))
    else:
        case.set_operand(model.position(0), 1)
        _set_dividend(case, 1 << bits)


def _division_extras(model):
    return ((0, _division_overflow),)


def _exchange_equal(case):
    model = case.model
    bits = model.operand_bits(model.explicit[0])
    value = case.value_of(model.position(0), bits)
    if model.mnemonic == "cmpxchg":
        case.set_gpr(0, 0, bits, value)
        return
    half = bits // 2
    case.set_gpr(0, 0, half, value)
    case.set_gpr(2, 0, half, value >> half)


def _exchange_extras(model):
    return tuple((case, _exchange_equal) for case in range(model.base_cases))


def _cpuid_inputs(model, inputs):
    def apply(case, value):
        leaf, subleaf = _CPUID_QUERIES[value]
        case.set_gpr(0, 0, 32, leaf)
        case.set_gpr(1, 0, 32, case.random.bits(32) if subleaf is None else subleaf)

    query = Input("query", choice_values(range(len(_CPUID_QUERIES))), apply, True)
    return [query] + _without(inputs, {"eax", "ecx"})


def _xgetbv_inputs(model, inputs):
    def apply(case, value):
        case.set_gpr(1, 0, 32, value)

    return [Input("register", choice_values(_XGETBV_REGISTERS), apply, True)] + _without(inputs, {"ecx"})


def _xsave_inputs(model, inputs):
    def apply(case, value):
        components = _XSAVE_COMPONENTS if case.special is not None else value
        case.scratch["components"] = components
        case.set_gpr(0, 0, 32, components)
        case.set_gpr(2, 0, 32, 0)

    kept = [Input("components", choice_values(range(_XSAVE_COMPONENTS + 1)), apply, True)]
    kept += _without(inputs, {"eax", "edx"})
    if model.mnemonic in _XRSTOR:
        kept.append(Input("image", choice_values(range(len(_MXCSR_LOADS))), _xrstor_image, True))
    return kept


def _xsave_case(case):
    case.writes = []
    if case.model.mnemonic not in _XRSTOR:
        avx = _XSAVE_AVX if case.scratch["components"] & _XSAVE_AVX_COMPONENT else 0
        case.write(case.targets[case.model.position(0)], _XSAVE_LEGACY + _XSAVE_HEADER + avx)


def _float80_slots(number):
    edges = float_edges(80)
    return [edges[(number + slot) % len(edges)] for slot in range(8)]


def _x87_valid(number):
    return 0xFF if number % 2 == 0 else _X87_LOW_HALF


def _fxsave_image(number):
    image = bytearray(_XSAVE_LEGACY)
    top = number % 8
    tags = _abridged_tags(top, _x87_valid(number))
    struct.pack_into("<HHB", image, records.FX_FCW, _x87_control_word(number), top << 11, tags)
    struct.pack_into("<I", image, records.FX_MXCSR, _MXCSR_LOADS[number])
    for slot, value in enumerate(_float80_slots(number)):
        offset = records.fx_st_offset(slot)
        image[offset:offset + 10] = value.to_bytes(10, "little")
    lanes = integer_edges(64)
    for word in range(2 * records.YMM_COUNT):
        struct.pack_into("<Q", image, _X87_IMAGE_SIZE + 8 * word, lanes[(number + word) % len(lanes)])
    return bytes(image)


def _fxrstor_inputs(model, inputs):
    def apply(case, value):
        case.store(case.targets[model.position(0)], _fxsave_image(value))
        case.loads_mxcsr = _MXCSR_LOADS[value]

    return inputs + [Input("image", choice_values(range(len(_MXCSR_LOADS))), apply, True)]


def _xrstor_mxcsr(components, stored, compacted, loaded):
    if not compacted:
        return loaded if components & _XSAVE_MXCSR_COMPONENTS else None
    if not components & _XSAVE_SSE_COMPONENT:
        return None
    return loaded if stored & _XSAVE_SSE_COMPONENT else _MXCSR_INITIAL


def _store_xrstor_image(case, number, stored, compacted):
    lanes = integer_edges(64)
    compaction = _COMPACTED | _XSAVE_COMPONENTS if compacted else 0
    image = _fxsave_image(number) + struct.pack("<QQ", stored, compaction) + bytes(_XSAVE_HEADER - 16)
    image += b"".join(struct.pack("<Q", lanes[(number + lane) % len(lanes)]) for lane in range(_XSAVE_AVX // 8))
    case.store(case.targets[case.model.position(0)], image)
    case.loads_mxcsr = _xrstor_mxcsr(case.scratch["components"], stored, compacted, _MXCSR_LOADS[number])


def _xrstor_image(case, value):
    number = _XSAVE_COMPONENTS if case.special is not None else value
    stored = _XSAVE_COMPONENTS if case.special is not None else number % (_XSAVE_COMPONENTS + 1)
    _store_xrstor_image(case, number, stored, number > _XSAVE_COMPONENTS)


def _xrstor_misaligned_sse(case):
    case.scratch["components"] = _XSAVE_COMPONENTS
    case.set_gpr(0, 0, 32, _XSAVE_COMPONENTS)
    _store_xrstor_image(case, _MXCSR_LOADS.index(_MISALIGNED_SSE_LOAD), _XSAVE_COMPONENTS, False)


def _xrstor_extras(model):
    return ((0, _xrstor_misaligned_sse),)


def _tag(value):
    exponent = value >> 64 & 0x7FFF
    significand = value & MASK64
    if exponent == 0 and significand == 0:
        return 1
    if exponent in (0, 0x7FFF) or not significand >> 63:
        return 2
    return 0


def _environment(number, wide):
    top = number % 8
    valid = _x87_valid(number)
    slots = _float80_slots(number)
    tags = 0
    for physical in range(8):
        slot = (physical - top) % 8
        tags |= (_tag(slots[slot]) if valid >> slot & 1 else 3) << 2 * physical
    status = top << 11 | _X87_PENDING[number % len(_X87_PENDING)] & _X87_MASKS[number % len(_X87_MASKS)]
    control = _x87_control_word(number)
    if wide:
        return struct.pack("<HxxHxxHxxIIIxxxx", control, status, tags, 0, 0, 0), slots
    return struct.pack("<HHHHHHH", control, status, tags, 0, 0, 0, 0), slots


def _environment_inputs(model, inputs):
    position = model.position(0)

    def apply(case, value):
        environment, slots = _environment(value, case.layout.widths[position] >= 28)
        if model.mnemonic == "frstor":
            environment += b"".join(slot.to_bytes(10, "little") for slot in slots)
        case.store(case.targets[position], environment)

    return inputs + [Input("environment", choice_values(range(len(_X87_CONTROLS))), apply, True)]


def _control_word_inputs(model, inputs):
    values = choice_values(_x87_control_word(number) for number in range(len(_X87_CONTROLS)))
    return inputs + [Input("control word", values, set_operand(model.position(0)), True)]


def _mxcsr_inputs(model, inputs):
    position = model.position(0)

    def apply(case, value):
        case.set_operand(position, value)
        case.loads_mxcsr = value

    return _replacing(inputs, "operand 0", values=choice_values(_MXCSR_LOADS), apply=apply)


def _bit_offset_in_memory(model):
    return model.explicit[0].category == "memory" and model.explicit[1].category == "register"


def _bit_test_inputs(model, inputs):
    if not _bit_offset_in_memory(model):
        return inputs
    bits = model.operand_size
    offsets = (0, 1, bits - 1, bits, bits + 1, 255, -1, -bits, -bits - 1, *_FAR_BIT_OFFSETS, _BIT_OFFSET_REACH - 1)
    values = Values((offset & (1 << bits) - 1 for offset in offsets), lambda random: random.below(_BIT_OFFSET_REACH))
    return _replacing(inputs, "operand 1", values=values)


def _bit_past_block(case):
    case.set_operand(case.model.position(1), _BIT_OFFSET_REACH)


def _bit_test_extras(model):
    return ((0, _bit_past_block),) if _bit_offset_in_memory(model) else ()


def _vsib_operand(model):
    return next(operand for operand in model.explicit if operand.spec["operand_type"] in VSIB_KINDS)


def _gather_inputs(model, inputs):
    memory = _vsib_operand(model)
    values = model.lane_values(memory, _GATHER_LANES * element_bits(memory.spec["element_type"]))
    return inputs + [Input("gathered", values, keep("gathered"), True)]


def _gather_case(case):
    operand = _vsib_operand(case.model)
    memory = case.instance.operands[operand.choice]
    element_bytes = case.layout.widths[operand.position]
    index_bits = 32 if "gatherd" in case.model.mnemonic else 64
    index = LOCATIONS[memory.index]
    destination = LOCATIONS[case.instance.operands[0].name]
    count = min(destination[2] // element_bytes, index[2] * 8 // index_bits)
    target = case.targets[operand.position]
    if case.special is not None:
        slots = [-number for number in range(count)]
        case.vectors[LOCATIONS[case.instance.operands[2].name][1]] = _ALL_LANES
    else:
        shift = case.base if case.base is not None else case.random.below(_GATHER_SLOTS)
        slots = [(number * 5 + shift) % _GATHER_SLOTS - _GATHER_SLOTS // 2 for number in range(count)]
    case.set_address(LOCATIONS[memory.base][1], target - memory.displacement)
    indexes = bytearray(case.vectors[index[1]])
    element_mask = (1 << 8 * element_bytes) - 1
    for number, slot in enumerate(slots):
        offset = slot * _GATHER_SPACING
        struct.pack_into("<i" if index_bits == 32 else "<q", indexes, number * index_bits // 8, offset // memory.scale)
        if case.base is None:
            value = case.random.bits(8 * element_bytes)
        else:
            value = case.scratch["gathered"] >> 8 * element_bytes * number & element_mask
        case.store(target + offset, value.to_bytes(element_bytes, "little"))
    case.vectors[index[1]] = bytes(indexes)


def _masked_store_case(case):
    if case.special is not None:
        location = LOCATIONS[case.instance.operands[1].name]
        if location[0] == "mmx":
            case.st[location[1]] = _MMX_EXPONENT | MASK64
        else:
            case.vectors[location[1]] = _ALL_LANES


def _masked_move_case(case):
    if case.special is not None:
        mask = next(operand for operand in case.model.explicit if operand.spec.get("encoding") == "ndsndd")
        case.vectors[LOCATIONS[case.instance.operands[mask.choice].name][1]] = _ALL_LANES


_NO_RULE = Rule()
_STRING_RULE = Rule(inputs=_string_inputs, case=_string_case)
_COUNTER_RULE = Rule(inputs=_counter_inputs)
_DIVISION_RULE = Rule(inputs=_division_inputs, case=_division_case, extras=_division_extras)
_XSAVE_RULE = Rule(inputs=_xsave_inputs, case=_xsave_case, access_width=_XSAVE_FULL)
_RULES = {
    "xlat": Rule(pins=(IMPLICIT_BASES["bx"],), inputs=_xlat_inputs, case=_xlat_case),
    "enter": Rule(pins=(IMPLICIT_BASES["bp"],), case=_enter_case),
    "popf": Rule(inputs=_popf_inputs),
    "popfq": Rule(inputs=_popf_inputs),
    **{mnemonic: _COUNTER_RULE for mnemonic in ("loop", "loope", "loopne", "jrcxz", "jecxz")},
    "div": _DIVISION_RULE,
    "idiv": _DIVISION_RULE,
    **{mnemonic: Rule(extras=_exchange_extras) for mnemonic in ("cmpxchg", "cmpxchg8b", "cmpxchg16b")},
    "cpuid": Rule(inputs=_cpuid_inputs),
    "xgetbv": Rule(inputs=_xgetbv_inputs),
    **{mnemonic: _XSAVE_RULE for mnemonic in _XSAVE_FAMILY - _XRSTOR},
    **{mnemonic: replace(_XSAVE_RULE, extras=_xrstor_extras) for mnemonic in _XRSTOR},
    "fxrstor": Rule(inputs=_fxrstor_inputs),
    "fxrstor64": Rule(inputs=_fxrstor_inputs),
    "fldenv": Rule(inputs=_environment_inputs),
    "frstor": Rule(inputs=_environment_inputs),
    "fldcw": Rule(inputs=_control_word_inputs),
    "ldmxcsr": Rule(inputs=_mxcsr_inputs),
    "vldmxcsr": Rule(inputs=_mxcsr_inputs),
    **{mnemonic: Rule(inputs=_bit_test_inputs, extras=_bit_test_extras) for mnemonic in BIT_TESTS},
    **{mnemonic: Rule(inputs=_gather_inputs, case=_gather_case) for mnemonic in _GATHERS},
    **{mnemonic: Rule(case=_masked_store_case) for mnemonic in _MASKED_STORES},
    **{mnemonic: Rule(case=_masked_move_case) for mnemonic in _MASKED_MOVES},
}
