"""What particular mnemonics need of their cases beyond what `model` gives every form: a `model.Rule` for each, with
registers that hold addresses, inputs of its own, a step that runs after the inputs are set, extra cases built on given
base cases, and the width of its memory access where the database's differs. `rule_for` gives a definition's rule: the
string instructions share one, `_RULES` names the others' by mnemonic, and every other definition has none.

`xrstor` loads MXCSR from its image with SSE or AVX in the standard format, and with SSE alone in the compacted one,
where an image whose XSTATE_BV leaves SSE out sets it to 0x1f80; `loads_mxcsr` says so. Each `xrstor` form adds a case
that loads MXCSR with its misaligned-SSE bit (MM) in the standard format with every component, which Zen 2 accepts and
Intel refuses.

`clzero` points rax into the block: at the start, inside and at the end of a cache line, and into the block's first and
last lines. One more case points it at the guard's first byte, where the line it zeroes faults."""
import struct
from dataclasses import replace

from . import records
from .encoding import immediate_values
from .machine import (
    CONSOLE_MXCSR,
    ENTER_FRAME,
    MEMORY,
    MMX_EXPONENT,
    MXCSR_STATES,
    X87_CONTROLS,
    X87_IMAGE_SIZE,
    X87_LOW_HALF,
    X87_MASKS,
    X87_PENDING,
    abridged_tags,
    x87_control_word,
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
    Rule,
    element_bits,
    keep,
    set_operand,
    signed,
)
from .values import MASK64, Values, choice_values, float_edges, integer_edges, integer_values, random_bits

_RESERVED_MXCSR = 1 << 31
_MISALIGNED_SSE_LOAD = CONSOLE_MXCSR | records.MXCSR_MISALIGNED_SSE
_MXCSR_LOADS = MXCSR_STATES + (CONSOLE_MXCSR | 0x3F, _MISALIGNED_SSE_LOAD, 0x1F80 | _RESERVED_MXCSR)
_MXCSR_INITIAL = 0x1F80

_GATHER_SPACING = 8
_GATHER_LANES = 8
_GATHER_SLOTS = 16
_BIT_OFFSET_REACH = (records.BLOCK_SIZE - MEMORY) * 8
_FAR_BIT_OFFSETS = (8 * 1000 + 3, -8 * 1000 + 5)
_XSAVE_FULL = records.XSAVE_AVX + records.XSAVE_AVX_SIZE
_XSAVE_MXCSR_COMPONENTS = records.XSTATE_SSE | records.XSTATE_AVX
_ALL_LANES = bytes([0xFF]) * 32

_RFLAGS_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7, "DF": 10, "OF": 11}
_RFLAGS_USER = 0x202

_CACHE_LINE = 64
_CLZERO_OFFSETS = (
    MEMORY, MEMORY + 1, MEMORY + _CACHE_LINE - 1, MEMORY + _CACHE_LINE + 5, 0, records.BLOCK_SIZE - _CACHE_LINE,
    records.BLOCK_SIZE - 1,
)
_RAX = records.GPR_NAMES.index("rax")

_GATHERS = frozenset("vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd vpgatherdq vpgatherqd vpgatherqq".split())
_XSAVE_FAMILY = frozenset("xsave xsave64 xsavec xsavec64 xsaveopt xsaveopt64 xrstor xrstor64".split())
XRSTOR = frozenset({"xrstor", "xrstor64"})
_MASKED_STORES = frozenset("maskmovq maskmovdqu vmaskmovdqu".split())
_MASKED_MOVES = frozenset("vmaskmovps vmaskmovpd vpmaskmovd vpmaskmovq".split())
_CPUID_QUERIES = (
    (0x0, None), (0x1, None), (0x2, None), (0x3, None), (0x4, None), (0x7, 0), (0x7, 1), (0x8, None), (0x9, None),
    (0xA, None), (0xB, None), (0xC, None), (0xD, 0), (0xD, 1), (0xD, 2), (0xD, 3), (0x80000000, None),
    (0x80000001, None),
)
_XGETBV_REGISTERS = (0, 1)
_LOOP_COUNTS = (0, 1, 2, 1 << 32)
_REP_COUNTS = (0, 1, 2, 7, 64)
_REP_LIMIT = 64


def _rflags(mask):
    value = _RFLAGS_USER
    for name, bit in FLAG_BITS.items():
        if mask >> bit & 1:
            value |= 1 << _RFLAGS_BITS[name]
    return value


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
    table = case.targets[position] - index if case.special is not None else MEMORY
    case.set_address(IMPLICIT_BASES["bx"], table)
    edges = integer_edges(8)
    start = case.base if case.base is not None else case.random.below(len(edges))
    case.store(table, bytes(edges[(start + number) % len(edges)] for number in range(256)))


def _enter_case(case):
    case.set_address(IMPLICIT_BASES["bp"], ENTER_FRAME)
    level = immediate_values(case.instance.operands)[1] % 32
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
        components = records.XSTATE_COMPONENTS if case.special is not None else value
        case.scratch["components"] = components
        case.set_gpr(0, 0, 32, components)
        case.set_gpr(2, 0, 32, 0)

    kept = [Input("components", choice_values(range(records.XSTATE_COMPONENTS + 1)), apply, True)]
    kept += _without(inputs, {"eax", "edx"})
    if model.mnemonic in XRSTOR:
        kept.append(Input("image", choice_values(range(len(_MXCSR_LOADS))), _xrstor_image, True))
    return kept


def _xsave_case(case):
    case.writes = []
    if case.model.mnemonic not in XRSTOR:
        avx = records.XSAVE_AVX_SIZE if case.scratch["components"] & records.XSTATE_AVX else 0
        case.write(case.targets[case.model.position(0)], records.XSAVE_AVX + avx)


def _float80_slots(number):
    edges = float_edges(80)
    return [edges[(number + slot) % len(edges)] for slot in range(8)]


def _x87_valid(number):
    return 0xFF if number % 2 == 0 else X87_LOW_HALF


def _fxsave_image(number):
    image = bytearray(records.FX_SIZE)
    top = number % 8
    tags = abridged_tags(top, _x87_valid(number))
    struct.pack_into("<HHB", image, records.FX_FCW, x87_control_word(number), top << 11, tags)
    struct.pack_into("<I", image, records.FX_MXCSR, _MXCSR_LOADS[number])
    for slot, value in enumerate(_float80_slots(number)):
        offset = records.fx_st_offset(slot)
        image[offset:offset + 10] = value.to_bytes(10, "little")
    lanes = integer_edges(64)
    for word in range(2 * records.YMM_COUNT):
        struct.pack_into("<Q", image, X87_IMAGE_SIZE + 8 * word, lanes[(number + word) % len(lanes)])
    return bytes(image)


def _fxrstor_inputs(model, inputs):
    def apply(case, value):
        case.store(case.targets[model.position(0)], _fxsave_image(value))
        case.loads_mxcsr = _MXCSR_LOADS[value]

    return inputs + [Input("image", choice_values(range(len(_MXCSR_LOADS))), apply, True)]


def _xrstor_mxcsr(components, stored, compacted, loaded):
    if not compacted:
        return loaded if components & _XSAVE_MXCSR_COMPONENTS else None
    if not components & records.XSTATE_SSE:
        return None
    return loaded if stored & records.XSTATE_SSE else _MXCSR_INITIAL


def _store_xrstor_image(case, number, stored, compacted):
    lanes = integer_edges(64)
    compaction = records.XSAVE_COMPACTED | records.XSTATE_COMPONENTS if compacted else 0
    image = _fxsave_image(number) + struct.pack("<QQ", stored, compaction) + bytes(records.XSAVE_HEADER_SIZE - 16)
    image += b"".join(struct.pack("<Q", lanes[(number + lane) % len(lanes)])
                      for lane in range(records.XSAVE_AVX_SIZE // 8))
    case.store(case.targets[case.model.position(0)], image)
    case.loads_mxcsr = _xrstor_mxcsr(case.scratch["components"], stored, compacted, _MXCSR_LOADS[number])


def _xrstor_image(case, value):
    number = records.XSTATE_COMPONENTS if case.special is not None else value
    stored = records.XSTATE_COMPONENTS if case.special is not None else number % (records.XSTATE_COMPONENTS + 1)
    _store_xrstor_image(case, number, stored, number > records.XSTATE_COMPONENTS)


def _xrstor_misaligned_sse(case):
    case.scratch["components"] = records.XSTATE_COMPONENTS
    case.set_gpr(0, 0, 32, records.XSTATE_COMPONENTS)
    _store_xrstor_image(case, _MXCSR_LOADS.index(_MISALIGNED_SSE_LOAD), records.XSTATE_COMPONENTS, False)


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
    status = top << 11 | X87_PENDING[number % len(X87_PENDING)] & X87_MASKS[number % len(X87_MASKS)]
    control = x87_control_word(number)
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

    return inputs + [Input("environment", choice_values(range(len(X87_CONTROLS))), apply, True)]


def _control_word_inputs(model, inputs):
    values = choice_values(x87_control_word(number) for number in range(len(X87_CONTROLS)))
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


def _clzero_inputs(model, inputs):
    def apply(case, offset):
        case.set_address(_RAX, offset)
        case.write(offset - offset % _CACHE_LINE, _CACHE_LINE)

    values = Values(_CLZERO_OFFSETS, lambda random: random.below(records.BLOCK_SIZE))
    return [Input("line", values, apply, True)] + inputs


def _clzero_in_guard(case):
    case.set_address(_RAX, records.BLOCK_SIZE)
    case.writes = []


def _clzero_extras(model):
    return ((0, _clzero_in_guard),)


def _masked_store_case(case):
    if case.special is not None:
        location = LOCATIONS[case.instance.operands[1].name]
        if location[0] == "mmx":
            case.st[location[1]] = MMX_EXPONENT | MASK64
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
    **{mnemonic: _XSAVE_RULE for mnemonic in _XSAVE_FAMILY - XRSTOR},
    **{mnemonic: replace(_XSAVE_RULE, extras=_xrstor_extras) for mnemonic in XRSTOR},
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
    "clzero": Rule(pins=(_RAX,), inputs=_clzero_inputs, extras=_clzero_extras),
}


def rule_for(definition):
    return _STRING_RULE if definition.category == "STRINGOP" else _RULES.get(definition.mnemonic, _NO_RULE)
