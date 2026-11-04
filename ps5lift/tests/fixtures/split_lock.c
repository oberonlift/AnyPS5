#include "host.h"

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

enum {
    Threads = 2,
    Steps = 1000,
    CounterOffset = 14
};

static _Alignas(16) u8 buffer[32];
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

static u32 exchange_add(void* target, u32 value) {
    __asm__ volatile("lock xaddl %0, %1" : "+r"(value), "+m"(*(u8(*)[4])target) : : "memory", "cc");
    return value;
}

[[gnu::noinline]] static long worker(long argument) {
    (void)argument;
    exchange_add(&started, 1);
    while (exchange_add(&started, 0) < Threads) __builtin_ia32_pause();
    for (int i = 0; i < Steps; ++i) exchange_add(buffer + CounterOffset, 1);
    return 0;
}

int main(void) {
    unsigned long threads[Threads];
    for (int i = 0; i < Threads; ++i) threads[i] = host_thread_create(worker, 0);
    for (int i = 0; i < Threads; ++i) host_thread_join(threads[i]);
    u32 count = 0;
    for (int i = 3; i >= 0; --i) count = count << 8 | buffer[CounterOffset + i];
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
