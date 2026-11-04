#include "stop.h"

int main(void) {
    host_write("before stop\n", 12);
    void (*volatile unmapped)(void) = (void (*)(void))0x1000;
    unmapped();
    return 0;
}
