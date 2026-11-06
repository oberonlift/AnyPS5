#pragma once

extern "C" {
#include <softfloat.h>
}

namespace Ps5Lift::X87 {

inline constexpr float128_t Pi{{0x8469898cc51701b8ull, 0x4000921fb54442d1ull}};
inline constexpr float128_t ThreeQuarterPi{{0x234f272993d1414aull, 0x40002d97c7f3321dull}};
inline constexpr float128_t Ln2{{0xf35793c7673007e6ull, 0x3ffe62e42fefa39eull}};
inline constexpr float128_t Log2E{{0xe1777d0ffda0d23aull, 0x3fff71547652b82full}};
inline constexpr int LinearExponent = -50;

float128_t Sine(extFloat80_t x);
float128_t Cosine(extFloat80_t x);
float128_t Tangent(extFloat80_t x);
float128_t Arctangent(extFloat80_t y, extFloat80_t x, int scale);
float128_t PowerOfTwoMinusOne(extFloat80_t x, int scale);
float128_t LogarithmProduct(extFloat80_t y, extFloat80_t x, int scale);
float128_t LogarithmPlusOneProduct(extFloat80_t y, extFloat80_t x, int scale);
float128_t Scaled(float128_t value, int scale);

}
