#ifndef PS5LIFT_FIXTURE_HOST_H
#define PS5LIFT_FIXTURE_HOST_H

long host_write(const char* text, unsigned long length);
void host_exit(int code) __attribute__((noreturn));

#endif
