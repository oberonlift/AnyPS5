#include "kernel_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
void guest_exit(int code) __asm__(NID_exit) __attribute__((noreturn));
int guest_pthread_create(void** thread, const void* attr, void* (*entry)(void*), void* argument, const char* name) __asm__(NID_scePthreadCreate);
int guest_pthread_join(void* thread, void** result) __asm__(NID_scePthreadJoin);
int guest_mutex_init(void** mutex, const void* attr, const char* name) __asm__(NID_scePthreadMutexInit);
int guest_mutex_lock(void** mutex) __asm__(NID_scePthreadMutexLock);
int guest_mutex_unlock(void** mutex) __asm__(NID_scePthreadMutexUnlock);
int guest_mutex_destroy(void** mutex) __asm__(NID_scePthreadMutexDestroy);
int guest_usleep(unsigned int microseconds) __asm__(NID_sceKernelUsleep);
unsigned long guest_process_time(void) __asm__(NID_sceKernelGetProcessTime);
unsigned long guest_read_tsc(void) __asm__(NID_sceKernelReadTsc);
unsigned long guest_tsc_frequency(void) __asm__(NID_sceKernelGetTscFrequency);

enum { Threads = 4, Iterations = 10000 };

static void* mutex;
static volatile long counter;

static void* worker(void* argument) {
    for (int i = 0; i < Iterations; ++i) {
        guest_mutex_lock(&mutex);
        counter = counter + 1;
        guest_mutex_unlock(&mutex);
    }
    return (void*)((long)argument * 3);
}

static unsigned long rdtsc(void) {
    unsigned int low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return (unsigned long)high << 32 | low;
}

int main(void) {
    guest_printf("mutex init %d\n", guest_mutex_init(&mutex, 0, "counter"));
    void* threads[Threads];
    for (long i = 0; i < Threads; ++i) {
        const int created = guest_pthread_create(&threads[i], 0, worker, (void*)(i + 1), "worker");
        if (created != 0) guest_printf("create %ld failed 0x%x\n", i, created);
    }
    long sum = 0;
    for (int i = 0; i < Threads; ++i) {
        void* result = 0;
        guest_pthread_join(threads[i], &result);
        sum += (long)result;
    }
    guest_printf("counter %ld results %ld\n", counter, sum);
    guest_printf("mutex destroy %d\n", guest_mutex_destroy(&mutex));

    const unsigned long before = guest_process_time();
    guest_usleep(20000);
    const unsigned long after = guest_process_time();
    guest_printf("slept %s\n", after - before >= 20000 ? "at least 20 ms" : "too briefly");

    const unsigned long frequency = guest_tsc_frequency();
    const unsigned long first = guest_read_tsc();
    const unsigned long middle = rdtsc();
    const unsigned long last = guest_read_tsc();
    guest_printf("tsc %s, frequency %s\n", first <= middle && middle <= last ? "ordered" : "out of order", frequency > 1000000 ? "plausible" : "implausible");
    return 0;
}

void start_main(void) {
    guest_exit(main());
}

__asm__(".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call start_main\n"
        "    ud2\n");
