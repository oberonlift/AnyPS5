#ifndef PS5LIFT_FIXTURE_STOP_H
#define PS5LIFT_FIXTURE_STOP_H

#include "host.h"

__asm__(".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call main\n"
        "    mov %eax, %edi\n"
        "    call host_exit\n"
        "    ud2\n");

#endif
