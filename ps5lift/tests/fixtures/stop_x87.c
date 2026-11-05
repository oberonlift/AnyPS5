#include "stop.h"

int main(void) {
    const unsigned short control = 0x037e;
    host_write("before fsqrt\n", 13);
    __asm__ volatile("fninit\n\t"
                     "fldcw %0\n\t"
                     "fld1\n\t"
                     "fchs\n\t"
                     "fsqrt"
                     :
                     : "m"(control)
                     : "memory");
    host_write("after fsqrt\n", 12);
    __asm__ volatile("fld1" : : : "memory");
    return 0;
}
