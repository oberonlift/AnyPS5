#include "stop.h"

int main(void) {
    host_write("before stop\n", 12);
    __asm__ volatile("pushq $0x40202\n\t"
                     "popfq");
    return 0;
}
