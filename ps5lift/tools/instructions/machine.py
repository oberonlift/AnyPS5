"""The parts of a case's initial machine state that the case schedule (`cases`) and the per-mnemonic rules (`rules`)
both build.

The memory block holds the memory operands at `MEMORY`, a gather's elements around `GATHER_BASE`, a string
instruction's source at `SOURCE` and destination at `DESTINATION`, and the stack below `STACK`, with the frames `leave`
and `enter` take at `LEAVE_FRAME` and `ENTER_FRAME`. MXCSR is one of the 13 `MXCSR_STATES`: the console's
(`CONSOLE_MXCSR`), each rounding control with every exception masked, FTZ alone (`FTZ_ALONE`), DAZ alone
(`DAZ_ALONE`), and each exception unmasked. An x87 control word takes a precision and rounding control from
`X87_CONTROLS` and exception masks from `X87_MASKS` (`x87_control_word`), and a status word the pending exceptions of
`X87_PENDING`; `X87_LOW_HALF` marks the low four registers valid. The x87 state is an FXSAVE image of
`X87_IMAGE_SIZE` bytes, whose abridged tag byte `abridged_tags` gives. An MMX register is an x87 register whose
exponent and sign bits are `MMX_EXPONENT`."""
from . import records

MEMORY = 1024
GATHER_BASE = 1536
SOURCE = 1536
DESTINATION = 2560
STACK = 3072
LEAVE_FRAME = STACK + 64
ENTER_FRAME = STACK + 256

CONSOLE_MXCSR = 0x9FC0
FTZ_ALONE = CONSOLE_MXCSR & ~records.MXCSR_DAZ
DAZ_ALONE = CONSOLE_MXCSR & ~records.MXCSR_FTZ
MXCSR_STATES = (
    CONSOLE_MXCSR,
    *(0x1F80 | rounding << 13 for rounding in range(4)),
    FTZ_ALONE,
    DAZ_ALONE,
    *(0x1F80 & ~(1 << 7 + exception) for exception in range(6)),
)

X87_IMAGE_SIZE = records.FX_XMM
X87_CONTROLS = tuple((precision, rounding) for precision in (0, 2, 3) for rounding in range(4))
X87_MASKS = (0x3F, *(0x3F & ~(1 << exception) for exception in range(6)))
X87_PENDING = (0, 0x20, 0x30, 0x01, 0x02)
X87_LOW_HALF = 0x0F
MMX_EXPONENT = 0xFFFF << 64

_X87_RESERVED_CONTROL = 0x40


def x87_control_word(number):
    precision, rounding = X87_CONTROLS[number % len(X87_CONTROLS)]
    return _X87_RESERVED_CONTROL | X87_MASKS[number % len(X87_MASKS)] | precision << 8 | rounding << 10


def abridged_tags(top, valid):
    return sum(1 << (top + number) % 8 for number in range(8) if valid >> number & 1)
