#include <kernel/fs/block_dev.h>
#include <kernel/ktest.h>

namespace {

uint32_t g_flush_calls = 0;

int counting_flush(BlockDevice *dev)
{
    (void)dev;
    g_flush_calls++;
    return 0;
}

} // namespace

KTEST(block_dev_flush_skips_clean_devices_and_clears_dirty)
{
    BlockDevice dev = {};
    dev.flush = counting_flush;
    g_flush_calls = 0;

    dev.cache_dirty = false;
    KTEST_EXPECT_EQ(block_dev_flush(&dev), 0);
    KTEST_EXPECT_EQ(g_flush_calls, 0u);

    dev.cache_dirty = true;
    KTEST_EXPECT_EQ(block_dev_flush(&dev), 0);
    KTEST_EXPECT_EQ(g_flush_calls, 1u);
    KTEST_EXPECT(!dev.cache_dirty);

    KTEST_EXPECT_EQ(block_dev_flush(&dev), 0);
    KTEST_EXPECT_EQ(g_flush_calls, 1u);
}

KTEST(block_dev_flush_handles_missing_device_and_flush_op)
{
    KTEST_EXPECT_EQ(block_dev_flush(nullptr), 0);

    BlockDevice dev = {};
    dev.cache_dirty = true;
    KTEST_EXPECT_EQ(block_dev_flush(&dev), 0);
    KTEST_EXPECT(dev.cache_dirty); // no flush op: nothing could commit it
}
