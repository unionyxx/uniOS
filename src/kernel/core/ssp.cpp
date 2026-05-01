#include <kernel/cpu.h>
#include <kernel/panic.h>
#include <stdint.h>

#if UINT32_MAX == UINTPTR_MAX
#define STACK_CHK_GUARD 0xe2dee396
#else
#define STACK_CHK_GUARD 0x595e9fbd94fda766
#endif

uintptr_t __stack_chk_guard = STACK_CHK_GUARD;

extern "C" void __stack_chk_guard_init()
{
    uintptr_t canary = STACK_CHK_GUARD;

    if (g_cpu_features.has_rdrand) {
        uint64_t val;
        uint8_t ok;
        asm volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok)
            canary ^= (uintptr_t)val;
    } else {
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        canary ^= (uintptr_t)lo | ((uintptr_t)hi << 32);
    }

    __stack_chk_guard = canary;
}

extern "C" __attribute__((noreturn)) void __stack_chk_fail(void)
{
    panic("Stack smashing detected!");
    for (;;)
        asm volatile("hlt");
}
