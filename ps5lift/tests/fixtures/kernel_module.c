long kernel_module_answer(long value) {
    return value * 7;
}

static unsigned long start_args;
static const long* start_argp;
static long start_sum;
static int initialized;

static int module_start(unsigned long args, const long* argp) {
    start_args = args;
    start_argp = argp;
    start_sum = argp[0] + argp[1];
    return 5;
}

static void module_initializer(void) {
    initialized = 1;
}

static void (*const initializers[])(void) = {module_initializer};

void* __aps5_guest_lifecycle_nid_no_patch[9] = {(void*)0x415053354c494658ul, 0, (void*)module_start, 0, (void*)1, 0, 0, 0, (void*)&initializers[0]};

long kernel_module_started(void) {
    return start_sum * 100 + initialized * 10 + (long)start_args;
}

const long* kernel_module_argp(void) {
    return start_argp;
}
