#include "host.h"

long module_scale;
long module_counter = 40;

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

long (*module_function(void))(long) {
    return module_value;
}
