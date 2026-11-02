#include "json_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
[[noreturn]] void guest_exit(int code) __asm__(NID_exit);
void guest_parameter_construct(void* self) __asm__(NID__ZN3sce4Json14InitParameter2C1Ev);
void guest_parameter_set_allocator(void* self, void* allocator, void* user) __asm__(NID__ZN3sce4Json14InitParameter212setAllocatorEPNS0_12MemAllocatorEPv);
void guest_initializer_construct(void* self) __asm__(NID__ZN3sce4Json11InitializerC1Ev);
int guest_initialize(void* self, const void* parameter) __asm__(NID__ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E);
int guest_terminate(void* self) __asm__(NID__ZN3sce4Json11Initializer9terminateEv);

struct allocator {
    const void* const* vtable;
    int marker;
};

static unsigned char arena[1 << 16] __attribute__((aligned(16)));
static unsigned long used;
static int allocations;
static int foreign;

static int ours(const struct allocator* self, const void* user) {
    return self->marker == 0x51 && user == &used;
}

static void* allocate(struct allocator* self, unsigned long size, void* user) {
    if (!ours(self, user) || used + size > sizeof(arena)) {
        ++foreign;
        return 0;
    }
    void* memory = arena + used;
    used += (size + 15) & ~15ul;
    ++allocations;
    return memory;
}

static void deallocate(struct allocator* self, void* memory, void* user) {
    if (!ours(self, user) || (unsigned char*)memory < arena || (unsigned char*)memory >= arena + sizeof(arena)) ++foreign;
}

static void notify(struct allocator* self, int error, unsigned long size, void* user) {
    (void)error, (void)size;
    if (!ours(self, user)) ++foreign;
}

static void destroy(struct allocator* self) {
    (void)self;
}

static const void* const vtable[] = {(const void*)destroy, (const void*)destroy, (const void*)allocate, (const void*)deallocate, (const void*)notify};

int main(void) {
    struct allocator allocator = {vtable, 0x51};
    unsigned long parameter[5];
    unsigned long initializer[2];
    guest_parameter_construct(parameter);
    guest_parameter_set_allocator(parameter, &allocator, &used);
    guest_initializer_construct(initializer);
    const int initialized = guest_initialize(initializer, parameter);
    guest_printf("json initialize %d, allocated from the title's allocator %d\n", initialized, allocations > 0);
    const int terminated = guest_terminate(initializer);
    guest_printf("json terminate %d, every call reached the title's allocator with its own pointers %d\n", terminated, foreign == 0);
    return 0;
}

[[noreturn]] void start_main(void) {
    guest_exit(main());
}

__asm__(".globl _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    and $-16, %rsp\n"
        "    call start_main\n"
        "    ud2\n");
