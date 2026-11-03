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
        far = _float32(1000.0)
        samples = (
            (Fraction(4), Fraction(1, 4), False), (Fraction(-1, 2**20), -Fraction(2**20), False),
            (Fraction(4), Fraction(1, 2), True), (Fraction(1, 2**30), Fraction(2**15), True),
        )
        for source, exact, square_root in samples:
            at_bound = exact + abs(exact) * bound
            past = _bits_of(at_bound) + 1 if exact > 0 else _bits_of(at_bound) - 1
            below = exact - abs(exact) * bound
            with self.subTest(source=source, square_root=square_root):
                self.assertTrue(tolerance.within_rcp(far, _bits_of(at_bound), _bits_of(source), 32, square_root))
                self.assertTrue(tolerance.within_rcp(far, _bits_of(below), _bits_of(source), 32, square_root))
                self.assertFalse(tolerance.within_rcp(far, past, _bits_of(source), 32, square_root))

    def test_rcp_measured_against_the_exact_reciprocal(self):
        exact = Fraction(1, 3)
        intel = _float32(float(exact * (1 + Fraction(14, 10) / 2**12)))
        lifted = _float32(float(exact * (1 + Fraction(28, 10) / 2**12)))
        self.assertFalse(tolerance.within_rcp(intel, lifted, _float32(3.0), 32, False))
        self.assertTrue(tolerance.within_rcp(lifted, intel, _float32(3.0), 32, False))

    def test_rcp_exact_specials(self):
        infinity, zero, nan = 0x7F800000, 0x00000000, 0x7FC00000
        for source, expected in ((zero, infinity), (0x80000000, 0xFF800000), (infinity, zero), (nan, nan),
                                 (0x00000001, infinity), (0x7F000000, zero)):
            with self.subTest(source=hex(source)):
                self.assertTrue(tolerance.within_rcp(expected, expected, source, 32, False))
                self.assertFalse(tolerance.within_rcp(expected, expected ^ 1, source, 32, False))
        self.assertTrue(tolerance.within_rcp(nan, nan, _float32(-4.0), 32, True))
        self.assertFalse(tolerance.within_rcp(nan, _float32(-0.5), _float32(-4.0), 32, True))
        self.assertFalse(tolerance.within_rcp(_float32(0.25), nan, _float32(4.0), 32, False))
        self.assertFalse(tolerance.within_rcp(_float32(0.25), infinity, _float32(4.0), 32, False))


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



def _float64(value):
    return struct.unpack("<Q", struct.pack("<d", value))[0]


class SpecialProductTest(unittest.TestCase):
    def test_specials_whatever_the_rounding(self):
        largest, one, infinity, nan = 0x7F7FFFFF, _bits_of(1), 0x7F800000, 0x7FC00001
        for rounding in (NEAREST, DOWN, UP, TOWARD_ZERO):
            with self.subTest(rounding=rounding):
                self.assertFalse(tolerance.special_product(largest, one, 32, rounding))
                self.assertFalse(tolerance.special_product(0x00000001, largest, 32, rounding))
                self.assertTrue(tolerance.special_product(nan, one, 32, rounding))
                self.assertTrue(tolerance.special_product(one, nan, 32, rounding))
                self.assertTrue(tolerance.special_product(infinity, 0, 32, rounding))
                self.assertTrue(tolerance.special_product(infinity, one, 32, rounding))

    def test_overflow_by_rounding(self):
        modes = (NEAREST, DOWN, UP, TOWARD_ZERO)
        positive = (_bits_of(Fraction(2**127)), _bits_of(Fraction(2)))
        negative = (_bits_of(Fraction(-2**127)), _bits_of(Fraction(2)))
        self.assertEqual([tolerance.special_product(*positive, 32, mode) for mode in modes], [True, False, True, False])
        self.assertEqual([tolerance.special_product(*negative, 32, mode) for mode in modes], [True, True, False, False])
        tie = (_bits_of(Fraction(18631 * 2**90)), _bits_of(Fraction(1801 * 2**13)))
        self.assertEqual([tolerance.special_product(*tie, 32, mode) for mode in modes], [True, False, True, False])
        below_tie = (_bits_of(Fraction(37 * 2**90)), _bits_of(Fraction(1813753 * 2**12)))
        self.assertEqual([tolerance.special_product(*below_tie, 32, mode) for mode in modes],
                         [False, False, True, False])
        self.assertTrue(tolerance.special_product(_float64(2.0**1023), _float64(2.0), 64, NEAREST))
        self.assertFalse(tolerance.special_product(_float64(2.0**1022), _float64(2.0), 64, NEAREST))


if __name__ == "__main__":
    unittest.main()
