"""Judge a case's lifted result against its expectation, by the rules of the instruction-tests design.

A `State` is what a case ended with: its outcome, the full final slot (the input slot with the result's words applied)
and the changed block words. `compare` takes the case, its instance's `Rules`, the expected and the actual state, the
floating-point mode and the `Run` the case belongs to, and lists every `Mismatch`.

- **Faults** compare by outcome, address and detail, and a page fault by its data address. For the FXSAVE and XSAVE
  family, whose processors report different bytes of a multi-byte state save, the data address only has to lie in the
  guard and in the instruction's access.
- **Completed cases** compare every slot field and every block word, except the storer's FOP, FIP, FDP, MXCSR_MASK,
  reserved bytes and XMM area (the YMM slots hold the vector state), flags the database marks undefined (x87 condition
  codes included), and the status flags of calls, returns and indirect jumps, which lifted code drops (DF compares).
- **Tolerances** (`tolerance`): `rcp*` and `rsqrt*` lanes within the documented relative error; the results an x87
  transcendental approximates within one ulp of the 64-bit format of the true result, whatever the precision control,
  where the operands lie in the documented range, with C1, the rounding indicator, not compared there.
- **Properties:** `rdrand` and `rdseed` set CF and clear the other status flags, and keep the destination's untouched
  bits as recorded; the counters of `rdtsc` and `rdtscp` do not decrease from case to case in a run and leave the high
  halves 0; `rdpid` and `rdtscp`'s ecx are below the host's processor count; `cpuid` and `xgetbv` give the console's
  values, which the lifted driver appends to its results.
- **Fast mode** allows a NaN lane to be any NaN; under FTZ or DAZ, which fast mode treats as one switch, a `min*` or
  `max*` lane that x86 makes a signed zero to be a denormal of that sign, and a lane that x86 rounds to the smallest
  normal to be a zero of that sign; it does not compare MXCSR's exception flags, wherever MXCSR is stored; it compares
  an FTZ-only or DAZ-only case with its twin's expectation (`reference_case`), with MXCSR's control bits as the case
  set them; and it expects NEEDS_EXACT_FLOAT at the loader's fxrstor for a case whose initial MXCSR unmasks an
  exception, and at the instance for one that loads such an MXCSR, where the recording shows the load completing.

`format_mismatch` gives the instance as Zydis formats it, the case's inputs, and each differing field's values."""
import enum
import functools
import struct
from dataclasses import dataclass
from typing import Optional

from . import records, tolerance
from .cases import CaseSpec, Instance, expression_offset, form_model
from .encoding import Mem, Reg
from .model import FLOAT_BITS, IMPLICIT_BASES, LOCATIONS, Shape
from .program import Layout
from .records import CaseInput, Outcome

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
_COUNTER_HALVES = ("rax", "rdx")
_PROCESSOR_REGISTER = "rcx"
_CONSOLE_REGISTERS = {"cpuid": ("rax", "rbx", "rcx", "rdx"), "xgetbv": ("rax", "rdx")}
_CONSOLE_VALUE_COUNTS = {"cpuid": 4, "xgetbv": 1}
_STATE_SAVES = frozenset("fxsave fxsave64 fxrstor fxrstor64 xsave xsave64 xsavec xsavec64 xsaveopt xsaveopt64 xrstor "
                         "xrstor64".split())
_MXCSR_STORES = {
    "stmxcsr": 0, "vstmxcsr": 0,
    **{mnemonic: records.FX_MXCSR for mnemonic in "fxsave fxsave64 xsave xsave64 xsavec xsavec64 xsaveopt "
       "xsaveopt64".split()},
}
_X87_PUSHES = frozenset({"fsincos", "fptan"})
_STATUS_FLAGS = ("CF", "PF", "AF", "ZF", "SF", "OF")
_RANDOM_FLAGS = {"CF": 1, "PF": 0, "AF": 0, "ZF": 0, "SF": 0, "OF": 0}
_DROPPING_SHAPES = frozenset({Shape.CALL, Shape.RETURN, Shape.INDIRECT_JUMP})
_CONDITION_BITS = {"C0": 8, "C1": 9, "C2": 10, "C3": 14}
_AH_FLAG_BITS = {"CF": 0, "PF": 2, "AF": 4, "ZF": 6, "SF": 7}
_AX_FLAG_MASK = sum(1 << 8 + bit for bit in _AH_FLAG_BITS.values()) | 1
_DF_MASK = 1
_MXCSR_FLAGS = 0x3F
_MXCSR_MASKS = 0x1F80
_MXCSR_CONTROL = 0xFFC0 | 1 << 17
_FTZ = 1 << 15
_DAZ = 1 << 6
_GPR_BYTES = 8
_HALF_BITS = 32
_EXTENDED_BYTES = 10
_INPUT_BYTES_SHOWN = 32
_MASK64 = (1 << 64) - 1


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
    tolerance: Tolerance
    min_max: bool
    console: Optional[str]
    destination: Optional[tuple]
    access: Optional[tuple]
    mxcsr_stores: tuple
    x87_names: tuple
    inputs: tuple
    show_mxcsr: bool
    show_x87: bool


@dataclass(frozen=True)
class Case:
    spec: CaseSpec
    instance: Instance
    input: CaseInput
    layout: Layout

    @property
    def address(self):
        return self.layout.instances[self.instance.index].address


@dataclass(frozen=True)
class State:
    outcome: Outcome
    detail: int
    address: int
    data: int
    slot: Optional[bytes]
    memory: dict
    console: Optional[tuple]

    @classmethod
    def of(cls, result, slot):
        final = bytearray(slot)
        for offset, value in result.slot_words.items():
            if offset % _GPR_BYTES or not 0 <= offset < records.SLOT_SIZE:
                raise CompareError(f"case {result.case} holds a slot word at {offset}, which is no word of the slot")
            struct.pack_into("<Q", final, offset, value)
        return cls(result.outcome, result.detail, result.address, result.data, bytes(final), result.memory_words,
                   result.expected)

    @classmethod
    def stop(cls, outcome, address):
        return cls(outcome, 0, address, 0, None, {}, None)


@dataclass(frozen=True)
class Mismatch:
    field: str
    expected: str
    actual: str


class Run:
    def __init__(self, cpu_count):
        self.cpu_count = cpu_count
        self.counter = None


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


def _written_float_lanes(model, instance):
    if not model.floating:
        return ()
    lanes = []
    for operand in model.operands:
        element = operand.spec.get("element_type")
        if not operand.writes or element not in FLOAT_BITS:
            continue
        bits = FLOAT_BITS[element]
        if operand.choice is not None:
            choice = instance.operands[operand.choice]
            if isinstance(choice, Reg):
                location = LOCATIONS[choice.name][:2]
            elif isinstance(choice, Mem) and model.accesses_memory:
                location = ("memory", operand.choice)
            else:
                continue
        elif operand.category == "implicit_reg" and model.implicit_location(operand) is not None:
            location = model.implicit_location(operand)[:2]
        else:
            continue
        lanes.append(Lanes(location, bits, max(model.operand_bits(operand) // bits, 1)))
    return tuple(lanes)


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


def _destination(model, instance):
    choice = instance.operands[0]
    location = LOCATIONS[choice.name]
    return location[1], model.operand_bits(model.explicit[0])


def rules_for(instance):
    model = form_model(instance.form)
    definition = model.definition
    mnemonic = model.mnemonic
    x87_names = tuple(f"{'mm' if model.mmx else 'st'}{number}" for number in range(8))
    kind = _TOLERANCES.get(mnemonic, Tolerance.NONE)
    access = None
    if mnemonic in _STATE_SAVES:
        memory = model.explicit[0]
        access = (memory.choice, model.memory_width(memory))
    stores = ((model.explicit[0].choice, _MXCSR_STORES[mnemonic]),) if mnemonic in _MXCSR_STORES else ()
    base = mnemonic[1:] if mnemonic.startswith("v") else mnemonic
    return Rules(
        mnemonic=mnemonic,
        undefined_flags=frozenset(flag.upper() for flag in definition.undefined_flags),
        status_flags_dropped=model.shape in _DROPPING_SHAPES,
        float_lanes=_written_float_lanes(model, instance),
        tolerance=kind,
        min_max=model.floating and base.startswith(("min", "max")),
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
    for _, start, size in _slot_fields(tuple(f"st{number}" for number in range(8))):
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


def _operand_address(case, choice, address_size):
    memory = case.instance.operands[choice]
    if not isinstance(memory, Mem):
        raise CompareError(f"{case.instance.form.key}: operand {choice} is no memory operand")
    if memory.base in (None, "rip"):
        return case.layout.block + expression_offset(memory.displacement)
    total = _register_value(case.input.slot, memory.base) + memory.displacement
    if memory.index is not None:
        total += _register_value(case.input.slot, memory.index) * memory.scale
    return total & (1 << address_size) - 1


def _register_value(slot, name):
    _, number, shift, bits = LOCATIONS[name]
    word = int.from_bytes(slot[records.gpr_offset(number):records.gpr_offset(number) + _GPR_BYTES], "little")
    return word >> shift & (1 << bits) - 1


def _address_size(case):
    return form_model(case.instance.form).address_size


def _page_fault_data(case, rules, expected, actual):
    if rules.access is None:
        return expected.data == actual.data
    choice, width = rules.access
    start = _operand_address(case, choice, _address_size(case))
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


def _lane_matches(expected, actual, bits, rules, mxcsr, float_mode):
    if expected == actual:
        return True
    if float_mode == FAST:
        if _is_nan(expected, bits) and _is_nan(actual, bits):
            return True
        expected_sign, expected_exponent, expected_fraction = _float_parts(expected, bits)
        actual_sign, actual_exponent, actual_fraction = _float_parts(actual, bits)
        if mxcsr & (_FTZ | _DAZ) and expected_sign == actual_sign:
            if rules.min_max and expected_exponent == expected_fraction == 0 and actual_exponent == 0:
                return True
            if expected_exponent == 1 and expected_fraction == 0 and actual_exponent == actual_fraction == 0:
                return True
    return rules.tolerance is Tolerance.RCP and tolerance.within_rcp(expected, actual, bits)


def _lanes_match(expected, actual, lanes, rules, mxcsr, float_mode):
    size = lanes.element_bits // 8
    end = min(lanes.count * size, len(expected))
    if expected[end:] != actual[end:]:
        return False
    for start in range(0, end, size):
        expected_lane = int.from_bytes(expected[start:start + size], "little")
        actual_lane = int.from_bytes(actual[start:start + size], "little")
        if not _lane_matches(expected_lane, actual_lane, lanes.element_bits, rules, mxcsr, float_mode):
            return False
    return True


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
        self.mismatches = []
        self.settled = set()

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
        flags, _, _ = _flags(self.actual)
        for flag, value in _RANDOM_FLAGS.items():
            if flags[flag] != value:
                self.mismatch(flag, str(value), str(flags[flag]))
        self.settled.update({name, *_RANDOM_FLAGS})

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
        if name == "mxcsr":
            return _mxcsr_matches(int.from_bytes(expected, "little"), int.from_bytes(actual, "little"),
                                  self.own_mxcsr, self.float_mode, self.twin)
        if name == "fsw":
            ignored = sum(1 << bit for flag, bit in _CONDITION_BITS.items() if flag in self.rules.undefined_flags)
            if x87 is not None:
                ignored |= 1 << _CONDITION_BITS["C1"]
            return (int.from_bytes(expected, "little") ^ int.from_bytes(actual, "little")) & ~ignored == 0
        if x87 is not None and name in self.rules.x87_names and self.rules.x87_names.index(name) in x87:
            reference = x87[self.rules.x87_names.index(name)]
            value = int.from_bytes(actual[:_EXTENDED_BYTES], "little")
            return expected[_EXTENDED_BYTES:] == actual[_EXTENDED_BYTES:] and tolerance.within_ulp(reference, value)
        if expected == actual:
            return True
        location = ("vector", int(name[3:])) if name.startswith("ymm") else None
        if name in records.GPR_NAMES:
            location = ("gpr", records.GPR_NAMES.index(name))
        lanes = self.lanes_at(location) if location is not None else None
        return lanes is not None and _lanes_match(expected, actual, lanes, self.rules, self.own_mxcsr, self.float_mode)

    def slot(self, x87):
        for name, start, size in _slot_fields(self.rules.x87_names):
            if name in self.settled:
                continue
            if not self.field_matches(name, start, size, x87):
                self.mismatch(name, _hex(self.expected[start:start + size]), _hex(self.actual[start:start + size]))
        differing = [offset for offset in _SCAFFOLD_OFFSETS if self.expected[offset] != self.actual[offset]]
        if differing:
            self.mismatch(f"scaffold bytes {differing[:8]}", _hex(bytes(self.expected[offset] for offset in differing)),
                          _hex(bytes(self.actual[offset] for offset in differing)))

    def special_ranges(self):
        address_size = _address_size(self.case)
        block = self.case.layout.block
        for lanes in self.rules.float_lanes:
            if lanes.location[0] == "memory":
                start = _operand_address(self.case, lanes.location[1], address_size) - block
                yield start, lanes.count * lanes.element_bits // 8, lanes
        for choice, offset in self.rules.mxcsr_stores:
            yield _operand_address(self.case, choice, address_size) - block + offset, 4, None

    def memory(self):
        if self.expected_memory == self.actual_memory:
            return
        initial = _initial_block(self.case.input)
        expected = _apply_words(initial, self.expected_memory)
        actual = _apply_words(initial, self.actual_memory)
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
                matches = _lanes_match(expected_bytes, actual_bytes, lanes, self.rules, self.own_mxcsr,
                                       self.float_mode)
            if not matches:
                self.mismatch(f"block+{start:#x}", _hex(expected_bytes), _hex(actual_bytes))
        for offset in sorted(set(self.expected_memory) | set(self.actual_memory)):
            uncovered = [byte for byte in range(offset, offset + _GPR_BYTES) if byte not in covered]
            if any(expected[byte] != actual[byte] for byte in uncovered):
                self.mismatch(f"block+{offset:#x}", _hex(expected[offset:offset + _GPR_BYTES]),
                              _hex(actual[offset:offset + _GPR_BYTES]))


def _initial_block(case_input):
    block = bytearray(records.fill_block(case_input.fill_seed))
    for offset, data in case_input.patches:
        block[offset:offset + len(data)] = data
    return bytes(block)


def _apply_words(block, words):
    final = bytearray(block)
    for offset, value in words.items():
        if offset % _GPR_BYTES or not 0 <= offset < records.BLOCK_SIZE:
            raise CompareError(f"a block word at {offset} is no word of the block")
        struct.pack_into("<Q", final, offset, value)
    return bytes(final)


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
