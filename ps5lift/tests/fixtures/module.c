#include "host.h"

long module_scale;
long module_counter = 40;
static __thread long module_local = 5;

static unsigned long length_of(const char* text) {
    unsigned long length = 0;
    while (text[length]) ++length;
    return length;
}

__attribute__((constructor)) static void initialize(void) {
    static const char message[] = "module initialized\n";
    module_scale = 7;
    module_counter += 2;
    host_write(message, length_of(message));
}

long module_value(long x) {
    return x * module_scale + module_counter;
}

long module_tls(void) {
    return ++module_local;
}

long (*module_function(void))(long) {
    return module_value;
}

long* module_local_address(void) {
    return &module_local;
}

static long twice(long x) {
    return 2 * x;
}

static long squared(long x) {
    return x * x;
}

static long (*const module_table[])(long) = {twice, squared, module_value};

long module_table_call(int index, long x) {
    return module_table[index](x);
}

__asm__(".globl module_absolute\n.set module_absolute, 0x12345");
__asm__(".pushsection .text\n"
        ".globl module_text_data\n"
        ".type module_text_data, @object\n"
        ".size module_text_data, 4\n"
        "module_text_data: .byte 0x61, 0x62, 0x63, 0x64\n"
        ".popsection");
extern char module_absolute[];

long module_absolute_address(void) {
    return (long)module_absolute;
}
