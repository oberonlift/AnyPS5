#include "Runtime.hpp"

#include <ps5lift/State.h>

extern "C" {
#include <softfloat.h>
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr unsigned RoundingShift = 13;
constexpr std::size_t SlotBytes = 32;

struct Single {
    using Bits = std::uint32_t;
    using Value = float32_t;
    static constexpr Bits Sign = 0x80000000u;
    static constexpr Bits Exponent = 0x7f800000u;
    static constexpr Bits Fraction = 0x007fffffu;
    static Value Add(const Value a, const Value b) { return f32_add(a, b); }
    static Value Multiply(const Value a, const Value b) { return f32_mul(a, b); }
    static Value Divide(const Value a, const Value b) { return f32_div(a, b); }
    static Value Sqrt(const Value a) { return f32_sqrt(a); }
    static Value Fma(const Value a, const Value b, const Value c) { return f32_mulAdd(a, b, c); }
};

struct Double {
    using Bits = std::uint64_t;
    using Value = float64_t;
    static constexpr Bits Sign = 0x8000000000000000ull;
    static constexpr Bits Exponent = 0x7ff0000000000000ull;
    static constexpr Bits Fraction = 0x000fffffffffffffull;
    static Value Add(const Value a, const Value b) { return f64_add(a, b); }
    static Value Multiply(const Value a, const Value b) { return f64_mul(a, b); }
    static Value Divide(const Value a, const Value b) { return f64_div(a, b); }
    static Value Sqrt(const Value a) { return f64_sqrt(a); }
    static Value Fma(const Value a, const Value b, const Value c) { return f64_mulAdd(a, b, c); }
};

template <typename Format>
bool IsDenormal(const typename Format::Bits bits) {
    return (bits & Format::Exponent) == 0 && (bits & Format::Fraction) != 0;
}

template <typename Format>
bool IsNan(const typename Format::Bits bits) {
    return (bits & Format::Exponent) == Format::Exponent && (bits & Format::Fraction) != 0;
}

struct Environment {
    bool DenormalsAreZero;
    bool FlushToZero;
    bool UnderflowUnmasked;
};

struct Raised {
    std::uint32_t Pre = 0;
    std::uint32_t Post = 0;
};

template <typename Format>
typename Format::Bits Load(const std::uint8_t* lanes, const unsigned slot, const unsigned lane) {
    typename Format::Bits bits;
    std::memcpy(&bits, lanes + slot * SlotBytes + lane * sizeof(bits), sizeof(bits));
    return bits;
}

template <typename Format>
void Store(std::uint8_t* lanes, const unsigned lane, const typename Format::Bits bits) {
    std::memcpy(lanes + lane * sizeof(bits), &bits, sizeof(bits));
}

template <typename Source, typename Result, std::size_t Count, typename Compute>
typename Result::Bits Lane(const Environment& environment, const std::array<typename Source::Bits, Count>& inputs, const Compute& compute, Raised& raised) {
    bool denormal = false;
    bool nan = false;
    std::array<typename Source::Value, Count> operands{};
    for (std::size_t i = 0; i < Count; ++i) {
        denormal = denormal || IsDenormal<Source>(inputs[i]);
        nan = nan || IsNan<Source>(inputs[i]);
        operands[i].v = environment.DenormalsAreZero && IsDenormal<Source>(inputs[i]) ? inputs[i] & Source::Sign : inputs[i];
    }
    softfloat_exceptionFlags = 0;
    auto result = compute(operands).v;
    const auto flags = softfloat_exceptionFlags;
    std::uint32_t pre = 0;
    if (flags & softfloat_flag_invalid) pre |= PS5LIFT_MXCSR_INVALID;
    if (flags & softfloat_flag_infinite) pre |= PS5LIFT_MXCSR_DIVIDE_BY_ZERO;
    if (denormal && !nan && pre == 0 && !environment.DenormalsAreZero) pre |= PS5LIFT_MXCSR_DENORMAL_OPERAND;
    raised.Pre |= pre;
    if (flags & softfloat_flag_overflow) raised.Post |= PS5LIFT_MXCSR_OVERFLOW;
    if (flags & softfloat_flag_inexact) raised.Post |= PS5LIFT_MXCSR_PRECISION;
    const bool tiny = (flags & softfloat_flag_underflow) != 0 || IsDenormal<Result>(result);
    if (environment.UnderflowUnmasked) {
        if (tiny) raised.Post |= PS5LIFT_MXCSR_UNDERFLOW;
    } else if (environment.FlushToZero && tiny) {
        result &= Result::Sign;
        raised.Post |= PS5LIFT_MXCSR_UNDERFLOW | PS5LIFT_MXCSR_PRECISION;
    } else if (flags & softfloat_flag_underflow) {
        raised.Post |= PS5LIFT_MXCSR_UNDERFLOW;
    }
    return result;
}

template <typename Format>
Raised Arithmetic(const Environment& environment, const std::uint32_t operation, const std::uint32_t count, std::uint8_t* lanes) {
    using Bits = typename Format::Bits;
    using Operands1 = std::array<typename Format::Value, 1>;
    using Operands2 = std::array<typename Format::Value, 2>;
    using Operands3 = std::array<typename Format::Value, 3>;
    Raised raised;
    for (unsigned lane = 0; lane < count; ++lane) {
        const auto source = [&](const unsigned slot) { return Load<Format>(lanes, slot, lane); };
        Bits result = 0;
        switch (operation) {
        case PS5LIFT_SIMD_ADD:
            result = Lane<Format, Format, 2>(environment, {source(1), source(2)}, [](const Operands2& x) { return Format::Add(x[0], x[1]); }, raised);
            break;
        case PS5LIFT_SIMD_MULTIPLY:
            result = Lane<Format, Format, 2>(environment, {source(1), source(2)}, [](const Operands2& x) { return Format::Multiply(x[0], x[1]); }, raised);
            break;
        case PS5LIFT_SIMD_DIVIDE:
            result = Lane<Format, Format, 2>(environment, {source(1), source(2)}, [](const Operands2& x) { return Format::Divide(x[0], x[1]); }, raised);
            break;
        case PS5LIFT_SIMD_SQRT:
            result = Lane<Format, Format, 1>(environment, {source(1)}, [](const Operands1& x) { return Format::Sqrt(x[0]); }, raised);
            break;
        case PS5LIFT_SIMD_FMA:
            result = Lane<Format, Format, 3>(environment, {source(1), source(2), source(3)}, [](const Operands3& x) { return Format::Fma(x[0], x[1], x[2]); }, raised);
            break;
        default:
            ps5lift_fail("unknown SIMD floating-point operation %u on %zu-bit elements", operation, sizeof(Bits) * 8);
        }
        Store<Format>(lanes, lane, result);
    }
    return raised;
}

template <typename Source, typename Result, typename Convert>
Raised Conversion(const Environment& environment, const std::uint32_t count, std::uint8_t* lanes, const Convert& convert) {
    Raised raised;
    for (unsigned lane = 0; lane < count; ++lane) {
        const auto result = Lane<Source, Result, 1>(environment, {Load<Source>(lanes, 1, lane)}, [&](const std::array<typename Source::Value, 1>& x) { return convert(x[0]); }, raised);
        Store<Result>(lanes, lane, result);
    }
    return raised;
}

Raised Compute(const Environment& environment, const std::uint32_t operation, const std::uint32_t element, const std::uint32_t count, std::uint8_t* lanes) {
    if (operation == PS5LIFT_SIMD_NARROW) return Conversion<Double, Single>(environment, count, lanes, [](const float64_t x) { return f64_to_f32(x); });
    if (operation == PS5LIFT_SIMD_WIDEN) return Conversion<Single, Double>(environment, count, lanes, [](const float32_t x) { return f32_to_f64(x); });
    if (element == 32) return Arithmetic<Single>(environment, operation, count, lanes);
    if (element == 64) return Arithmetic<Double>(environment, operation, count, lanes);
    ps5lift_fail("SIMD floating-point operation %u on %u-bit elements", operation, element);
}

}

Ps5Lift::SoftFloatEnvironment::SoftFloatEnvironment() : _rounding(softfloat_roundingMode), _tininess(softfloat_detectTininess), _flags(softfloat_exceptionFlags), _extendedPrecision(extF80_roundingPrecision) {}

Ps5Lift::SoftFloatEnvironment::~SoftFloatEnvironment() {
    softfloat_roundingMode = _rounding;
    softfloat_detectTininess = _tininess;
    softfloat_exceptionFlags = _flags;
    extF80_roundingPrecision = _extendedPrecision;
}

extern "C" std::uint32_t ps5lift_simd_float(Ps5LiftState* state, const std::uint32_t operation, const std::uint32_t element, const std::uint32_t count, std::uint8_t* lanes) {
    static constexpr std::uint_fast8_t Rounding[4] = {softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag};
    const auto mxcsr = state->Mxcsr;
    const auto unmasked = ~(mxcsr >> PS5LIFT_MXCSR_MASK_SHIFT) & PS5LIFT_MXCSR_EXCEPTIONS;
    softfloat_roundingMode = Rounding[(mxcsr >> RoundingShift) & 3];
    softfloat_detectTininess = softfloat_tininess_afterRounding;
    const Environment environment{(mxcsr & PS5LIFT_MXCSR_DENORMALS_ARE_ZERO) != 0, (mxcsr & PS5LIFT_MXCSR_FLUSH_TO_ZERO) != 0, (unmasked & PS5LIFT_MXCSR_UNDERFLOW) != 0};
    const auto raised = Compute(environment, operation, element, count, lanes);
    if ((raised.Pre & unmasked) != 0) {
        state->Mxcsr |= raised.Pre;
        return 1;
    }
    state->Mxcsr |= raised.Pre | raised.Post;
    return (raised.Post & unmasked) != 0 ? 1 : 0;
}
