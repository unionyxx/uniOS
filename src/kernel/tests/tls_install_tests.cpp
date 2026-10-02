#include <kernel/cpu.h>
#include <kernel/ktest.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/syscall.h>
#include <kernel/tls.h>
#include <uapi/syscalls.h>
#include <uapi/tcb.h>

namespace {

// Unmap a range installed by tls_install in a test and clear the thread
// pointer, so later ktests start from a clean pid-0 task.
void tls_test_teardown(Process *p, uint64_t start, uint64_t map_len)
{
    SyscallFrame frame = {};
    (void)syscall_handler(SYS_MUNMAP, start, map_len, 0, &frame);
    p->fs_base = 0;
}

const uint8_t *tls_direct_read(Process *p, uint64_t va)
{
    uint64_t *pml4 = p->page_table ? p->page_table : vmm_get_kernel_pml4();
    const uint64_t phys = vmm_virt_to_phys_in(pml4, va);
    if (phys == 0)
        return nullptr;
    return reinterpret_cast<const uint8_t *>(vmm_phys_to_virt(phys));
}

} // namespace

KTEST(tls_install_block_and_tcb)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);
    if (!p)
        return;

    static uint8_t image[0x100];
    for (uint32_t i = 0; i < sizeof(image); i++)
        image[i] = static_cast<uint8_t>(i * 7 + 3);

    const uint64_t start = tls_install(p, image, sizeof(image), 16);
    KTEST_EXPECT(start != 0);
    if (start == 0)
        return;

    // 0x100 bytes at align 16: the block ends exactly at the thread
    // pointer, which sits 0x100 above the mapping start.
    const uint64_t fs = p->fs_base;
    KTEST_EXPECT_EQ(fs, start + 0x100);
    KTEST_EXPECT_EQ(fs % 16, 0ULL);

    const UniTcb *tcb = reinterpret_cast<const UniTcb *>(tls_direct_read(p, fs));
    KTEST_EXPECT(tcb != nullptr);
    if (tcb) {
        KTEST_EXPECT_EQ(tcb->self, fs);
        KTEST_EXPECT_EQ(tcb->tid, p->pid);
    }

    // The block holds the template image, byte for byte.
    const uint8_t *block = tls_direct_read(p, fs - sizeof(image));
    KTEST_EXPECT(block != nullptr);
    if (block) {
        bool match = true;
        for (uint32_t i = 0; i < sizeof(image); i++) {
            if (block[i] != image[i]) {
                match = false;
                break;
            }
        }
        KTEST_EXPECT(match);
    }

    tls_test_teardown(p, start, 4096);
}

KTEST(tls_install_tcb_only)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);
    if (!p)
        return;

    const uint64_t start = tls_install(p, nullptr, 0, 0);
    KTEST_EXPECT(start != 0);
    if (start == 0)
        return;

    // No TLS content: padded size 0, the thread pointer is the mapping
    // start and the TCB is the only payload.
    KTEST_EXPECT_EQ(p->fs_base, start);

    const UniTcb *tcb = reinterpret_cast<const UniTcb *>(tls_direct_read(p, start));
    KTEST_EXPECT(tcb != nullptr);
    if (tcb) {
        KTEST_EXPECT_EQ(tcb->self, start);
        KTEST_EXPECT_EQ(tcb->tid, p->pid);
    }

    tls_test_teardown(p, start, 4096);
}

KTEST(tls_install_zero_fill)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);
    if (!p)
        return;

    const uint64_t start = tls_install(p, nullptr, 64, 16);
    KTEST_EXPECT(start != 0);
    if (start == 0)
        return;

    const uint64_t fs = p->fs_base;
    KTEST_EXPECT_EQ(fs, start + 64);

    const uint8_t *block = tls_direct_read(p, fs - 64);
    KTEST_EXPECT(block != nullptr);
    if (block) {
        bool zeroed = true;
        for (uint32_t i = 0; i < 64; i++) {
            if (block[i] != 0) {
                zeroed = false;
                break;
            }
        }
        KTEST_EXPECT(zeroed);
    }

    tls_test_teardown(p, start, 4096);
}

KTEST(tls_cpu_fs_base_roundtrip)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);
    if (!p)
        return;

    const uint64_t start = tls_install(p, nullptr, 0, 0);
    KTEST_EXPECT(start != 0);
    if (start == 0)
        return;

    cpu_set_user_fs_base(p->fs_base);

    // fs:0 must now load the TCB self pointer we installed.
    uint64_t got = 0;
    asm volatile("movq %%fs:0x0, %0" : "=r"(got));
    KTEST_EXPECT_EQ(got, p->fs_base);

    cpu_set_user_fs_base(0);
    tls_test_teardown(p, start, 4096);
}
