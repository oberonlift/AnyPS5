#include "prx/libc/include/Api.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <limits>

#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestLongDouble.hpp"

namespace {

class ErrnoGuard {
public:
    ErrnoGuard() : saved(errno) {}
    ~ErrnoGuard() { errno = saved; }
    ErrnoGuard(const ErrnoGuard&) = delete;
    ErrnoGuard& operator=(const ErrnoGuard&) = delete;

private:
    int saved;
};

struct ExtendedBits {
    std::uint64_t mantissa;
    std::uint16_t signExponent;
};

ExtendedBits Bits(const void* bytes) {
    ExtendedBits bits;
    std::memcpy(&bits.mantissa, bytes, sizeof(bits.mantissa));
    std::memcpy(&bits.signExponent, static_cast<const unsigned char*>(bytes) + 8, sizeof(bits.signExponent));
    return bits;
}

ExtendedBits Bits(const LibcDetail::GuestLongDouble& value) {
    return Bits(LibcDetail::GuestLongDoubleImage(value).data());
}

constexpr short Denormal = -2, Finite = -1, Zero = 0, Infinite = 1, NotANumber = 2;

short TestExtended(const ExtendedBits& bits) {
    const int exponent = bits.signExponent & 0x7fff;
    const std::uint64_t fraction = bits.mantissa & 0x7fffffffffffffffull;
    if (exponent == 0x7fff) return fraction != 0 ? NotANumber : Infinite;
    if ((bits.mantissa >> 63) != 0) return Finite;
    return exponent != 0 || bits.mantissa != 0 ? Denormal : Zero;
}

template<typename TFloat>
short TestFloat(TFloat value) {
    switch (std::fpclassify(value)) {
    case FP_NAN:
        return NotANumber;
    case FP_INFINITE:
        return Infinite;
    case FP_ZERO:
        return Zero;
    case FP_SUBNORMAL:
        return Denormal;
    default:
        return Finite;
    }
}

template <typename T>
T Remainder(T x, T y) {
    const bool domain = !LibcDetail::IsNan(x) && !LibcDetail::IsNan(y) && (LibcDetail::IsInf(x) || y == T(0));
    T result;
    {
        ErrnoGuard guard;
        long long quotient = 0;
        result = LibcDetail::Remainder(x, y, true, &quotient);
    }
    if (domain) errno = EDOM;
    return result;
}

int Exponent(std::int64_t exponent) {
    return static_cast<int>(std::clamp<std::int64_t>(exponent, INT_MIN, INT_MAX));
}

#if !defined(__SSE2__)
std::int64_t ConvertToInteger(double x) {
    if (!(x >= -0x1p63 && x < 0x1p63)) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(std::nearbyint(x));
}
#endif

}

extern "C" {

LibcDetail::GuestLongDouble APS5_VABI copysignl_nid_postfix(LibcDetail::GuestLongDouble x, LibcDetail::GuestLongDouble y) { return LibcDetail::CopySign(x, y); }
LibcDetail::GuestLongDouble APS5_VABI fdiml_nid_postfix(LibcDetail::GuestLongDouble x, LibcDetail::GuestLongDouble y) {
    if (LibcDetail::IsNan(x)) return x;
    if (LibcDetail::IsNan(y)) return y;
    return x > y ? x - y : LibcDetail::GuestLongDouble {};
}

LibcDetail::GuestLongDouble APS5_VABI fmodl_nid_postfix(LibcDetail::GuestLongDouble x, LibcDetail::GuestLongDouble y) {
    if (!LibcDetail::IsFinite(x) || y == 0 || LibcDetail::IsNan(y)) {
        errno = EDOM;
        return (x * y) / (x * y);
    }
    ErrnoGuard guard;
    long long quotient = 0;
    return LibcDetail::Remainder(x, y, false, &quotient);
}

LibcDetail::GuestLongDouble APS5_VABI remainderl_nid_postfix(LibcDetail::GuestLongDouble x, LibcDetail::GuestLongDouble y) {
    return Remainder(x, y);
}

LibcDetail::GuestLongDouble APS5_VABI remquol_nid_postfix(LibcDetail::GuestLongDouble x, LibcDetail::GuestLongDouble y, int* quotient) {
    const bool domain = !LibcDetail::IsNan(x) && !LibcDetail::IsNan(y) && (LibcDetail::IsInf(x) || y == 0);
    long long bits = 0;
    LibcDetail::GuestLongDouble result;
    {
        ErrnoGuard guard;
        result = LibcDetail::Remainder(x, y, true, &bits);
    }
    if (domain) errno = EDOM;
    if (!LibcDetail::IsNan(result)) *quotient = static_cast<int>(bits < 0 ? -(-bits & 7) : (bits & 7));
    return result;
}

LibcDetail::GuestLongDouble APS5_VABI logbl_nid_postfix(LibcDetail::GuestLongDouble x) {
    ErrnoGuard guard;
    return LibcDetail::Logb(x);
}

int APS5_VABI ilogbl_nid_postfix(LibcDetail::GuestLongDouble x) {
    if ((Bits(x).signExponent & 0x7fff) == 0x7fff) return INT_MAX;
    ErrnoGuard guard;
    return LibcDetail::Ilogb(x);
}

LibcDetail::GuestLongDouble APS5_VABI rintl_nid_postfix(LibcDetail::GuestLongDouble x) { return LibcDetail::Rint(x); }
LibcDetail::GuestLongDouble APS5_VABI roundl_nid_postfix(LibcDetail::GuestLongDouble x) {
    return LibcDetail::IsFinite(x) ? LibcDetail::RoundIntegral(x, LibcDetail::IntegralRounding::HalfAwayFromZero) : x;
}

std::int64_t APS5_VABI lroundl_nid_postfix(LibcDetail::GuestLongDouble x) {
    ErrnoGuard guard;
    return LibcDetail::LlRound(x);
}

std::int64_t APS5_VABI lrintl_nid_postfix(LibcDetail::GuestLongDouble x) {
    ErrnoGuard guard;
    return LibcDetail::LlRint(x);
}

long long APS5_VABI llrintl_nid_postfix(LibcDetail::GuestLongDouble x) {
    ErrnoGuard guard;
    return LibcDetail::LlRint(x);
}

LibcDetail::GuestLongDouble APS5_VABI sqrtl_nid_postfix(LibcDetail::GuestLongDouble x) {
    return LibcDetail::Sqrt(x);
}

LibcDetail::GuestLongDouble APS5_VABI modfl_nid_postfix(LibcDetail::GuestLongDouble x, LibcDetail::GuestLongDouble* integral) {
    if (LibcDetail::IsNan(x)) {
        LibcDetail::GuestLongDoubleStore(integral, x);
        return x;
    }
    LibcDetail::GuestLongDouble whole;
    const LibcDetail::GuestLongDouble fraction = LibcDetail::Modf(x, &whole);
    LibcDetail::GuestLongDoubleStore(integral, whole);
    return fraction;
}
LibcDetail::GuestLongDouble APS5_VABI nanl_nid_postfix(const char*) { return std::numeric_limits<LibcDetail::GuestLongDouble>::quiet_NaN(); }

LibcDetail::GuestLongDouble APS5_VABI scalblnl_nid_postfix(LibcDetail::GuestLongDouble x, std::int64_t exponent) {
    ErrnoGuard guard;
    return LibcDetail::Scalbn(x, static_cast<int>(exponent));
}

LibcDetail::GuestLongDouble APS5_VABI fmal_nid_postfix(LibcDetail::GuestLongDouble x, LibcDetail::GuestLongDouble y, LibcDetail::GuestLongDouble z) {
    if (x == 0 || y == 0) return x * y + z;
    if (z == 0) return x * y;
    if (!LibcDetail::IsFinite(x) || !LibcDetail::IsFinite(y)) return x * y + z;
    if (!LibcDetail::IsFinite(z)) return z;
    int xExponent = 0, yExponent = 0, zExponent = 0;
    LibcDetail::Frexp(x, &xExponent);
    LibcDetail::Frexp(y, &yExponent);
    LibcDetail::Frexp(z, &zExponent);
    if (xExponent + yExponent - zExponent > 2 * std::numeric_limits<LibcDetail::GuestLongDouble>::digits) return x * y;
    ErrnoGuard guard;
    return LibcDetail::Fma(x, y, z);
}

double APS5_VABI rint_nid_postfix(double x) { return std::isnan(x) ? x + x : std::rint(x); }
float APS5_VABI rintf_nid_postfix(float x) { return std::isnan(x) ? x + x : std::rint(x); }
float APS5_VABI nearbyintf_nid_postfix(float x) { return std::isnan(x) ? x + x : std::rintf(x); }

#if defined(__SSE2__)
std::int64_t APS5_VABI lrint_nid_postfix(double x) { return _mm_cvtsd_si64(_mm_set_sd(x)); }

std::int64_t APS5_VABI lrintf_nid_postfix(float x) { return _mm_cvtss_si64(_mm_set_ss(x)); }

long long APS5_VABI llrint_nid_postfix(double x) { return _mm_cvtsd_si64(_mm_set_sd(x)); }

long long APS5_VABI llrintf_nid_postfix(float x) { return _mm_cvtss_si64(_mm_set_ss(x)); }
#else
std::int64_t APS5_VABI lrint_nid_postfix(double x) { return ConvertToInteger(x); }

std::int64_t APS5_VABI lrintf_nid_postfix(float x) { return ConvertToInteger(x); }

long long APS5_VABI llrint_nid_postfix(double x) { return ConvertToInteger(x); }

long long APS5_VABI llrintf_nid_postfix(float x) { return ConvertToInteger(x); }
#endif

double APS5_VABI drem_nid_postfix(double x, double y) {
    return Remainder(x, y);
}

float APS5_VABI dremf_nid_postfix(float x, float y) {
    return Remainder(x, y);
}

double APS5_VABI scalbln_nid_postfix(double x, std::int64_t exponent) {
    ErrnoGuard guard;
    return std::scalbn(x, Exponent(exponent));
}

float APS5_VABI scalblnf_nid_postfix(float x, std::int64_t exponent) {
    ErrnoGuard guard;
    return std::scalbn(x, Exponent(exponent));
}

double APS5_VABI scalb_nid_postfix(double x, double exponent) {
    ErrnoGuard guard;
    if (std::isnan(x) || std::isnan(exponent)) return x * exponent;
    if (std::isinf(exponent)) return exponent > 0 ? x * exponent : x / -exponent;
    if (std::rint(exponent) != exponent) return (exponent - exponent) / (exponent - exponent);
    if (exponent > 65000.0) return std::scalbn(x, 65000);
    if (exponent < -65000.0) return std::scalbn(x, -65000);
    return std::scalbn(x, static_cast<int>(exponent));
}

float APS5_VABI scalbf_nid_postfix(float x, float exponent) {
    ErrnoGuard guard;
    if (std::isnan(x) || std::isnan(exponent)) return x * exponent;
    if (std::isinf(exponent)) return exponent > 0 ? x * exponent : x / -exponent;
    if (std::rint(exponent) != exponent) return (exponent - exponent) / (exponent - exponent);
    if (exponent > 65000.0f) return std::scalbn(x, 65000);
    if (exponent < -65000.0f) return std::scalbn(x, -65000);
    return std::scalbn(x, static_cast<int>(exponent));
}

int APS5_VABI finite_nid_postfix(double x) { return std::isfinite(x) ? 1 : 0; }
int APS5_VABI finitef_nid_postfix(float x) { return std::isfinite(x) ? 1 : 0; }

int APS5_VABI __fpclassifyl_nid_postfix(LibcDetail::GuestLongDouble x) {
    const ExtendedBits bits = Bits(x);
    const int exponent = bits.signExponent & 0x7fff;
    if (exponent == 0) return bits.mantissa != 0 ? Denormal : Zero;
    return exponent != 0x7fff ? Finite : TestExtended(bits);
}
int APS5_VABI __isinfl_nid_postfix(LibcDetail::GuestLongDouble x) { return TestExtended(Bits(x)) == Infinite ? 1 : 0; }
int APS5_VABI __isnanl_nid_postfix(LibcDetail::GuestLongDouble x) { return TestExtended(Bits(x)) == NotANumber ? 1 : 0; }

int APS5_VABI __isnormall_nid_postfix(LibcDetail::GuestLongDouble x) {
    const int exponent = Bits(x).signExponent & 0x7fff;
    return exponent != 0 && exponent != 0x7fff ? 1 : 0;
}

short APS5_VABI _LDtest_nid_postfix(const LibcDetail::GuestLongDouble* value) { return TestExtended(Bits(static_cast<const void*>(value))); }
int APS5_VABI _Dclass_nid_postfix(double x) { return TestFloat(x); }
int APS5_VABI _FDclass_nid_postfix(float x) { return TestFloat(x); }
int APS5_VABI _LDclass_nid_postfix(LibcDetail::GuestLongDouble x) { return TestExtended(Bits(x)); }
short APS5_VABI _LDsign_nid_postfix(LibcDetail::GuestLongDouble x) { return static_cast<short>(Bits(x).signExponent & 0x8000); }

}
