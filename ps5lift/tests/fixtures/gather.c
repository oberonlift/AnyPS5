#include "host.h"

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;

static const u32 dwords[16] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
static const u64 qwords[8] = {0x1000000000000001, 0x2000000000000002, 0x3000000000000003, 0x4000000000000004,
                              0x5000000000000005, 0x6000000000000006, 0x7000000000000007, 0x8000000000000008};
static __thread u32 thread_dwords[4] = {0x31, 0x32, 0x33, 0x34};
static const u64 LowAddress = 0x40000000;

static void print(const char* text) {
    u64 length = 0;
    while (text[length]) ++length;
    host_write(text, length);
}

static void print_hex(u64 value, int count) {
    char digits[17];
    digits[count] = 0;
    for (int i = count - 1; i >= 0; --i) {
        digits[i] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
    print(digits);
}

static void print_dwords(const char* name, const u32* values, int count, const u32* mask, int masks) {
    print(name);
    for (int i = 0; i < count; ++i) {
        print(" ");
        print_hex(values[i], 8);
    }
    u32 left = 0;
    for (int i = 0; i < masks; ++i) left |= mask[i];
    print(", mask ");
    print_hex(left, 8);
    print("\n");
}

static void print_qwords(const char* name, const u64* values, int count, const u64* mask, int masks) {
    print(name);
    for (int i = 0; i < count; ++i) {
        print(" ");
        print_hex(values[i], 16);
    }
    u64 left = 0;
    for (int i = 0; i < masks; ++i) left |= mask[i];
    print(", mask ");
    print_hex(left, 16);
    print("\n");
}

[[gnu::noinline]] static void displaced_dwords(void) {
    static const int indexes[8] = {-2, 3, 0, 1 << 28, 5, -1, 7, 1 << 29};
    static const int mask[8] = {-1, -1, -1, 0, -1, (int)0x80000000, -1, 0x7fffffff};
    static const u32 fill = 0x77;
    u32 out[8];
    u32 maskOut[8];
    __asm__ volatile("vmovdqu %[index], %%ymm1\n\t"
                     "vmovdqu %[mask], %%ymm2\n\t"
                     "vpbroadcastd %[fill], %%ymm0\n\t"
                     "vpgatherdd %%ymm2, 16(%[base],%%ymm1,4), %%ymm0\n\t"
                     "vmovdqu %%ymm0, %[out]\n\t"
                     "vmovdqu %%ymm2, %[maskOut]"
                     : [out] "=m"(out), [maskOut] "=m"(maskOut)
                     : [index] "m"(indexes), [mask] "m"(mask), [fill] "m"(fill), [base] "r"(dwords)
                     : "xmm0", "xmm1", "xmm2");
    print_dwords("vpgatherdd displaced", out, 8, maskOut, 8);
}

[[gnu::noinline]] static void negative_qwords(void) {
    const u64* base = qwords + 4;
    const i64 indexes[4] = {-4, -1, 3, -(i64)((u64)base / 8)};
    static const u64 mask[4] = {~0ul, ~0ul, 0x8000000000000000, 0x7fffffffffffffff};
    static const u64 fill = 0x5555555555555555;
    u64 out[4];
    u64 maskOut[4];
    __asm__ volatile("vmovdqu %[index], %%ymm4\n\t"
                     "vmovdqu %[mask], %%ymm5\n\t"
                     "vpbroadcastq %[fill], %%ymm3\n\t"
                     "vpgatherqq %%ymm5, (%[base],%%ymm4,8), %%ymm3\n\t"
                     "vmovdqu %%ymm3, %[out]\n\t"
                     "vmovdqu %%ymm5, %[maskOut]"
                     : [out] "=m"(out), [maskOut] "=m"(maskOut)
                     : [index] "m"(indexes), [mask] "m"(mask), [fill] "m"(fill), [base] "r"(base)
                     : "xmm3", "xmm4", "xmm5");
    print_qwords("vpgatherqq negative", out, 4, maskOut, 4);
}

[[gnu::noinline]] static void narrowed_singles(void) {
    static const i64 indexes[4] = {-8, 7, -1, 0};
    static const int mask[4] = {-1, 0, -1, -1};
    u32 out[8];
    u32 maskOut[4];
    __asm__ volatile("vmovdqu %[index], %%ymm10\n\t"
                     "vmovdqu %[mask], %%xmm11\n\t"
                     "vpcmpeqd %%ymm9, %%ymm9, %%ymm9\n\t"
                     "vgatherqps %%xmm11, (%[base],%%ymm10,4), %%xmm9\n\t"
                     "vmovdqu %%ymm9, %[out]\n\t"
                     "vmovdqu %%xmm11, %[maskOut]"
                     : [out] "=m"(out), [maskOut] "=m"(maskOut)
                     : [index] "m"(indexes), [mask] "m"(mask), [base] "r"(dwords + 8)
                     : "xmm9", "xmm10", "xmm11");
    print_dwords("vgatherqps narrowed", out, 8, maskOut, 4);
}

[[gnu::noinline]] static void thread_segment(void) {
    static const i64 indexes[2] = {3, 1};
    static const int mask[4] = {-1, -1, 0, 0};
    u64 thread_pointer;
    __asm__("mov %%fs:0, %0" : "=r"(thread_pointer));
    const u64 offset = (u64)&thread_dwords[0] - thread_pointer;
    u32 out[4];
    u32 maskOut[4];
    __asm__ volatile("vmovdqu %[index], %%xmm7\n\t"
                     "vmovdqu %[mask], %%xmm8\n\t"
                     "vpcmpeqd %%xmm6, %%xmm6, %%xmm6\n\t"
                     "vpgatherqd %%xmm8, %%fs:(%[offset],%%xmm7,4), %%xmm6\n\t"
                     "vmovdqu %%xmm6, %[out]\n\t"
                     "vmovdqu %%xmm8, %[maskOut]"
                     : [out] "=m"(out), [maskOut] "=m"(maskOut)
                     : [index] "m"(indexes), [mask] "m"(mask), [offset] "r"(offset)
                     : "xmm6", "xmm7", "xmm8");
    print_dwords("vpgatherqd fs", out, 4, maskOut, 4);
}

[[gnu::noinline]] static void address_size_32(void) {
    u64* low = host_map_fixed(LowAddress, 4096);
    if ((u64)low != LowAddress) {
        print("host_map_fixed failed\n");
        return;
    }
    for (int i = 0; i < 4; ++i) low[i] = 0xa1 + (u64)i;
    static const i64 indexes[4] = {-1, 0, 1l << 29, 2};
    static const u64 mask[4] = {~0ul, ~0ul, ~0ul, ~0ul};
    static const u64 fill = 0x66;
    u64 out[4];
    u64 maskOut[4];
    __asm__ volatile("vmovdqu %[index], %%ymm13\n\t"
                     "vmovdqu %[mask], %%ymm14\n\t"
                     "vpbroadcastq %[fill], %%ymm12\n\t"
                     "vpgatherqq %%ymm14, 8(%%eax,%%ymm13,8), %%ymm12\n\t"
                     "vmovdqu %%ymm12, %[out]\n\t"
                     "vmovdqu %%ymm14, %[maskOut]"
                     : [out] "=m"(out), [maskOut] "=m"(maskOut)
                     : [index] "m"(indexes), [mask] "m"(mask), [fill] "m"(fill), "a"(low)
                     : "xmm12", "xmm13", "xmm14");
    print_qwords("vpgatherqq addr32", out, 4, maskOut, 4);
}

int main(void) {
    displaced_dwords();
    negative_qwords();
    narrowed_singles();
    thread_segment();
    address_size_32();
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
