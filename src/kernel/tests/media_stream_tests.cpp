#include <drivers/sound/sound.h>
#include <kernel/fs/vfs.h>
#include <kernel/ktest.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <libk/kstring.h>
#include <uapi/fs.h>
#include <uapi/sound.h>
#include <uapi/syscalls.h>

extern "C" int64_t sys_memfd_create(const char *name, unsigned int flags);
extern "C" int64_t sys_lseek(int fd, int64_t offset, int whence);

KTEST(media_lseek_syscall)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);
    if (!p->page_table)
        p->page_table = vmm_get_kernel_pml4();

    // Error paths that never need an open fd.
    KTEST_EXPECT_EQ(sys_lseek(3, 0, 42), -22);       // -EINVAL: bad whence
    KTEST_EXPECT_EQ(sys_lseek(-1, 0, SEEK_SET), -9); // -EBADF
    KTEST_EXPECT_EQ(sys_lseek(9999, 0, SEEK_CUR), -9);

    int64_t fd = sys_memfd_create(nullptr, 0);
    KTEST_EXPECT(fd >= 3);

    const char *data = "abcdefgh";
    KTEST_EXPECT_EQ(vfs_write(static_cast<int>(fd), data, 8), 8);

    SyscallFrame frame = {};
    KTEST_EXPECT_EQ(static_cast<int64_t>(syscall_handler(SYS_LSEEK, fd, 0, SEEK_SET, &frame)), 0);
    char buf[16] = {};
    KTEST_EXPECT_EQ(vfs_read(static_cast<int>(fd), buf, 4), 4);
    KTEST_EXPECT(kstring::strncmp(buf, "abcd", 4) == 0);

    KTEST_EXPECT_EQ(static_cast<int64_t>(syscall_handler(SYS_LSEEK, fd, 2, SEEK_CUR, &frame)), 6);
    kstring::zero_memory(buf, sizeof(buf));
    KTEST_EXPECT_EQ(vfs_read(static_cast<int>(fd), buf, 2), 2);
    KTEST_EXPECT(kstring::strncmp(buf, "gh", 2) == 0);

    KTEST_EXPECT_EQ(static_cast<int64_t>(syscall_handler(SYS_LSEEK, fd, -3, SEEK_END, &frame)), 5);
    kstring::zero_memory(buf, sizeof(buf));
    KTEST_EXPECT_EQ(vfs_read(static_cast<int>(fd), buf, 3), 3);
    KTEST_EXPECT(kstring::strncmp(buf, "fgh", 3) == 0);

    // Resulting offsets before the start of the file are rejected.
    KTEST_EXPECT_EQ(sys_lseek(static_cast<int>(fd), -100, SEEK_SET), -22);

    vfs_close(static_cast<int>(fd));
}

KTEST(media_sound_stream_api)
{
    // Invalid stream formats are rejected regardless of card presence.
    KTEST_EXPECT(!sound_stream_open(44100, 3, 16)); // bad channel count
    KTEST_EXPECT(!sound_stream_open(44100, 2, 8));  // bad bit depth
    KTEST_EXPECT(!sound_stream_open(0, 2, 16));     // bad sample rate

    // Writing without an open stream fails with -EPIPE.
    uint8_t pcm[64] = {};
    KTEST_EXPECT_EQ(sound_stream_write(pcm, sizeof(pcm)), -32);

    // With a card present (not guaranteed in every test VM), exercise the
    // open/write/status/stop lifecycle without ever starting DMA: the ring
    // stays far below the 1 MiB start threshold.
    if (sound_stream_open(44100, 2, 16)) {
        KTEST_EXPECT(sound_stream_active());
        KTEST_EXPECT_EQ(sound_stream_write(pcm, 3), -22); // partial sample frame
        KTEST_EXPECT_EQ(sound_stream_write(pcm, 64), 64);

        sound_status st = {};
        KTEST_EXPECT(sound_stream_status(&st));
        KTEST_EXPECT_EQ(st.active, 1);
        KTEST_EXPECT_EQ(st.channels, 2);
        KTEST_EXPECT_EQ(st.sample_rate, 44100u);
        KTEST_EXPECT_EQ(st.queued_bytes, 64u);

        sound_stream_stop();
        KTEST_EXPECT(!sound_stream_active());
        KTEST_EXPECT_EQ(sound_stream_write(pcm, 64), -32);
    }
}
