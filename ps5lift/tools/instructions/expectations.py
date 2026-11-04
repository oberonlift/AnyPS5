"""What a case is expected to do: its recording, or what AMD's manual documents where Zen 2 differs from the Intel
recording or where there is none, or outside the set the console's stop.

`expected_result(case, recorded)` gives the result a case is judged against. The forms `cases.expectation_written`
names have no recording, and their results are written:
- Outside the set, the stop the console makes at the instance, with no state compared: an invalid opcode for an
  instruction the PS5's CPU lacks, with the detail State.h's `PS5LIFT_INVALID_OPCODE_NOT_ON_PS5` gives it (an
  instruction invalid by definition, such as `ud2`, has detail 0), a general-protection fault for a privileged or I/O
  instruction, the segment-table stop for one whose result depends on the console's descriptor tables, and the
  console-kernel stop for one whose effect is the console kernel's, with `int`'s vector as its detail (0 for `syscall`
  and `int1`).
- SSE4A (APM volume 4). `extrq` and `insertq` take a field's length from bits 5:0 of their first immediate, or of the
  control quadword (the low quadword of `extrq`'s source register, the high one of `insertq`'s), and its index from
  bits 5:0 of their second immediate or bits 13:8 of that quadword; a length of 0 is 64. `extrq` writes the field of
  its destination's low quadword, zero-extended, to that quadword; `insertq` writes the low `length` bits of its
  source's low quadword into its destination's at the index and keeps the other bits. `movntsd` and `movntss` store
  their source's low quadword or doubleword, and fault as any store does when it reaches the guard.
- MONITORX and CLZERO (APM volume 3), as Zen 2 runs them in user mode: `monitorx` and `mwaitx`, which returns at once,
  change no architectural state; `clzero` zeroes the 64-byte cache line that holds the address in rax, and faults as a
  store does when that line is in the guard.
- 16-bit near branches (APM volume 3), `jmp`, `call` and `jcc` with a 66 prefix: the target is (end + rel16) & 0xffff,
  where nothing is mapped, so a taken branch faults on fetching it, with that address as the faulting address and as
  the page fault's data address. A `call` first pushes its 2-byte return address, so one whose push reaches the guard
  faults at the instance instead, with the push's first byte in the guard as the data address. A `jcc` that is not
  taken completes with PATH `PATH_NOT_TAKEN`.

Every other case is its recording, changed only where AMD documents another result:
- `bsf` and `bsr` with a zero source (APM volume 3) leave the destination's 64 bits as they were and set ZF.
- The x87 pointers in the images `fxsave`, `fnstenv`, `fnsave` and the XSAVE family write (APM volume 2): FOP, FIP
  and FDP, with their selectors in the 32-bit formats, are 0 unless an unmasked x87 exception is pending (FSW.ES),
  where they are as recorded; MXCSR_MASK in an FXSAVE or XSAVE image is `AMD_MXCSR_MASK`, which allows MXCSR's
  misaligned-SSE bit (MM). Both apply where the recording shows the instruction wrote the field.
- `ldmxcsr`, `vldmxcsr`, `fxrstor` and `xrstor` accept MM, which Intel refuses with a general-protection fault: such a
  case completes with the state loaded, `loaded_state(case)`, unless its image is misaligned, which faults on both.
  `fxrstor` loads FCW, FSW, the abridged FTW, ST0 to ST7 and XMM0 to XMM15 from its image. `xrstor` loads each
  component its mask requests, x87, SSE and AVX, from the image where the header's XSTATE_BV has it and sets it to its
  initial state where it does not. MXCSR becomes the case's `loads_mxcsr`, which `cases` derives from the image.

`undefined_bits(case)` is a mask over the slot of the bits AMD's manual leaves undefined, which the comparison skips:
the high quadword of an `extrq` or `insertq` destination, and its low quadword too when the field's index and length
add up to more than 64, or the length is 0 and the index is not."""
import struct
from dataclasses import replace

from . import records
from .cases import TARGET, expectation_written, form_model
from .database import Group, classify, in_set
from .encoding import Reg, immediate_values
from .model import LOCATIONS, Shape
from .records import Outcome, Result
from .state import State, apply_words, changed_words, initial_block, operand_address, register_value

NO_EXPECTATION = "no recording and no written expectation"
AMD_MXCSR_MASK = 0x0002FFFF
NO_UNDEFINED_BITS = bytes(records.SLOT_SIZE)

_ES = 1 << 7
_FIELD_BITS = 0x3F
_QUADWORD_BITS = 64
_QUADWORD_BYTES = 8
_XMM_BYTES = 16
_SHORT_TARGET = 0xFFFF
_SHORT_RETURN_BYTES = 2
_CACHE_LINE = 64
_REL16 = range(-0x8000, 0x8000)
_CONDITIONS = {
    "o": lambda flags: flags["OF"],
    "b": lambda flags: flags["CF"],
    "z": lambda flags: flags["ZF"],
    "be": lambda flags: flags["CF"] | flags["ZF"],
    "s": lambda flags: flags["SF"],
    "p": lambda flags: flags["PF"],
    "l": lambda flags: flags["SF"] ^ flags["OF"],
    "le": lambda flags: flags["ZF"] | flags["SF"] ^ flags["OF"],
}
_LEGACY_POINTERS = {False: ((6, 8), (8, 14), (16, 22)), True: ((6, 8), (8, 16), (16, 24))}
_ENVIRONMENT_POINTERS = {14: ((6, 14),), 28: ((12, 26),)}
_FSAVE_REGISTERS = 80
_MXCSR_MASK_FIELD = (records.FX_MXCSR_MASK, records.FX_MXCSR_MASK + 4)
_X87_FIELDS = (records.FX_FCW, records.FX_FTW + 1)
_X87_REGISTER_BYTES = 10
_OUTSIDE_STOPS = {
    Group.NOT_ON_PS5: Outcome.INVALID_OPCODE,
    Group.PRIVILEGED: Outcome.GENERAL_PROTECTION,
    Group.SEGMENT_TABLES: Outcome.SEGMENT_TABLES,
    Group.CONSOLE_KERNEL: Outcome.CONSOLE_KERNEL,
}
_NOT_ON_PS5 = 1
_STATE_IMAGE_STORES = frozenset(
    "fxsave fxsave64 fnstenv fnsave xsave xsave64 xsaveopt xsaveopt64 xsavec xsavec64".split()
)
_ENVIRONMENT_SAVES = frozenset({"fnstenv", "fnsave"})


class ExpectationError(Exception):
    pass


class NoExpectation(ExpectationError):
    pass


class _Final:
    def __init__(self, case, slot, block):
        self.case = case
        self.slot = bytearray(slot)
        self.block = bytearray(block)

    @classmethod
    def recorded(cls, case, result):
        block = apply_words(initial_block(case.input), result.memory_words, f"the recording of case {result.case}")
        return cls(case, State.of(result, case.input.slot).slot, block)

    @classmethod
    def completed(cls, case, path=None):
        final = cls(case, case.input.slot, initial_block(case.input))
        final.capture(case.spec.flags)
        if path is not None:
            final.slot[records.PATH] = path
        return final

    def result(self, like=None):
        if like is None:
            like = Result(self.case.spec.index, Outcome.COMPLETED, 0, 0, 0, {}, {}, None)
        return replace(like, slot_words=changed_words(self.case.input.slot, self.slot),
                       memory_words=changed_words(initial_block(self.case.input), self.block))

    def offset(self, address, size):
        start = address - self.case.layout.block
        if not 0 <= start <= records.BLOCK_SIZE - size:
            raise ExpectationError(f"{_key(self.case)} accesses {size} bytes at {address:#x}, outside the block")
        return start

    def image(self, size):
        start = self.offset(operand_address(self.case, 0), size)
        return bytes(self.block[start:start + size])

    def capture(self, flags):
        ax, df = records.store_bytes(flags)
        struct.pack_into("<H", self.slot, records.STORE_AX, ax)
        self.slot[records.STORE_DF] = df

    def flag(self, name, value):
        ax = struct.unpack_from("<H", self.slot, records.STORE_AX)[0]
        self.capture({**records.store_flags(ax, self.slot[records.STORE_DF]), name: value})


def _key(case):
    return f"case {case.spec.index} of {case.instance.form.key}"


def _mnemonic(case):
    return case.instance.form.definition.mnemonic


def _vector(case, choice):
    return LOCATIONS[case.instance.operands[choice].name][1]


def _quadword(slot, vector, half):
    return struct.unpack_from("<Q", slot, records.ymm_offset(vector) + _QUADWORD_BYTES * half)[0]


def _mask(length):
    return (1 << length) - 1


def _sse4a_field(case):
    immediates = immediate_values(case.instance.operands)
    if immediates:
        length, index = immediates
    else:
        control = _quadword(case.input.slot, _vector(case, 1), 0 if _mnemonic(case) == "extrq" else 1)
        length, index = control, control >> 8
    length, index = length & _FIELD_BITS, index & _FIELD_BITS
    if length == 0:
        return (_QUADWORD_BITS, 0) if index == 0 else None
    return (length, index) if index + length <= _QUADWORD_BITS else None


def _bit_field(case):
    final = _Final.completed(case)
    field = _sse4a_field(case)
    if field is not None:
        length, index = field
        destination = _vector(case, 0)
        low = _quadword(case.input.slot, destination, 0)
        if _mnemonic(case) == "extrq":
            low = low >> index & _mask(length)
        else:
            source = _quadword(case.input.slot, _vector(case, 1), 0)
            low = low & ~(_mask(length) << index) | (source & _mask(length)) << index
        struct.pack_into("<Q", final.slot, records.ymm_offset(destination), low & _mask(_QUADWORD_BITS))
    return final.result()


def _page_fault(case, address, data):
    return Result(case.spec.index, Outcome.PAGE_FAULT, 0, address, data, {}, {}, None)


def _store_low(case, size):
    address = operand_address(case, 0)
    if address + size > case.layout.guard:
        return _page_fault(case, case.address, max(address, case.layout.guard))
    final = _Final.completed(case)
    start = final.offset(address, size)
    vector = records.ymm_offset(_vector(case, 1))
    final.block[start:start + size] = case.input.slot[vector:vector + size]
    return final.result()


def _taken(case):
    if case.instance.shape is Shape.CALL or _mnemonic(case) == "jmp":
        return True
    condition = _mnemonic(case)[1:]
    if condition in _CONDITIONS:
        return bool(_CONDITIONS[condition](case.spec.flags))
    if condition.startswith("n") and condition[1:] in _CONDITIONS:
        return not _CONDITIONS[condition[1:]](case.spec.flags)
    raise ExpectationError(f"{_key(case)}: {_mnemonic(case)} is no condition of a jcc")


def _near_branch16(case):
    entry = case.layout.instances[case.instance.index]
    end = entry.address + entry.length
    relative = entry.label(TARGET) - end
    if relative not in _REL16:
        raise ExpectationError(f"{_key(case)}: its target lies {relative:#x} from its end, beyond a rel16")
    if case.instance.shape is Shape.CALL:
        pushed = register_value(case.input.slot, "rsp") - _SHORT_RETURN_BYTES
        if pushed + _SHORT_RETURN_BYTES > case.layout.guard:
            return _page_fault(case, case.address, max(pushed, case.layout.guard))
    if not _taken(case):
        return _Final.completed(case, records.PATH_NOT_TAKEN).result()
    target = end + relative & _SHORT_TARGET
    return _page_fault(case, target, target)


def _unchanged(case):
    return _Final.completed(case).result()


def _clear_line(case):
    line = register_value(case.input.slot, "rax") & ~(_CACHE_LINE - 1)
    if line + _CACHE_LINE > case.layout.guard:
        return _page_fault(case, case.address, max(line, case.layout.guard))
    final = _Final.completed(case)
    start = final.offset(line, _CACHE_LINE)
    final.block[start:start + _CACHE_LINE] = bytes(_CACHE_LINE)
    return final.result()


_AMD_ONLY = {
    "extrq": _bit_field,
    "insertq": _bit_field,
    "movntsd": lambda case: _store_low(case, 8),
    "movntss": lambda case: _store_low(case, 4),
    "monitorx": _unchanged,
    "mwaitx": _unchanged,
    "clzero": _clear_line,
}


def _kernel_vector(case):
    immediates = immediate_values(case.instance.operands)
    if len(immediates) > 1:
        raise ExpectationError(f"{_key(case)} enters the console kernel with {len(immediates)} immediates, not one")
    return immediates[0] if immediates else 0


def _outside_detail(case, group):
    if group is Group.CONSOLE_KERNEL:
        return _kernel_vector(case)
    return _NOT_ON_PS5 if group is Group.NOT_ON_PS5 else 0


def _outside(case, group):
    return Result(case.spec.index, _OUTSIDE_STOPS[group], _outside_detail(case, group), case.address, 0, {}, {}, None)


def _written(case):
    definition = case.instance.form.definition
    if not in_set(definition):
        return _outside(case, classify(definition))
    if case.instance.shape in (Shape.BRANCH, Shape.CALL):
        return _near_branch16(case)
    if _mnemonic(case) not in _AMD_ONLY:
        raise ExpectationError(f"{_key(case)}: no expectation is written for {_mnemonic(case)}")
    return _AMD_ONLY[_mnemonic(case)](case)


def _source_value(case, final):
    source = case.instance.operands[1]
    if isinstance(source, Reg):
        return register_value(case.input.slot, source.name)
    size = form_model(case.instance.form).operand_size // 8
    start = final.offset(operand_address(case, 1), size)
    return int.from_bytes(initial_block(case.input)[start:start + size], "little")


def _bit_scan(case, recorded):
    if recorded.outcome is not Outcome.COMPLETED:
        return recorded
    final = _Final.recorded(case, recorded)
    if _source_value(case, final):
        return recorded
    destination = records.gpr_offset(LOCATIONS[case.instance.operands[0].name][1])
    final.slot[destination:destination + 8] = case.input.slot[destination:destination + 8]
    final.flag("ZF", 1)
    return final.result(recorded)


def _save_fields(case):
    mnemonic = _mnemonic(case)
    if mnemonic not in _ENVIRONMENT_SAVES:
        return _LEGACY_POINTERS[mnemonic.endswith("64")], (_MXCSR_MASK_FIELD,)
    model = form_model(case.instance.form)
    width = model.memory_width(model.explicit[0])
    return _ENVIRONMENT_POINTERS[width if mnemonic == "fnstenv" else width - _FSAVE_REGISTERS], ()


def _state_save(case, recorded):
    if recorded.outcome is not Outcome.COMPLETED:
        return recorded
    pointers, masks = _save_fields(case)
    final = _Final.recorded(case, recorded)
    initial = initial_block(case.input)
    start = final.offset(operand_address(case, 0), max(end for _, end in pointers + masks))
    pending = struct.unpack_from("<H", case.input.slot, records.FX_FSW)[0] & _ES
    for fields, value in ((() if pending else pointers, 0), (masks, AMD_MXCSR_MASK)):
        for begin, end in fields:
            span = slice(start + begin, start + end)
            if final.block[span] != initial[span]:
                final.block[span] = value.to_bytes(end - begin, "little")
    return final.result(recorded)


def _load_x87(final, image):
    slot = final.slot
    slot[_X87_FIELDS[0]:_X87_FIELDS[1]] = image[_X87_FIELDS[0]:_X87_FIELDS[1]]
    for number in range(8):
        offset = records.fx_st_offset(number)
        slot[offset:offset + _X87_REGISTER_BYTES] = image[offset:offset + _X87_REGISTER_BYTES]


def _initial_x87(final):
    initial = bytearray(records.FX_XMM)
    struct.pack_into("<H", initial, records.FX_FCW, records.FX_DEFAULT_FCW)
    _load_x87(final, initial)


def _load_mxcsr(final):
    loaded = final.case.spec.loads_mxcsr
    if loaded is None:
        raise ExpectationError(f"{_key(final.case)} loads MXCSR, but its spec gives no value")
    struct.pack_into("<I", final.slot, records.FX_MXCSR, loaded)


def _load_xmm(final, image):
    for number in range(records.YMM_COUNT):
        offset = records.ymm_offset(number)
        final.slot[offset:offset + _XMM_BYTES] = image[records.fx_xmm_offset(number):][:_XMM_BYTES]


def _load_upper_halves(final, area):
    for number in range(records.YMM_COUNT):
        offset = records.ymm_offset(number) + _XMM_BYTES
        final.slot[offset:offset + _XMM_BYTES] = area[_XMM_BYTES * number:_XMM_BYTES * (number + 1)]


def _ldmxcsr(case):
    final = _Final.completed(case)
    _load_mxcsr(final)
    return final


def _fxrstor(case):
    final = _Final.completed(case)
    image = final.image(records.FX_SIZE)
    _load_x87(final, image)
    _load_mxcsr(final)
    _load_xmm(final, image)
    return final


def _xrstor(case):
    final = _Final.completed(case)
    requested = register_value(case.input.slot, "edx") << 32 | register_value(case.input.slot, "eax")
    if requested & ~records.XSTATE_COMPONENTS:
        raise ExpectationError(f"{_key(case)} restores components {requested:#x}, beyond x87, SSE and AVX")
    image = final.image(records.XSAVE_AVX + records.XSAVE_AVX_SIZE)
    present, compaction = struct.unpack_from("<QQ", image, records.XSAVE_HEADER)
    if compaction & records.XSAVE_COMPACTED and present & ~compaction:
        raise ExpectationError(f"{_key(case)}: XSTATE_BV {present:#x} is not within XCOMP_BV {compaction:#x}, "
                               "which faults")
    if requested & records.XSTATE_X87 and present & records.XSTATE_X87:
        _load_x87(final, image)
    elif requested & records.XSTATE_X87:
        _initial_x87(final)
    if requested & records.XSTATE_SSE:
        _load_xmm(final, image if present & records.XSTATE_SSE else bytes(records.FX_SIZE))
    if case.spec.loads_mxcsr is not None:
        _load_mxcsr(final)
    if requested & records.XSTATE_AVX:
        _load_upper_halves(final, image[records.XSAVE_AVX:] if present & records.XSTATE_AVX
                           else bytes(records.XSAVE_AVX_SIZE))
    return final


_LOADS = {
    "ldmxcsr": _ldmxcsr, "vldmxcsr": _ldmxcsr, "fxrstor": _fxrstor, "fxrstor64": _fxrstor, "xrstor": _xrstor,
    "xrstor64": _xrstor,
}
_IMAGE_ALIGNMENT = {"ldmxcsr": 1, "vldmxcsr": 1, "fxrstor": 16, "fxrstor64": 16, "xrstor": 64, "xrstor64": 64}


def loaded_state(case):
    return _LOADS[_mnemonic(case)](case).result()


def _mxcsr_load(case, recorded):
    loaded = case.spec.loads_mxcsr
    if loaded is None or not loaded & records.MXCSR_MISALIGNED_SSE or loaded & ~AMD_MXCSR_MASK:
        return recorded
    if recorded.outcome is Outcome.COMPLETED:
        raise ExpectationError(f"{_key(case)} loads MXCSR {loaded:#x}, whose MM bit Intel refuses, but its recording "
                               "completes")
    if recorded.outcome is not Outcome.GENERAL_PROTECTION or recorded.address != case.address:
        return recorded
    if operand_address(case, 0) % _IMAGE_ALIGNMENT[_mnemonic(case)]:
        return recorded
    return loaded_state(case)


_PATCHES = {
    "bsf": _bit_scan,
    "bsr": _bit_scan,
    **{mnemonic: _state_save for mnemonic in _STATE_IMAGE_STORES},
    **{mnemonic: _mxcsr_load for mnemonic in _LOADS},
}


def expected_result(case, recorded):
    if expectation_written(case.instance.form):
        if recorded is not None:
            raise ExpectationError(f"{_key(case)} has a recording, but its form's expectations are written, not "
                                   "recorded")
        return _written(case)
    if recorded is None:
        raise NoExpectation(NO_EXPECTATION)
    patch = _PATCHES.get(_mnemonic(case))
    return recorded if patch is None else patch(case, recorded)


def undefined_bits(case):
    if _mnemonic(case) not in ("extrq", "insertq"):
        return NO_UNDEFINED_BITS
    destination = records.ymm_offset(_vector(case, 0))
    undefined = range(destination if _sse4a_field(case) is None else destination + _QUADWORD_BYTES,
                      destination + _XMM_BYTES)
    return bytes(0xFF if offset in undefined else 0 for offset in range(records.SLOT_SIZE))
