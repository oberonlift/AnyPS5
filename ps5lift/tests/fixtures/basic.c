#include "host.h"

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
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

static void print_unsigned(u64 value) {
    char buffer[32];
    int position = 31;
    buffer[position] = 0;
    do {
        buffer[--position] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(buffer + position);
}

static void print_signed(i64 value) {
    if (value < 0) {
        print("-");
        print_unsigned((u64)0 - (u64)value);
    } else {
        print_unsigned((u64)value);
    }
}

static void print_hex(u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; ++i) buffer[2 + i] = digits[(value >> (60 - 4 * i)) & 0xf];
    buffer[18] = 0;
    print(buffer);
}

static void line(const char* name, i64 value) {
    print(name);
    print(" = ");
    print_signed(value);
    print("\n");
}

static void line_hex(const char* name, u64 value) {
    print(name);
    print(" = ");
    print_hex(value);
    print("\n");
}

__attribute__((noinline)) static u64 fibonacci(u32 n) {
    return n < 2 ? n : fibonacci(n - 1) + fibonacci(n - 2);
}

__attribute__((noinline)) static i64 add(i64 a, i64 b) { return a + b; }
__attribute__((noinline)) static i64 subtract(i64 a, i64 b) { return a - b; }
__attribute__((noinline)) static i64 multiply(i64 a, i64 b) { return a * b; }
__attribute__((noinline)) static i64 divide(i64 a, i64 b) { return a / b; }

static i64 (*volatile operations[])(i64, i64) = {add, subtract, multiply, divide};

__attribute__((noinline)) static i64 many_arguments(i64 a, i64 b, i64 c, i64 d, i64 e, i64 f, i64 g, i64 h, i64 i) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
}

__attribute__((noinline)) static const char* classify(int value) {
    switch (value) {
    case 0: return "zero";
    case 1: return "one";
    case 2: return "two";
    case 3: return "three";
    case 4: return "four";
    case 5: return "five";
    case 6: return "six";
    case 7: return "seven";
    default: return "many";
    }
}

struct Record {
    i64 key;
    u32 values[6];
    u16 flags;
    u8 tag;
};

__attribute__((noinline)) static struct Record make_record(i64 key) {
    struct Record record;
    memset(&record, 0, sizeof(record));
    record.key = key;
    for (int i = 0; i < 6; ++i) record.values[i] = (u32)(key * (i + 1));
    record.flags = (u16)(key ^ 0xbeef);
    record.tag = (u8)key;
    return record;
}

static void insertion_sort(i32* values, int count) {
    for (int i = 1; i < count; ++i) {
        i32 value = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > value) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = value;
    }
}

static volatile u64 opaque_unsigned = 0xfedcba9876543210ul;
static volatile i64 opaque_signed = -1234567890123l;
static volatile u32 opaque_count = 13;
static volatile i32 opaque_small = -77;

int main(void) {
    print("ps5lift basic fixture\n");
    line("fibonacci(20)", (i64)fibonacci(20));

    for (int i = 0; i < 4; ++i) line("operation", operations[i](1000, 7));
    line("many_arguments", many_arguments(1, 2, 3, 4, 5, 6, 7, 8, 9));

    u64 u = opaque_unsigned;
    i64 s = opaque_signed;
    u32 c = opaque_count;
    line_hex("u >> c", u >> c);
    line_hex("u << c", u << c);
    line("s >> c", s >> c);
    line_hex("rotate", (u >> c) | (u << (64 - c)));
    line_hex("u / 1000003", u / 1000003);
    line_hex("u % 1000003", u % 1000003);
    line("s / -97", s / -97);
    line("s % -97", s % -97);
    line("small / 5", opaque_small / 5);
    line("small % 5", opaque_small % 5);
    line_hex("high product", (u64)(((unsigned __int128)u * u) >> 64));
    line("signed high", (i64)(((__int128)s * s) >> 64));
    line("ctz", __builtin_ctzll(u));
    line("clz", __builtin_clzll(u >> 9));
    line("popcount", __builtin_popcountll(u));
    line("byte add", (i64)(u8)((u8)u + (u8)0xf7));
    line("word sub", (i64)(i16)((u16)u - (u16)0x8000));
    line("compare", (s < -5) + 2 * (u > 5) + 4 * ((i32)c == 13) + 8 * (opaque_small >= 0));

    i64 total = 0;
    for (int i = 0; i < 9; ++i) {
        const char* name = classify(i);
        total += (i64)length_of(name) * (i + 1);
    }
    line("classify total", total);

    struct Record record = make_record(41);
    struct Record copy;
    memcpy(&copy, &record, sizeof(copy));
    u64 checksum = (u64)copy.key + copy.flags + copy.tag;
    for (int i = 0; i < 6; ++i) checksum = checksum * 31 + copy.values[i];
    line("record checksum", (i64)checksum);

    i32 values[] = {5, -3, 99, 0, 42, -100, 7, 7, 1};
    insertion_sort(values, 9);
    for (int i = 0; i < 9; ++i) line("sorted", values[i]);

    static u64 counter;
    for (int i = 0; i < 10; ++i) __atomic_fetch_add(&counter, (u64)i, __ATOMIC_SEQ_CST);
    u64 expected = 45;
    int exchanged = __atomic_compare_exchange_n(&counter, &expected, 1000, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    line("atomic counter", (i64)counter);
    line("atomic exchanged", exchanged);

    print("done\n");
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
