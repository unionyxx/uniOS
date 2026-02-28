#include <stddef.h>

extern "C" {
    using ctor_func = void (*)();
    extern ctor_func __init_array_start[];
    extern ctor_func __init_array_end[];

    void* __dso_handle = nullptr;

    int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
    void __cxa_pure_virtual() { for (;;) asm volatile("hlt"); }
}

void call_global_constructors() {
    for (ctor_func* p = __init_array_start; p < __init_array_end; p++) (*p)();
}
