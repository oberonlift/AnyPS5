#include "stop.h"

int main(void) {
    host_write("before stop\n", 12);
    __asm__ volatile("vaddps %zmm1, %zmm2, %zmm3");
    return 0;
}
