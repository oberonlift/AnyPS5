#include "stop.h"

typedef unsigned char u8;

static const _Alignas(16) u8 source[16] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0};
static const _Alignas(16) u8 block[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

static void print(const char* text) {
    unsigned long length = 0;
    while (text[length]) ++length;
    host_write(text, length);
}

static void print_bytes(const char* name, const u8* bytes) {
    static const char digits[] = "0123456789abcdef";
    print(name);
    for (int i = 0; i < 16; ++i) {
        const char text[] = {' ', digits[bytes[i] >> 4], digits[bytes[i] & 0xf], 0};
        print(text);
    }
    print("\n");
}

int main(void) {
    const unsigned mxcsr = 0x9fc0 | 0x20000;
    const u8 (*misaligned)[16] = (const u8 (*)[16])(block + 1);
    u8 result[16];
    __asm__ volatile("ldmxcsr %[mxcsr]\n\t"
                     "movdqu %[source], %%xmm0\n\t"
                     "pxor %[misaligned], %%xmm0\n\t"
                     "movdqu %%xmm0, %[result]"
                     : [result] "=m"(result)
                     : [mxcsr] "m"(mxcsr), [source] "m"(source), [misaligned] "m"(*misaligned)
                     : "xmm0");
    print_bytes("pxor", result);
    __asm__ volatile("movdqa %[misaligned], %%xmm0" : : [misaligned] "m"(*misaligned) : "xmm0");
    print("movdqa completed\n");
    return 0;
}
