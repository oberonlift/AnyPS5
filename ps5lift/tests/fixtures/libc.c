#include <stdarg.h>

#include "host.h"
#include "libc_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
int guest_snprintf(char* buffer, unsigned long size, const char* format, ...) __asm__(NID_snprintf);
int guest_vsnprintf(char* buffer, unsigned long size, const char* format, va_list arguments) __asm__(NID_vsnprintf);
int guest_sscanf(const char* input, const char* format, ...) __asm__(NID_sscanf);
int guest_puts(const char* text) __asm__(NID_puts);
long double guest_strtold(const char* text, char** end) __asm__(NID_strtold);
double guest_strtod(const char* text, char** end) __asm__(NID_strtod);
long double guest_modfl(long double value, long double* integral) __asm__(NID_modfl);
long double guest_fabsl(long double value) __asm__(NID_fabsl);
int guest_fprintf(void* stream, const char* format, ...) __asm__(NID_fprintf);
extern char guest_stdout[] __asm__(NID__Stdout);

static int format_list(char* buffer, unsigned long size, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int count = guest_vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return count;
}

static volatile long double third = 1.0L;

int main(void) {
    char buffer[256];
    guest_printf("integers %d %u %ld %lld %x %llx %o %c|%s|\n", -42, 42u, -1234567890123l, 9223372036854775807ll, 0xbeefu, 0x123456789abcdefull, 8, 'z', "text");
    guest_printf("many %d %d %d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    guest_printf("doubles %.3f %e %g %a %.17g %.17g %.17g %.17g %.17g %.17g\n", 3.14159, 6.02e23, 0.0001, 1.5, 0.1, 0.2, 0.3, 1e300, -2.5, 1.0 / 3.0);
    third = third / 3.0L;
    guest_printf("extended %.20Lf %.19Le %Le %d %.3Lf\n", third, third, -third * 1e100L, 7, (long double)2.0005);
    guest_printf("mixed %d %.2f %s %Lg %ld %.1f %c\n", 1, 2.25, "three", (long double)4.5, 5l, 6.75, '7');
    const int count = guest_snprintf(buffer, sizeof(buffer), "%08.3f|%-6d|%+d|%5s|%.2s", -3.5, 12, 34, "ab", "xyz");
    guest_puts(buffer);
    guest_printf("count %d\n", count);
    const int truncated = guest_snprintf(buffer, 8, "%s", "truncated output");
    guest_printf("truncated %d %s\n", truncated, buffer);
    const int listed = format_list(buffer, sizeof(buffer), "list %d %.2f %s %Lf %d %d %d %d %d", 1, 2.5, "c", (long double)0.25, 5, 6, 7, 8, 9);
    guest_printf("%s (%d)\n", buffer, listed);

    int number = 0;
    double real = 0;
    long double extended = 0;
    char word[16] = {0};
    const int scanned = guest_sscanf("  17 2.75 0.1 word", "%d %lf %Lf %15s", &number, &real, &extended, word);
    guest_printf("scanned %d: %d %.2f %.25Lf %s\n", scanned, number, real, extended, word);

    char* end = 0;
    const long double parsed = guest_strtold("3.14159265358979323846264338327950288 rest", &end);
    guest_printf("strtold %.25Lg %.19Le |%s|\n", parsed, parsed, end);
    guest_printf("strtold tiny %.10Lg huge %Lg\n", guest_strtold("1e-4940", 0), guest_strtold("1e4933", 0));
    guest_printf("strtod %.17g\n", guest_strtod("2.718281828459045", 0));
    long double whole = 0;
    const long double fraction = guest_modfl(-12.625L, &whole);
    guest_printf("modfl %Lf %Lf\n", fraction, whole);
    guest_printf("fabsl %.19Le\n", guest_fabsl(-third));
    guest_fprintf(guest_stdout, "fprintf %s %d\n", "_Stdout", 5);
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
