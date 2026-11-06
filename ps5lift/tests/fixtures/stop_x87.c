#include "stop.h"

static const unsigned char QuietNan[10] = {0, 0, 0, 0, 0, 0, 0, 0xc0, 0xff, 0x7f};
static const unsigned short InvalidUnmasked = 0x037e;
static unsigned short comparedStatus;
static unsigned short flagsStatus;
static unsigned char flags;
static unsigned char overflow;

static void print(const char* text) {
    unsigned long length = 0;
    while (text[length]) ++length;
    host_write(text, length);
}

static void print_hex(const char* name, const unsigned value, const int digits) {
    static const char hex[] = "0123456789abcdef";
    char text[5];
    for (int i = 0; i < digits; ++i) text[i] = hex[(value >> 4 * (digits - 1 - i)) & 0xf];
    text[digits] = 0;
    print(name);
    print(text);
}

int main(void) {
    __asm__ volatile("fninit\n\t"
                     "fldcw %[control]\n\t"
                     "fldz\n\t"
                     "fldt %[nan]\n\t"
                     "fcomp %%st(1)\n\t"
                     "fnstsw %[compared]\n\t"
                     "fninit\n\t"
                     "fldcw %[control]\n\t"
                     "fldz\n\t"
                     "fldt %[nan]\n\t"
                     "movb $0x7f, %%al\n\t"
                     "addb $1, %%al\n\t"
                     "stc\n\t"
                     "fcomip %%st(1), %%st\n\t"
                     "lahf\n\t"
                     "seto %%al\n\t"
                     "movb %%ah, %[flags]\n\t"
                     "movb %%al, %[overflow]\n\t"
                     "fnstsw %[status]\n\t"
                     "fninit"
                     : [compared] "=m"(comparedStatus), [status] "=m"(flagsStatus), [flags] "=m"(flags), [overflow] "=m"(overflow)
                     : [control] "m"(InvalidUnmasked), [nan] "m"(QuietNan)
                     : "rax", "cc", "memory");
    print_hex("fcomp of a quiet NaN: fsw 0x", comparedStatus, 4);
    print_hex("\nfcomip of a quiet NaN: fsw 0x", flagsStatus, 4);
    print_hex(", SF ZF AF PF CF 0x", flags, 2);
    print_hex(", OF ", overflow, 1);
    print("\n");

    host_write("before fsqrt\n", 13);
    __asm__ volatile("fninit\n\t"
                     "fldcw %0\n\t"
                     "fld1\n\t"
                     "fchs\n\t"
                     "fsqrt"
                     :
                     : "m"(InvalidUnmasked)
                     : "memory");
    host_write("after fsqrt\n", 12);
    __asm__ volatile("fld1" : : : "memory");
    return 0;
}
