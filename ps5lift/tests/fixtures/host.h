#ifndef PS5LIFT_FIXTURE_HOST_H
#define PS5LIFT_FIXTURE_HOST_H

long host_write(const char* text, unsigned long length);
void host_exit(int code) __attribute__((noreturn));
unsigned long host_ticks(void);
unsigned long host_thread_create(long (*entry)(long), long argument);
long host_thread_join(unsigned long thread);
void host_sort(void* base, unsigned long count, unsigned long size, int (*compare)(const void*, const void*));
double host_apply(double (*function)(double, long), double value, long count);
long host_on_thread(long (*function)(long), long argument);
long host_call_eight(long (*function)(long, long, long, long, long, long, long, long));
long double host_extended(long a, long b, long c, long d, long e, long f, long g, long double x, long double y);

#endif
