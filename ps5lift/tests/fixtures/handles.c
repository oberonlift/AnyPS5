#include "handles_nids.h"

int guest_printf(const char* format, ...) __asm__(NID_printf);
[[noreturn]] void guest_exit(int code) __asm__(NID_exit);
int guest_create_object(void** object) __asm__(NID_sceNpUniversalDataSystemCreateEventPropertyObject);
int guest_create_array(void** array) __asm__(NID_sceNpUniversalDataSystemCreateEventPropertyArray);
int guest_create_event(const char* name, const void* properties, void** event, void** properties_ptr) __asm__(NID_sceNpUniversalDataSystemCreateEvent);
int guest_object_set_string(void* object, const char* key, const char* value) __asm__(NID_sceNpUniversalDataSystemEventPropertyObjectSetString);
int guest_object_set_object(void* object, const char* key, const void* value, void** value_ptr) __asm__(NID_sceNpUniversalDataSystemEventPropertyObjectSetObject);
int guest_object_set_array(void* object, const char* key, const void* value, void** value_ptr) __asm__(NID_sceNpUniversalDataSystemEventPropertyObjectSetArray);
int guest_array_set_string(void* array, const char* value) __asm__(NID_sceNpUniversalDataSystemEventPropertyArraySetString);
int guest_array_set_object(void* array, const void* value, void** value_ptr) __asm__(NID_sceNpUniversalDataSystemEventPropertyArraySetObject);
int guest_array_set_array(void* array, const void* value, void** value_ptr) __asm__(NID_sceNpUniversalDataSystemEventPropertyArraySetArray);
int guest_destroy_object(void* object) __asm__(NID_sceNpUniversalDataSystemDestroyEventPropertyObject);
int guest_destroy_array(void* array) __asm__(NID_sceNpUniversalDataSystemDestroyEventPropertyArray);
int guest_destroy_event(void* event) __asm__(NID_sceNpUniversalDataSystemDestroyEvent);
int guest_http_init(int memory, int ssl, unsigned long pool) __asm__(NID_sceHttpInit);
int guest_http_term(int context) __asm__(NID_sceHttpTerm);
int guest_http_create_epoll(int context, void** epoll) __asm__(NID_sceHttpCreateEpoll);
int guest_http_destroy_epoll(int context, void* epoll) __asm__(NID_sceHttpDestroyEpoll);

int main(void) {
    void* object = 0;
    void* child = 0;
    void* array = 0;
    guest_printf("uds create object %d array %d child %d\n", guest_create_object(&object), guest_create_array(&array), guest_create_object(&child));
    guest_printf("uds object set string %d\n", guest_object_set_string(object, "name", "title"));
    guest_printf("uds array set string %d\n", guest_array_set_string(array, "entry"));
    void* child_in_object = 0;
    void* array_in_object = 0;
    guest_printf("uds object set object %d array %d\n", guest_object_set_object(object, "child", child, &child_in_object),
                 guest_object_set_array(object, "list", array, &array_in_object));
    guest_printf("uds set string through the object's copies %d %d\n", guest_object_set_string(child_in_object, "depth", "one"),
                 guest_array_set_string(array_in_object, "more"));
    void* child_in_array = 0;
    void* array_in_array = 0;
    guest_printf("uds array set object %d array %d\n", guest_array_set_object(array, child, &child_in_array), guest_array_set_array(array, array, &array_in_array));
    guest_printf("uds set string through the array's copies %d %d\n", guest_object_set_string(child_in_array, "depth", "two"),
                 guest_array_set_string(array_in_array, "nested"));
    void* event = 0;
    void* event_properties = 0;
    guest_printf("uds create event %d\n", guest_create_event("titleStarted", object, &event, &event_properties));
    guest_printf("uds set string through the event's properties %d\n", guest_object_set_string(event_properties, "mode", "offline"));
    guest_printf("uds destroy event %d object %d child %d array %d\n", guest_destroy_event(event), guest_destroy_object(object), guest_destroy_object(child),
                 guest_destroy_array(array));

    const int http = guest_http_init(1, 1, 0x10000);
    void* epoll = 0;
    guest_printf("http create epoll %d\n", guest_http_create_epoll(http, &epoll));
    const int destroyed = guest_http_destroy_epoll(http, epoll);
    guest_printf("http destroy epoll %d, term %d\n", destroyed, guest_http_term(http));
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
