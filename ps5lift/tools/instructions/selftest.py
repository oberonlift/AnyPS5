"""The recorder's self-test: a small guest program whose results only a working driver on a working host gives, which
`record.py` checks before it records any group.

Its first five cases cover the outcomes a recording rests on: `mov rax, rbx` completes with rbx's value in rax, `ud2`
is an invalid opcode, `div rbx` with rbx 0 a divide error, `mov rax, [rbx]` with rbx 7 bytes before the guard a page
fault at the guard's first byte, and `int3` a breakpoint at its own address. Their rsp is no address, so a fault's
signal is handled on the driver's alternate stack or not at all. Then for each of `FILL_SEEDS`, two `rep movsb` cases
copy each half of the block over the other, so that their memory words show every word of the driver's fill, which must
be what `records.fill_block` gives.

`program(forms, decoder)` returns the instances and case specs. `expectations(layout, instances, specs)` gives what
each case's result holds, as JSON-ready dicts: the outcome, address and data, slot words that the result must hold (`slot`), and
its memory words exactly (`memory`). A fault records no words at all."""
from . import records
from .cases import DEFAULT_X87_IMAGE, SEED, BlockOffset, CaseSpec, Instance
from .database import test_group
from .encoding import Mem, Reg, encode
from .machine import CONSOLE_MXCSR
from .model import Shape
from .values import MASK64
from .verification import verify_all

GROUP = "base"
FILL_SEEDS = (0, 1, MASK64, SEED)

_HALF = records.BLOCK_SIZE // 2
_WORD = 8
_LOAD_WIDTH = 8
_MOVED = 0x0123456789ABCDEF
_NO_ADDRESS = 0x0505050505050505
_MOV, _UD2, _DIV, _LOAD, _INT3, _MOVSB = range(6)
_PLAN = (
    ("mov", None, 0x8B, 64, {"modrm_mod": "3"}, (Reg("rax"), Reg("rbx"))),
    ("ud2", "0f", 0x0B, None, {}, ()),
    ("div", None, 0xF7, 64, {"modrm_mod": "3"}, (Reg("rbx"),)),
    ("mov", None, 0x8B, 64, {"modrm_mod": "!3"}, (Reg("rax"), Mem("rbx"))),
    ("int3", None, 0xCC, None, {}, ()),
    ("movsb", None, 0xA4, None, {"mandatory_prefix": "f3"}, ()),
)


class SelfTestError(Exception):
    pass


def _gpr(name):
    return records.GPR_NAMES.index(name)


def _form(forms, mnemonic, opcode_map, opcode, operand_size, filters):
    matches = [
        form for form in forms
        if form.definition.mnemonic == mnemonic and form.definition.encoding is None
        and form.definition.opcode_map == opcode_map and form.definition.opcode == opcode
        and form.operand_size == operand_size
        and all(form.definition.filters.get(name) == value for name, value in filters.items())
    ]
    if len(matches) != 1:
        raise SelfTestError(f"the self-test's {mnemonic} matches {len(matches)} forms, not 1: "
                            f"{[form.key for form in matches]}")
    if test_group(matches[0]) != GROUP:
        raise SelfTestError(f"the self-test's {matches[0].key} belongs to {test_group(matches[0])}, not {GROUP}")
    return matches[0]


def _instances(forms, decoder):
    batch = []
    for mnemonic, opcode_map, opcode, operand_size, filters, operands in _PLAN:
        form = _form(forms, mnemonic, opcode_map, opcode, operand_size, filters)
        batch.append((form, operands, (), tuple(encode(form, operands))))
    decoded = verify_all(decoder, batch)
    return tuple(
        Instance(index, form, operands, prefixes, Shape.STRAIGHT, encoded, decoding.text, "self-test")
        for index, ((form, operands, prefixes, encoded), decoding) in enumerate(zip(batch, decoded))
    )


def _plan():
    yield _MOV, {"rbx": _MOVED}
    yield _UD2, {}
    yield _DIV, {"rbx": 0}
    yield _LOAD, {"rbx": BlockOffset(records.BLOCK_SIZE - _LOAD_WIDTH + 1)}
    yield _INT3, {}
    for _ in FILL_SEEDS:
        for source in (0, _HALF):
            yield _MOVSB, {"rsi": BlockOffset(source), "rdi": BlockOffset(source ^ _HALF), "rcx": _HALF}


def _fill_seeds():
    fixed = [SEED + number for number in range(_MOVSB)]
    return fixed + [seed for seed in FILL_SEEDS for _ in range(2)]


def program(forms, decoder):
    instances = _instances(forms, decoder)
    specs = []
    for index, ((instance, registers), seed) in enumerate(zip(_plan(), _fill_seeds())):
        gprs = [_NO_ADDRESS + number for number in range(len(records.GPR_NAMES))]
        for name, value in registers.items():
            gprs[_gpr(name)] = value
        specs.append(CaseSpec(
            index=index,
            instance=instance,
            gprs=tuple(gprs),
            vectors=tuple(bytes(records.YMM_STRIDE) for _ in range(records.YMM_COUNT)),
            flags={name: 0 for name in records.FLAG_NAMES},
            mxcsr=CONSOLE_MXCSR,
            x87=DEFAULT_X87_IMAGE,
            patches=(),
            fixups=(),
            fill_seed=seed,
            twin=None,
            loads_mxcsr=None,
            console_defined=False,
            changeable_words=0,
        ))
    return instances, tuple(specs)


def _moved_words(spec, layout):
    source, destination = (spec.gprs[_gpr(name)].offset for name in ("rsi", "rdi"))
    block = records.fill_block(spec.fill_seed)
    words = {}
    for offset in range(0, _HALF, _WORD):
        before = block[destination + offset:destination + offset + _WORD]
        after = block[source + offset:source + offset + _WORD]
        if before != after:
            words[destination + offset] = int.from_bytes(after, "little")
    registers = {
        records.gpr_offset(_gpr("rcx")): 0,
        records.gpr_offset(_gpr("rsi")): layout.block + source + _HALF,
        records.gpr_offset(_gpr("rdi")): layout.block + destination + _HALF,
    }
    return registers, words


def _expectation(spec, layout):
    address = layout.instances[spec.instance].address
    if spec.instance == _MOV:
        return "COMPLETED", 0, 0, {records.gpr_offset(_gpr("rax")): _MOVED}, {}
    if spec.instance == _UD2:
        return "INVALID_OPCODE", address, 0, {}, {}
    if spec.instance == _DIV:
        return "DIVIDE_ERROR", address, 0, {}, {}
    if spec.instance == _LOAD:
        return "PAGE_FAULT", address, layout.guard, {}, {}
    if spec.instance == _INT3:
        return "BREAKPOINT", address, 0, {}, {}
    if spec.instance == _MOVSB:
        return ("COMPLETED", 0, 0, *_moved_words(spec, layout))
    raise SelfTestError(f"no expectation for the self-test's instance {spec.instance}")


def expectations(layout, instances, specs):
    result = []
    for spec in specs:
        outcome, address, data, slot, memory = _expectation(spec, layout)
        result.append({
            "case": spec.index,
            "name": f"{instances[spec.instance].text} (fill seed {spec.fill_seed:#x})",
            "outcome": outcome,
            "address": address,
            "data": data,
            "slot": {str(offset): value for offset, value in slot.items()},
            "memory": {str(offset): value for offset, value in memory.items()},
        })
    return result
