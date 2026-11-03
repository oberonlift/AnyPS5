"""Judge a case's lifted result against its expectation, by the rules of the instruction-tests design.

`compare` takes a `state.Case`, its instance's `Rules`, the expected and the actual `state.State`, the floating-point
mode and the `Run` the case belongs to, and lists every `Mismatch`.

- **Faults and stops** compare by outcome, address and detail, and a page fault by its data address. A case outside
  the set expects a stop and nothing else, so its rules (`rules_for`) name only its mnemonic. For the FXSAVE and XSAVE
  family, whose processors report different bytes of a multi-byte state save, the data address only has to lie in the
  guard and in the instruction's access.
- **Completed cases** compare every slot field and every block word, except the storer's FOP, FIP, FDP, MXCSR_MASK,
  reserved bytes and XMM area (the YMM slots hold the vector state), flags the database marks undefined (x87 condition
  codes included), the status flags of calls, returns and indirect jumps, which lifted code drops (DF compares), and
  the bits AMD's manual leaves undefined (`expectations.undefined_bits`).
- **Canonical NaNs:** in the destination of `dpps` and `dppd` and their VEX forms, where two or more of the products
  the immediate selects in a 128-bit half are a NaN or an infinity (`tolerance.special_product`), a NaN lane of that
  half matches any NaN, as the `sse41` fixture canonicalizes them: processors add the products in different orders,
  which decides the NaN.
- **Tolerances** (`tolerance`): `rcp*` and `rsqrt*` lanes within the documented relative error; the results an x87
  transcendental approximates within one ulp of the 64-bit format of the true result, whatever the precision control,
  where the operands lie in the documented range, with C1, the rounding indicator, not compared there.
- **Properties:** `rdrand` and `rdseed` set CF, leave the other flags and the destination's untouched bits as
  recorded, and write: in a run, at most one case of a form may leave the destination's written bits as they were
  (`form_mismatches`); the counters of `rdtsc` and `rdtscp` do not decrease from case to case in a run and leave the
  high halves 0; `rdpid` and `rdtscp`'s ecx are below the host's processor count; `cpuid` and `xgetbv` give the
  console's values, which the lifted driver appends to its results.
- **Fast mode** allows, in the destination lanes an instruction computes (those of instructions whose exception class
  is SDM type 2 or 3, compares excepted, as their destination is a mask; lane 0 of a scalar form), a NaN to be any NaN;
  and under FTZ or DAZ, which fast mode treats as one switch, a `min*` or `max*` lane that x86 makes a signed zero to
  be a denormal of that sign that equals the lane of one of its operands, and a lane that x86 rounds to the smallest
  normal to be a zero of that sign. It does not compare MXCSR's exception flags, wherever MXCSR is stored. It compares
  an FTZ-only or DAZ-only case with its twin's expectation (`reference_case`), with MXCSR's control bits as the case
  set them. It expects NEEDS_EXACT_FLOAT at the loader's fxrstor for a case whose initial MXCSR unmasks an exception,
  and at the instance for one that loads such an MXCSR, where the recording shows the load completing.

`format_mismatch` gives the instance as Zydis formats it, the case's inputs, and each differing field's values."""
import collections
import enum
import functools
import struct
from dataclasses import dataclass
from typing import Optional

from . import records, tolerance
from .cases import form_model
from .database import in_set
from .encoding import Mem, Reg, immediate_values
from .expectations import undefined_bits
from .model import COMPARISON_PREDICATES, FLOAT_BITS, IMPLICIT_BASES, LOCATIONS, Shape
from .records import Outcome
from .state import State, apply_words, initial_block, operand_address

EXACT = "exact"
FAST = "fast"
FLOAT_MODES = (EXACT, FAST)


class Tolerance(enum.Enum):
    NONE = "none"
    RCP = "rcp"
    X87_TRANSCENDENTAL = "x87_transcendental"
    RANDOM_VALUE = "random_value"
    COUNTER = "counter"
    PROCESSOR_ID = "processor_id"


class Verdict(enum.Enum):
    PASS = "pass"
    MISMATCHED = "mismatched"
    MISSING = "missing"
    STOPPED = "stopped"
    CRASHED = "crashed"
    TIMED_OUT = "timed out"


_TOLERANCES = {
    **{mnemonic: Tolerance.RCP for mnemonic in "rcpps rcpss rsqrtps rsqrtss vrcpps vrcpss vrsqrtps vrsqrtss".split()},
    **{mnemonic: Tolerance.X87_TRANSCENDENTAL for mnemonic in tolerance.OPERAND_COUNTS},
    "rdrand": Tolerance.RANDOM_VALUE,
    "rdseed": Tolerance.RANDOM_VALUE,
    "rdtsc": Tolerance.COUNTER,
    "rdtscp": Tolerance.COUNTER,
    "rdpid": Tolerance.PROCESSOR_ID,
}
_COMPUTING_EXCEPTION_CLASSES = frozenset({"sse2", "sse3", "avx2", "avx3"})
_COUNTER_HALVES = ("rax", "rdx")
_PROCESSOR_REGISTER = "rcx"
_CONSOLE_REGISTERS = {"cpuid": ("rax", "rbx", "rcx", "rdx"), "xgetbv": ("rax", "rdx")}
_CONSOLE_VALUE_COUNTS = {"cpuid": 4, "xgetbv": 1}
_STATE_IMAGE_ACCESSES = frozenset("fxsave fxsave64 fxrstor fxrstor64 xsave xsave64 xsavec xsavec64 xsaveopt "
                                  "xsaveopt64 xrstor xrstor64".split())
_MXCSR_STORES = {
    "stmxcsr": 0, "vstmxcsr": 0,
    **{mnemonic: records.FX_MXCSR for mnemonic in "fxsave fxsave64 xsave xsave64 xsavec xsavec64 xsaveopt "
       "xsaveopt64".split()},
}
_X87_PUSHES = frozenset({"fsincos", "fptan"})
_STATUS_FLAGS = ("CF", "PF", "AF", "ZF", "SF", "OF")
_RANDOM_CARRY = 1
_UNWRITTEN_ALLOWED = 1
_DROPPING_SHAPES = frozenset({Shape.CALL, Shape.RETURN, Shape.INDIRECT_JUMP})
_CONDITION_BITS = {"C0": 8, "C1": 9, "C2": 10, "C3": 14}
_AH_FLAG_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7}
_AX_FLAG_MASK = sum(1 << 8 + bit for bit in _AH_FLAG_BITS.values()) | 1
_DF_MASK = 1
_MXCSR_FLAGS = 0x3F
_MXCSR_MASKS = 0x1F80
_MXCSR_CONTROL = 0xFFC0 | records.MXCSR_MISALIGNED_SSE
_ROUNDING_SHIFT = 13
_ROUNDING_BITS = 3
_DOT_PRODUCTS = frozenset({"dpps", "dppd"})
_DOT_PRODUCT_SELECTION = 4
_HALF_LANE_BITS = 128
_SPECIAL_PRODUCTS = 2
_GPR_BYTES = 8
_HALF_BITS = 32
_EXTENDED_BYTES = 10
_INPUT_BYTES_SHOWN = 32
_MASK64 = (1 << 64) - 1
_ST_NAMES = tuple(f"st{number}" for number in range(8))


class CompareError(Exception):
    pass


@dataclass(frozen=True)
class Lanes:
    location: tuple
    element_bits: int
    count: int


@dataclass(frozen=True)
class Rules:
    mnemonic: str
    undefined_flags: frozenset
    status_flags_dropped: bool
    float_lanes: tuple
    float_sources: tuple
    computes: bool
    tolerance: Tolerance
    square_root: bool
    min_max: bool
    dot_product: Optional[int]
    console: Optional[str]
    destination: Optional[tuple]
    access: Optional[tuple]
    mxcsr_stores: tuple
    x87_names: tuple
    inputs: tuple
    show_mxcsr: bool
    show_x87: bool


@dataclass(frozen=True)
class Mismatch:
    field: str
    expected: str
    actual: str


class Run:
    def __init__(self, cpu_count):
        self.cpu_count = cpu_count
        self.counter = None
        self.unwritten = collections.Counter()


def _register_name(name):
    return records.GPR_NAMES[LOCATIONS[name][1]]


def _location_name(location, x87_names):
    kind, number = location[:2]
    if kind == "gpr":
        return records.GPR_NAMES[number]
    if kind == "vector":
        return f"ymm{number}"
    if kind in ("st", "mmx"):
        return x87_names[number]
    raise CompareError(f"no slot field for a {kind} register")


def _scalar(mnemonic):
    return (mnemonic[1:] if mnemonic.startswith("v") else mnemonic).endswith(("ss", "sd"))


def _float_location(model, instance, operand):
    if operand.choice is not None:
        choice = instance.operands[operand.choice]
        if isinstance(choice, Reg):
            return LOCATIONS[choice.name][:2]
        if isinstance(choice, Mem) and model.accesses_memory:
            return "memory", operand.choice
        return None
    if operand.category == "implicit_reg" and model.implicit_location(operand) is not None:
        return model.implicit_location(operand)[:2]
    return None


def _float_operands(model, instance, writes):
    if not model.floating:
        return []
    found = []
    for operand in model.operands:
        element = operand.spec.get("element_type")
        if element not in FLOAT_BITS or not (operand.writes if writes else operand.reads):
            continue
        location = _float_location(model, instance, operand)
        if location is not None:
            found.append((operand, location, FLOAT_BITS[element]))
    return found


def _computes(model):
    compares = any(operand.spec.get("element_type") in COMPARISON_PREDICATES for operand in model.operands)
    return model.definition.exception_class in _COMPUTING_EXCEPTION_CLASSES and not compares


def _written_float_lanes(model, instance, kind):
    if not _computes(model) and kind is not Tolerance.RCP:
        return ()
    lanes = []
    for operand, location, bits in _float_operands(model, instance, writes=True):
        count = 1 if _scalar(model.mnemonic) else max(model.operand_bits(operand) // bits, 1)
        lanes.append(Lanes(location, bits, count))
    return tuple(lanes)


def _float_sources(model, instance):
    return tuple((location, bits) for _, location, bits in _float_operands(model, instance, writes=False))


def _read_inputs(model, instance, x87_names):
    names = []
    for operand in model.operands:
        if not operand.reads:
            continue
        if operand.choice is not None:
            choice = instance.operands[operand.choice]
            if isinstance(choice, Reg) and LOCATIONS[choice.name][0] != "bnd":
                names.append(_location_name(LOCATIONS[choice.name], x87_names))
            elif isinstance(choice, Mem):
                names += [_register_name(name) for name in (choice.base, choice.index) if name not in (None, "rip")]
        elif operand.category == "implicit_reg":
            location = model.implicit_location(operand)
            if location is not None and location[0] != "bnd":
                names.append(_location_name(location, x87_names))
        elif operand.category == "implicit_mem":
            names.append(records.GPR_NAMES[IMPLICIT_BASES[operand.spec["mem_base"]]])
    return tuple(dict.fromkeys(names))


def _dot_product_control(instance, base, sources):
    if base not in _DOT_PRODUCTS:
        return None
    if len(sources) != 2:
        raise CompareError(f"{instance.form.key}: a dot product reads two floating-point sources, not {len(sources)}")
    (control,) = immediate_values(instance.operands)
    return control


def _destination(model, instance):
    choice = instance.operands[0]
    location = LOCATIONS[choice.name]
    return location[1], model.operand_bits(model.explicit[0])


def _outside_rules(instance):
    return Rules(
        mnemonic=instance.form.definition.mnemonic,
        undefined_flags=frozenset(),
        status_flags_dropped=False,
        float_lanes=(),
        float_sources=(),
        computes=False,
        tolerance=Tolerance.NONE,
        square_root=False,
        min_max=False,
        dot_product=None,
        console=None,
        destination=None,
        access=None,
        mxcsr_stores=(),
        x87_names=_ST_NAMES,
        inputs=(),
        show_mxcsr=False,
        show_x87=False,
    )


def rules_for(instance):
    if not in_set(instance.form.definition):
        return _outside_rules(instance)
    model = form_model(instance.form)
    definition = model.definition
    mnemonic = model.mnemonic
    x87_names = tuple(f"{'mm' if model.mmx else 'st'}{number}" for number in range(8))
    kind = _TOLERANCES.get(mnemonic, Tolerance.NONE)
    access = None
    if mnemonic in _STATE_IMAGE_ACCESSES:
        memory = model.explicit[0]
        access = (memory.choice, model.memory_width(memory))
    stores = ((model.explicit[0].choice, _MXCSR_STORES[mnemonic]),) if mnemonic in _MXCSR_STORES else ()
    base = mnemonic[1:] if mnemonic.startswith("v") else mnemonic
    sources = _float_sources(model, instance)
    return Rules(
        mnemonic=mnemonic,
        undefined_flags=frozenset(flag.upper() for flag in definition.undefined_flags),
        status_flags_dropped=model.shape in _DROPPING_SHAPES,
        float_lanes=_written_float_lanes(model, instance, kind),
        float_sources=sources,
        computes=_computes(model),
        tolerance=kind,
        square_root=base.startswith("rsqrt"),
        min_max=model.floating and base.startswith(("min", "max")),
        dot_product=_dot_product_control(instance, base, sources),
        console=mnemonic if mnemonic in _CONSOLE_REGISTERS else None,
        destination=_destination(model, instance) if kind in (Tolerance.RANDOM_VALUE, Tolerance.PROCESSOR_ID) else None,
        access=access,
        mxcsr_stores=stores,
        x87_names=x87_names,
        inputs=_read_inputs(model, instance, x87_names),
        show_mxcsr=model.floating,
        show_x87=model.x87,
    )


def reference_case(spec, float_mode):
    _check_mode(float_mode)
    return spec.twin if float_mode == FAST and spec.twin is not None else spec.index


def _check_mode(float_mode):
    if float_mode not in FLOAT_MODES:
        raise CompareError(f"the floating-point mode is one of {FLOAT_MODES}, not {float_mode!r}")


@functools.lru_cache(maxsize=None)
def _slot_fields(x87_names):
    fields = [("fcw", records.FX_FCW, 2), ("fsw", records.FX_FSW, 2), ("ftw", records.FX_FTW, 1),
              ("mxcsr", records.FX_MXCSR, 4)]
    fields += [(x87_names[number], records.fx_st_offset(number), records.FX_ST_STRIDE) for number in range(8)]
    fields += [(f"ymm{number}", records.ymm_offset(number), records.YMM_STRIDE) for number in range(records.YMM_COUNT)]
    fields += [(name, records.gpr_offset(number), _GPR_BYTES) for number, name in enumerate(records.GPR_NAMES)]
    fields.append(("path", records.PATH, 1))
    return tuple(fields)


def _scaffold_offsets():
    covered = set()
    for _, start, size in _slot_fields(_ST_NAMES):
        covered.update(range(start, start + size))
    for start, end in records.UNRECORDED_SLOT_RANGES:
        covered.update(range(start, end))
    covered.update((records.STORE_AX, records.STORE_AX + 1, records.STORE_DF))
    return tuple(offset for offset in range(records.SLOT_SIZE) if offset not in covered)


_SCAFFOLD_OFFSETS = _scaffold_offsets()


def _hex(data):
    return f"0x{int.from_bytes(data, 'little'):0{2 * len(data)}x}"


def _mxcsr_of(slot):
    return struct.unpack_from("<I", slot, records.FX_MXCSR)[0]


def _describe(state, case):
    if state.outcome is Outcome.COMPLETED:
        return state.outcome.name
    text = f"{state.outcome.name} at {_place(state.address, case)}"
    if state.detail:
        text += f", detail {state.detail}"
    if state.outcome is Outcome.PAGE_FAULT:
        text += f", data {state.data:#x}"
    return text


def _place(address, case):
    if address == case.address:
        return f"{address:#x} (the instance)"
    if address == case.layout.loader_fxrstor:
        return f"{address:#x} (the loader's fxrstor)"
    return f"{address:#x}"


def _expected_stop(case, expected, float_mode):
    if float_mode != FAST:
        return expected
    if _mxcsr_of(case.input.slot) & _MXCSR_MASKS != _MXCSR_MASKS:
        return State.stop(Outcome.NEEDS_EXACT_FLOAT, case.layout.loader_fxrstor)
    loaded = case.spec.loads_mxcsr
    if loaded is not None and loaded & _MXCSR_MASKS != _MXCSR_MASKS and expected.outcome is Outcome.COMPLETED:
        return State.stop(Outcome.NEEDS_EXACT_FLOAT, case.address)
    return expected


def _page_fault_data(case, rules, expected, actual):
    if rules.access is None:
        return expected.data == actual.data
    choice, width = rules.access
    start = operand_address(case, choice)
    guard = case.layout.guard
    return guard <= actual.data < guard + records.GUARD_SIZE and start <= actual.data < start + width


def _compare_ending(case, rules, expected, actual):
    if expected.outcome is not actual.outcome:
        return [Mismatch("outcome", _describe(expected, case), _describe(actual, case))]
    if expected.outcome is Outcome.COMPLETED:
        return []
    mismatches = []
    if expected.address != actual.address:
        mismatches.append(Mismatch("address", _place(expected.address, case), _place(actual.address, case)))
    if expected.detail != actual.detail:
        mismatches.append(Mismatch("detail", str(expected.detail), str(actual.detail)))
    if expected.outcome is Outcome.PAGE_FAULT and not _page_fault_data(case, rules, expected, actual):
        mismatches.append(Mismatch("data", f"{expected.data:#x}", f"{actual.data:#x}"))
    return mismatches


def _float_parts(value, bits):
    exponent_bits, fraction_bits = tolerance.FLOAT_FORMATS[bits]
    return value >> bits - 1 & 1, value >> fraction_bits & (1 << exponent_bits) - 1, value & (1 << fraction_bits) - 1


def _is_nan(value, bits):
    _, exponent, fraction = _float_parts(value, bits)
    return exponent == (1 << tolerance.FLOAT_FORMATS[bits][0]) - 1 and fraction != 0


def _mxcsr_matches(expected, actual, own, float_mode, twin):
    if twin:
        expected = expected & ~_MXCSR_CONTROL | own & _MXCSR_CONTROL
    ignored = _MXCSR_FLAGS if float_mode == FAST else 0
    return (expected ^ actual) & ~ignored & 0xFFFFFFFF == 0


def _x87_reference(case, rules):
    slot = case.input.slot
    control, status, tags = struct.unpack_from("<HHB", slot, records.FX_FCW)
    top = status >> 11 & 7
    count = tolerance.OPERAND_COUNTS[rules.mnemonic]
    if any(not tags >> (top + number) % 8 & 1 for number in range(count)):
        return None
    if rules.mnemonic in _X87_PUSHES and tags >> (top + 7) % 8 & 1:
        return None
    operands = tuple(
        int.from_bytes(slot[records.fx_st_offset(number):records.fx_st_offset(number) + _EXTENDED_BYTES], "little")
        for number in range(count)
    )
    return tolerance.x87_reference(rules.mnemonic, operands, control >> 10 & 3)


def _defined(data, undefined):
    return bytes(byte & ~mask for byte, mask in zip(data, undefined))


def _flags(slot):
    ax = struct.unpack_from("<H", slot, records.STORE_AX)[0]
    return records.store_flags(ax, slot[records.STORE_DF] & _DF_MASK), ax, slot[records.STORE_DF]


class _Judgement:
    def __init__(self, case, rules, expected, actual, float_mode, run):
        self.case = case
        self.rules = rules
        self.expected = bytearray(expected.slot)
        self.actual = actual.slot
        self.expected_memory = expected.memory
        self.actual_memory = actual.memory
        self.float_mode = float_mode
        self.run = run
        self.own_mxcsr = _mxcsr_of(case.input.slot)
        self.twin = reference_case(case.spec, float_mode) != case.spec.index
        self.undefined = undefined_bits(case)
        self.mismatches = []
        self.settled = set()
        self.initial = None
        self.special_halves = {}

    def initial_block(self):
        if self.initial is None:
            self.initial = initial_block(self.case.input)
        return self.initial

    def source_lane(self, location, index, bits):
        size = bits // 8
        kind, number = location
        if kind == "vector":
            start = records.ymm_offset(number) + index * size
            return int.from_bytes(self.case.input.slot[start:start + size], "little")
        if kind == "memory":
            address = operand_address(self.case, number)
            start = address - self.case.layout.block + index * size
            if not 0 <= start <= records.BLOCK_SIZE - size:
                raise CompareError(f"case {self.case.spec.index} reads lane {index} outside the block")
            return int.from_bytes(self.initial_block()[start:start + size], "little")
        raise CompareError(f"{self.case.instance.form.key}: no rule for a floating-point source in a {kind} register")

    def several_special(self, index, bits):
        lanes = _HALF_LANE_BITS // bits
        half = index // lanes
        if half not in self.special_halves:
            (first, _), (second, _) = self.rules.float_sources
            rounding = self.own_mxcsr >> _ROUNDING_SHIFT & _ROUNDING_BITS
            selected = [half * lanes + lane for lane in range(lanes)
                        if self.rules.dot_product >> _DOT_PRODUCT_SELECTION + lane & 1]
            special = sum(tolerance.special_product(self.source_lane(first, lane, bits),
                                                    self.source_lane(second, lane, bits), bits, rounding)
                          for lane in selected)
            self.special_halves[half] = special >= _SPECIAL_PRODUCTS
        return self.special_halves[half]

    def lane_matches(self, expected, actual, index, bits):
        if expected == actual:
            return True
        if (self.rules.dot_product is not None and _is_nan(expected, bits) and _is_nan(actual, bits)
                and self.several_special(index, bits)):
            return True
        if self.float_mode == FAST and self.rules.computes:
            if _is_nan(expected, bits) and _is_nan(actual, bits):
                return True
            expected_sign, expected_exponent, expected_fraction = _float_parts(expected, bits)
            actual_sign, actual_exponent, actual_fraction = _float_parts(actual, bits)
            if self.own_mxcsr & (records.MXCSR_FTZ | records.MXCSR_DAZ) and expected_sign == actual_sign:
                if (self.rules.min_max and expected_exponent == expected_fraction == 0 and actual_exponent == 0
                        and self.is_source_lane(actual, index, bits)):
                    return True
                if expected_exponent == 1 and expected_fraction == 0 and actual_exponent == actual_fraction == 0:
                    return True
        if self.rules.tolerance is Tolerance.RCP:
            source = self.source_lane(self.rules.float_sources[-1][0], index, bits)
            return tolerance.within_rcp(expected, actual, source, bits, self.rules.square_root)
        return False

    def is_source_lane(self, value, index, bits):
        return any(source_bits == bits and self.source_lane(location, index, bits) == value
                   for location, source_bits in self.rules.float_sources)

    def lanes_match(self, expected, actual, lanes):
        size = lanes.element_bits // 8
        end = min(lanes.count * size, len(expected))
        if expected[end:] != actual[end:]:
            return False
        for index, start in enumerate(range(0, end, size)):
            expected_lane = int.from_bytes(expected[start:start + size], "little")
            actual_lane = int.from_bytes(actual[start:start + size], "little")
            if not self.lane_matches(expected_lane, actual_lane, index, lanes.element_bits):
                return False
        return True

    def mismatch(self, field, expected, actual):
        self.mismatches.append(Mismatch(field, expected, actual))

    def register(self, name, slot):
        offset = records.gpr_offset(records.GPR_NAMES.index(name))
        return int.from_bytes(slot[offset:offset + _GPR_BYTES], "little")

    def console_values(self, values):
        mnemonic = self.rules.console
        names = _CONSOLE_REGISTERS[mnemonic]
        if values is None or len(values) != _CONSOLE_VALUE_COUNTS[mnemonic]:
            self.mismatch("console values", f"{_CONSOLE_VALUE_COUNTS[mnemonic]} values from the lifted driver",
                          "none" if values is None else f"{len(values)} values")
            self.settled.update(names)
            return
        if mnemonic == "xgetbv":
            values = (values[0] & 0xFFFFFFFF, values[0] >> _HALF_BITS)
        for name, value in zip(names, values):
            struct.pack_into("<Q", self.expected, records.gpr_offset(records.GPR_NAMES.index(name)), value)

    def random_value(self):
        number, bits = self.rules.destination
        name = records.GPR_NAMES[number]
        untouched = _MASK64 & ~((1 << bits) - 1)
        expected, actual = self.register(name, self.expected), self.register(name, self.actual)
        if (expected ^ actual) & untouched:
            self.mismatch(name, f"0x{expected & untouched:016x} in the bits above the low {bits}", f"0x{actual:016x}")
        if (actual ^ self.register(name, self.case.input.slot)) & ~untouched == 0:
            self.run.unwritten[self.case.instance.form.key] += 1
        flags, _, _ = _flags(self.actual)
        if flags["CF"] != _RANDOM_CARRY:
            self.mismatch("CF", str(_RANDOM_CARRY), str(flags["CF"]))
        self.settled.update({name, "CF"})

    def below_processor_count(self, name):
        value = self.register(name, self.actual)
        if value >= self.run.cpu_count:
            self.mismatch(name, f"below the host's {self.run.cpu_count} processors", f"{value:#x}")
        self.settled.add(name)

    def counter(self):
        halves = []
        for name in _COUNTER_HALVES:
            value = self.register(name, self.actual)
            if value >> _HALF_BITS:
                self.mismatch(name, "a value below 2^32", f"0x{value:016x}")
            halves.append(value & 0xFFFFFFFF)
        counter = halves[1] << _HALF_BITS | halves[0]
        if self.run.counter is not None and counter < self.run.counter:
            self.mismatch("counter", f"at least {self.run.counter:#x}, the previous case's", f"{counter:#x}")
        self.run.counter = counter
        self.settled.update(_COUNTER_HALVES)
        if self.rules.mnemonic == "rdtscp":
            self.below_processor_count(_PROCESSOR_REGISTER)

    def flags(self, ignored_status):
        expected, expected_ax, expected_df = _flags(self.expected)
        actual, actual_ax, actual_df = _flags(self.actual)
        for flag in records.FLAG_NAMES:
            if flag in self.settled or flag in self.rules.undefined_flags or flag in ignored_status:
                continue
            if expected[flag] != actual[flag]:
                self.mismatch(flag, str(expected[flag]), str(actual[flag]))
        if (expected_ax ^ actual_ax) & ~_AX_FLAG_MASK or (expected_df ^ actual_df) & ~_DF_MASK:
            self.mismatch("flag capture bits", f"ax {expected_ax:#06x} df {expected_df:#04x}",
                          f"ax {actual_ax:#06x} df {actual_df:#04x}")

    def lanes_at(self, location):
        return next((lanes for lanes in self.rules.float_lanes if lanes.location == location), None)

    def field_matches(self, name, start, size, x87):
        expected = bytes(self.expected[start:start + size])
        actual = self.actual[start:start + size]
        undefined = self.undefined[start:start + size]
        if any(undefined):
            expected, actual = _defined(expected, undefined), _defined(actual, undefined)
        if name == "mxcsr":
            return _mxcsr_matches(int.from_bytes(expected, "little"), int.from_bytes(actual, "little"),
                                  self.own_mxcsr, self.float_mode, self.twin)
        if name == "fsw":
            ignored = sum(1 << bit for flag, bit in _CONDITION_BITS.items() if flag in self.rules.undefined_flags)
            if x87 is not None:
                ignored |= 1 << _CONDITION_BITS["C1"]
            return (int.from_bytes(expected, "little") ^ int.from_bytes(actual, "little")) & ~ignored == 0
        reference = self.transcendental_reference(name, x87)
        if reference is not None:
            value = int.from_bytes(actual[:_EXTENDED_BYTES], "little")
            return expected[_EXTENDED_BYTES:] == actual[_EXTENDED_BYTES:] and tolerance.within_ulp(reference, value)
        if expected == actual:
            return True
        location = ("vector", int(name[3:])) if name.startswith("ymm") else None
        if name in records.GPR_NAMES:
            location = ("gpr", records.GPR_NAMES.index(name))
        lanes = self.lanes_at(location) if location is not None else None
        return lanes is not None and self.lanes_match(expected, actual, lanes)

    def transcendental_reference(self, name, x87):
        if x87 is None or name not in self.rules.x87_names:
            return None
        return x87.get(self.rules.x87_names.index(name))

    def expected_text(self, name, start, size, x87):
        expected = bytes(self.expected[start:start + size])
        reference = self.transcendental_reference(name, x87)
        if reference is None:
            return _hex(expected)
        return f"{_hex(reference.to_bytes(_EXTENDED_BYTES, 'little') + expected[_EXTENDED_BYTES:])} within one ulp"

    def slot(self, x87):
        for name, start, size in _slot_fields(self.rules.x87_names):
            if name in self.settled:
                continue
            if not self.field_matches(name, start, size, x87):
                self.mismatch(name, self.expected_text(name, start, size, x87), _hex(self.actual[start:start + size]))
        differing = [offset for offset in _SCAFFOLD_OFFSETS if self.expected[offset] != self.actual[offset]]
        if differing:
            self.mismatch(f"scaffold bytes {differing[:8]}", _hex(bytes(self.expected[offset] for offset in differing)),
                          _hex(bytes(self.actual[offset] for offset in differing)))

    def special_ranges(self):
        block = self.case.layout.block
        for lanes in self.rules.float_lanes:
            if lanes.location[0] == "memory":
                start = operand_address(self.case, lanes.location[1]) - block
                yield start, lanes.count * lanes.element_bits // 8, lanes
        for choice, offset in self.rules.mxcsr_stores:
            yield operand_address(self.case, choice) - block + offset, 4, None

    def memory(self):
        if self.expected_memory == self.actual_memory:
            return
        what = f"case {self.case.spec.index}'s block"
        expected = apply_words(self.initial_block(), self.expected_memory, f"the expectation of {what}")
        actual = apply_words(self.initial_block(), self.actual_memory, f"the result of {what}")
        covered = set()
        for start, size, lanes in self.special_ranges():
            if start < 0 or start + size > records.BLOCK_SIZE:
                continue
            covered.update(range(start, start + size))
            expected_bytes, actual_bytes = expected[start:start + size], actual[start:start + size]
            if expected_bytes == actual_bytes:
                continue
            if lanes is None:
                matches = _mxcsr_matches(int.from_bytes(expected_bytes, "little"),
                                         int.from_bytes(actual_bytes, "little"), self.own_mxcsr, self.float_mode,
                                         self.twin)
            else:
                matches = self.lanes_match(expected_bytes, actual_bytes, lanes)
            if not matches:
                self.mismatch(f"block+{start:#x}", _hex(expected_bytes), _hex(actual_bytes))
        for offset in sorted(set(self.expected_memory) | set(self.actual_memory)):
            uncovered = [byte for byte in range(offset, offset + _GPR_BYTES) if byte not in covered]
            if any(expected[byte] != actual[byte] for byte in uncovered):
                self.mismatch(f"block+{offset:#x}", _hex(expected[offset:offset + _GPR_BYTES]),
                              _hex(actual[offset:offset + _GPR_BYTES]))


def compare(case, rules, expected, actual, float_mode, run):
    _check_mode(float_mode)
    expected = _expected_stop(case, expected, float_mode)
    mismatches = _compare_ending(case, rules, expected, actual)
    if mismatches or expected.outcome is not Outcome.COMPLETED:
        return mismatches
    judgement = _Judgement(case, rules, expected, actual, float_mode, run)
    if rules.console is not None:
        judgement.console_values(actual.console)
    if rules.tolerance is Tolerance.RANDOM_VALUE:
        judgement.random_value()
    elif rules.tolerance is Tolerance.COUNTER:
        judgement.counter()
    elif rules.tolerance is Tolerance.PROCESSOR_ID:
        judgement.below_processor_count(records.GPR_NAMES[rules.destination[0]])
    x87 = _x87_reference(case, rules) if rules.tolerance is Tolerance.X87_TRANSCENDENTAL else None
    judgement.flags(_STATUS_FLAGS if rules.status_flags_dropped else ())
    judgement.slot(x87)
    judgement.memory()
    return judgement.mismatches


def form_mismatches(run):
    return {
        form: [Mismatch("destination written", f"at most {_UNWRITTEN_ALLOWED} case whose destination's written bits "
                        "keep their input, as each case writes a new value", f"{count} cases")]
        for form, count in run.unwritten.items() if count > _UNWRITTEN_ALLOWED
    }


def verdict(actual, mismatches):
    if not mismatches:
        return Verdict.PASS
    if actual.outcome is Outcome.CRASHED:
        return Verdict.CRASHED
    if actual.outcome is Outcome.TIMED_OUT:
        return Verdict.TIMED_OUT
    if actual.outcome is Outcome.MISSING_TRANSLATION:
        return Verdict.STOPPED
    return Verdict.MISMATCHED


def _inputs_text(case, rules):
    slot = case.input.slot
    fields = {name: (start, size) for name, start, size in _slot_fields(rules.x87_names)}
    parts = []
    for name in rules.inputs:
        start, size = fields[name]
        parts.append(f"{name}={_hex(slot[start:start + size])}")
    parts.append("flags " + " ".join(f"{flag}={case.spec.flags[flag]}" for flag in records.FLAG_NAMES))
    if rules.show_mxcsr:
        parts.append(f"mxcsr={_mxcsr_of(slot):#06x}")
    if rules.show_x87:
        control, status, tags = struct.unpack_from("<HHB", slot, records.FX_FCW)
        parts.append(f"fcw={control:#06x} fsw={status:#06x} ftw={tags:#04x}")
    for offset, data in case.input.patches:
        shown = _hex(data[:_INPUT_BYTES_SHOWN]) + ("…" if len(data) > _INPUT_BYTES_SHOWN else "")
        parts.append(f"block+{offset:#x}={shown}")
    parts.append(f"fill seed {case.input.fill_seed:#x}")
    return ", ".join(parts)


def format_mismatch(case, mismatches):
    rules = rules_for(case.instance)
    lines = [f"{case.instance.form.key} case {case.spec.index}: {case.instance.text} at {case.address:#x}",
             f"  inputs: {_inputs_text(case, rules)}"]
    lines += [f"  {mismatch.field}: expected {mismatch.expected}, actual {mismatch.actual}" for mismatch in mismatches]
    return "\n".join(lines)
