"""The comparisons that are not exact: the approximations `rcpps` and `rsqrtps` and the x87 transcendentals.

`within_rcp` accepts a result within the relative error both vendors document for the approximations, and requires
zeros, infinities and NaNs exactly. `x87_reference` computes the true result of an x87 transcendental with `decimal` at
`DIGITS` digits, rounded to a precision of 24, 53 or 64 bits with the x87 rounding control, and gives the results in the
stack order they end in (`fsincos` leaves the cosine in ST(0), `fptan` leaves 1.0). It gives None for an operand outside
the instruction's documented range, or for an operand or result that is no normal number (NaN, infinity, zero,
denormal, an unnormal encoding, or a result past the extended range): such a case is compared exactly. `within_ulp`
accepts a result within one unit in the last place of the reference, at the reference's binade and the given
precision. Values are bit patterns: integers of the float's width, and 80-bit extended values as 80-bit integers."""
from decimal import ROUND_HALF_EVEN, Decimal, localcontext
from fractions import Fraction

RCP_RELATIVE_ERROR = Fraction(3, 2) / 2**12
DIGITS = 80
OPERAND_COUNTS = {
    "fsin": 1, "fcos": 1, "fsincos": 1, "fptan": 1, "f2xm1": 1, "fpatan": 2, "fyl2x": 2, "fyl2xp1": 2,
}
PRECISIONS = (24, 53, 64)
NEAREST, DOWN, UP, TOWARD_ZERO = range(4)

_REDUCTION_DIGITS = 20
FLOAT_FORMATS = {16: (5, 10), 32: (8, 23), 64: (11, 52)}
_EXTENDED_BIAS = 16383
_EXTENDED_MAXIMUM_EXPONENT = 0x7FFF
_EXTENDED_SIGNIFICAND_BITS = 64
_SIGN_80 = 1 << 79
_INTEGER_BIT = 1 << 63
_MASK64 = (1 << 64) - 1
_TRIGONOMETRY_LIMIT = Decimal(2) ** 63
_F2XM1_LIMIT = Decimal(1)
_ATAN_STEP = Decimal("0.1")


class ToleranceError(Exception):
    pass


def _float_value(bits, width):
    if width not in FLOAT_FORMATS:
        raise ToleranceError(f"no float format of {width} bits")
    exponent_bits, fraction_bits = FLOAT_FORMATS[width]
    maximum = (1 << exponent_bits) - 1
    exponent = bits >> fraction_bits & maximum
    fraction = bits & (1 << fraction_bits) - 1
    sign = -1 if bits >> width - 1 & 1 else 1
    if exponent == maximum:
        return None
    if exponent == 0:
        return sign * Fraction(fraction, 1 << fraction_bits) * Fraction(2) ** (2 - (1 << exponent_bits - 1))
    bias = (1 << exponent_bits - 1) - 1
    return sign * Fraction((1 << fraction_bits) + fraction, 1 << fraction_bits) * Fraction(2) ** (exponent - bias)


def within_rcp(expected, actual, width):
    expected_value = _float_value(expected, width)
    if expected_value is None or expected_value == 0:
        return actual == expected
    actual_value = _float_value(actual, width)
    if actual_value is None:
        return False
    return abs(actual_value - expected_value) <= RCP_RELATIVE_ERROR * abs(expected_value)


def _extended_value(bits):
    exponent = bits >> _EXTENDED_SIGNIFICAND_BITS & _EXTENDED_MAXIMUM_EXPONENT
    significand = bits & _MASK64
    if exponent in (0, _EXTENDED_MAXIMUM_EXPONENT) or not significand & _INTEGER_BIT:
        return None
    sign = -1 if bits & _SIGN_80 else 1
    return sign * Fraction(significand) * Fraction(2) ** (exponent - _EXTENDED_BIAS - (_EXTENDED_SIGNIFICAND_BITS - 1))


def _exponent(magnitude):
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    return exponent - 1 if Fraction(2) ** exponent > magnitude else exponent


def _round_extended(value, precision, rounding):
    if precision not in PRECISIONS:
        raise ToleranceError(f"an x87 precision is one of {PRECISIONS} bits, not {precision}")
    if value == 0:
        return None
    negative = value < 0
    magnitude = Fraction(abs(value))
    exponent = _exponent(magnitude)
    scaled = magnitude / Fraction(2) ** (exponent - precision + 1)
    significand = scaled.numerator // scaled.denominator
    remainder = scaled - significand
    if rounding == NEAREST:
        significand += remainder > Fraction(1, 2) or (remainder == Fraction(1, 2) and significand & 1)
    elif rounding == DOWN:
        significand += bool(remainder) and negative
    elif rounding == UP:
        significand += bool(remainder) and not negative
    elif rounding != TOWARD_ZERO:
        raise ToleranceError(f"an x87 rounding control is 0 to 3, not {rounding}")
    if significand == 1 << precision:
        significand >>= 1
        exponent += 1
    biased = exponent + _EXTENDED_BIAS
    if not 0 < biased < _EXTENDED_MAXIMUM_EXPONENT:
        return None
    sign = _SIGN_80 if negative else 0
    return sign | biased << _EXTENDED_SIGNIFICAND_BITS | significand << _EXTENDED_SIGNIFICAND_BITS - precision


def _pi():
    with localcontext() as context:
        context.prec += 2
        total, previous, term = Decimal(3), 0, Decimal(3)
        numerator, numerator_step, denominator, denominator_step = 1, 0, 0, 24
        while total != previous:
            previous = total
            numerator, numerator_step = numerator + numerator_step, numerator_step + 8
            denominator, denominator_step = denominator + denominator_step, denominator_step + 32
            term = term * numerator / denominator
            total += term
    return +total


def _series(first, ratio):
    total, term, index = first, first, 0
    while True:
        index += 1
        term = ratio(term, index)
        if total + term == total:
            return total
        total += term


def _sine_series(x):
    square = x * x
    return _series(x, lambda term, k: -term * square / ((2 * k) * (2 * k + 1)))


def _cosine_series(x):
    square = x * x
    return _series(Decimal(1), lambda term, k: -term * square / ((2 * k - 1) * (2 * k)))


def _sine_cosine(x):
    with localcontext() as context:
        context.prec += _REDUCTION_DIGITS
        half_pi = _pi() / 2
        quadrant = (x / half_pi).to_integral_value(ROUND_HALF_EVEN)
        reduced = x - quadrant * half_pi
    sine, cosine = _sine_series(reduced), _cosine_series(reduced)
    return {0: (sine, cosine), 1: (cosine, -sine), 2: (-sine, -cosine), 3: (-cosine, sine)}[int(quadrant) % 4]


def _arctangent(t):
    if t < 0:
        return -_arctangent(-t)
    if t > 1:
        return _pi() / 2 - _arctangent(1 / t)
    halvings = 0
    while t > _ATAN_STEP:
        t = t / (1 + (1 + t * t).sqrt())
        halvings += 1
    square = t * t
    return _series(t, lambda term, k: -term * square * (2 * k - 1) / (2 * k + 1)) * 2**halvings


def _exp_minus_one(y):
    return _series(y, lambda term, k: term * y / (k + 1))


def _log1p(x):
    z = x / (2 + x)
    square = z * z
    return 2 * _series(z, lambda term, k: term * square * (2 * k - 1) / (2 * k + 1))


def _true_results(mnemonic, x, y):
    if mnemonic in ("fsin", "fcos", "fsincos", "fptan"):
        if abs(x) >= _TRIGONOMETRY_LIMIT:
            return None
        sine, cosine = _sine_cosine(x)
        return {"fsin": (sine,), "fcos": (cosine,), "fsincos": (cosine, sine), "fptan": (Decimal(1), sine / cosine)}[
            mnemonic]
    if mnemonic == "f2xm1":
        return (_exp_minus_one(x * Decimal(2).ln()),) if abs(x) <= _F2XM1_LIMIT else None
    if mnemonic == "fpatan":
        angle = _arctangent(y / x)
        if x < 0:
            angle += _pi() if y > 0 else -_pi()
        return (angle,)
    if mnemonic == "fyl2x":
        return (y * x.ln() / Decimal(2).ln(),) if x > 0 else None
    if mnemonic == "fyl2xp1":
        limit = 1 - Decimal(2).sqrt() / 2
        return (y * _log1p(x) / Decimal(2).ln(),) if abs(x) < limit else None
    raise ToleranceError(f"no reference for {mnemonic}")


def x87_reference(mnemonic, operands, precision, rounding):
    if mnemonic not in OPERAND_COUNTS:
        raise ToleranceError(f"{mnemonic} is no x87 transcendental: those are {sorted(OPERAND_COUNTS)}")
    if len(operands) != OPERAND_COUNTS[mnemonic]:
        raise ToleranceError(f"{mnemonic} takes {OPERAND_COUNTS[mnemonic]} operands, not {len(operands)}")
    values = [_extended_value(operand) for operand in operands]
    if any(value is None for value in values):
        return None
    with localcontext() as context:
        context.prec = DIGITS
        x = Decimal(values[0].numerator) / Decimal(values[0].denominator)
        y = Decimal(values[1].numerator) / Decimal(values[1].denominator) if len(values) > 1 else None
        results = _true_results(mnemonic, x, y)
        if results is None:
            return None
        rounded = tuple(_round_extended(Fraction(result), precision, rounding) for result in results)
    return None if any(value is None for value in rounded) else rounded


def within_ulp(reference, actual, precision):
    reference_value = _extended_value(reference)
    if reference_value is None:
        raise ToleranceError(f"a reference is a normal extended value, not {reference:#x}")
    actual_value = _extended_value(actual)
    if actual_value is None:
        return False
    unit = Fraction(2) ** (_exponent(abs(reference_value)) - precision + 1)
    return abs(actual_value - reference_value) <= unit
