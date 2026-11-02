#include "unconverted_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
int guest_fflush(void* stream) __asm__(NID_fflush);
[[noreturn]] void guest_exit(int code) __asm__(NID_exit);
int guest_pthread_mutexattr_init(void** attr) __asm__(NID_pthread_mutexattr_init);

static const unsigned long GuestBase = 0x400000000000ul;

int main(void) {
    void* attr = 0;
    guest_pthread_mutexattr_init(&attr);
    const volatile char* native = (const volatile char*)attr + GuestBase;
    guest_printf("reading a native object through its unconverted host address\n");
    guest_fflush(0);
    guest_printf("read %d without a fault\n", *native);
    return 0;
}

[[noreturn]] void start_main(void) {
    guest_exit(main());
}

__asm__(".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call start_main\n"
        "    ud2\n");
