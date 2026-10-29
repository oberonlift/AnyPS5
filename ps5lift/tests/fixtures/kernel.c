#include "kernel_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
void guest_exit(int code) __asm__(NID_exit) __attribute__((noreturn));
int guest_pthread_create(void** thread, const void* attr, void* (*entry)(void*), void* argument, const char* name) __asm__(NID_scePthreadCreate);
int guest_pthread_join(void* thread, void** result) __asm__(NID_scePthreadJoin);
void guest_pthread_exit(void* result) __asm__(NID_scePthreadExit) __attribute__((noreturn));
int guest_attr_init(void** attr) __asm__(NID_scePthreadAttrInit);
int guest_attr_destroy(void** attr) __asm__(NID_scePthreadAttrDestroy);
int guest_attr_set_stack_size(void** attr, unsigned long size) __asm__(NID_scePthreadAttrSetstacksize);
int guest_attr_get(void* thread, void** attr) __asm__(NID_scePthreadAttrGet);
int guest_attr_get_stack(void** attr, void** address, unsigned long* size) __asm__(NID_scePthreadAttrGetstack);
void* guest_pthread_self(void) __asm__(NID_scePthreadSelf);
int guest_mutex_init(void** mutex, const void* attr, const char* name) __asm__(NID_scePthreadMutexInit);
int guest_mutex_lock(void** mutex) __asm__(NID_scePthreadMutexLock);
int guest_mutex_unlock(void** mutex) __asm__(NID_scePthreadMutexUnlock);
int guest_mutex_destroy(void** mutex) __asm__(NID_scePthreadMutexDestroy);
int guest_usleep(unsigned int microseconds) __asm__(NID_sceKernelUsleep);
unsigned long guest_process_time(void) __asm__(NID_sceKernelGetProcessTime);
unsigned long guest_read_tsc(void) __asm__(NID_sceKernelReadTsc);
unsigned long guest_tsc_frequency(void) __asm__(NID_sceKernelGetTscFrequency);
void (*guest_signal(int number, void (*handler)(int)))(int) __asm__(NID_signal);
int guest_raise(int number) __asm__(NID_raise);
int guest_sigprocmask(int how, const unsigned* set, unsigned* old) __asm__(NID_sigprocmask);
int guest_sigsetjmp(long* buffer, long save_mask) __asm__(NID_sigsetjmp) __attribute__((returns_twice));
void guest_siglongjmp(long* buffer, int value) __asm__(NID_siglongjmp) __attribute__((noreturn));

static void interrupted(int number) {
    (void)number;
}

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

static void* exiting_worker(void* argument) {
    guest_pthread_exit((void*)((long)argument + 70));
}

enum { LargeStack = 32 << 20, Levels = 5000 };

__attribute__((noinline)) static long reach(long depth, volatile char* above) {
    volatile char frame[4096];
    frame[0] = above[0];
    if (depth == 0) return frame[0];
    return reach(depth - 1, frame) + 1;
}

static long stack_reported;

static void* deep_worker(void* argument) {
    (void)argument;
    volatile char top = 0;
    void* attr = 0;
    void* address = 0;
    unsigned long size = 0;
    guest_attr_init(&attr);
    guest_attr_get(guest_pthread_self(), &attr);
    guest_attr_get_stack(&attr, &address, &size);
    guest_attr_destroy(&attr);
    stack_reported = size >= LargeStack && (volatile char*)address <= &top && &top < (volatile char*)address + size;
    return (void*)reach(Levels, &top);
}

enum { SigBlock = 1, SigSetmask = 3, Sigusr1 = 30 };

static long jump_buffer[12];
static const unsigned empty_set[4];
static const unsigned usr1_set[4] = {1u << (Sigusr1 - 1)};

static int sigsetjmp_keeps_blocked_set(void) {
    guest_sigprocmask(SigSetmask, empty_set, 0);
    if (guest_sigsetjmp(jump_buffer, 1l << 32) == 0) {
        guest_sigprocmask(SigBlock, usr1_set, 0);
        guest_siglongjmp(jump_buffer, 1);
    }
    unsigned blocked[4];
    guest_sigprocmask(SigBlock, 0, blocked);
    guest_sigprocmask(SigSetmask, empty_set, 0);
    return (blocked[0] >> (Sigusr1 - 1)) & 1;
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

    void* exiting = 0;
    void* exited = 0;
    guest_pthread_create(&exiting, 0, exiting_worker, (void*)7, "exiting");
    guest_printf("scePthreadExit join %d result %ld\n", guest_pthread_join(exiting, &exited), (long)exited);

    void* large = 0;
    void* large_attr = 0;
    void* levels = 0;
    guest_attr_init(&large_attr);
    guest_attr_set_stack_size(&large_attr, LargeStack);
    guest_pthread_create(&large, &large_attr, deep_worker, 0, "deep");
    guest_attr_destroy(&large_attr);
    guest_printf("32 MiB stack join %d levels %ld, reported stack holds it %ld\n", guest_pthread_join(large, &levels), (long)levels, stack_reported);

    const unsigned long before = guest_process_time();
    guest_usleep(20000);
    const unsigned long after = guest_process_time();
    guest_printf("slept %s\n", after - before >= 20000 ? "at least 20 ms" : "too briefly");

    const unsigned long frequency = guest_tsc_frequency();
    const unsigned long first = guest_read_tsc();
    const unsigned long middle = rdtsc();
    const unsigned long last = guest_read_tsc();
    guest_printf("tsc %s, frequency %s\n", first <= middle && middle <= last ? "ordered" : "out of order", frequency > 1000000 ? "plausible" : "implausible");
    guest_signal(2, interrupted);
    guest_printf("signal returns the guest's handler %d\n", guest_signal(2, (void (*)(int))0) == interrupted);
    guest_printf("ignoring SIGPIPE replaces the default %d\n", guest_signal(13, (void (*)(int))1) == 0);
    guest_printf("raising an ignored SIGPIPE returns %d\n", guest_raise(13));
    guest_printf("signal reports SIG_IGN %d\n", guest_signal(13, (void (*)(int))0) == (void (*)(int))1);
    guest_printf("siglongjmp without a saved mask keeps the blocked set %d\n", sigsetjmp_keeps_blocked_set());
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
