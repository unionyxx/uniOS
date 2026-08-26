#include <stddef.h>
#include <stdint.h>

extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);

void *operator new(size_t size)
{
    return malloc(size);
}
void *operator new[](size_t size)
{
    return malloc(size);
}
void operator delete(void *p) noexcept
{
    free(p);
}
void operator delete[](void *p) noexcept
{
    free(p);
}
void operator delete(void *p, size_t) noexcept
{
    free(p);
}
void operator delete[](void *p, size_t) noexcept
{
    free(p);
}
void *operator new(size_t, void *p) noexcept
{
    return p;
}
void *operator new[](size_t, void *p) noexcept
{
    return p;
}

extern "C" void __cxa_pure_virtual()
{
    const char *err = "FATAL: Pure virtual call!\n";
    __asm__ __volatile__("syscall" : : "a"(1), "D"(1), "S"(err), "d"(26) : "rcx", "r11", "memory");
    while (1)
        ;
}

// Static-local initialization guards. uniOS apps are single-threaded, so the
// guard word only needs to record "initialized" for the fast path. Clang
// passes an 8-byte guard object and expects a non-zero return when the
// constructor must run.
extern "C" int __cxa_guard_acquire(uint64_t *guard)
{
    return guard && (*guard & 0xFFu) == 0;
}

extern "C" void __cxa_guard_release(uint64_t *guard)
{
    if (guard)
        *guard = 1;
}

extern "C" void __cxa_guard_abort(uint64_t *guard)
{
    if (guard)
        *guard = 0;
}

// Programs are single-image and never unload, and process teardown reclaims
// everything, so static destructors are not tracked; the symbols exist so
// static objects with destructors link.
extern "C" int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle)
{
    (void)func;
    (void)arg;
    (void)dso_handle;
    return 0;
}

extern "C" void __cxa_finalize(void *dso_handle)
{
    (void)dso_handle;
}

extern "C" void *__dso_handle = nullptr;
