#pragma once
#include <kernel/debug.h>

struct KTestCase
{
    const char *name;
    void (*func)();
};

void ktest_record_failure(const char *condition, const char *file, int line);
bool ktest_verbose_enabled();

#define KTEST(name)                                                                                                    \
    static void ktest_##name();                                                                                        \
    __attribute__((section(".ktests"), used)) static const KTestCase ktest_case_##name = {#name, ktest_##name};        \
    static void ktest_##name()

#define KTEST_EXPECT(condition)                                                                                        \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ktest_record_failure(#condition, __FILE__, __LINE__);                                                      \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#define KTEST_EXPECT_EQ(a, b) KTEST_EXPECT((a) == (b))

void ktest_run_all();
