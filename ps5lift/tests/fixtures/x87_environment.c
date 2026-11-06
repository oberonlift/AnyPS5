#include "host.h"

typedef unsigned long u64;
typedef unsigned short u16;
typedef unsigned char u8;

static const u8 Denormal[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const u16 Control = 0x027f;
static const float Zero = 0.0f;
static _Alignas(16) u8 state[512];
static _Alignas(16) u8 image[128];
static _Alignas(16) u8 loaded[512];

static void print(const char* text) {
    u64 length = 0;
    while (text[length]) ++length;
    host_write(text, length);
}

static void print_hex(const u64 value, const int digits) {
    static const char Hex[] = "0123456789abcdef";
    char text[16];
    for (int i = 0; i < digits; ++i) text[i] = Hex[(value >> 4 * (digits - 1 - i)) & 0xf];
    host_write(text, (u64)digits);
}

static u64 word(const u8* bytes) { return (u64)bytes[0] | (u64)bytes[1] << 8; }

static void print_state(const char* label) {
    __asm__ volatile("fxsave %0" : "=m"(state));
    print(label);
    print(": fcw ");
    print_hex(word(state), 4);
    print(" fsw ");
    print_hex(word(state + 2), 4);
    print(" ftw ");
    print_hex(state[4], 2);
    for (int i = 0; i < 8; ++i) {
        print(" ");
        print_hex(word(state + 32 + 16 * i + 8), 4);
        print(":");
        for (int byte = 7; byte >= 0; --byte) print_hex(state[32 + 16 * i + byte], 2);
    }
    print("\n");
}

static void print_image_words(const char* label, const int stride) {
    print(label);
    print(": image fcw ");
    print_hex(word(image), 4);
    print(" fsw ");
    print_hex(word(image + stride), 4);
    print(" ftw ");
    print_hex(word(image + 2 * stride), 4);
    print("\n");
}

static void prepare(void) {
    __asm__ volatile("fninit\n\t"
                     "fldcw %[control]\n\t"
                     "fld1\n\t"
                     "fld1\n\t"
                     "fdivs %[zero]\n\t"
                     "fldt %[denormal]\n\t"
                     "fldpi\n\t"
                     "fldz\n\t"
                     :
                     : [control] "m"(Control), [denormal] "m"(Denormal), [zero] "m"(Zero)
                     : "memory");
}

static void environment_round_trip(const char* label, const int narrow) {
    prepare();
    print_state(label);
    if (narrow) {
        __asm__ volatile(".byte 0x66\n\tfnstenv (%0)\n\tfninit\n\t.byte 0x66\n\tfldenv (%0)" : : "r"(image) : "memory");
    } else {
        __asm__ volatile("fnstenv (%0)\n\tfninit\n\tfldenv (%0)" : : "r"(image) : "memory");
    }
    print_image_words(label, narrow ? 2 : 4);
    print_state(label);
}

static void state_round_trip(const char* label, const int narrow) {
    prepare();
    print_state(label);
    if (narrow) {
        __asm__ volatile(".byte 0x66\n\tfnsave (%0)" : : "r"(image) : "memory");
    } else {
        __asm__ volatile("fnsave (%0)" : : "r"(image) : "memory");
    }
    print_state(label);
    if (narrow) {
        __asm__ volatile(".byte 0x66\n\tfrstor (%0)" : : "r"(image) : "memory");
    } else {
        __asm__ volatile("frstor (%0)" : : "r"(image) : "memory");
    }
    print_image_words(label, narrow ? 2 : 4);
    for (int i = 0; i < 8; ++i) {
        const u8* reg = image + (narrow ? 14 : 28) + 10 * i;
        print(" ");
        print_hex(word(reg + 8), 4);
        print(":");
        for (int byte = 7; byte >= 0; --byte) print_hex(reg[byte], 2);
    }
    print("\n");
    print_state(label);
}

static void summary_load(const char* label, const u16 control, const u16 status) {
    __asm__ volatile("fninit\n\tfnstenv (%0)" : : "r"(image) : "memory");
    image[0] = (u8)control;
    image[1] = (u8)(control >> 8);
    image[4] = (u8)status;
    image[5] = (u8)(status >> 8);
    __asm__ volatile("fldenv (%0)" : : "r"(image) : "memory");
    print_state(label);
    __asm__ volatile("fninit\n\tfnsave (%0)" : : "r"(loaded) : "memory");
    loaded[0] = (u8)control;
    loaded[1] = (u8)(control >> 8);
    loaded[4] = (u8)status;
    loaded[5] = (u8)(status >> 8);
    __asm__ volatile("frstor (%0)" : : "r"(loaded) : "memory");
    print_state(label);
    __asm__ volatile("fninit\n\tfxsave %0" : "=m"(loaded) : : "memory");
    loaded[0] = (u8)control;
    loaded[1] = (u8)(control >> 8);
    loaded[2] = (u8)status;
    loaded[3] = (u8)(status >> 8);
    __asm__ volatile("fxrstor %0" : : "m"(loaded) : "memory");
    print_state(label);
    __asm__ volatile("fninit");
}

int main(void) {
    environment_round_trip("fnstenv/fldenv 28", 0);
    environment_round_trip("fnstenv/fldenv 14", 1);
    state_round_trip("fnsave/frstor 108", 0);
    state_round_trip("fnsave/frstor 94", 1);
    summary_load("summary without a flag", 0x037f, 0x8080);
    summary_load("summary under a mask", 0x037f, 0x8084);
    summary_load("flag unmasked without summary", 0x037b, 0x0004);
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
