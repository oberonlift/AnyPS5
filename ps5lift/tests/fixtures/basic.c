#include "host.h"

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
typedef unsigned char u8;

void* memcpy(void* destination, const void* source, u64 length) {
    u8* to = destination;
    const u8* from = source;
    while (length--) *to++ = *from++;
    return destination;
}

void* memset(void* destination, int value, u64 length) {
    u8* to = destination;
    while (length--) *to++ = (u8)value;
    return destination;
}

static u64 length_of(const char* text) {
    u64 length = 0;
    while (text[length]) ++length;
    return length;
}

static void print(const char* text) {
    host_write(text, length_of(text));
}

static void print_unsigned(u64 value) {
    char buffer[32];
    int position = 31;
    buffer[position] = 0;
    do {
        buffer[--position] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    print(buffer + position);
}

static void print_signed(i64 value) {
    if (value < 0) {
        print("-");
        print_unsigned((u64)0 - (u64)value);
    } else {
        print_unsigned((u64)value);
    }
}

static void print_hex(u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; ++i) buffer[2 + i] = digits[(value >> (60 - 4 * i)) & 0xf];
    buffer[18] = 0;
    print(buffer);
}

static void line(const char* name, i64 value) {
    print(name);
    print(" = ");
    print_signed(value);
    print("\n");
}

static void line_hex(const char* name, u64 value) {
    print(name);
    print(" = ");
    print_hex(value);
    print("\n");
}

__attribute__((noinline)) static u64 fibonacci(u32 n) {
    return n < 2 ? n : fibonacci(n - 1) + fibonacci(n - 2);
}

__attribute__((noinline)) static i64 add(i64 a, i64 b) { return a + b; }
__attribute__((noinline)) static i64 subtract(i64 a, i64 b) { return a - b; }
__attribute__((noinline)) static i64 multiply(i64 a, i64 b) { return a * b; }
__attribute__((noinline)) static i64 divide(i64 a, i64 b) { return a / b; }

static i64 (*volatile operations[])(i64, i64) = {add, subtract, multiply, divide};

__attribute__((noinline)) static i64 many_arguments(i64 a, i64 b, i64 c, i64 d, i64 e, i64 f, i64 g, i64 h, i64 i) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
}

__attribute__((noinline)) static const char* classify(int value) {
    switch (value) {
    case 0: return "zero";
    case 1: return "one";
    case 2: return "two";
    case 3: return "three";
    case 4: return "four";
    case 5: return "five";
    case 6: return "six";
    case 7: return "seven";
    default: return "many";
    }
}

i64 table_index_copied_after_compare(i64 value);
i64 table_index_copied_before_compare(i64 value);
i64 table_base_before_loop(i64 value);
void store_address_taken_by_lea(i64 (**slot)(void));
i64 pass_address_taken_by_lea(void);

__attribute__((noinline)) i64 call_function_pointer(i64 (*function)(void)) {
    return function();
}

__asm__(".text\n"
        ".globl table_index_copied_after_compare\n"
        ".type table_index_copied_after_compare, @function\n"
        "table_index_copied_after_compare:\n"
        "    cmp $2, %edi\n"
        "    ja 3f\n"
        "    mov %edi, %eax\n"
        "    lea 4f(%rip), %rcx\n"
        "    movslq (%rcx,%rax,4), %rax\n"
        "    add %rcx, %rax\n"
        "    jmp *%rax\n"
        "0:  mov $100, %eax\n"
        "    ret\n"
        "1:  mov $101, %eax\n"
        "    ret\n"
        "2:  mov $102, %eax\n"
        "    ret\n"
        "3:  mov $-1, %rax\n"
        "    ret\n"
        "5:  .byte 0xff, 0xff\n"
        ".size table_index_copied_after_compare, .-table_index_copied_after_compare\n"
        ".pushsection .rodata\n"
        ".p2align 2\n"
        "4:  .long 0b-4b, 1b-4b, 2b-4b, 5b-4b\n"
        ".popsection\n"
        ".globl table_index_copied_before_compare\n"
        ".type table_index_copied_before_compare, @function\n"
        "table_index_copied_before_compare:\n"
        "    mov %edi, %edx\n"
        "    cmp $2, %edi\n"
        "    ja 3f\n"
        "    mov %edx, %eax\n"
        "    lea 4f(%rip), %rcx\n"
        "    movslq (%rcx,%rax,4), %rax\n"
        "    add %rcx, %rax\n"
        "    jmp *%rax\n"
        "0:  mov $200, %eax\n"
        "    ret\n"
        "1:  mov $201, %eax\n"
        "    ret\n"
        "2:  mov $202, %eax\n"
        "    ret\n"
        "3:  mov $-1, %rax\n"
        "    ret\n"
        "5:  .byte 0xff, 0xff\n"
        ".size table_index_copied_before_compare, .-table_index_copied_before_compare\n"
        ".pushsection .rodata\n"
        ".p2align 2\n"
        "4:  .long 0b-4b, 1b-4b, 2b-4b, 5b-4b\n"
        ".popsection\n"
        ".globl table_base_before_loop\n"
        ".type table_base_before_loop, @function\n"
        "table_base_before_loop:\n"
        "    lea 9f(%rip), %r8\n"
        "    xor %eax, %eax\n"
        "    jmp 2f\n"
        "1:  mov %rcx, %rdi\n"
        "2:  cmp $3, %edi\n"
        "    ja 8f\n"
        "    mov %edi, %ecx\n"
        "    movslq (%r8,%rcx,4), %rcx\n"
        "    mov %rdi, %rdx\n"
        "    add %r8, %rcx\n"
        "    xor %edx, %edx\n"
        "    jmp *%rcx\n"
        "3:  ret\n"
        "4:  add $10, %rax\n"
        "    mov %r8, %r9\n"
        "    mov $3, %r8d\n"
        "    add %r8, %rax\n"
        "    mov %r9, %r8\n"
        "    mov $0, %ecx\n"
        "    jmp 1b\n"
        "5:  add $100, %rax\n"
        "    mov $7, %r8d\n"
        "    add %r8, %rax\n"
        "    lea 9f(%rip), %r8\n"
        "    mov $1, %ecx\n"
        "    jmp 1b\n"
        "6:  add $1000, %rax\n"
        "    mov $2, %ecx\n"
        "    jmp 1b\n"
        "8:  mov $-1, %rax\n"
        "    ret\n"
        ".size table_base_before_loop, .-table_base_before_loop\n"
        ".pushsection .rodata\n"
        ".p2align 2\n"
        "9:  .long 3b-9b, 4b-9b, 5b-9b, 6b-9b\n"
        ".popsection\n"
        ".text\n");

/* Functions with no symbol type and no unwind entry, whose addresses only a RIP-relative lea takes: one stored to
   memory and one passed to a call. */
__asm__(".text\n"
        "stored_through_lea:\n"
        "    mov $77, %eax\n"
        "    ret\n"
        "passed_through_lea:\n"
        "    mov $78, %eax\n"
        "    ret\n"
        ".globl store_address_taken_by_lea\n"
        ".type store_address_taken_by_lea, @function\n"
        "store_address_taken_by_lea:\n"
        "    lea stored_through_lea(%rip), %rax\n"
        "    mov %rax, (%rdi)\n"
        "    ret\n"
        ".size store_address_taken_by_lea, .-store_address_taken_by_lea\n"
        ".globl pass_address_taken_by_lea\n"
        ".type pass_address_taken_by_lea, @function\n"
        "pass_address_taken_by_lea:\n"
        "    sub $8, %rsp\n"
        "    lea passed_through_lea(%rip), %rdi\n"
        "    call call_function_pointer\n"
        "    add $8, %rsp\n"
        "    ret\n"
        ".size pass_address_taken_by_lea, .-pass_address_taken_by_lea\n");

static i64 flags_of(u8 carry, u8 overflow, u8 zero, u8 sign) {
    return carry | overflow << 1 | zero << 2 | sign << 3;
}

__attribute__((noinline)) static void zen2_integer(void) {
    u64 value = 0x0123456789abcdefUL;
    u64 swapped64 = value;
    __asm__("bswap %0" : "+r"(swapped64));
    line_hex("bswap 64", swapped64);
    u32 swapped32 = 0x01234567;
    __asm__("bswap %0" : "+r"(swapped32));
    line_hex("bswap 32", swapped32);

    u64 loaded64;
    __asm__("movbe %1, %0" : "=r"(loaded64) : "m"(value));
    line_hex("movbe load 64", loaded64);
    u16 half = 0x1234;
    u16 loaded16;
    __asm__("movbe %1, %0" : "=r"(loaded16) : "m"(half));
    line_hex("movbe load 16", loaded16);
    u32 stored32;
    __asm__("movbe %1, %0" : "=m"(stored32) : "r"((u32)0x01234567));
    line_hex("movbe store 32", stored32);

    u8 carry, overflow, zero, sign;
    u64 left64 = 0x0123456789abcdefUL;
    __asm__("shld %%cl, %[source], %[target]\n\tsetc %[carry]\n\tsetz %[zero]\n\tsets %[sign]"
            : [target] "+r"(left64), [carry] "=r"(carry), [zero] "=r"(zero), [sign] "=r"(sign) : [source] "r"(0xfedcba9876543210UL), "c"(4) : "cc");
    line_hex("shld 64 by 4", left64);
    line("shld 64 by 4 flags", flags_of(carry, 0, zero, sign));
    u32 left32 = 0x81000001;
    __asm__("shld $8, %[source], %[target]\n\tsetc %[carry]\n\tsetz %[zero]\n\tsets %[sign]"
            : [target] "+r"(left32), [carry] "=r"(carry), [zero] "=r"(zero), [sign] "=r"(sign) : [source] "r"(0xa0000000u) : "cc");
    line_hex("shld 32 by 8", left32);
    line("shld 32 by 8 flags", flags_of(carry, 0, zero, sign));
    u64 right64 = 0x8000000000000001UL;
    __asm__("shrd $1, %[source], %[target]\n\tsetc %[carry]\n\tseto %[overflow]\n\tsetz %[zero]\n\tsets %[sign]"
            : [target] "+r"(right64), [carry] "=r"(carry), [overflow] "=r"(overflow), [zero] "=r"(zero), [sign] "=r"(sign) : [source] "r"(0UL) : "cc");
    line_hex("shrd 64 by 1", right64);
    line("shrd 64 by 1 flags", flags_of(carry, overflow, zero, sign));
    u32 unshifted = 0x12345678;
    __asm__("stc\n\tshrd %%cl, %[source], %[target]\n\tsetc %[carry]" : [target] "+r"(unshifted), [carry] "=r"(carry) : [source] "r"(0xffffffffu), "c"(0) : "cc");
    line_hex("shrd 32 by 0", unshifted);
    line("shrd 32 by 0 carry", carry);
    u32 right32 = 0x80000000;
    __asm__("shrd %%cl, %[source], %[target]\n\tsetc %[carry]\n\tsetz %[zero]\n\tsets %[sign]"
            : [target] "+r"(right32), [carry] "=r"(carry), [zero] "=r"(zero), [sign] "=r"(sign) : [source] "r"(0xffffffffu), "c"(31) : "cc");
    line_hex("shrd 32 by 31", right32);
    line("shrd 32 by 31 flags", flags_of(carry, 0, zero, sign));

    u64 all = ~0UL, high = 0x8000000000000000UL, five = 5, seven = 7;
    __asm__("xor %%eax, %%eax\n\tadcx %[one], %[all]\n\tadox %[high2], %[high]\n\tadcx %[none], %[five]\n\tadox %[none], %[seven]\n\tsetc %[carry]\n\tseto %[overflow]"
            : [all] "+r"(all), [high] "+r"(high), [five] "+r"(five), [seven] "+r"(seven), [carry] "=r"(carry), [overflow] "=r"(overflow)
            : [one] "r"(1UL), [high2] "r"(0x8000000000000000UL), [none] "r"(0UL) : "rax", "cc");
    line("adcx sum", all);
    line("adox sum", high);
    line("adcx carried", five);
    line("adox carried", seven);
    line("adcx adox flags", flags_of(carry, overflow, 0, 0));

    u64 wrapped;
    __asm__("lea 0x10(%k1,%k2), %0" : "=r"(wrapped) : "r"(0xfffffff0UL), "r"(8UL));
    line_hex("lea 32-bit wraps", wrapped);
    static u32 words[2] = {0x11111111, 0x22222222};
    u32 word;
    __asm__("movl 4(%k1), %0" : "=r"(word) : "r"((u64)words));
    line_hex("load through 32-bit address", word);
    static _Alignas(64) u64 line_data[16];
    for (int i = 0; i < 16; ++i) line_data[i] = 0x5a5a5a5a00000000UL + (u64)i;
    __asm__ volatile("clflush (%0)\n\tclflushopt 64(%0)\n\tclwb 8(%0)\n\tmfence" : : "r"(line_data) : "memory");
    u64 flushed = 0;
    for (int i = 0; i < 16; ++i) flushed += line_data[i];
    line_hex("after cache line flushes", flushed);
}

static volatile u64 opaque_misalignment = 13;

__attribute__((noinline)) static void unaligned_accesses(void) {
    static _Alignas(16) u8 bytes[64];
    for (int i = 0; i < 64; ++i) bytes[i] = (u8)i;
    u8* base = bytes + opaque_misalignment;
    u64 quad;
    u32 double_word;
    u16 word;
    __asm__ volatile("movq (%1), %0" : "=r"(quad) : "r"(base) : "memory");
    __asm__ volatile("movl 1(%1), %0" : "=r"(double_word) : "r"(base) : "memory");
    __asm__ volatile("movw 2(%1), %0" : "=r"(word) : "r"(base) : "memory");
    line_hex("unaligned load 64", quad);
    line_hex("unaligned load 32", double_word);
    line_hex("unaligned load 16", word);
    __asm__ volatile("movq %1, 16(%0)" : : "r"(base), "r"(0x1122334455667788UL) : "memory");
    __asm__ volatile("movl %1, 34(%0)" : : "r"(base), "r"(0xaabbccddU) : "memory");
    __asm__ volatile("movw %1, 18(%0)" : : "r"(base), "r"((u16)0xeeff) : "memory");
    for (int i = 3; i < 7; ++i) {
        u64 aligned;
        __asm__ volatile("movq (%1), %0" : "=r"(aligned) : "r"(bytes + 8 * i) : "memory");
        line_hex("after unaligned stores", aligned);
    }
}

struct Record {
    i64 key;
    u32 values[6];
    u16 flags;
    u8 tag;
};

__attribute__((noinline)) static struct Record make_record(i64 key) {
    struct Record record;
    memset(&record, 0, sizeof(record));
    record.key = key;
    for (int i = 0; i < 6; ++i) record.values[i] = (u32)(key * (i + 1));
    record.flags = (u16)(key ^ 0xbeef);
    record.tag = (u8)key;
    return record;
}

static void insertion_sort(i32* values, int count) {
    for (int i = 1; i < count; ++i) {
        i32 value = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > value) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = value;
    }
}

static volatile u64 opaque_unsigned = 0xfedcba9876543210ul;
static volatile i64 opaque_signed = -1234567890123l;
static volatile u32 opaque_count = 13;
static volatile i32 opaque_small = -77;

int main(void) {
    print("ps5lift basic fixture\n");
    line("fibonacci(20)", (i64)fibonacci(20));

    for (int i = 0; i < 4; ++i) line("operation", operations[i](1000, 7));
    line("many_arguments", many_arguments(1, 2, 3, 4, 5, 6, 7, 8, 9));

    u64 u = opaque_unsigned;
    i64 s = opaque_signed;
    u32 c = opaque_count;
    line_hex("u >> c", u >> c);
    line_hex("u << c", u << c);
    line("s >> c", s >> c);
    line_hex("rotate", (u >> c) | (u << (64 - c)));
    line_hex("u / 1000003", u / 1000003);
    line_hex("u % 1000003", u % 1000003);
    line("s / -97", s / -97);
    line("s % -97", s % -97);
    line("small / 5", opaque_small / 5);
    line("small % 5", opaque_small % 5);
    line_hex("high product", (u64)(((unsigned __int128)u * u) >> 64));
    line("signed high", (i64)(((__int128)s * s) >> 64));
    line("ctz", __builtin_ctzll(u));
    line("clz", __builtin_clzll(u >> 9));
    line("popcount", __builtin_popcountll(u));
    line("byte add", (i64)(u8)((u8)u + (u8)0xf7));
    line("word sub", (i64)(i16)((u16)u - (u16)0x8000));
    line("compare", (s < -5) + 2 * (u > 5) + 4 * ((i32)c == 13) + 8 * (opaque_small >= 0));

    i64 total = 0;
    for (int i = 0; i < 9; ++i) {
        const char* name = classify(i);
        total += (i64)length_of(name) * (i + 1);
    }
    line("classify total", total);
    for (i64 i = 0; i < 4; ++i) line("table after compare", table_index_copied_after_compare(i));
    for (i64 i = 0; i < 4; ++i) line("table before compare", table_index_copied_before_compare(i));
    for (i64 i = 0; i < 5; ++i) line("table base before loop", table_base_before_loop(i));
    i64 (*stored)(void);
    store_address_taken_by_lea(&stored);
    line("function stored through lea", call_function_pointer(stored));
    line("function passed through lea", pass_address_taken_by_lea());
    zen2_integer();
    unaligned_accesses();

    struct Record record = make_record(41);
    struct Record copy;
    memcpy(&copy, &record, sizeof(copy));
    u64 checksum = (u64)copy.key + copy.flags + copy.tag;
    for (int i = 0; i < 6; ++i) checksum = checksum * 31 + copy.values[i];
    line("record checksum", (i64)checksum);

    i32 values[] = {5, -3, 99, 0, 42, -100, 7, 7, 1};
    insertion_sort(values, 9);
    for (int i = 0; i < 9; ++i) line("sorted", values[i]);

    static u64 counter;
    for (int i = 0; i < 10; ++i) __atomic_fetch_add(&counter, (u64)i, __ATOMIC_SEQ_CST);
    u64 expected = 45;
    int exchanged = __atomic_compare_exchange_n(&counter, &expected, 1000, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    line("atomic counter", (i64)counter);
    line("atomic exchanged", exchanged);

    print("done\n");
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
