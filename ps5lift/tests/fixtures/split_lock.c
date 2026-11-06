#include "host.h"

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

enum {
    Threads = 2,
    Steps = 1000,
    CounterOffset = 14
};

enum {
    Arithmetic = 0xd501,
    Logic = 0xc501,
    Carry = 0x0100,
    Zero = 0x4000,
    Unaffected = 0
};

static _Alignas(16) u8 counter[32];
static _Alignas(16) u8 operands[32];
static u32 started;

static void print(const char* text) {
    u64 length = 0;
    while (text[length]) ++length;
    host_write(text, length);
}

static void print_unsigned(u64 value) {
    char digits[32];
    int position = 31;
    digits[position] = 0;
    do {
        digits[--position] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(digits + position);
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

static void set_operand(int offset, u64 value, int bytes) {
    for (int i = 0; i < bytes; ++i) operands[offset + i] = (u8)(value >> (8 * i));
}

static u64 operand(int offset, int bytes) {
    u64 value = 0;
    for (int i = bytes - 1; i >= 0; --i) value = value << 8 | operands[offset + i];
    return value;
}

static void line(const char* name, int offset, int bytes, u64 reg, u64 flags) {
    print(name);
    print(": memory ");
    print_hex(operand(offset, bytes), 2 * bytes);
    print(", register ");
    print_hex(reg, 2 * bytes);
    print(", flags ");
    print_hex(flags, 4);
    print("\n");
}

#define FLAGS "\n\tlahf\n\tseto %%al"

#define UPDATE(name, mask, text, bytes, offset, initial, value)                                                                                         \
    do {                                                                                                                                                \
        set_operand(offset, initial, bytes);                                                                                                            \
        u64 reg = value;                                                                                                                                \
        u64 flags;                                                                                                                                      \
        __asm__ volatile(text FLAGS : "+r"(reg), "+m"(*(u8(*)[8])(operands + (offset))), "=&a"(flags) : : "cc", "memory");                              \
        line(name, offset, bytes, reg, flags & (mask));                                                                                                 \
    } while (0)

#define COMPARE_EXCHANGE(name, offset, initial, compare, exchange)                                                                                      \
    do {                                                                                                                                                \
        set_operand(offset, initial, 4);                                                                                                                \
        u64 rax = compare;                                                                                                                              \
        u64 old;                                                                                                                                        \
        __asm__ volatile("lock cmpxchgl %k[desired], %[memory]\n\tmov %%rax, %[old]" FLAGS                                                              \
                         : "+a"(rax), [old] "=&r"(old), [memory] "+m"(*(u8(*)[8])(operands + (offset)))                                                 \
                         : [desired] "r"((u64)(exchange))                                                                                               \
                         : "cc", "memory");                                                                                                             \
        line(name, offset, 4, old, rax & Arithmetic);                                                                                                   \
    } while (0)

#define COMPARE_EXCHANGE_8B(name, offset, initial, compare, exchange)                                                                                   \
    do {                                                                                                                                                \
        set_operand(offset, initial, 8);                                                                                                                \
        u64 rax = (u32)(compare);                                                                                                                       \
        u64 rdx = (u64)(compare) >> 32;                                                                                                                 \
        u64 old;                                                                                                                                        \
        __asm__ volatile("lock cmpxchg8b %[memory]\n\tmov %%eax, %k[old]" FLAGS                                                                         \
                         : "+a"(rax), "+d"(rdx), [old] "=&r"(old), [memory] "+m"(*(u8(*)[8])(operands + (offset)))                                      \
                         : "b"((u64)(u32)(exchange)), "c"((u64)(exchange) >> 32)                                                                        \
                         : "cc", "memory");                                                                                                             \
        line(name, offset, 8, rdx << 32 | old, rax & Zero);                                                                                             \
    } while (0)

[[gnu::noinline]] static void split_forms(void) {
    UPDATE("lock add", Arithmetic, "lock addw %w0, %1", 2, 15, 0x7ff0, 0x0011);
    UPDATE("lock adc", Arithmetic, "stc\n\tlock adcl %k0, %1", 4, 14, 0xfffffff0, 0x1f);
    UPDATE("lock sub", Arithmetic, "lock subq %q0, %1", 8, 12, 0x100000000, 1);
    UPDATE("lock sbb", Arithmetic, "stc\n\tlock sbbl %k0, %1", 4, 14, 5, 7);
    UPDATE("lock and", Logic, "lock andl %k0, %1", 4, 14, 0xff00ff00, 0x0ff00ff0);
    UPDATE("lock or", Logic, "lock orl %k0, %1", 4, 14, 0x80000001, 0x00f000f0);
    UPDATE("lock xor", Logic, "lock xorl %k0, %1", 4, 14, 0xaaaaaaaa, 0x5555aaaa);
    UPDATE("lock inc", Arithmetic, "stc\n\tlock incl %1", 4, 14, 0x7fffffff, 0);
    UPDATE("lock dec", Arithmetic, "clc\n\tlock decl %1", 4, 14, 0x100, 0);
    UPDATE("lock not", Unaffected, "lock notl %1", 4, 14, 0x0f0f1234, 0);
    UPDATE("lock neg", Arithmetic, "lock negw %1", 2, 15, 0x0010, 0);
    UPDATE("lock bts", Carry, "lock btsl $17, %1", 4, 14, 0, 0);
    UPDATE("lock btr", Carry, "lock btrl %k0, %1", 4, 14, 0xffffffff, 18);
    UPDATE("lock btc", Carry, "lock btcq %q0, %1", 8, 12, 0x0123456789abcdef, 36);
    UPDATE("lock xadd", Arithmetic, "lock xaddq %q0, %1", 8, 12, 0x7ffffffffffffff0, 0x20);
    UPDATE("xchg", Unaffected, "xchgl %k0, %1", 4, 14, 0x13579bdf, 0x2468ace0);
    COMPARE_EXCHANGE("lock cmpxchg equal", 14, 0x11111111, 0x11111111, 0x22222222);
    COMPARE_EXCHANGE("lock cmpxchg unequal", 14, 0x33333333, 0x44444444, 0x55555555);
    COMPARE_EXCHANGE_8B("lock cmpxchg8b", 12, 0x1122334455667788, 0x1122334455667788, 0x99aabbccddeeff00);
}

static u32 exchange_add(void* target, u32 value) {
    __asm__ volatile("lock xaddl %0, %1" : "+r"(value), "+m"(*(u8(*)[4])target) : : "memory", "cc");
    return value;
}

[[gnu::noinline]] static long worker(long argument) {
    (void)argument;
    exchange_add(&started, 1);
    while (exchange_add(&started, 0) < Threads) __builtin_ia32_pause();
    for (int i = 0; i < Steps; ++i) exchange_add(counter + CounterOffset, 1);
    return 0;
}

int main(void) {
    split_forms();
    unsigned long threads[Threads];
    for (int i = 0; i < Threads; ++i) threads[i] = host_thread_create(worker, 0);
    for (int i = 0; i < Threads; ++i) host_thread_join(threads[i]);
    u32 count = 0;
    for (int i = 3; i >= 0; --i) count = count << 8 | counter[CounterOffset + i];
    print("threads: ");
    print_unsigned(count);
    print("\n");
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
