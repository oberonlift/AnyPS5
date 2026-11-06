#include "host.h"

typedef unsigned long u64;
typedef unsigned short u16;
typedef unsigned char u8;

static const u8 SmallestDenormal[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const u8 LargestPowerOfTwo[10] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xfe, 0x7f};
static const u8 Largest[10] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0x7f};
static const u16 UnderflowUnmasked = 0x036f;
static const u16 OverflowUnmasked = 0x0377;
static _Alignas(16) u8 state[512];

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

static void report(const char* label) {
    __asm__ volatile("fxsave %0\n\tfninit" : "=m"(state));
    print(label);
    print(": fsw ");
    print_hex((u64)state[2] | (u64)state[3] << 8, 4);
    print(" st0 ");
    print_hex((u64)state[40] | (u64)state[41] << 8, 4);
    print(":");
    for (int byte = 7; byte >= 0; --byte) print_hex(state[32 + byte], 2);
    print("\n");
}

int main(void) {
    __asm__ volatile("fninit\n\tfldcw %[control]\n\tfldt %[x]\n\tf2xm1"
                     :
                     : [control] "m"(UnderflowUnmasked), [x] "m"(SmallestDenormal));
    report("f2xm1 of the smallest denormal, underflow unmasked");
    __asm__ volatile("fninit\n\tfldcw %[control]\n\tfld1\n\tfldt %[x]\n\tfpatan"
                     :
                     : [control] "m"(UnderflowUnmasked), [x] "m"(LargestPowerOfTwo));
    report("fpatan of 1 over 2^16383, underflow unmasked");
    __asm__ volatile("fninit\n\tfldcw %[control]\n\tfldt %[y]\n\tfld1\n\tfadd %%st(0), %%st\n\tfadd %%st(0), %%st\n\tfyl2x"
                     :
                     : [control] "m"(OverflowUnmasked), [y] "m"(Largest));
    report("fyl2x of the largest value and 4, overflow unmasked");
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
