#include "stop.h"

int main(void) {
    host_write("before stop\n", 12);
    host_clear_fs_base();
    __asm__ volatile("syscall");
    return 0;
}
