#include <stddef.h>

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
