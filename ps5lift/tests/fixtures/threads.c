#include "host.h"

typedef unsigned long u64;
typedef long i64;
typedef unsigned char u8;

#ifdef PS5LIFT_EMULATED
#define PS5LIFT_STEPS 2000
#else
#define PS5LIFT_STEPS 200000
#endif

enum {
    Threads = 4,
    CounterSteps = PS5LIFT_STEPS,
    LockSteps = PS5LIFT_STEPS / 2,
    Messages = PS5LIFT_STEPS * 10
};

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

static void line(const char* name, u64 value) {
    print(name);
    print(" = ");
    print_unsigned(value);
    print("\n");
}

static __thread i64 tls_value = 5;
static __thread i64 tls_zero;

__attribute__((noinline)) static i64 tls_worker(i64 argument) {
    for (int i = 0; i < 1000; ++i) {
        tls_value += argument;
        tls_zero ^= i;
    }
    return tls_value * 1000 + tls_zero;
}

static u64 counter;

__attribute__((noinline)) static i64 counter_worker(i64 argument) {
    for (int i = 0; i < CounterSteps; ++i) __atomic_fetch_add(&counter, (u64)argument, __ATOMIC_SEQ_CST);
    return 0;
}

static volatile int lock;
static volatile u64 locked_counter;

__attribute__((noinline)) static i64 lock_worker(i64 argument) {
    (void)argument;
    for (int i = 0; i < LockSteps; ++i) {
        while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {
            while (lock) __builtin_ia32_pause();
        }
        locked_counter = locked_counter + 1;
        __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    }
    return 0;
}

static volatile u64 message_data;
static volatile u64 message_flag;

__attribute__((noinline)) static i64 message_writer(i64 argument) {
    (void)argument;
    for (u64 i = 1; i <= Messages; ++i) {
        message_data = i;
        message_flag = i;
    }
    return 0;
}

__attribute__((noinline)) static i64 message_reader(i64 argument) {
    (void)argument;
    i64 violations = 0;
    u64 flag = 0;
    while (flag < Messages) {
        flag = message_flag;
        u64 data = message_data;
        if (data < flag) ++violations;
    }
    return violations;
}

int main(void) {
    unsigned long threads[Threads];
    for (int i = 0; i < Threads; ++i) threads[i] = host_thread_create(tls_worker, i + 1);
    u64 tls_total = 0;
    for (int i = 0; i < Threads; ++i) tls_total += (u64)host_thread_join(threads[i]);
    line("tls total", tls_total);
    line("main tls", (u64)tls_value * 1000 + (u64)tls_zero);

    for (int i = 0; i < Threads; ++i) threads[i] = host_thread_create(counter_worker, i + 1);
    for (int i = 0; i < Threads; ++i) host_thread_join(threads[i]);
    line("atomic counter / steps", counter / CounterSteps);

    for (int i = 0; i < Threads; ++i) threads[i] = host_thread_create(lock_worker, 0);
    for (int i = 0; i < Threads; ++i) host_thread_join(threads[i]);
    line("locked counter / steps", locked_counter / LockSteps);
    line("locked counter remainder", locked_counter % LockSteps);

    unsigned long reader = host_thread_create(message_reader, 0);
    unsigned long writer = host_thread_create(message_writer, 0);
    host_thread_join(writer);
    line("message ordering violations", (u64)host_thread_join(reader));
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
