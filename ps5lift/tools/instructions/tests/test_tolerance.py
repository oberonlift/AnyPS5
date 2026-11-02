import struct
import unittest
from fractions import Fraction

from instructions import tolerance

ONE_80 = 0x3FFF8000000000000000
TWO_62_80 = (16383 + 62) << 64 | 1 << 63
TWO_63_80 = (16383 + 63) << 64 | 1 << 63
BELOW_TWO_63_80 = (16383 + 62) << 64 | (1 << 64) - 1
INFINITY_80 = 0x7FFF8000000000000000
SIN_1 = 0x3FFED76AA47848677021
SIN_2_62 = 0xBFFEB3F2B9AAF80A3946
NEAREST, DOWN, UP, TOWARD_ZERO = range(4)


def _float32(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _bits_of(fraction):
    bits = _float32(float(fraction))
    if Fraction(struct.unpack("<f", struct.pack("<I", bits))[0]) != fraction:
        raise AssertionError(f"{fraction} is not a float32")
    return bits


class RcpTest(unittest.TestCase):
    def test_rcp_bound(self):
        bound = Fraction(3, 2) * Fraction(1, 2**12)
        for expected in (Fraction(1), Fraction(-3, 4), Fraction(5, 2**20)):
            at_bound = expected + abs(expected) * bound
            past = _bits_of(at_bound) + 1 if expected > 0 else _bits_of(at_bound) - 1
            below = expected - abs(expected) * bound
            with self.subTest(expected=expected):
                self.assertTrue(tolerance.within_rcp(_bits_of(expected), _bits_of(at_bound), 32))
                self.assertTrue(tolerance.within_rcp(_bits_of(expected), _bits_of(below), 32))
                self.assertFalse(tolerance.within_rcp(_bits_of(expected), past, 32))

    def test_rcp_exact_specials(self):
        infinity, zero, nan = 0x7F800000, 0x00000000, 0x7FC00000
        self.assertTrue(tolerance.within_rcp(infinity, infinity, 32))
        self.assertFalse(tolerance.within_rcp(infinity, 0x7F7FFFFF, 32))
        self.assertFalse(tolerance.within_rcp(zero, 0x00000001, 32))
        self.assertFalse(tolerance.within_rcp(zero, 0x80000000, 32))
        self.assertTrue(tolerance.within_rcp(nan, nan, 32))
        self.assertFalse(tolerance.within_rcp(nan, 0x7FC00001, 32))
        self.assertFalse(tolerance.within_rcp(_float32(2.0), nan, 32))


class X87ReferenceTest(unittest.TestCase):
    def test_fsin_reference(self):
        self.assertEqual(tolerance.x87_reference("fsin", (ONE_80,), NEAREST), {0: SIN_1})
        self.assertEqual(tolerance.x87_reference("fsin", (TWO_62_80,), NEAREST), {0: SIN_2_62})
        self.assertEqual(tolerance.x87_reference("fsin", (ONE_80,), TOWARD_ZERO), {0: SIN_1 - 1})
        self.assertEqual(tolerance.x87_reference("fsin", (ONE_80,), DOWN), {0: SIN_1 - 1})
        self.assertEqual(tolerance.x87_reference("fsin", (ONE_80,), UP), {0: SIN_1})
        for actual, inside in ((SIN_1, True), (SIN_1 + 1, True), (SIN_1 - 1, True), (SIN_1 + 2, False),
                               (SIN_1 - 2, False), (SIN_1 ^ 1 << 79, False), (SIN_1 + (1 << 39), False)):
            with self.subTest(actual=hex(actual)):
                self.assertEqual(tolerance.within_ulp(SIN_1, actual), inside)

    def test_ulp_at_a_power_of_two(self):
        below_one = 0x3FFEFFFFFFFFFFFFFFFF
        for actual, inside in ((ONE_80, True), (ONE_80 + 1, True), (below_one, True), (ONE_80 + 2, False),
                               (below_one - 1, False)):
            with self.subTest(actual=hex(actual)):
                self.assertEqual(tolerance.within_ulp(ONE_80, actual), inside)
        self.assertTrue(tolerance.within_ulp(below_one, ONE_80))
        self.assertFalse(tolerance.within_ulp(below_one, ONE_80 + 1))
        self.assertTrue(tolerance.within_ulp(ONE_80 | 1 << 79, below_one | 1 << 79))
        self.assertFalse(tolerance.within_ulp(ONE_80 | 1 << 79, (below_one - 1) | 1 << 79))

    def test_fsin_out_of_range_exact(self):
        for operand in (TWO_63_80, TWO_63_80 | 1 << 79, INFINITY_80, TWO_63_80 + (1 << 64)):
            with self.subTest(operand=hex(operand)):
                self.assertIsNone(tolerance.x87_reference("fsin", (operand,), NEAREST))
        self.assertIsNotNone(tolerance.x87_reference("fsin", (BELOW_TWO_63_80,), NEAREST))
        self.assertFalse(tolerance.within_ulp(SIN_1, INFINITY_80))

    def test_approximated_results_by_stack_slot(self):
        self.assertEqual(tolerance.x87_reference("fsincos", (ONE_80,), NEAREST), {0: 0x3FFE8A51407DA8345C92, 1: SIN_1})
        self.assertEqual(tolerance.x87_reference("fptan", (ONE_80,), NEAREST), {1: 0x3FFFC75922E5F71D2DC5})

    def test_ranges(self):
        two = 0x40008000000000000000
        half = 0x3FFE8000000000000000
        self.assertIsNone(tolerance.x87_reference("f2xm1", (two,), NEAREST))
        self.assertEqual(tolerance.x87_reference("f2xm1", (ONE_80,), NEAREST), {0: ONE_80})
        self.assertIsNone(tolerance.x87_reference("fyl2x", (ONE_80 | 1 << 79, ONE_80), NEAREST))
        self.assertEqual(tolerance.x87_reference("fyl2x", (two, half), NEAREST), {0: half})
        self.assertIsNone(tolerance.x87_reference("fyl2xp1", (half, ONE_80), NEAREST))
        self.assertEqual(tolerance.x87_reference("fpatan", (ONE_80, ONE_80), NEAREST), {0: 0x3FFEC90FDAA22168C235})


if __name__ == "__main__":
    unittest.main()
