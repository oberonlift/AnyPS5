#include "X87Transcendental.hpp"

#include <ps5lift/State.h>

#include <cstdint>

namespace Ps5Lift::X87 {
namespace {

constexpr int ExtendedBias = 0x3fff;
constexpr int QuadrupleBias = 0x3fff;
constexpr int FirstOrderLinearExponent = -120;
constexpr std::uint16_t LogarithmPlusOneLimitExponent = 0x3ffd;
constexpr std::uint64_t LogarithmPlusOneLimitSignificand = 0x95f619980c4336f7ull;
constexpr int ScaleStep = 12288;
constexpr int SeriesTerms = 40;
constexpr std::uint64_t TwoOverPi[5] = {0xa2f9836e4e441529ull, 0xfc2757d1f534ddc0ull, 0xdb6295993c439041ull, 0xfe5163abdebbc561ull, 0xb7246e3a424dd2e0ull};
constexpr int TwoOverPiBits = 320;
constexpr int ReductionFractionWords = 3;

class NearestEven {
public:
    NearestEven() : _rounding(softfloat_roundingMode), _flags(softfloat_exceptionFlags) { softfloat_roundingMode = softfloat_round_near_even; }
    ~NearestEven() {
        softfloat_roundingMode = _rounding;
        softfloat_exceptionFlags = _flags;
    }
    NearestEven(const NearestEven&) = delete;
    NearestEven& operator=(const NearestEven&) = delete;

private:
    std::uint_fast8_t _rounding;
    std::uint_fast8_t _flags;
};

float128_t Integer(const std::int32_t value) { return i32_to_f128(value); }

float128_t Negate(float128_t value) {
    value.v[1] ^= 1ull << 63;
    return value;
}

float128_t Absolute(float128_t value) {
    value.v[1] &= ~(1ull << 63);
    return value;
}

bool IsZero(const float128_t value) { return (value.v[1] << 1) == 0 && value.v[0] == 0; }

int ExtendedExponent(const extFloat80_t value) { return (value.signExp & 0x7fff) - ExtendedBias; }

int LeadingExponent(const extFloat80_t value) {
    int exponent = (value.signExp & 0x7fff) == 0 ? 1 - ExtendedBias : ExtendedExponent(value);
    for (auto significand = value.signif; (significand >> 63) == 0; significand <<= 1) --exponent;
    return exponent;
}

bool IsNegative(const extFloat80_t value) { return (value.signExp & 0x8000) != 0; }

bool IsLinear(const float128_t value) { return ((value.v[1] >> 48) & 0x7fff) < static_cast<std::uint64_t>(QuadrupleBias + LinearExponent); }

float128_t PowerOfTwo(const int exponent) {
    float128_t value;
    value.v[0] = 0;
    value.v[1] = static_cast<std::uint64_t>(exponent + QuadrupleBias) << 48;
    return value;
}

enum class Direction { TowardZero, AwayFromZero };

float128_t Nudged(float128_t value, const Direction direction) {
    if (IsZero(value)) return value;
    if (direction == Direction::AwayFromZero) {
        if (++value.v[0] == 0) ++value.v[1];
    } else if (value.v[0]-- == 0) {
        --value.v[1];
    }
    return value;
}

float128_t SineSeries(const float128_t angle) {
    if (IsLinear(angle)) return Nudged(angle, Direction::TowardZero);
    const auto square = f128_mul(angle, angle);
    auto term = angle;
    auto sum = angle;
    for (int n = 2; n < SeriesTerms; n += 2) {
        term = f128_div(f128_mul(term, Negate(square)), Integer(n * (n + 1)));
        sum = f128_add(sum, term);
    }
    return sum;
}

float128_t CosineSeries(const float128_t angle) {
    if (IsLinear(angle)) return Nudged(Integer(1), Direction::TowardZero);
    const auto square = f128_mul(angle, angle);
    auto term = Integer(1);
    auto sum = term;
    for (int n = 1; n < SeriesTerms; n += 2) {
        term = f128_div(f128_mul(term, Negate(square)), Integer(n * (n + 1)));
        sum = f128_add(sum, term);
    }
    return sum;
}

float128_t OddSeries(const float128_t value, const bool alternating) {
    const auto square = f128_mul(value, value);
    auto power = value;
    auto sum = value;
    for (int n = 3; n < 2 * SeriesTerms; n += 2) {
        power = f128_mul(power, alternating ? Negate(square) : square);
        sum = f128_add(sum, f128_div(power, Integer(n)));
    }
    return sum;
}

struct Reduction {
    float128_t Angle;
    unsigned Quadrant;
};

Reduction Reduce(const extFloat80_t x) {
    const int exponent = ExtendedExponent(x);
    if (exponent < -1) return {Absolute(extF80_to_f128(x)), 0};
    std::uint64_t product[6];
    unsigned __int128 carry = 0;
    for (int word = 0; word < 5; ++word) {
        const unsigned __int128 part = static_cast<unsigned __int128>(x.signif) * TwoOverPi[4 - word] + carry;
        product[word] = static_cast<std::uint64_t>(part);
        carry = part >> 64;
    }
    product[5] = static_cast<std::uint64_t>(carry);
    const int point = TwoOverPiBits + 63 - exponent;
    const auto bit = [&](const int index) -> std::uint64_t { return index < 0 || index >= 384 ? 0 : (product[index / 64] >> (index % 64)) & 1; };
    unsigned quadrant = static_cast<unsigned>(bit(point) | bit(point + 1) << 1);
    std::uint64_t fraction[ReductionFractionWords] = {};
    for (int i = 0; i < 64 * ReductionFractionWords; ++i) fraction[i / 64] |= bit(point - 1 - i) << (63 - i % 64);
    const bool negative = (fraction[0] >> 63) != 0;
    if (negative) {
        ++quadrant;
        for (auto& word : fraction) word = ~word;
        if (++fraction[2] == 0 && ++fraction[1] == 0) ++fraction[0];
    }
    auto value = Scaled(ui64_to_f128(fraction[2]), -192);
    value = f128_add(value, Scaled(ui64_to_f128(fraction[1]), -128));
    value = f128_add(value, Scaled(ui64_to_f128(fraction[0]), -64));
    const auto angle = f128_mul(value, Scaled(Pi, -1));
    return {negative ? Negate(angle) : angle, quadrant & 3};
}

float128_t SineOfMagnitude(const Reduction& reduction) {
    switch (reduction.Quadrant) {
    case 0: return SineSeries(reduction.Angle);
    case 1: return CosineSeries(reduction.Angle);
    case 2: return Negate(SineSeries(reduction.Angle));
    default: return Negate(CosineSeries(reduction.Angle));
    }
}

float128_t CosineOfMagnitude(const Reduction& reduction) {
    switch (reduction.Quadrant) {
    case 0: return CosineSeries(reduction.Angle);
    case 1: return Negate(SineSeries(reduction.Angle));
    case 2: return Negate(CosineSeries(reduction.Angle));
    default: return SineSeries(reduction.Angle);
    }
}

float128_t ArctangentOfRatio(float128_t ratio) {
    for (int halving = 0; halving < 2; ++halving) ratio = f128_div(ratio, f128_add(Integer(1), f128_sqrt(f128_add(Integer(1), f128_mul(ratio, ratio)))));
    return Scaled(OddSeries(ratio, true), 2);
}

float128_t NaturalLogarithmOfOnePlus(const float128_t value) {
    return Scaled(OddSeries(f128_div(value, f128_add(Integer(2), value)), false), 1);
}

float128_t LogarithmOfParts(int exponent, float128_t fraction) {
    if (f128_lt(f128_sqrt(Integer(2)), fraction)) {
        fraction = Scaled(fraction, -1);
        ++exponent;
    }
    if (f128_eq(fraction, Integer(1))) return Integer(exponent);
    const auto logarithm = f128_mul(NaturalLogarithmOfOnePlus(f128_sub(fraction, Integer(1))), Log2E);
    return f128_add(Integer(exponent), logarithm);
}

float128_t BinaryLogarithm(const extFloat80_t x) {
    const int exponent = LeadingExponent(x);
    return LogarithmOfParts(exponent, Scaled(extF80_to_f128(x), -exponent));
}

float128_t BinaryLogarithm(const float128_t value) {
    const auto biased = static_cast<int>((value.v[1] >> 48) & 0x7fff);
    auto fraction = value;
    fraction.v[1] = (fraction.v[1] & 0xffffffffffffull) | static_cast<std::uint64_t>(QuadrupleBias) << 48;
    return LogarithmOfParts(biased - QuadrupleBias, fraction);
}


bool WithinLogarithmPlusOneRange(const extFloat80_t x) {
    const auto exponent = x.signExp & 0x7fff;
    return exponent < LogarithmPlusOneLimitExponent || (exponent == LogarithmPlusOneLimitExponent && x.signif < LogarithmPlusOneLimitSignificand);
}

}

float128_t Scaled(float128_t value, int scale) {
    const NearestEven nearest;
    while (scale != 0) {
        const int step = scale > ScaleStep ? ScaleStep : scale < -ScaleStep ? -ScaleStep : scale;
        value = f128_mul(value, PowerOfTwo(step));
        scale -= step;
    }
    return value;
}

float128_t Sine(const extFloat80_t x) {
    const NearestEven nearest;
    const auto value = SineOfMagnitude(Reduce(x));
    return IsNegative(x) ? Negate(value) : value;
}

float128_t Cosine(const extFloat80_t x) {
    const NearestEven nearest;
    return CosineOfMagnitude(Reduce(x));
}

float128_t Tangent(const extFloat80_t x) {
    const NearestEven nearest;
    const auto reduction = Reduce(x);
    const bool odd = (reduction.Quadrant & 1) != 0;
    float128_t value;
    if (IsLinear(reduction.Angle)) {
        value = odd ? Negate(Nudged(f128_div(Integer(1), reduction.Angle), Direction::TowardZero)) : Nudged(reduction.Angle, Direction::AwayFromZero);
    } else {
        const auto sine = SineSeries(reduction.Angle);
        const auto cosine = CosineSeries(reduction.Angle);
        value = odd ? Negate(f128_div(cosine, sine)) : f128_div(sine, cosine);
    }
    return IsNegative(x) ? Negate(value) : value;
}

float128_t Arctangent(const extFloat80_t y, const extFloat80_t x, const int scale) {
    const NearestEven nearest;
    const auto rise = Absolute(extF80_to_f128(y));
    const auto run = Absolute(extF80_to_f128(x));
    float128_t angle;
    if (scale != 0) {
        if (IsNegative(x) || f128_lt(run, rise)) ps5lift_fail("fpatan scaled by 2^%d with a result that is not tiny", scale);
        const int rising = LeadingExponent(y);
        const int running = LeadingExponent(x);
        const auto ratio = f128_div(Scaled(rise, -rising), Scaled(run, -running));
        angle = Scaled(ratio, rising - running + scale);
    } else {
        const bool steep = f128_lt(run, rise);
        const auto ratio = steep ? f128_div(run, rise) : f128_div(rise, run);
        angle = IsLinear(ratio) ? Nudged(ratio, Direction::TowardZero) : ArctangentOfRatio(ratio);
        if (steep) angle = f128_sub(Scaled(Pi, -1), angle);
        if (IsNegative(x)) angle = f128_sub(Pi, angle);
    }
    return IsNegative(y) ? Negate(angle) : angle;
}

float128_t PowerOfTwoMinusOne(const extFloat80_t x, const int scale) {
    const NearestEven nearest;
    if (scale != 0) return f128_mul(Scaled(extF80_to_f128(x), scale), Ln2);
    const auto exponent = f128_mul(extF80_to_f128(x), Ln2);
    auto term = exponent;
    auto sum = exponent;
    for (int n = 2; n < SeriesTerms; ++n) {
        term = f128_div(f128_mul(term, exponent), Integer(n));
        sum = f128_add(sum, term);
    }
    return sum;
}

float128_t LogarithmProduct(const extFloat80_t y, const extFloat80_t x, const int scale) {
    const NearestEven nearest;
    return f128_mul(Scaled(extF80_to_f128(y), scale), BinaryLogarithm(x));
}

float128_t LogarithmPlusOneProduct(const extFloat80_t y, const extFloat80_t x, const int scale) {
    const NearestEven nearest;
    const auto value = extF80_to_f128(x);
    if (!WithinLogarithmPlusOneRange(x)) return f128_mul(Scaled(extF80_to_f128(y), scale), BinaryLogarithm(f128_add(Integer(1), value)));
    if (ExtendedExponent(x) < FirstOrderLinearExponent) {
        const int exponent = LeadingExponent(x);
        return f128_mul(f128_mul(Scaled(extF80_to_f128(y), scale + exponent), Scaled(value, -exponent)), Log2E);
    }
    return f128_mul(Scaled(extF80_to_f128(y), scale), f128_mul(NaturalLogarithmOfOnePlus(value), Log2E));
}

}
