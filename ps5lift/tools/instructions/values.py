"""The values the instruction cases draw their inputs from.

Integers take 0, 1, 2, all ones, the minimum and maximum of their width and of each narrower width, and the values
either side of the sign and carry boundaries. Shift and rotate counts take 0, 1, the width less one, the width and one
more, the 5-bit and 6-bit mask boundaries and 255. Each floating-point format takes both zeros, both signs of the
smallest and largest denormal and normal, both infinities, a quiet and a signalling NaN with payloads, 1.0, the value
after 1.0 and half an ulp of 1.0, whose sum with 1.0 is a rounding tie. A conversion adds the values either side of
its integer format's limits or the narrower format's range. `reduction_values` gives the extended values either side
of k·π/2 for k from 1 to 8, 1000, 2^20, 2^40 and 2^62, where a trigonometric reduction with too short a π loses every
bit; k·π/2 is computed with `decimal` at `tolerance.DIGITS` digits.

`Values` cycles an edge list through the lanes of a value: lane j of the value that starts at s takes edge (s + j) mod
n. `Random` is the one source of randomness: splitmix64 keyed by a seed and integer identifiers, so the same seed and
identifiers always give the same values."""
import functools
from dataclasses import dataclass
from decimal import localcontext
from fractions import Fraction

from . import tolerance

MASK64 = (1 << 64) - 1
_GOLDEN = 0x9E3779B97F4A7C15
_NARROWER_WIDTHS = (8, 16, 32)
_COUNT_EDGES = (0, 1, 31, 32, 63, 64, 255)
_WIDE_COUNTS = (1 << 32, 1 << 63)
_PAYLOADS = (0x123456789, 0x987654321)
_RANDOM_COUNT_LIMIT = 256
_REDUCTION_MULTIPLES = (1, 2, 3, 4, 5, 6, 7, 8, 1000, 1 << 20, 1 << 40, 1 << 62)


def _mix64(value):
    value = (value ^ value >> 30) * 0xBF58476D1CE4E5B9 & MASK64
    value = (value ^ value >> 27) * 0x94D049BB133111EB & MASK64
    return value ^ value >> 31


def text_key(text):
    value = 0xCBF29CE484222325
    for byte in text.encode():
        value = (value ^ byte) * 0x100000001B3 & MASK64
    return value


class Random:
    __slots__ = ("state",)

    def __init__(self, seed, *identifiers):
        state = seed
        for identifier in identifiers:
            state = _mix64((state ^ identifier) + _GOLDEN & MASK64)
        self.state = state

    def word(self):
        self.state = self.state + _GOLDEN & MASK64
        return _mix64(self.state)

    def bits(self, count):
        value = 0
        for shift in range(0, count, 64):
            value |= self.word() << shift
        return value & (1 << count) - 1

    def below(self, bound):
        return self.word() % bound


class Values:
    __slots__ = ("edges", "random_lane", "lane_bits", "lanes")

    def __init__(self, edges, random_lane, lane_bits=0, lanes=1):
        self.edges = tuple(edges)
        self.random_lane = random_lane
        self.lane_bits = lane_bits
        self.lanes = lanes

    def __len__(self):
        return len(self.edges)

    def value(self, start):
        edges = self.edges
        if self.lanes == 1:
            return edges[start % len(edges)]
        value = 0
        for lane in range(self.lanes):
            value |= edges[(start + lane) % len(edges)] << lane * self.lane_bits
        return value

    def random(self, random):
        value = 0
        for lane in range(self.lanes):
            value |= self.random_lane(random) << lane * self.lane_bits
        return value


def random_bits(bits):
    return lambda random: random.bits(bits)


def _random_count(random):
    return random.below(_RANDOM_COUNT_LIMIT)


def choice_values(edges):
    edges = tuple(edges)
    return Values(edges, lambda random: edges[random.below(len(edges))])


def integer_edges(bits):
    ones = (1 << bits) - 1
    top = 1 << bits - 1
    values = [0, 1, 2, ones, ones - 1, top, top + 1, top - 1, top - 2]
    for narrower in _NARROWER_WIDTHS:
        if narrower < bits:
            values += [(1 << narrower) - 1, 1 << narrower, (1 << narrower - 1) - 1, 1 << narrower - 1]
    return tuple(dict.fromkeys(values))


def count_edges(bits):
    return tuple(sorted({bits - 1, bits, bits + 1, *_COUNT_EDGES}))


@functools.lru_cache(maxsize=None)
def integer_values(bits, lanes=1):
    return Values(integer_edges(bits), random_bits(bits), bits, lanes)


@functools.lru_cache(maxsize=None)
def count_values(width, lane_bits, lanes=1):
    edges = count_edges(width) + (_WIDE_COUNTS if lane_bits == 64 else ())
    return Values(edges, _random_count, lane_bits, lanes)


@dataclass(frozen=True)
class FloatFormat:
    bits: int
    exponent_bits: int
    fraction_bits: int
    integer_bit: bool

    @property
    def bias(self):
        return (1 << self.exponent_bits - 1) - 1

    @property
    def precision(self):
        return self.fraction_bits + 1

    @property
    def top_exponent(self):
        return (1 << self.exponent_bits) - 1

    def pack(self, negative, exponent, fraction):
        integer = 1 << self.fraction_bits if self.integer_bit and exponent else 0
        return negative << self.bits - 1 | exponent << self.bits - 1 - self.exponent_bits | integer | fraction

    def _fields(self, magnitude):
        exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
        if Fraction(2) ** exponent > magnitude:
            exponent -= 1
        biased = exponent + self.bias
        if biased <= 0:
            return 0, magnitude * Fraction(2) ** (self.bias - 1 + self.fraction_bits)
        return biased, (magnitude / Fraction(2) ** exponent - 1) * 2**self.fraction_bits

    def exact(self, value):
        magnitude = abs(value)
        if magnitude == 0:
            return self.pack(int(value < 0), 0, 0)
        field, scaled = self._fields(magnitude)
        if scaled.denominator != 1 or field >= self.top_exponent:
            raise ValueError(f"{value} is not exact in {self.bits}-bit floating point")
        return self.pack(int(value < 0), field, int(scaled))

    def neighbours(self, value):
        if value <= 0:
            raise ValueError(f"{value} is not positive")
        field, scaled = self._fields(value)
        if scaled.denominator == 1:
            raise ValueError(f"{value} is exact in {self.bits}-bit floating point, so it has no neighbours either side")
        fraction = scaled.numerator // scaled.denominator
        above = (field, fraction + 1) if fraction + 1 < 1 << self.fraction_bits else (field + 1, 0)
        if above[0] >= self.top_exponent:
            raise ValueError(f"{value} lies past the largest finite {self.bits}-bit value")
        return self.pack(0, field, fraction), self.pack(0, *above)


FLOAT_FORMATS = {
    16: FloatFormat(16, 5, 10, False),
    32: FloatFormat(32, 8, 23, False),
    64: FloatFormat(64, 11, 52, False),
    80: FloatFormat(80, 15, 63, True),
}


def float_edges(bits):
    form = FLOAT_FORMATS[bits]
    fraction = (1 << form.fraction_bits) - 1
    quiet = 1 << form.fraction_bits - 1
    payload = quiet - 1
    top = form.top_exponent
    magnitudes = [(0, 0), (0, 1), (0, fraction), (1, 0), (top - 1, fraction), (top, 0)]
    values = [form.pack(negative, exponent, field) for exponent, field in magnitudes for negative in (0, 1)]
    values += [
        form.pack(0, top, quiet | _PAYLOADS[0] & payload),
        form.pack(1, top, _PAYLOADS[1] & payload),
        form.pack(0, form.bias, 0),
        form.pack(0, form.bias, 1),
        form.pack(0, form.bias - form.precision, 0),
    ]
    return tuple(values)


def integer_limit_edges(bits, integer_bits):
    form = FLOAT_FORMATS[bits]
    limit = Fraction(2) ** (integer_bits - 1)
    values = [limit - Fraction(2) ** (integer_bits - 1 - form.precision), limit, -limit]
    values.append(-limit - Fraction(2) ** (integer_bits - form.precision))
    if form.precision >= integer_bits:
        values.append(limit - Fraction(1, 2))
    return tuple(dict.fromkeys(form.exact(value) for value in values))


def narrowing_edges(bits, narrower_bits):
    form, narrow = FLOAT_FORMATS[bits], FLOAT_FORMATS[narrower_bits]
    largest = (2 - Fraction(2) ** -narrow.fraction_bits) * Fraction(2) ** narrow.bias
    smallest_normal = Fraction(2) ** (1 - narrow.bias)
    smallest = Fraction(2) ** (1 - narrow.bias - narrow.fraction_bits)
    return tuple(form.exact(value) for value in (largest, smallest_normal, smallest, smallest / 2))


@functools.lru_cache(maxsize=None)
def reduction_values():
    with localcontext() as context:
        context.prec = tolerance.DIGITS
        half_pi = tolerance.pi() / 2
        multiples = [Fraction(multiple * half_pi) for multiple in _REDUCTION_MULTIPLES]
    return tuple(value for multiple in multiples for value in FLOAT_FORMATS[80].neighbours(multiple))


def random_float80(random):
    return FLOAT_FORMATS[80].pack(random.below(2), random.below(1 << 15), random.bits(63))


@functools.lru_cache(maxsize=None)
def float_values(bits, lanes, supplements=()):
    random_lane = random_float80 if bits == 80 else random_bits(bits)
    return Values(dict.fromkeys(float_edges(bits) + supplements), random_lane, bits, lanes)


def bcd(digits, negative):
    value = 0
    for position, digit in enumerate(reversed(str(digits))):
        value |= int(digit) << 4 * position
    return value | negative << 79


def _random_bcd(random):
    return bcd(random.below(10**18), random.below(2))


BCD_VALUES = Values(
    tuple(bcd(digits, negative) for digits in (0, 1, 10**18 - 1, 123456789012345678) for negative in (0, 1)),
    _random_bcd,
    80,
)
