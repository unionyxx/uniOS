#include <kernel/fs/vfs.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <libk/kstring.h>

#ifndef O_CREAT
#define O_CREAT 0x40
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#endif

// The uniFS RAM overlay must zero the gap a sparse write (seek past EOF +
// write) leaves behind: the gap becomes readable file data, and without the
// zeroing it disclosed kernel heap contents to userspace.
KTEST(unifs_sparse_write_zeroes_gap)
{
    // Pre-dirty the heap so the RAM file's realloc reuses a block with a
    // known non-zero pattern (fresh PMM frames are zeroed and would hide the
    // bug).
    uint8_t *dirty = static_cast<uint8_t *>(malloc(64 * 1024));
    KTEST_EXPECT(dirty != nullptr);
    kstring::memset(dirty, 0xAA, 64 * 1024);
    free(dirty);

    const char *path = "/sparse_gap_test";
    int fd = static_cast<int>(vfs_open(path, O_CREAT | O_RDWR, 0));
    KTEST_EXPECT(fd >= 0);
    if (fd < 0)
        return;

    int64_t w = vfs_write(fd, "HEAD", 4);
    KTEST_EXPECT_EQ(w, 4);

    int64_t sk = vfs_seek(fd, 5000, SEEK_SET);
    KTEST_EXPECT_EQ(sk, 5000);
    w = vfs_write(fd, "TAIL", 4);
    KTEST_EXPECT_EQ(w, 4);

    // Flush to the RAM overlay: close purges the page cache for the file.
    KTEST_EXPECT_EQ(vfs_close(fd), 0);

    // Reopen and read the gap back from uniFS itself (cache was purged).
    fd = static_cast<int>(vfs_open(path, O_RDWR, 0));
    KTEST_EXPECT(fd >= 0);
    if (fd < 0) {
        vfs_unlink(path);
        return;
    }

    uint8_t buf[5008];
    int64_t r = vfs_read(fd, buf, sizeof(buf));
    // File size is 5004 (HEAD at 0..4, TAIL at 5000..5004).
    KTEST_EXPECT_EQ(r, 5004);
    if (r == 5004) {
        KTEST_EXPECT_EQ(buf[0], 'H');
        KTEST_EXPECT_EQ(buf[3], 'D');
        KTEST_EXPECT_EQ(buf[5000], 'T');
        KTEST_EXPECT_EQ(buf[5003], 'L');
        for (size_t i = 4; i < 5000; i++) {
            if (buf[i] != 0) {
                KTEST_EXPECT_EQ(buf[i], 0);
                break;
            }
        }
    }

    vfs_close(fd);
    KTEST_EXPECT_EQ(vfs_unlink(path), 0);
}
