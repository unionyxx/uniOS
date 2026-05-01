#include <stddef.h>
#include <stdint.h>

extern "C" {
using ctor_func = void (*)();
using guard_func = uint64_t;

extern ctor_func __init_array_start[];
extern ctor_func __init_array_end[];

void *__dso_handle = &__dso_handle;

static inline void runtime_halt_forever()
{
    asm volatile("cli" ::: "memory");
    for (;;)
        asm volatile("hlt");
}

int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

int __cxa_guard_acquire(guard_func *guard)
{
    if (!guard)
        return 1;

    guard_func expected = 0;
    if (__atomic_compare_exchange_n(guard, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 1;

    while (__atomic_load_n(guard, __ATOMIC_ACQUIRE) == 1)
        asm volatile("pause" ::: "memory");

    return 0;
}

void __cxa_guard_release(guard_func *guard)
{
    if (!guard)
        return;
    __atomic_store_n(guard, static_cast<guard_func>(2), __ATOMIC_RELEASE);
}

void __cxa_guard_abort(guard_func *guard)
{
    if (!guard)
        return;
    __atomic_store_n(guard, static_cast<guard_func>(0), __ATOMIC_RELEASE);
}

void __cxa_pure_virtual()
{
    runtime_halt_forever();
}
}

void call_global_constructors()
{
    static bool constructors_called = false;
    if (constructors_called)
        return;

    constructors_called = true;
    for (ctor_func *p = __init_array_start; p < __init_array_end; ++p) {
        if (*p)
            (*p)();
    }
}
