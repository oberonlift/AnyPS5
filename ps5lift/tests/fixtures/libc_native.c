#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "libc_nids.h"

int nid_printf(const char* format, ...) __asm__(NID_printf);
int nid_printf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int count = vprintf(format, arguments);
    va_end(arguments);
    return count;
}

int nid_snprintf(char* buffer, unsigned long size, const char* format, ...) __asm__(NID_snprintf);
int nid_snprintf(char* buffer, unsigned long size, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int count = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return count;
}

int nid_vsnprintf(char* buffer, unsigned long size, const char* format, va_list arguments) __asm__(NID_vsnprintf);
int nid_vsnprintf(char* buffer, unsigned long size, const char* format, va_list arguments) {
    return vsnprintf(buffer, size, format, arguments);
}

int nid_sscanf(const char* input, const char* format, ...) __asm__(NID_sscanf);
int nid_sscanf(const char* input, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int count = vsscanf(input, format, arguments);
    va_end(arguments);
    return count;
}

int nid_puts(const char* text) __asm__(NID_puts);
int nid_puts(const char* text) { return puts(text); }

long double nid_strtold(const char* text, char** end) __asm__(NID_strtold);
long double nid_strtold(const char* text, char** end) { return strtold(text, end); }

double nid_strtod(const char* text, char** end) __asm__(NID_strtod);
double nid_strtod(const char* text, char** end) { return strtod(text, end); }

long double nid_modfl(long double value, long double* integral) __asm__(NID_modfl);
long double nid_modfl(long double value, long double* integral) { return modfl(value, integral); }

long double nid_fabsl(long double value) __asm__(NID_fabsl);
long double nid_fabsl(long double value) { return fabsl(value); }

int nid_fprintf(void* stream, const char* format, ...) __asm__(NID_fprintf);
int nid_fprintf(void* stream, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int count = vfprintf(stream, format, arguments);
    va_end(arguments);
    return count;
}

char nid_stdout[8] __asm__(NID__Stdout);

void host_exit(int code) {
    fflush(stdout);
    _Exit(code);
}
