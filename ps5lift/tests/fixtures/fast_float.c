#include "host.h"
#include "fast_float_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
int guest_fflush(void* stream) __asm__(NID_fflush);
int guest_feenableexcept(int exceptions) __asm__(NID_feenableexcept);

int main(void) {
    guest_printf("before feenableexcept\n");
    guest_fflush(0);
    guest_feenableexcept(0x04);
    guest_printf("after feenableexcept\n");
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
