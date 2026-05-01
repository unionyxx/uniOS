#include <kernel/ktest.h>

extern "C" KTestCase __ktests_start[];
extern "C" KTestCase __ktests_end[];

static const char *g_current_test_name = nullptr;
static bool g_current_test_failed = false;

bool ktest_verbose_enabled()
{
#ifdef DEBUG
    return false;
#else
    return false;
#endif
}

void ktest_record_failure(const char *condition, const char *file, int line)
{
    g_current_test_failed = true;
    DEBUG_ERROR("ktest %s failed: %s (%s:%d)", g_current_test_name ? g_current_test_name : "<unknown>", condition, file,
                line);
}

void ktest_run_all()
{
    int passed = 0;
    int total = 0;

    DEBUG_INFO("ktest suite started");

    for (KTestCase *test = __ktests_start; test < __ktests_end; test++) {
        if (!test->name || !test->func)
            continue;
        total++;
        g_current_test_name = test->name;
        g_current_test_failed = false;
        if (ktest_verbose_enabled())
            DEBUG_TRACE("ktest %s started", test->name);
        test->func();
        if (g_current_test_failed)
            continue;

        passed++;
        if (ktest_verbose_enabled())
            DEBUG_TRACE("ktest %s passed", test->name);
    }

    g_current_test_name = nullptr;
    g_current_test_failed = false;

    if (total == 0) {
        DEBUG_WARN("ktest suite empty");
        return;
    }

    if (passed == total)
        DEBUG_SUCCESS("ktest suite passed (%d/%d)", passed, total);
    else
        DEBUG_ERROR("ktest suite failed (%d/%d passed, %d failed)", passed, total, total - passed);
}
