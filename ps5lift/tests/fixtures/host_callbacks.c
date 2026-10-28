#include "host.h"

#include <pthread.h>
#include <stdlib.h>

void host_sort(void* base, unsigned long count, unsigned long size, int (*compare)(const void*, const void*)) {
    qsort(base, count, size, compare);
}

double host_apply(double (*function)(double, long), double value, long count) {
    return function(function(value, count), count + 1);
}

struct ThreadCall {
    long (*function)(long);
    long argument;
    long result;
};

static void* run(void* argument) {
    struct ThreadCall* call = argument;
    call->result = call->function(call->argument);
    return 0;
}

long host_on_thread(long (*function)(long), long argument) {
    struct ThreadCall call = {function, argument, 0};
    pthread_t thread;
    if (pthread_create(&thread, 0, run, &call) != 0) abort();
    pthread_join(thread, 0);
    return call.result;
}

long host_call_eight(long (*function)(long, long, long, long, long, long, long, long)) {
    return function(1, 2, 3, 4, 5, 6, 7, 8) + function(10, 20, 30, 40, 50, 60, 70, 80);
}
