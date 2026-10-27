#ifndef PS5LIFT_FIXTURE_HOST_H
#define PS5LIFT_FIXTURE_HOST_H

long host_write(const char* text, unsigned long length);
void host_exit(int code) __attribute__((noreturn));
unsigned long host_ticks(void);
unsigned long host_thread_create(long (*entry)(long), long argument);
long host_thread_join(unsigned long thread);

#endif
