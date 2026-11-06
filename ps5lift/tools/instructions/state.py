"""A case and the state it ends in, as the expectations and the comparator both read them.

A `Case` is one case of a test group: its spec, its instance, its input and the guest program's layout. A `State` is
how a case ended: its outcome, the full final slot (the input slot with the result's words applied) and the changed
block words. `initial_block` is the block a case starts with, `apply_words` lays a result's words over a slot or a block
and `changed_words` gives the words in which two images differ, as a driver records them. `operand_address` is the
address a memory operand of the instance takes from the case's input, and `register_value` reads a register by any of
its names from a slot."""
import struct
from dataclasses import dataclass
from typing import Optional

from . import records
from .cases import CaseSpec, Instance, expression_offset, form_model
from .encoding import Mem
from .model import LOCATIONS
from .program import Layout
from .records import CaseInput, Outcome

WORD_BYTES = 8


class StateError(Exception):
    pass


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
        return cls(result.outcome, result.detail, result.address, result.data,
                   apply_words(slot, result.slot_words, f"case {result.case}'s slot"), result.memory_words,
                   result.expected)

    @classmethod
    def stop(cls, outcome, address):
        return cls(outcome, 0, address, 0, None, {}, None)


def apply_words(image, words, what):
    final = bytearray(image)
    for offset, value in words.items():
        if offset % WORD_BYTES or not 0 <= offset <= len(image) - WORD_BYTES:
            raise StateError(f"{what} holds a word at {offset}, which is no word of its {len(image)} bytes")
        struct.pack_into("<Q", final, offset, value)
    return bytes(final)


def changed_words(before, after):
    if len(before) != len(after):
        raise StateError(f"images of {len(before)} and {len(after)} bytes have no words in common")
    return {
        offset: int.from_bytes(after[offset:offset + WORD_BYTES], "little")
        for offset in range(0, len(before), WORD_BYTES)
        if before[offset:offset + WORD_BYTES] != after[offset:offset + WORD_BYTES]
    }


def initial_block(case_input):
    block = bytearray(records.fill_block(case_input.fill_seed))
    for offset, data in case_input.patches:
        block[offset:offset + len(data)] = data
    return bytes(block)


def register_value(slot, name):
    _, number, shift, bits = LOCATIONS[name]
    word = int.from_bytes(slot[records.gpr_offset(number):records.gpr_offset(number) + WORD_BYTES], "little")
    return word >> shift & (1 << bits) - 1


def operand_address(case, choice):
    memory = case.instance.operands[choice]
    if not isinstance(memory, Mem):
        raise StateError(f"{case.instance.form.key}: operand {choice} is no memory operand")
    if memory.base in (None, "rip"):
        return case.layout.block + expression_offset(memory.displacement)
    total = register_value(case.input.slot, memory.base) + memory.displacement
    if memory.index is not None:
        total += register_value(case.input.slot, memory.index) * memory.scale
    return total & (1 << form_model(case.instance.form).address_size) - 1
