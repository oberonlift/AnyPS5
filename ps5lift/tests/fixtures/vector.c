#include "host.h"

#include <immintrin.h>

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

void* memcpy(void* destination, const void* source, u64 length) {
    u8* to = destination;
    const u8* from = source;
    while (length--) *to++ = *from++;
    return destination;
}

void* memset(void* destination, int value, u64 length) {
    u8* to = destination;
    while (length--) *to++ = (u8)value;
    return destination;
}

static u64 length_of(const char* text) {
    u64 length = 0;
    while (text[length]) ++length;
    return length;
}

static void print(const char* text) {
    host_write(text, length_of(text));
}

static void print_hex(u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buffer[17];
    for (int i = 0; i < 16; ++i) buffer[i] = digits[(value >> (60 - 4 * i)) & 0xf];
    buffer[16] = 0;
    print(buffer);
}

static u64 rng_state = 0x243f6a8885a308d3ul;

static u64 next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static volatile u64 sink;

__attribute__((noinline)) static __m256i random256(int special) {
    u64 lanes[4];
    for (int i = 0; i < 4; ++i) lanes[i] = next_random();
    if (special == 1) lanes[1] = 0x8000000080000000ul;
    if (special == 2) lanes[2] = 0x7fff80007fff8000ul;
    if (special == 3) lanes[3] = 0x00ff7f8001fe0000ul;
    __m256i value;
    memcpy(&value, lanes, sizeof(value));
    return value;
}

static u64 fold256(__m256i value) {
    u64 lanes[4];
    memcpy(lanes, &value, sizeof(lanes));
    u64 hash = 0xcbf29ce484222325ul;
    for (int i = 0; i < 4; ++i) hash = (hash ^ lanes[i]) * 0x100000001b3ul;
    return hash;
}

static u64 fold128(__m128i value) {
    return fold256(_mm256_set_m128i(value, _mm_setzero_si128())) ^ fold256(_mm256_set_m128i(_mm_setzero_si128(), value));
}

static void report(const char* name, u64 value) {
    print(name);
    print(" ");
    print_hex(value);
    print("\n");
}

#define BINARY256(name, expression)                                     \
    __attribute__((noinline)) static u64 test_##name(void) {            \
        u64 hash = 0;                                                   \
        for (int round = 0; round < 8; ++round) {                       \
            __m256i a = random256(round & 3), b = random256((round + 1) & 3); \
            hash = hash * 31 + fold256(expression);                     \
        }                                                               \
        return hash;                                                    \
    }

#define BINARY128(name, expression)                                     \
    __attribute__((noinline)) static u64 test_##name(void) {            \
        u64 hash = 0;                                                   \
        for (int round = 0; round < 8; ++round) {                       \
            __m128i a = _mm256_castsi256_si128(random256(round & 3));   \
            __m128i b = _mm256_extracti128_si256(random256((round + 1) & 3), 1); \
            hash = hash * 31 + fold128(expression);                     \
        }                                                               \
        return hash;                                                    \
    }

BINARY256(add8, _mm256_add_epi8(a, b))
BINARY256(add16, _mm256_add_epi16(a, b))
BINARY256(add32, _mm256_add_epi32(a, b))
BINARY256(add64, _mm256_add_epi64(a, b))
BINARY256(sub8, _mm256_sub_epi8(a, b))
BINARY256(sub64, _mm256_sub_epi64(a, b))
BINARY256(adds8, _mm256_adds_epi8(a, b))
BINARY256(adds16, _mm256_adds_epi16(a, b))
BINARY256(addsu8, _mm256_adds_epu8(a, b))
BINARY256(subs16, _mm256_subs_epi16(a, b))
BINARY256(subsu16, _mm256_subs_epu16(a, b))
BINARY256(mullo16, _mm256_mullo_epi16(a, b))
BINARY256(mullo32, _mm256_mullo_epi32(a, b))
BINARY256(mulhi16, _mm256_mulhi_epi16(a, b))
BINARY256(mulhiu16, _mm256_mulhi_epu16(a, b))
BINARY256(mulu32, _mm256_mul_epu32(a, b))
BINARY256(mul32, _mm256_mul_epi32(a, b))
BINARY256(madd16, _mm256_madd_epi16(a, b))
BINARY256(min8, _mm256_min_epi8(a, b))
BINARY256(minu16, _mm256_min_epu16(a, b))
BINARY256(max32, _mm256_max_epi32(a, b))
BINARY256(maxu32, _mm256_max_epu32(a, b))
BINARY256(cmpeq8, _mm256_cmpeq_epi8(a, _mm256_or_si256(a, _mm256_and_si256(b, _mm256_set1_epi8(1)))))
BINARY256(cmpeq64, _mm256_cmpeq_epi64(a, _mm256_blend_epi32(a, b, 0x30)))
BINARY256(cmpgt16, _mm256_cmpgt_epi16(a, b))
BINARY256(cmpgt64, _mm256_cmpgt_epi64(a, b))
BINARY256(avg8, _mm256_avg_epu8(a, b))
BINARY256(avg16, _mm256_avg_epu16(a, b))
BINARY256(andnot, _mm256_andnot_si256(a, b))
BINARY256(abs8, _mm256_abs_epi8(a))
BINARY256(abs32, _mm256_abs_epi32(b))
BINARY256(slli16, _mm256_slli_epi16(a, 5))
BINARY256(slli64, _mm256_slli_epi64(a, 63))
BINARY256(srli32, _mm256_srli_epi32(a, 31))
BINARY256(srai16, _mm256_srai_epi16(a, 3))
BINARY256(srai32big, _mm256_srai_epi32(a, 40))
BINARY256(sll32, _mm256_sll_epi32(a, _mm_cvtsi32_si128((int)(next_random() & 31))))
BINARY256(srl64big, _mm256_srl_epi64(a, _mm_cvtsi32_si128(70)))
BINARY256(sra16, _mm256_sra_epi16(a, _mm_cvtsi32_si128((int)(next_random() & 15))))
BINARY256(sllv32, _mm256_sllv_epi32(a, _mm256_and_si256(b, _mm256_set1_epi32(63))))
BINARY256(srlv64, _mm256_srlv_epi64(a, _mm256_and_si256(b, _mm256_set1_epi64x(127))))
BINARY256(srav32, _mm256_srav_epi32(a, _mm256_and_si256(b, _mm256_set1_epi32(63))))
BINARY256(bslli, _mm256_bslli_epi128(a, 5))
BINARY256(bsrli, _mm256_bsrli_epi128(a, 11))
BINARY256(shuffle32, _mm256_shuffle_epi32(a, 0x9c))
BINARY256(shufflelo, _mm256_shufflelo_epi16(a, 0x1b))
BINARY256(shufflehi, _mm256_shufflehi_epi16(a, 0xe4 ^ 0x5a))
BINARY256(shufps, _mm256_castps_si256(_mm256_shuffle_ps(_mm256_castsi256_ps(a), _mm256_castsi256_ps(b), 0x4e)))
BINARY256(unpacklo8, _mm256_unpacklo_epi8(a, b))
BINARY256(unpackhi16, _mm256_unpackhi_epi16(a, b))
BINARY256(unpacklo32, _mm256_unpacklo_epi32(a, b))
BINARY256(unpackhi64, _mm256_unpackhi_epi64(a, b))
BINARY256(packs16, _mm256_packs_epi16(a, b))
BINARY256(packs32, _mm256_packs_epi32(a, b))
BINARY256(packus16, _mm256_packus_epi16(a, b))
BINARY256(packus32, _mm256_packus_epi32(a, b))
BINARY256(alignr, _mm256_alignr_epi8(a, b, 7))
BINARY256(alignr20, _mm256_alignr_epi8(a, b, 20))
BINARY256(shuffle8, _mm256_shuffle_epi8(a, b))
BINARY256(permute4x64, _mm256_permute4x64_epi64(a, 0x72))
BINARY256(permutevar8x32, _mm256_permutevar8x32_epi32(a, b))
BINARY256(permute2x128, _mm256_permute2x128_si256(a, b, 0x21))
BINARY256(permute2x128zero, _mm256_permute2x128_si256(a, b, 0x83))
BINARY256(inserti128, _mm256_inserti128_si256(a, _mm256_castsi256_si128(b), 1))
BINARY256(blend32, _mm256_blend_epi32(a, b, 0xa5))
BINARY256(blend16, _mm256_blend_epi16(a, b, 0x3c))
BINARY256(blendv8, _mm256_blendv_epi8(a, b, _mm256_xor_si256(a, b)))
BINARY256(cvt8to16, _mm256_cvtepu8_epi16(_mm256_castsi256_si128(a)))
BINARY256(cvt8to64s, _mm256_cvtepi8_epi64(_mm256_castsi256_si128(a)))
BINARY256(cvt16to32s, _mm256_cvtepi16_epi32(_mm256_castsi256_si128(b)))
BINARY256(cvt32to64, _mm256_cvtepu32_epi64(_mm256_castsi256_si128(b)))
BINARY256(movemask, _mm256_set1_epi32(_mm256_movemask_epi8(a)))
BINARY256(movemaskps, _mm256_set1_epi32(_mm256_movemask_ps(_mm256_castsi256_ps(a))))
BINARY256(testz, _mm256_set1_epi32(_mm256_testz_si256(a, b) + 2 * _mm256_testc_si256(a, a) + 4 * _mm256_testz_si256(a, _mm256_setzero_si256())))
BINARY256(broadcast, _mm256_add_epi32(_mm256_broadcastd_epi32(_mm256_castsi256_si128(a)), _mm256_broadcastb_epi8(_mm256_castsi256_si128(b))))

BINARY128(add16x, _mm_add_epi16(a, b))
BINARY128(sub32x, _mm_sub_epi32(a, b))
BINARY128(shuffle8x, _mm_shuffle_epi8(a, b))
BINARY128(unpacklo8x, _mm_unpacklo_epi8(a, b))
BINARY128(packus16x, _mm_packus_epi16(a, b))
BINARY128(alignrx, _mm_alignr_epi8(a, b, 3))
BINARY128(slli32x, _mm_slli_epi32(a, 9))
BINARY128(srli64x, _mm_srli_epi64(a, 33))
BINARY128(bsrlix, _mm_srli_si128(a, 9))
BINARY128(shuffle32x, _mm_shuffle_epi32(a, 0x39))
BINARY128(blendvx, _mm_blendv_epi8(a, b, _mm_xor_si128(a, b)))
BINARY128(extract, _mm_set_epi32(_mm_extract_epi8(a, 13), _mm_extract_epi16(b, 6), _mm_extract_epi32(a, 2), (int)_mm_extract_epi64(b, 1)))
BINARY128(insert, _mm_insert_epi64(_mm_insert_epi32(_mm_insert_epi16(_mm_insert_epi8(a, 0x5a, 9), 0x1234, 3), (int)0xdeadbeef, 2), (long long)0x0123456789abcdef, 0))
BINARY128(extracti128, _mm256_extracti128_si256(_mm256_set_m128i(a, b), 1))
BINARY128(cvt16to64x, _mm_cvtepi16_epi64(a))
BINARY128(min32ux, _mm_min_epu32(a, b))
BINARY128(cmpeq32x, _mm_cmpeq_epi32(a, _mm_blend_epi16(a, b, 0x0f)))
BINARY128(movemaskx, _mm_set1_epi32(_mm_movemask_epi8(a)))
BINARY256(maddubs16, _mm256_maddubs_epi16(a, b))
BINARY256(mulhrs16, _mm256_mulhrs_epi16(a, b))
BINARY256(hadd32, _mm256_hadd_epi32(a, b))
BINARY256(hadds16, _mm256_hadds_epi16(a, b))
BINARY256(hsub16, _mm256_hsub_epi16(a, b))
BINARY256(hsubs16, _mm256_hsubs_epi16(a, b))
BINARY256(sad8, _mm256_sad_epu8(a, b))
BINARY256(testps, _mm256_set1_epi32(_mm256_testz_ps(_mm256_castsi256_ps(a), _mm256_castsi256_ps(b)) * 2 + _mm256_testc_ps(_mm256_castsi256_ps(a), _mm256_castsi256_ps(b))))
BINARY256(testpd, _mm256_set1_epi32(_mm256_testz_pd(_mm256_castsi256_pd(a), _mm256_castsi256_pd(a)) * 2 + _mm256_testc_pd(_mm256_castsi256_pd(b), _mm256_castsi256_pd(a))))
BINARY128(maddubs16x, _mm_maddubs_epi16(a, b))
BINARY128(mulhrs16x, _mm_mulhrs_epi16(a, b))
BINARY128(hadd16x, _mm_hadd_epi16(a, b))
BINARY128(hsub32x, _mm_hsub_epi32(a, b))
BINARY128(sad8x, _mm_sad_epu8(a, b))

struct Test {
    const char* name;
    u64 (*run)(void);
};

#define ENTRY(name) {#name, test_##name}

static const struct Test tests[] = {
    ENTRY(add8), ENTRY(add16), ENTRY(add32), ENTRY(add64), ENTRY(sub8), ENTRY(sub64), ENTRY(adds8), ENTRY(adds16), ENTRY(addsu8),
    ENTRY(subs16), ENTRY(subsu16), ENTRY(mullo16), ENTRY(mullo32), ENTRY(mulhi16), ENTRY(mulhiu16), ENTRY(mulu32), ENTRY(mul32),
    ENTRY(madd16), ENTRY(min8), ENTRY(minu16), ENTRY(max32), ENTRY(maxu32), ENTRY(cmpeq8), ENTRY(cmpeq64), ENTRY(cmpgt16),
    ENTRY(cmpgt64), ENTRY(avg8), ENTRY(avg16), ENTRY(andnot), ENTRY(abs8), ENTRY(abs32), ENTRY(slli16), ENTRY(slli64), ENTRY(srli32),
    ENTRY(srai16), ENTRY(srai32big), ENTRY(sll32), ENTRY(srl64big), ENTRY(sra16), ENTRY(sllv32), ENTRY(srlv64), ENTRY(srav32),
    ENTRY(bslli), ENTRY(bsrli), ENTRY(shuffle32), ENTRY(shufflelo), ENTRY(shufflehi), ENTRY(shufps), ENTRY(unpacklo8),
    ENTRY(unpackhi16), ENTRY(unpacklo32), ENTRY(unpackhi64), ENTRY(packs16), ENTRY(packs32), ENTRY(packus16), ENTRY(packus32),
    ENTRY(alignr), ENTRY(alignr20), ENTRY(shuffle8), ENTRY(permute4x64), ENTRY(permutevar8x32), ENTRY(permute2x128),
    ENTRY(permute2x128zero), ENTRY(inserti128), ENTRY(blend32), ENTRY(blend16), ENTRY(blendv8), ENTRY(cvt8to16), ENTRY(cvt8to64s),
    ENTRY(cvt16to32s), ENTRY(cvt32to64), ENTRY(movemask), ENTRY(movemaskps), ENTRY(testz), ENTRY(broadcast),
    ENTRY(add16x), ENTRY(sub32x), ENTRY(shuffle8x), ENTRY(unpacklo8x), ENTRY(packus16x), ENTRY(alignrx), ENTRY(slli32x),
    ENTRY(srli64x), ENTRY(bsrlix), ENTRY(shuffle32x), ENTRY(blendvx), ENTRY(extract), ENTRY(insert), ENTRY(extracti128),
    ENTRY(cvt16to64x), ENTRY(min32ux), ENTRY(cmpeq32x), ENTRY(movemaskx),
    ENTRY(maddubs16), ENTRY(mulhrs16), ENTRY(hadd32), ENTRY(hadds16), ENTRY(hsub16), ENTRY(hsubs16), ENTRY(sad8), ENTRY(testps),
    ENTRY(testpd), ENTRY(maddubs16x), ENTRY(mulhrs16x), ENTRY(hadd16x), ENTRY(hsub32x), ENTRY(sad8x),
};

int main(void) {
    for (u64 i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) report(tests[i].name, tests[i].run());
    return 0;
}

__asm__(".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call main\n"
        "    mov %eax, %edi\n"
        "    call host_exit\n"
        "    ud2\n");
