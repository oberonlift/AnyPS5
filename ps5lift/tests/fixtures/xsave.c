#include "host.h"

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

static _Alignas(32) u8 pattern[16 * 32];
static _Alignas(32) u8 restored[16 * 32];
static _Alignas(64) u8 area[1024];
static const u32 Mxcsr = 0x7f80;
static const u32 Clobbered = 0x1f80;
static u32 restoredMxcsr;

static void print(const char* text) {
    u64 length = 0;
    while (text[length]) ++length;
    host_write(text, length);
}

static void print_hex(const u64 value, const int digits) {
    static const char Hex[] = "0123456789abcdef";
    char text[16];
    for (int i = 0; i < digits; ++i) text[i] = Hex[(value >> 4 * (digits - 1 - i)) & 0xf];
    host_write(text, (u64)digits);
}

#define PS5LIFT_ROUND_TRIP(save)                                                                              \
    __asm__ volatile("vmovdqu 0(%[pattern]), %%ymm0\n\tvmovdqu 32(%[pattern]), %%ymm1\n\t"                  \
                     "vmovdqu 64(%[pattern]), %%ymm2\n\tvmovdqu 96(%[pattern]), %%ymm3\n\t"                 \
                     "vmovdqu 128(%[pattern]), %%ymm4\n\tvmovdqu 160(%[pattern]), %%ymm5\n\t"               \
                     "vmovdqu 192(%[pattern]), %%ymm6\n\tvmovdqu 224(%[pattern]), %%ymm7\n\t"               \
                     "vmovdqu 256(%[pattern]), %%ymm8\n\tvmovdqu 288(%[pattern]), %%ymm9\n\t"               \
                     "vmovdqu 320(%[pattern]), %%ymm10\n\tvmovdqu 352(%[pattern]), %%ymm11\n\t"             \
                     "vmovdqu 384(%[pattern]), %%ymm12\n\tvmovdqu 416(%[pattern]), %%ymm13\n\t"             \
                     "vmovdqu 448(%[pattern]), %%ymm14\n\tvmovdqu 480(%[pattern]), %%ymm15\n\t"             \
                     "ldmxcsr %[mxcsr]\n\t"                                                                 \
                     "movl $7, %%eax\n\txorl %%edx, %%edx\n\t" save " (%[area])\n\t"                       \
                     "vpxor %%ymm0, %%ymm0, %%ymm0\n\tvpxor %%ymm1, %%ymm1, %%ymm1\n\t"                     \
                     "vpxor %%ymm2, %%ymm2, %%ymm2\n\tvpxor %%ymm3, %%ymm3, %%ymm3\n\t"                     \
                     "vpxor %%ymm4, %%ymm4, %%ymm4\n\tvpxor %%ymm5, %%ymm5, %%ymm5\n\t"                     \
                     "vpxor %%ymm6, %%ymm6, %%ymm6\n\tvpxor %%ymm7, %%ymm7, %%ymm7\n\t"                     \
                     "vpxor %%ymm8, %%ymm8, %%ymm8\n\tvpxor %%ymm9, %%ymm9, %%ymm9\n\t"                     \
                     "vpxor %%ymm10, %%ymm10, %%ymm10\n\tvpxor %%ymm11, %%ymm11, %%ymm11\n\t"               \
                     "vpxor %%ymm12, %%ymm12, %%ymm12\n\tvpxor %%ymm13, %%ymm13, %%ymm13\n\t"               \
                     "vpxor %%ymm14, %%ymm14, %%ymm14\n\tvpxor %%ymm15, %%ymm15, %%ymm15\n\t"               \
                     "ldmxcsr %[clobbered]\n\t"                                                             \
                     "movl $7, %%eax\n\txorl %%edx, %%edx\n\txrstor (%[area])\n\t"                          \
                     "stmxcsr %[restoredMxcsr]\n\t"                                                         \
                     "vmovdqu %%ymm0, 0(%[restored])\n\tvmovdqu %%ymm1, 32(%[restored])\n\t"                \
                     "vmovdqu %%ymm2, 64(%[restored])\n\tvmovdqu %%ymm3, 96(%[restored])\n\t"               \
                     "vmovdqu %%ymm4, 128(%[restored])\n\tvmovdqu %%ymm5, 160(%[restored])\n\t"             \
                     "vmovdqu %%ymm6, 192(%[restored])\n\tvmovdqu %%ymm7, 224(%[restored])\n\t"             \
                     "vmovdqu %%ymm8, 256(%[restored])\n\tvmovdqu %%ymm9, 288(%[restored])\n\t"             \
                     "vmovdqu %%ymm10, 320(%[restored])\n\tvmovdqu %%ymm11, 352(%[restored])\n\t"           \
                     "vmovdqu %%ymm12, 384(%[restored])\n\tvmovdqu %%ymm13, 416(%[restored])\n\t"           \
                     "vmovdqu %%ymm14, 448(%[restored])\n\tvmovdqu %%ymm15, 480(%[restored])\n\t"           \
                     "vzeroupper"                                                                           \
                     : [restoredMxcsr] "=m"(restoredMxcsr)                                                  \
                     : [pattern] "r"(pattern), [restored] "r"(restored), [area] "r"(area), [mxcsr] "m"(Mxcsr), \
                       [clobbered] "m"(Clobbered)                                                           \
                     : "eax", "edx", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
                       "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15")

static void report(const char* label) {
    print(label);
    print(": mxcsr ");
    print_hex(restoredMxcsr, 8);
    print("\n");
    for (int reg = 0; reg < 16; ++reg) {
        print("  ymm");
        print_hex((u64)reg, 1);
        print(" ");
        for (int byte = 31; byte >= 0; --byte) print_hex(restored[32 * reg + byte], 2);
        print("\n");
    }
}

int main(void) {
    for (int i = 0; i < (int)sizeof(pattern); ++i) pattern[i] = (u8)(i * 37 + 11);
    PS5LIFT_ROUND_TRIP("xsave");
    report("xsave/xrstor");
    PS5LIFT_ROUND_TRIP("xsavec");
    report("xsavec/xrstor");
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
