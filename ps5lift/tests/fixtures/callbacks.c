#include "host.h"

typedef unsigned long u64;
typedef long i64;
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

static void print_signed(i64 value) {
    char buffer[32];
    int position = 31;
    int negative = value < 0;
    u64 magnitude = negative ? (u64)0 - (u64)value : (u64)value;
    buffer[position] = 0;
    do {
        buffer[--position] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (negative) buffer[--position] = '-';
    print(buffer + position);
}

static void line(const char* name, i64 value) {
    print(name);
    print(" = ");
    print_signed(value);
    print("\n");
}

struct Item {
    int key;
    int order;
};

static int compare_calls;

static int compare_items(const void* left, const void* right) {
    const struct Item* a = left;
    const struct Item* b = right;
    ++compare_calls;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->order - b->order;
}

static double scale(double value, long count) {
    return value * 1.5 + (double)count;
}

static __thread long thread_value = 100;

static long on_thread(long argument) {
    thread_value += argument;
    print("callback on a native thread\n");
    return thread_value;
}

static long eight(long a, long b, long c, long d, long e, long f, long g, long h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

int main(void) {
    struct Item items[12];
    for (int i = 0; i < 12; ++i) {
        items[i].key = (i * 7 + 3) % 5;
        items[i].order = i;
    }
    host_sort(items, 12, sizeof(items[0]), compare_items);
    i64 checksum = 0;
    for (int i = 0; i < 12; ++i) checksum = checksum * 13 + items[i].key * 100 + items[i].order;
    line("sorted checksum", checksum);
    line("comparator called", compare_calls > 0);

    double applied = host_apply(scale, 2.0, 3);
    line("apply * 1000", (i64)(applied * 1000.0));

    line("on thread", host_on_thread(on_thread, 23));
    line("main thread value", thread_value);
    line("eight arguments", host_call_eight(eight));
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
