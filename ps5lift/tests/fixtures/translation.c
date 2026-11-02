#include "translation_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
[[noreturn]] void guest_exit(int code) __asm__(NID_exit);
void* guest_mmap(void* address, unsigned long length, int protection, int flags, int descriptor, long offset) __asm__(NID_mmap);
int guest_munmap(void* address, unsigned long length) __asm__(NID_munmap);
unsigned long guest_strlen(const char* text) __asm__(NID_strlen);
char* guest_strchr(const char* text, int character) __asm__(NID_strchr);
long guest_strtol(const char* text, char** end, int base) __asm__(NID_strtol);
void guest_qsort(void* base, unsigned long count, unsigned long size, int (*compare)(const void*, const void*)) __asm__(NID_qsort);
long guest_writev(int descriptor, const void* vectors, int count) __asm__(NID_writev);
int guest_fflush(void* stream) __asm__(NID_fflush);
int guest_pthread_mutex_lock(void* mutex) __asm__(NID_pthread_mutex_lock);
int guest_pthread_mutex_unlock(void* mutex) __asm__(NID_pthread_mutex_unlock);
int guest_sce_mutex_lock(void* mutex) __asm__(NID_scePthreadMutexLock);
int guest_sce_mutex_unlock(void* mutex) __asm__(NID_scePthreadMutexUnlock);
int guest_pthread_mutexattr_init(void* attr) __asm__(NID_pthread_mutexattr_init);
int guest_sce_mutexattr_settype(void* attr, int type) __asm__(NID_scePthreadMutexattrSettype);
int guest_pthread_mutexattr_gettype(const void* attr, int* type) __asm__(NID_pthread_mutexattr_gettype);
int guest_pthread_mutexattr_getkind_np(void* attr) __asm__(NID_pthread_mutexattr_getkind_np);
int guest_pthread_mutexattr_destroy(void* attr) __asm__(NID_pthread_mutexattr_destroy);
void* guest_pthread_self(void) __asm__(NID_pthread_self);
int guest_pthread_equal(void* left, void* right) __asm__(NID_pthread_equal);
unsigned long guest_mbsrtowcs(unsigned short* destination, const char** source, unsigned long count, void* state) __asm__(NID_mbsrtowcs);
int guest_query_protection(void* address, void** start, void** end, int* protection) __asm__(NID_sceKernelQueryMemoryProtection);
int guest_virtual_query(const void* address, int flags, void* info, unsigned long size) __asm__(NID_sceKernelVirtualQuery);
int guest_mprotect(void* address, unsigned long length, int protection) __asm__(NID_sceKernelMprotect);
int guest_pool_reserve(void* address, unsigned long length, unsigned long alignment, int flags, void** reserved) __asm__(NID_sceKernelMemoryPoolReserve);
int guest_pool_batch(const void* entries, int count, int* done, int flags) __asm__(NID_sceKernelMemoryPoolBatch);

struct guest_query_info {
    char* start;
    char* end;
    unsigned char rest[112];
};

struct guest_pool_commit {
    unsigned op;
    unsigned flags;
    void* address;
    unsigned long length;
    unsigned char protection;
    unsigned char type;
    unsigned char padding[6];
};

struct guest_iovec {
    const void* base;
    unsigned long length;
};

enum {
    ProtectReadWrite = 3,
    MapPrivate = 0x2,
    MapFixed = 0x10,
    MapAnonymous = 0x1000,
    MapBytes = 0x10000,
};

static char* const Fixed = (char*)0x1000000000ul;
static int compared_inside = 1;

static int inside(const void* pointer) {
    return (const char*)pointer >= Fixed && (const char*)pointer < Fixed + MapBytes;
}

static int compare(const void* left, const void* right) {
    compared_inside &= inside(left) && inside(right);
    return *(const int*)left - *(const int*)right;
}

int main(void) {
    char* map = guest_mmap(Fixed, MapBytes, ProtectReadWrite, MapPrivate | MapFixed | MapAnonymous, -1, 0);
    guest_printf("fixed map at 64 GiB %d\n", map == Fixed);
    if (map != Fixed) guest_exit(1);

    char* text = map;
    text[0] = 'h', text[1] = 'e', text[2] = 'l', text[3] = 'l', text[4] = 'o', text[5] = 0;
    guest_printf("strlen %lu\n", guest_strlen(text));
    guest_printf("printf through %%s: %s\n", text);
    char* found = guest_strchr(text, 'l');
    guest_printf("strchr finds 'l' at offset %ld\n", (long)(found - text));
    guest_printf("strchr finds nothing: null %d\n", guest_strchr(text, 'z') == 0);

    char* number = map + 64;
    number[0] = '4', number[1] = '2', number[2] = 'x', number[3] = 0;
    char* end = 0;
    long value = guest_strtol(number, &end, 10);
    guest_printf("strtol %ld, end at offset %ld\n", value, (long)(end - number));
    guest_printf("strtol with a null end %ld\n", guest_strtol(number, 0, 10));

    int* values = (int*)(map + 128);
    values[0] = 3, values[1] = 1, values[2] = 4, values[3] = 2;
    guest_qsort(values, 4, sizeof(int), compare);
    guest_printf("qsort compares inside the map %d, sorted %d %d %d %d\n", compared_inside, values[0], values[1], values[2], values[3]);

    char* line = map + 256;
    line[0] = 'w', line[1] = 'r', line[2] = 'i', line[3] = 't', line[4] = 'e', line[5] = 'v', line[6] = ' ';
    char* tail = map + 512;
    tail[0] = 'g', tail[1] = 'a', tail[2] = 't', tail[3] = 'h', tail[4] = 'e', tail[5] = 'r', tail[6] = 's', tail[7] = '\n';
    struct guest_iovec* vectors = (struct guest_iovec*)(map + 1024);
    vectors[0].base = line, vectors[0].length = 7;
    vectors[1].base = tail, vectors[1].length = 8;
    guest_fflush(0);
    long written = guest_writev(1, vectors, 2);
    guest_printf("writev wrote %ld\n", written);

    void** mutex = (void**)(map + 2048);
    const int posix_locked = guest_pthread_mutex_lock(mutex);
    const int posix_unlocked = guest_pthread_mutex_unlock(mutex);
    const int sce_locked = guest_sce_mutex_lock(mutex);
    const int sce_unlocked = guest_sce_mutex_unlock(mutex);
    guest_printf("zeroed mutex: pthread lock %d unlock %d, sce lock %d unlock %d\n", posix_locked, posix_unlocked, sce_locked, sce_unlocked);

    void** attr = (void**)(map + 2112);
    int type = 0;
    const int initialized = guest_pthread_mutexattr_init(attr);
    const int set = guest_sce_mutexattr_settype(attr, 2);
    const int got = guest_pthread_mutexattr_gettype(attr, &type);
    const int kind = guest_pthread_mutexattr_getkind_np(*attr);
    guest_printf("mutexattr: pthread init %d, sce settype %d, pthread gettype %d type %d, kind %d, destroy %d\n", initialized, set, got, type, kind,
                 guest_pthread_mutexattr_destroy(attr));

    void* self = guest_pthread_self();
    guest_printf("pthread_equal on its own handle %d\n", guest_pthread_equal(self, self) != 0);

    guest_printf("%%p prints the guest address %p\n", (void*)Fixed);
    guest_printf("mmap failure returns MAP_FAILED %d\n", guest_mmap(0, 0, ProtectReadWrite, MapPrivate | MapAnonymous, -1, 0) == (void*)-1);

    void* start = 0;
    void* stop = 0;
    int protection = 0;
    const int queried = guest_query_protection(map + 100, &start, &stop, &protection);
    guest_printf("sceKernelQueryMemoryProtection %d: start at the map %d, end past the address %d\n", queried, start == Fixed, (char*)stop > map + 100);

    struct guest_query_info query;
    query.start = 0, query.end = 0;
    const int virtual_queried = guest_virtual_query(map + 100, 0, &query, sizeof(query));
    guest_printf("sceKernelVirtualQuery %d: start at the map %d, end at its end %d\n", virtual_queried, query.start == Fixed, query.end == Fixed + MapBytes);

    const int reprotected = guest_mprotect(map, 0x4000, ProtectReadWrite);
    struct guest_query_info image;
    image.start = 0, image.end = 0;
    const int image_queried = guest_virtual_query((const char*)0xfbfff00000, 1, &image, sizeof(image));
    guest_printf("after mprotect %d, sceKernelVirtualQuery %d finds the program image next: starts at it %d\n", reprotected, image_queried, image.start == (char*)0xfc00000000);

    void* pool = 0;
    const int pool_reserved = guest_pool_reserve(0, 0x400000, 0x200000, 0, &pool);
    struct guest_pool_commit commit;
    commit.op = 1, commit.flags = 0, commit.address = pool, commit.length = 0x10000, commit.protection = 3, commit.type = 0;
    int committed = 0;
    const int pool_batched = guest_pool_batch(&commit, 1, &committed, 0);
    volatile int* pooled = (volatile int*)pool;
    pooled[0] = 42;
    guest_printf("memory pool: reserve %d, batch commit %d done %d, write reads back %d\n", pool_reserved, pool_batched, committed, pooled[0] == 42);

    const char* cursor = text;
    const unsigned long converted = guest_mbsrtowcs((unsigned short*)(map + 4096), &cursor, 2, 0);
    guest_printf("mbsrtowcs converted %lu, source advanced by %ld\n", converted, (long)(cursor - text));

    guest_printf("munmap %d\n", guest_munmap(map, MapBytes));
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
