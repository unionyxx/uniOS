#include <kernel/fs/storage_guard.h>
#include <kernel/ktest.h>

KTEST(storage_guard_defaults_to_writable_and_can_toggle)
{
    KTEST_EXPECT(storage_get_mode() == STORAGE_MODE_WRITABLE);
    KTEST_EXPECT(storage_reads_allowed());
    KTEST_EXPECT(storage_writes_allowed());
    storage_set_mode(STORAGE_MODE_READ_ONLY);
    KTEST_EXPECT(storage_get_mode() == STORAGE_MODE_READ_ONLY);
    KTEST_EXPECT(storage_reads_allowed());
    KTEST_EXPECT(!storage_writes_allowed());
    storage_set_mode(STORAGE_MODE_WRITABLE);
    KTEST_EXPECT(storage_get_mode() == STORAGE_MODE_WRITABLE);
    KTEST_EXPECT(storage_reads_allowed());
    KTEST_EXPECT(storage_writes_allowed());
    storage_set_mode(STORAGE_MODE_OFF);
    KTEST_EXPECT(storage_get_mode() == STORAGE_MODE_OFF);
    KTEST_EXPECT(!storage_reads_allowed());
    KTEST_EXPECT(!storage_writes_allowed());
    storage_set_mode(STORAGE_MODE_WRITABLE);
}
