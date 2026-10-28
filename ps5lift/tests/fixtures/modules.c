#include "host.h"

typedef long i64;

extern long module_counter;
long module_value(long x);
long (*module_function(void))(long);

static unsigned long length_of(const char* text) {
    unsigned long length = 0;
    while (text[length]) ++length;
    return length;
}

static void print(const char* text) {
    host_write(text, length_of(text));
}

static void print_signed(i64 value) {
    char buffer[24];
    int position = 23;
    buffer[position] = 0;
    const int negative = value < 0;
    unsigned long magnitude = negative ? 0 - (unsigned long)value : (unsigned long)value;
    do {
        buffer[--position] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (negative) buffer[--position] = '-';
    print(buffer + position);
}

static void line(const char* name, i64 value) {
    print(name);
    print(" = ");
    print_signed(value);
    print("\n");
}

int main(void) {
    line("module counter", module_counter);
    line("module value(5)", module_value(5));
    line("through a module pointer", module_function()(3));
    module_counter = 100;
    line("after writing its data", module_value(1));
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
