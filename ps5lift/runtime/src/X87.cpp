#include <ps5lift/State.h>

extern "C" {
#include <softfloat.h>
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cstring>

namespace {

constexpr std::uint16_t StatusInvalid = 1u << 0;
constexpr std::uint16_t StatusDenormal = 1u << 1;
constexpr std::uint16_t StatusZeroDivide = 1u << 2;
constexpr std::uint16_t StatusOverflow = 1u << 3;
constexpr std::uint16_t StatusUnderflow = 1u << 4;
constexpr std::uint16_t StatusPrecision = 1u << 5;
constexpr std::uint16_t StatusStackFault = 1u << 6;
constexpr std::uint16_t StatusSummary = 1u << 7;
constexpr std::uint16_t StatusC0 = 1u << 8;
constexpr std::uint16_t StatusC1 = 1u << 9;
constexpr std::uint16_t StatusC2 = 1u << 10;
constexpr std::uint16_t StatusC3 = 1u << 14;
constexpr std::uint16_t StatusBusy = 1u << 15;
constexpr std::uint16_t StatusConditions = StatusC0 | StatusC1 | StatusC2 | StatusC3;
constexpr unsigned TopShift = 11;
constexpr std::uint16_t InitialControl = 0x037f;

constexpr std::uint32_t CompareGreater = 0;
constexpr std::uint32_t CompareLess = 1;
constexpr std::uint32_t CompareEqual = 2;
constexpr std::uint32_t CompareUnordered = 3;

[[noreturn]] void Fail(const char* message, const unsigned value) {
    std::fprintf(stderr, "ps5lift: %s 0x%x\n", message, value);
    std::fflush(stdout);
    std::_Exit(70);
}

extFloat80_t Indefinite() {
    extFloat80_t value;
    value.signExp = 0xffff;
    value.signif = 0xc000000000000000ull;
    return value;
}

extFloat80_t Zero(const bool negative) {
    extFloat80_t value;
    value.signExp = negative ? 0x8000 : 0;
    value.signif = 0;
    return value;
}

bool IsNan(const extFloat80_t value) {
    return (value.signExp & 0x7fff) == 0x7fff && (value.signif & 0x7fffffffffffffffull) != 0;
}

bool IsDenormal(const extFloat80_t value) {
    return (value.signExp & 0x7fff) == 0 && value.signif != 0;
}

bool IsNegative(const extFloat80_t value) {
    return (value.signExp & 0x8000) != 0;
}

class Fpu {
public:
    explicit Fpu(Ps5LiftState* state) : _state(*state) {
        const auto control = _state.X87Control;
        static constexpr std::uint_fast8_t Rounding[4] = {softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag};
        static constexpr std::uint_fast8_t Precision[4] = {32, 80, 64, 80};
        softfloat_roundingMode = Rounding[(control >> 10) & 3];
        extF80_roundingPrecision = Precision[(control >> 8) & 3];
        softfloat_detectTininess = softfloat_tininess_afterRounding;
        softfloat_exceptionFlags = 0;
    }

    ~Fpu() {
        std::uint16_t raised = 0;
        const auto flags = softfloat_exceptionFlags;
        if (flags & softfloat_flag_invalid) raised |= StatusInvalid;
        if (flags & softfloat_flag_infinite) raised |= StatusZeroDivide;
        if (flags & softfloat_flag_overflow) raised |= StatusOverflow;
        if (flags & softfloat_flag_underflow) raised |= StatusUnderflow;
        if (flags & softfloat_flag_inexact) raised |= StatusPrecision;
        raised |= _extra;
        auto status = static_cast<std::uint16_t>(_state.X87Status | raised);
        if ((status & ~_state.X87Control & 0x3f) != 0) status |= StatusSummary | StatusBusy;
        _state.X87Status = status;
    }

    unsigned Top() const { return (_state.X87Status >> TopShift) & 7; }

    void SetTop(const unsigned top) {
        _state.X87Status = static_cast<std::uint16_t>((_state.X87Status & ~(7u << TopShift)) | ((top & 7) << TopShift));
    }

    unsigned Physical(const unsigned index) const { return (Top() + index) & 7; }

    bool Valid(const unsigned index) const { return (_state.X87Valid >> Physical(index)) & 1; }

    extFloat80_t Get(const unsigned index) {
        if (!Valid(index)) {
            StackFault(false);
            return Indefinite();
        }
        extFloat80_t value;
        std::memcpy(&value.signif, _state.X87Registers[Physical(index)], 8);
        std::memcpy(&value.signExp, _state.X87Registers[Physical(index)] + 8, 2);
        return value;
    }

    void Set(const unsigned index, const extFloat80_t value) {
        const auto physical = Physical(index);
        std::memcpy(_state.X87Registers[physical], &value.signif, 8);
        std::memcpy(_state.X87Registers[physical] + 8, &value.signExp, 2);
        _state.X87Valid = static_cast<std::uint8_t>(_state.X87Valid | (1u << physical));
    }

    void Push(const extFloat80_t value) {
        SetTop(Top() - 1);
        if (Valid(0)) {
            StackFault(true);
            Set(0, Indefinite());
            return;
        }
        Set(0, value);
    }

    void Pop() {
        _state.X87Valid = static_cast<std::uint8_t>(_state.X87Valid & ~(1u << Physical(0)));
        SetTop(Top() + 1);
    }

    void Free(const unsigned index) {
        _state.X87Valid = static_cast<std::uint8_t>(_state.X87Valid & ~(1u << Physical(index)));
    }

    void SetConditions(const std::uint16_t conditions) {
        _state.X87Status = static_cast<std::uint16_t>((_state.X87Status & ~StatusConditions) | conditions);
    }

    void Raise(const std::uint16_t flags) { _extra |= flags; }

    void ClearRoundedUp() {
        _state.X87Status = static_cast<std::uint16_t>(_state.X87Status & ~StatusC1);
    }

    void SetRoundedUp() {
        _state.X87Status = static_cast<std::uint16_t>(_state.X87Status | StatusC1);
    }

    std::uint16_t Conditions() const {
        return static_cast<std::uint16_t>(_state.X87Status & StatusConditions);
    }

    void Operands(const extFloat80_t left, const extFloat80_t right) {
        if (IsNan(left) || IsNan(right)) return;
        if (IsDenormal(left) || IsDenormal(right)) _extra |= StatusDenormal;
    }

    void Operand(const extFloat80_t value) {
        if (IsDenormal(value)) _extra |= StatusDenormal;
    }

private:
    Ps5LiftState& _state;
    std::uint16_t _extra = 0;

    void StackFault(const bool overflow) {
        _extra |= StatusInvalid | StatusStackFault;
        _state.X87Status = static_cast<std::uint16_t>((_state.X87Status & ~StatusC1) | (overflow ? StatusC1 : 0));
    }
};

class FullPrecision {
public:
    FullPrecision() : _saved(extF80_roundingPrecision) { extF80_roundingPrecision = 80; }
    ~FullPrecision() { extF80_roundingPrecision = _saved; }

private:
    std::uint_fast8_t _saved;
};

bool SameBits(const extFloat80_t left, const extFloat80_t right) {
    return left.signif == right.signif && left.signExp == right.signExp;
}

bool SameBits(const float32_t left, const float32_t right) {
    return left.v == right.v;
}

bool SameBits(const float64_t left, const float64_t right) {
    return left.v == right.v;
}

template <typename TCompute>
auto WithRoundUp(Fpu& fpu, TCompute compute) -> decltype(compute()) {
    const auto saved = softfloat_exceptionFlags;
    softfloat_exceptionFlags = 0;
    const auto result = compute();
    const auto raised = softfloat_exceptionFlags;
    if (raised & softfloat_flag_inexact) {
        const auto rounding = softfloat_roundingMode;
        softfloat_roundingMode = softfloat_round_minMag;
        const auto truncated = compute();
        softfloat_roundingMode = rounding;
        if (!SameBits(result, truncated)) fpu.SetRoundedUp();
    }
    softfloat_exceptionFlags = static_cast<std::uint_fast8_t>(saved | raised);
    return result;
}

extFloat80_t LoadMemory(Fpu& fpu, const std::uint32_t format, const std::uint64_t address) {
    const void* source = reinterpret_cast<const void*>(address);
    switch (format & 7) {
    case PS5LIFT_X87_FLOAT32: {
        float32_t value;
        std::memcpy(&value, source, 4);
        if ((value.v & 0x7f800000u) == 0 && (value.v & 0x7fffffu) != 0) fpu.Raise(StatusDenormal);
        return f32_to_extF80(value);
    }
    case PS5LIFT_X87_FLOAT64: {
        float64_t value;
        std::memcpy(&value, source, 8);
        if ((value.v & 0x7ff0000000000000ull) == 0 && (value.v & 0xfffffffffffffull) != 0) fpu.Raise(StatusDenormal);
        return f64_to_extF80(value);
    }
    case PS5LIFT_X87_FLOAT80: {
        extFloat80_t value;
        std::memcpy(&value.signif, source, 8);
        std::memcpy(&value.signExp, static_cast<const std::uint8_t*>(source) + 8, 2);
        return value;
    }
    case PS5LIFT_X87_INTEGER16: {
        std::int16_t value;
        std::memcpy(&value, source, 2);
        return i32_to_extF80(value);
    }
    case PS5LIFT_X87_INTEGER32: {
        std::int32_t value;
        std::memcpy(&value, source, 4);
        return i32_to_extF80(value);
    }
    case PS5LIFT_X87_INTEGER64: {
        std::int64_t value;
        std::memcpy(&value, source, 8);
        return i64_to_extF80(value);
    }
    default:
        Fail("unknown x87 memory format", format);
    }
}

template <typename T>
void StoreInteger(Fpu& fpu, const extFloat80_t value, const bool truncate, void* destination) {
    const auto rounding = truncate ? static_cast<std::uint_fast8_t>(softfloat_round_minMag) : softfloat_roundingMode;
    T result = static_cast<T>(std::uint64_t{1} << (sizeof(T) * 8 - 1));
    if (!IsNan(value)) {
        const auto saved = softfloat_exceptionFlags;
        softfloat_exceptionFlags = 0;
        const auto wide = extF80_to_i64(value, rounding, true);
        const bool invalid = (softfloat_exceptionFlags & softfloat_flag_invalid) != 0;
        const bool inexact = (softfloat_exceptionFlags & softfloat_flag_inexact) != 0;
        softfloat_exceptionFlags = static_cast<std::uint_fast8_t>(saved | softfloat_exceptionFlags);
        if (!invalid && inexact && !truncate) {
            const auto flags = softfloat_exceptionFlags;
            if (extF80_to_i64(value, softfloat_round_minMag, false) != wide) fpu.SetRoundedUp();
            softfloat_exceptionFlags = flags;
        }
        if (!invalid && wide >= INT64_C(-1) << (sizeof(T) * 8 - 1) && (sizeof(T) == 8 || wide < INT64_C(1) << (sizeof(T) * 8 - 1))) {
            result = static_cast<T>(wide);
        } else {
            softfloat_exceptionFlags = static_cast<std::uint_fast8_t>(saved | softfloat_flag_invalid);
        }
    } else {
        fpu.Raise(StatusInvalid);
    }
    std::memcpy(destination, &result, sizeof(T));
}

void StoreMemory(Fpu& fpu, const std::uint32_t format, const std::uint64_t address, const extFloat80_t value) {
    void* destination = reinterpret_cast<void*>(address);
    const bool truncate = (format & PS5LIFT_X87_TRUNCATE) != 0;
    switch (format & 7) {
    case PS5LIFT_X87_FLOAT32: {
        const auto result = WithRoundUp(fpu, [&] { return extF80_to_f32(value); });
        std::memcpy(destination, &result, 4);
        return;
    }
    case PS5LIFT_X87_FLOAT64: {
        const auto result = WithRoundUp(fpu, [&] { return extF80_to_f64(value); });
        std::memcpy(destination, &result, 8);
        return;
    }
    case PS5LIFT_X87_FLOAT80:
        std::memcpy(destination, &value.signif, 8);
        std::memcpy(static_cast<std::uint8_t*>(destination) + 8, &value.signExp, 2);
        return;
    case PS5LIFT_X87_INTEGER16: StoreInteger<std::int16_t>(fpu, value, truncate, destination); return;
    case PS5LIFT_X87_INTEGER32: StoreInteger<std::int32_t>(fpu, value, truncate, destination); return;
    case PS5LIFT_X87_INTEGER64: StoreInteger<std::int64_t>(fpu, value, truncate, destination); return;
    default: Fail("unknown x87 memory format", format);
    }
}

extFloat80_t Arithmetic(const std::uint32_t operation, const extFloat80_t left, const extFloat80_t right) {
    switch (operation) {
    case PS5LIFT_X87_ADD: return extF80_add(left, right);
    case PS5LIFT_X87_SUBTRACT: return extF80_sub(left, right);
    case PS5LIFT_X87_SUBTRACT_REVERSE: return extF80_sub(right, left);
    case PS5LIFT_X87_MULTIPLY: return extF80_mul(left, right);
    case PS5LIFT_X87_DIVIDE: return extF80_div(left, right);
    default: return extF80_div(right, left);
    }
}

std::uint32_t Compare(const extFloat80_t left, const extFloat80_t right, const bool unordered) {
    const bool nan = IsNan(left) || IsNan(right);
    if (nan) {
        if (!unordered || extF80_isSignalingNaN(left) || extF80_isSignalingNaN(right)) softfloat_exceptionFlags |= softfloat_flag_invalid;
        return CompareUnordered;
    }
    if (extF80_eq(left, right)) return CompareEqual;
    return extF80_lt_quiet(left, right) ? CompareLess : CompareGreater;
}

std::uint16_t CompareConditions(const std::uint32_t result) {
    switch (result) {
    case CompareGreater: return 0;
    case CompareLess: return StatusC0;
    case CompareEqual: return StatusC3;
    default: return StatusC0 | StatusC2 | StatusC3;
    }
}

extFloat80_t Constant(const std::uint32_t index) {
    static constexpr std::uint64_t Constants[5][2] = {
        {0x4000a934f0979a37ull, 0x15fc9257edfe9b60ull},
        {0x3fff71547652b82full, 0xe1777d0ffda0d23aull},
        {0x4000921fb54442d1ull, 0x8469898cc51701b8ull},
        {0x3ffd34413509f79full, 0xef311f12b35816f9ull},
        {0x3ffe62e42fefa39eull, 0xf35793c7673007e6ull},
    };
    if (index == 0) return i32_to_extF80(1);
    if (index == 6) return Zero(false);
    if (index > 6) Fail("unknown x87 constant", index);
    float128_t value;
    value.v[0] = Constants[index - 1][1];
    value.v[1] = Constants[index - 1][0];
    const auto savedPrecision = extF80_roundingPrecision;
    extF80_roundingPrecision = 80;
    const auto result = f128_to_extF80(value);
    extF80_roundingPrecision = savedPrecision;
    softfloat_exceptionFlags &= static_cast<std::uint_fast8_t>(~softfloat_flag_inexact);
    return result;
}

struct Significand {
    std::uint64_t Bits;
    int Exponent;
};

Significand Normalize(const extFloat80_t value) {
    Significand result{value.signif, (value.signExp & 0x7fff) == 0 ? 1 : value.signExp & 0x7fff};
    while (result.Bits != 0 && (result.Bits >> 63) == 0) {
        result.Bits <<= 1;
        --result.Exponent;
    }
    return result;
}

unsigned TruncatedQuotientBits(const extFloat80_t dividend, const extFloat80_t divisor) {
    const auto x = Normalize(dividend);
    const auto y = Normalize(divisor);
    if (x.Bits == 0 || y.Bits == 0) return 0;
    const int difference = x.Exponent - y.Exponent;
    if (difference < 0) return 0;
    unsigned __int128 remainder = x.Bits;
    unsigned bits = 0;
    for (int step = 0; step <= difference; ++step) {
        const bool digit = remainder >= y.Bits;
        if (digit) remainder -= y.Bits;
        bits = ((bits << 1) | (digit ? 1u : 0u)) & 7u;
        remainder <<= 1;
    }
    return bits;
}

std::uint16_t Examine(Fpu& fpu) {
    const bool empty = !fpu.Valid(0);
    extFloat80_t value{};
    if (!empty) value = fpu.Get(0);
    const auto sign = IsNegative(value) ? StatusC1 : 0;
    if (empty) return static_cast<std::uint16_t>(StatusC3 | StatusC0 | sign);
    const auto exponent = value.signExp & 0x7fff;
    const bool integerBit = (value.signif >> 63) != 0;
    if (exponent == 0x7fff) {
        if (!integerBit) return sign;
        return static_cast<std::uint16_t>(((value.signif << 1) != 0 ? StatusC0 : StatusC2 | StatusC0) | sign);
    }
    if (exponent == 0) {
        if (value.signif == 0) return static_cast<std::uint16_t>(StatusC3 | sign);
        return static_cast<std::uint16_t>(StatusC3 | StatusC2 | sign);
    }
    if (!integerBit) return sign;
    return static_cast<std::uint16_t>(StatusC2 | sign);
}

std::uint16_t TagWord(const Ps5LiftState& state) {
    std::uint16_t tags = 0;
    for (unsigned physical = 0; physical < 8; ++physical) {
        unsigned tag = 3;
        if ((state.X87Valid >> physical) & 1) {
            std::uint64_t significand;
            std::uint16_t signExponent;
            std::memcpy(&significand, state.X87Registers[physical], 8);
            std::memcpy(&signExponent, state.X87Registers[physical] + 8, 2);
            const auto exponent = signExponent & 0x7fff;
            if (exponent == 0 && significand == 0) tag = 1;
            else if (exponent == 0x7fff || exponent == 0 || (significand >> 63) == 0) tag = 2;
            else tag = 0;
        }
        tags = static_cast<std::uint16_t>(tags | (tag << (2 * physical)));
    }
    return tags;
}

}

extern "C" std::uint32_t ps5lift_x87(Ps5LiftState* state, const std::uint32_t operation, const std::uint32_t format, const std::uint64_t address, const std::uint32_t index) {
    switch (operation) {
    case PS5LIFT_X87_INITIALIZE:
        state->X87Control = InitialControl;
        state->X87Status = 0;
        state->X87Valid = 0;
        return 0;
    case PS5LIFT_X87_CLEAR_EXCEPTIONS:
        state->X87Status = static_cast<std::uint16_t>(state->X87Status & 0x7f00);
        return 0;
    case PS5LIFT_X87_WAIT: {
        const auto pending = state->X87Status & ~state->X87Control & 0x3fu;
        if (pending != 0) Fail("x87 instruction with unmasked exceptions pending; the console raises SIGFPE for", pending);
        return 0;
    }
    case PS5LIFT_X87_STORE_STATUS:
        return state->X87Status;
    case PS5LIFT_X87_STORE_CONTROL:
        std::memcpy(reinterpret_cast<void*>(address), &state->X87Control, 2);
        return 0;
    case PS5LIFT_X87_LOAD_CONTROL:
        std::memcpy(&state->X87Control, reinterpret_cast<const void*>(address), 2);
        return 0;
    case PS5LIFT_X87_STORE_ENVIRONMENT: {
        const std::uint32_t environment[7] = {0xffff0000u | state->X87Control, 0xffff0000u | state->X87Status, 0xffff0000u | TagWord(*state), 0, 0, 0, 0xffff0000u};
        std::memcpy(reinterpret_cast<void*>(address), environment, sizeof(environment));
        state->X87Control = static_cast<std::uint16_t>(state->X87Control | 0x3f);
        return 0;
    }
    case PS5LIFT_X87_LOAD_ENVIRONMENT: {
        std::uint32_t environment[7];
        std::memcpy(environment, reinterpret_cast<const void*>(address), sizeof(environment));
        state->X87Control = static_cast<std::uint16_t>(environment[0]);
        state->X87Status = static_cast<std::uint16_t>(environment[1]);
        std::uint8_t valid = 0;
        for (unsigned physical = 0; physical < 8; ++physical) {
            if (((environment[2] >> (2 * physical)) & 3) != 3) valid = static_cast<std::uint8_t>(valid | (1u << physical));
        }
        state->X87Valid = valid;
        return 0;
    }
    default:
        break;
    }

    Fpu fpu(state);
    const bool memory = (format & 7) != PS5LIFT_X87_REGISTER;
    switch (operation) {
    case PS5LIFT_X87_LOAD: {
        fpu.ClearRoundedUp();
        const auto value = memory ? LoadMemory(fpu, format, address) : fpu.Get(index);
        fpu.Push(value);
        return 0;
    }
    case PS5LIFT_X87_LOAD_CONSTANT:
        fpu.ClearRoundedUp();
        fpu.Push(Constant(index));
        return 0;
    case PS5LIFT_X87_STORE: {
        fpu.ClearRoundedUp();
        const auto value = fpu.Get(0);
        if (memory) StoreMemory(fpu, format, address, value);
        else fpu.Set(index, value);
        if (format & PS5LIFT_X87_POP) fpu.Pop();
        return 0;
    }
    case PS5LIFT_X87_ADD:
    case PS5LIFT_X87_SUBTRACT:
    case PS5LIFT_X87_SUBTRACT_REVERSE:
    case PS5LIFT_X87_MULTIPLY:
    case PS5LIFT_X87_DIVIDE:
    case PS5LIFT_X87_DIVIDE_REVERSE: {
        fpu.ClearRoundedUp();
        const auto destination = memory ? 0u : (format & PS5LIFT_X87_TO_REGISTER) ? index : 0u;
        const auto left = fpu.Get(destination);
        const auto right = memory ? LoadMemory(fpu, format, address) : fpu.Get(destination == 0 ? index : 0);
        fpu.Operands(left, right);
        fpu.Set(destination, WithRoundUp(fpu, [&] { return Arithmetic(operation, left, right); }));
        if (!memory && (format & PS5LIFT_X87_TO_REGISTER) && (format & PS5LIFT_X87_POP)) fpu.Pop();
        return 0;
    }
    case PS5LIFT_X87_COMPARE:
    case PS5LIFT_X87_COMPARE_UNORDERED: {
        const auto right = memory ? LoadMemory(fpu, format, address) : fpu.Get(index);
        const auto left = fpu.Get(0);
        fpu.Operands(left, right);
        fpu.SetConditions(CompareConditions(Compare(left, right, operation == PS5LIFT_X87_COMPARE_UNORDERED)));
        if (format & (PS5LIFT_X87_POP | PS5LIFT_X87_POP_TWICE)) fpu.Pop();
        if (format & PS5LIFT_X87_POP_TWICE) fpu.Pop();
        return 0;
    }
    case PS5LIFT_X87_COMPARE_FLAGS:
    case PS5LIFT_X87_COMPARE_FLAGS_UNORDERED: {
        fpu.ClearRoundedUp();
        const auto left = fpu.Get(0);
        const auto right = fpu.Get(index);
        fpu.Operands(left, right);
        const auto result = Compare(left, right, operation == PS5LIFT_X87_COMPARE_FLAGS_UNORDERED);
        if (format & PS5LIFT_X87_POP) fpu.Pop();
        return result;
    }
    case PS5LIFT_X87_TEST: {
        const auto value = fpu.Get(0);
        fpu.Operands(value, Zero(false));
        fpu.SetConditions(CompareConditions(Compare(value, Zero(false), false)));
        return 0;
    }
    case PS5LIFT_X87_EXAMINE:
        fpu.SetConditions(Examine(fpu));
        return 0;
    case PS5LIFT_X87_CHANGE_SIGN: {
        fpu.ClearRoundedUp();
        auto value = fpu.Get(0);
        value.signExp = static_cast<std::uint16_t>(value.signExp ^ 0x8000);
        fpu.Set(0, value);
        return 0;
    }
    case PS5LIFT_X87_ABSOLUTE: {
        fpu.ClearRoundedUp();
        auto value = fpu.Get(0);
        value.signExp = static_cast<std::uint16_t>(value.signExp & 0x7fff);
        fpu.Set(0, value);
        return 0;
    }
    case PS5LIFT_X87_SQUARE_ROOT: {
        fpu.ClearRoundedUp();
        const auto value = fpu.Get(0);
        fpu.Operands(value, value);
        fpu.Set(0, WithRoundUp(fpu, [&] { return extF80_sqrt(value); }));
        return 0;
    }
    case PS5LIFT_X87_ROUND: {
        fpu.ClearRoundedUp();
        const auto value = fpu.Get(0);
        fpu.Operands(value, value);
        fpu.Set(0, WithRoundUp(fpu, [&] { return extF80_roundToInt(value, softfloat_roundingMode, true); }));
        return 0;
    }
    case PS5LIFT_X87_SCALE: {
        const FullPrecision precision;
        fpu.ClearRoundedUp();
        const auto value = fpu.Get(0);
        const auto scale = fpu.Get(1);
        fpu.Operands(value, scale);
        if (IsNan(value) || IsNan(scale)) {
            fpu.Set(0, extF80_add(value, scale));
            return 0;
        }
        const auto saved = softfloat_exceptionFlags;
        auto power = extF80_to_i64(scale, softfloat_round_minMag, false);
        softfloat_exceptionFlags = saved;
        if (power > 0x10000) power = 0x10000;
        if (power < -0x10000) power = -0x10000;
        const auto scaled = [&] {
            auto result = value;
            auto remaining = power;
            const auto step = remaining > 0 ? 16000 : -16000;
            while (remaining != 0) {
                const auto chunk = (remaining > 0 ? remaining > step : remaining < step) ? step : remaining;
                extFloat80_t factor;
                factor.signExp = static_cast<std::uint16_t>(0x3fff + chunk);
                factor.signif = 0x8000000000000000ull;
                result = extF80_mul(result, factor);
                remaining -= chunk;
            }
            return result;
        };
        fpu.Set(0, WithRoundUp(fpu, scaled));
        return 0;
    }
    case PS5LIFT_X87_PARTIAL_REMAINDER:
    case PS5LIFT_X87_IEEE_REMAINDER: {
        const FullPrecision precision;
        const auto dividend = fpu.Get(0);
        const auto divisor = fpu.Get(1);
        fpu.Operands(dividend, divisor);
        const auto nearest = extF80_rem(dividend, divisor);
        auto truncated = nearest;
        if (!IsNan(nearest) && (nearest.signif != 0 || (nearest.signExp & 0x7fff) != 0) && IsNegative(nearest) != IsNegative(dividend)) {
            auto magnitude = divisor;
            magnitude.signExp = static_cast<std::uint16_t>((magnitude.signExp & 0x7fff) | (dividend.signExp & 0x8000));
            const auto savedFlags = softfloat_exceptionFlags;
            truncated = extF80_add(nearest, magnitude);
            softfloat_exceptionFlags = savedFlags;
        }
        const auto remainder = operation == PS5LIFT_X87_PARTIAL_REMAINDER ? truncated : nearest;
        std::uint16_t conditions = IsNan(remainder) ? static_cast<std::uint16_t>(fpu.Conditions() & ~StatusC2) : 0;
        if (!IsNan(remainder) && (divisor.signExp & 0x7fff) != 0x7fff) {
            auto bits = TruncatedQuotientBits(dividend, divisor);
            const bool roundedUp = truncated.signif != nearest.signif || truncated.signExp != nearest.signExp;
            if (operation == PS5LIFT_X87_IEEE_REMAINDER && roundedUp) bits = (bits + 1) & 7;
            if (bits & 1) conditions |= StatusC1;
            if (bits & 2) conditions |= StatusC3;
            if (bits & 4) conditions |= StatusC0;
        }
        fpu.SetConditions(conditions);
        fpu.Set(0, remainder);
        return 0;
    }
    case PS5LIFT_X87_EXCHANGE: {
        fpu.ClearRoundedUp();
        const auto top = fpu.Get(0);
        const auto other = fpu.Get(index);
        fpu.Set(0, other);
        fpu.Set(index, top);
        return 0;
    }
    case PS5LIFT_X87_FREE:
        fpu.Free(index);
        return 0;
    case PS5LIFT_X87_INCREMENT_TOP:
        fpu.ClearRoundedUp();
        fpu.SetTop(fpu.Top() + 1);
        return 0;
    case PS5LIFT_X87_DECREMENT_TOP:
        fpu.ClearRoundedUp();
        fpu.SetTop(fpu.Top() - 1);
        return 0;
    case PS5LIFT_X87_MOVE_REGISTER:
        fpu.Set(0, fpu.Get(index));
        return 0;
    default:
        Fail("unknown x87 operation", operation);
    }
}

#if LDBL_MANT_DIG != 64
static_assert(LDBL_MANT_DIG == 113 && sizeof(long double) == sizeof(float128_t), "host long double is x87 extended or IEEE binary128");

namespace {

float128_t HostDefaultNan() {
    volatile long double zero = 0;
    const long double nan = zero / zero;
    float128_t bits;
    std::memcpy(&bits, &nan, sizeof(bits));
    return bits;
}

}
#endif

extern "C" void ps5lift_extended_to_host(const void* extended, long double* host) {
    std::memset(host, 0, sizeof(*host));
#if LDBL_MANT_DIG == 64
    std::memcpy(host, extended, 10);
#else
    extFloat80_t value;
    std::memcpy(&value.signif, extended, 8);
    std::memcpy(&value.signExp, static_cast<const unsigned char*>(extended) + 8, 2);
    float128_t converted;
    if (IsNan(value)) {
        converted.v[1] = static_cast<std::uint64_t>(value.signExp) << 48 | (value.signif & 0x7fffffffffffffffull) >> 15;
        converted.v[0] = value.signif << 49;
    } else {
        const auto flags = softfloat_exceptionFlags;
        converted = extF80_to_f128(value);
        softfloat_exceptionFlags = flags;
    }
    std::memcpy(host, &converted, sizeof(converted));
#endif
}

extern "C" void ps5lift_extended_from_host(const Ps5LiftState* state, const long double* host, void* extended) {
#if LDBL_MANT_DIG == 64
    (void)state;
    std::memcpy(extended, host, 10);
#else
    static constexpr std::uint_fast8_t Rounding[4] = {softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag};
    static const float128_t DefaultNan = HostDefaultNan();
    float128_t value;
    std::memcpy(&value, host, sizeof(value));
    if (value.v[0] == DefaultNan.v[0] && value.v[1] == DefaultNan.v[1]) {
        const auto indefinite = Indefinite();
        std::memcpy(extended, &indefinite.signif, 8);
        std::memcpy(static_cast<unsigned char*>(extended) + 8, &indefinite.signExp, 2);
        return;
    }
    const auto flags = softfloat_exceptionFlags;
    const auto rounding = softfloat_roundingMode;
    const auto tininess = softfloat_detectTininess;
    softfloat_roundingMode = Rounding[(state->X87Control >> 10) & 3];
    softfloat_detectTininess = softfloat_tininess_afterRounding;
    const extFloat80_t converted = f128_to_extF80(value);
    softfloat_exceptionFlags = flags;
    softfloat_roundingMode = rounding;
    softfloat_detectTininess = tininess;
    std::memcpy(extended, &converted.signif, 8);
    std::memcpy(static_cast<unsigned char*>(extended) + 8, &converted.signExp, 2);
#endif
}
