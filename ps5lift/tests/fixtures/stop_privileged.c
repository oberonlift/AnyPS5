#include "stop.h"

int main(void) {
    host_write("before stop\n", 12);
    __asm__ volatile("hlt");
    return 0;
}
