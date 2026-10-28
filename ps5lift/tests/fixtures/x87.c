#include "host.h"

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned short u16;
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

static void print_hex(u64 value, int digits) {
    static const char hex[] = "0123456789abcdef";
    char buffer[17];
    for (int i = 0; i < digits; ++i) buffer[i] = hex[(value >> (4 * (digits - 1 - i))) & 0xf];
    buffer[digits] = 0;
    print(buffer);
}

static u64 hash;

static void mix(const void* data, int size) {
    const u8* bytes = data;
    for (int i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ul;
}

static void mix_ld(long double value) {
    mix(&value, 10);
}

static void report(const char* name) {
    print(name);
    print(" ");
    print_hex(hash, 16);
    print("\n");
    hash = 0xcbf29ce484222325ul;
}

static const u64 bits[][2] = {
    {0x8000000000000000ul, 0x3fff}, {0x8000000000000000ul, 0xbfff}, {0xc90fdaa22168c235ul, 0x4000}, {0, 0}, {0, 0x8000},
    {0x8000000000000000ul, 0x7fff}, {0x8000000000000000ul, 0xffff}, {0xc000000000000000ul, 0x7fff}, {0xa000000000000001ul, 0x7fff},
    {0x8000000000000000ul, 0x0001}, {0x0000000000000001ul, 0x0000}, {0xfffffffffffffffful, 0x7ffe}, {0xaaaaaaaaaaaaaaabul, 0x3ffd},
    {0x9abcdef012345678ul, 0x403e}, {0xd000000000000000ul, 0x4004}, {0x8000000000000001ul, 0x3fff},
};

enum { Count = sizeof(bits) / sizeof(bits[0]) };

static volatile long double values[Count];

static long double value(int index) {
    return values[index % Count];
}

static void set_control(u16 control) {
    __asm__ volatile("fnclex\n\tfldcw %0" : : "m"(control));
}

static long double square_root(long double x) {
    __asm__("fsqrt" : "+t"(x));
    return x;
}

static long double round_integer(long double x) {
    __asm__("frndint" : "+t"(x));
    return x;
}

static long double scale(long double x, long double y) {
    long double result;
    __asm__("fscale" : "=t"(result) : "0"(x), "u"(y));
    return result;
}

static long double remainder_partial(long double x, long double y) {
    long double result;
    u16 status;
    __asm__("1:\n\tfprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b" : "=t"(result), "=a"(status) : "0"(x), "u"(y));
    mix(&status, 2);
    return result;
}

static long double remainder_ieee(long double x, long double y) {
    long double result;
    u16 status;
    __asm__("1:\n\tfprem1\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b" : "=t"(result), "=a"(status) : "0"(x), "u"(y));
    mix(&status, 2);
    return result;
}

static u16 examine(long double x) {
    u16 status;
    __asm__("fxam\n\tfnstsw %0" : "=a"(status) : "t"(x));
    return (u16)(status & 0x4700);
}

static long double constants(int which) {
    long double result;
    switch (which) {
    case 0: __asm__("fldpi" : "=t"(result)); break;
    case 1: __asm__("fldl2e" : "=t"(result)); break;
    case 2: __asm__("fldl2t" : "=t"(result)); break;
    case 3: __asm__("fldlg2" : "=t"(result)); break;
    case 4: __asm__("fldln2" : "=t"(result)); break;
    case 5: __asm__("fld1" : "=t"(result)); break;
    default: __asm__("fldz" : "=t"(result)); break;
    }
    return result;
}

static u16 status_word(void) {
    u16 status;
    __asm__ volatile("fnstsw %0" : "=a"(status));
    return (u16)(status & 0x3f);
}

static void run_tests(void) {
    for (int i = 0; i < Count; ++i) {
        for (int j = 0; j < Count; ++j) {
            long double a = value(i), b = value(j);
            mix_ld(a + b);
            mix_ld(a - b);
            mix_ld(a * b);
            mix_ld(a / b);
            int relation = (a < b) + 2 * (a == b) + 4 * (a > b) + 8 * (a != a || b != b);
            mix(&relation, 4);
        }
    }
    report("arithmetic");
    for (int i = 0; i < Count; ++i) {
        long double a = value(i);
        mix_ld(square_root(a));
        mix_ld(round_integer(a));
        mix_ld(-a);
        mix_ld(a < 0 ? -a : a);
        u16 kind = examine(a);
        mix(&kind, 2);
    }
    report("unary");
    for (int i = 0; i < Count; ++i) {
        long double a = value(i);
        float f = (float)a;
        double d = (double)a;
        long long l = (long long)a;
        int n = (int)a;
        short s = (short)a;
        mix(&f, 4);
        mix(&d, 8);
        mix(&l, 8);
        mix(&n, 4);
        mix(&s, 2);
        long double back = (long double)f + (long double)d + (long double)l + (long double)n;
        mix_ld(back);
    }
    report("conversions");
    for (int i = 0; i < Count; ++i) {
        for (int j = 2; j < Count; j += 5) {
            mix_ld(scale(value(i), value(j) - 3.0L));
            mix_ld(remainder_partial(value(i) * 1e30L, value(j)));
            mix_ld(remainder_ieee(value(i), value(j) + 0.75L));
        }
    }
    report("scale-remainder");
    for (int which = 0; which < 7; ++which) mix_ld(constants(which));
    report("constants");
    for (int i = 0; i < Count; ++i) {
        __asm__ volatile("fnclex");
        volatile long double sink = value(i) / value((i + 3) % Count) + value((i + 7) % Count) * 1e4000L;
        (void)sink;
        u16 flags = status_word();
        mix(&flags, 2);
    }
    report("flags");
}

int main(void) {
    for (int i = 0; i < Count; ++i) {
        long double v;
        memcpy(&v, bits[i], 10);
        values[i] = v;
    }
    static const u16 controls[] = {0x037f, 0x077f, 0x0b7f, 0x0f7f, 0x027f, 0x007f};
    static const char* const names[] = {"nearest", "down", "up", "zero", "double-precision", "single-precision"};
    for (unsigned c = 0; c < sizeof(controls) / sizeof(controls[0]); ++c) {
        hash = 0xcbf29ce484222325ul;
        set_control(controls[c]);
        print("== ");
        print(names[c]);
        print("\n");
        run_tests();
    }
    set_control(0x037f);
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
