#include <kernel/fs/memfd.h>
#include <kernel/fs/vfs.h>
#include <kernel/sync/spinlock.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <libk/kstring.h>

constexpr size_t MEMFD_MAX_PAGES = 4096; // 16MB limit

struct MemFd
{
    char name[64];
    Spinlock lock;
    void *pages[MEMFD_MAX_PAGES];
    size_t page_count;
    size_t size;
};

static int64_t memfd_vfs_read(VNode *node, void *buf, uint64_t size, uint64_t offset, FileDescriptor *)
{
    if (!node || !node->fs_data || !buf)
        return -1;

    MemFd *mfd = static_cast<MemFd *>(node->fs_data);
    uint64_t flags = spinlock_acquire_irqsave(&mfd->lock);

    if (offset >= mfd->size) {
        spinlock_release_irqrestore(&mfd->lock, flags);
        return 0;
    }

    uint64_t to_read = size;
    if (offset + to_read > mfd->size) {
        to_read = mfd->size - offset;
    }

    uint64_t read_bytes = 0;
    while (read_bytes < to_read) {
        uint64_t curr_offset = offset + read_bytes;
        uint64_t page_idx = curr_offset / 4096;
        uint64_t page_off = curr_offset % 4096;
        uint64_t chunk = 4096 - page_off;
        if (chunk > to_read - read_bytes) {
            chunk = to_read - read_bytes;
        }

        if (page_idx >= mfd->page_count || !mfd->pages[page_idx]) {
            kstring::zero_memory(static_cast<char *>(buf) + read_bytes, chunk);
        } else {
            uint64_t page_phys = reinterpret_cast<uint64_t>(mfd->pages[page_idx]);
            uint64_t page_virt = vmm_phys_to_virt(page_phys);
            kstring::memcpy(static_cast<char *>(buf) + read_bytes, reinterpret_cast<const void *>(page_virt + page_off), chunk);
        }
        read_bytes += chunk;
    }

    spinlock_release_irqrestore(&mfd->lock, flags);
    return static_cast<int64_t>(read_bytes);
}

static int64_t memfd_vfs_write(VNode *node, const void *buf, uint64_t size, uint64_t offset, FileDescriptor *)
{
    if (!node || !node->fs_data || !buf)
        return -1;

    MemFd *mfd = static_cast<MemFd *>(node->fs_data);
    uint64_t flags = spinlock_acquire_irqsave(&mfd->lock);

    uint64_t end_offset = offset + size;
    if (end_offset > MEMFD_MAX_PAGES * 4096) {
        spinlock_release_irqrestore(&mfd->lock, flags);
        return -1;
    }

    uint64_t needed_pages = (end_offset + 4095) / 4096;
    while (mfd->page_count < needed_pages) {
        void *new_frame = pmm_alloc_frame();
        if (!new_frame) {
            spinlock_release_irqrestore(&mfd->lock, flags);
            return -1;
        }
        kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(new_frame))), 4096);
        mfd->pages[mfd->page_count++] = new_frame;
    }

    uint64_t written_bytes = 0;
    while (written_bytes < size) {
        uint64_t curr_offset = offset + written_bytes;
        uint64_t page_idx = curr_offset / 4096;
        uint64_t page_off = curr_offset % 4096;
        uint64_t chunk = 4096 - page_off;
        if (chunk > size - written_bytes) {
            chunk = size - written_bytes;
        }

        uint64_t page_phys = reinterpret_cast<uint64_t>(mfd->pages[page_idx]);
        uint64_t page_virt = vmm_phys_to_virt(page_phys);
        kstring::memcpy(reinterpret_cast<void *>(page_virt + page_off), static_cast<const char *>(buf) + written_bytes, chunk);

        written_bytes += chunk;
    }

    if (end_offset > mfd->size) {
        mfd->size = end_offset;
        node->size = end_offset;
    }

    spinlock_release_irqrestore(&mfd->lock, flags);
    return static_cast<int64_t>(written_bytes);
}

static int memfd_vfs_truncate(VNode *node, uint64_t size)
{
    if (!node || !node->fs_data)
        return -1;

    MemFd *mfd = static_cast<MemFd *>(node->fs_data);
    uint64_t flags = spinlock_acquire_irqsave(&mfd->lock);

    if (size > MEMFD_MAX_PAGES * 4096) {
        spinlock_release_irqrestore(&mfd->lock, flags);
        return -1;
    }

    uint64_t needed_pages = (size + 4095) / 4096;

    if (needed_pages > mfd->page_count) {
        while (mfd->page_count < needed_pages) {
            void *new_frame = pmm_alloc_frame();
            if (!new_frame) {
                spinlock_release_irqrestore(&mfd->lock, flags);
                return -1;
            }
            kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(new_frame))), 4096);
            mfd->pages[mfd->page_count++] = new_frame;
        }
    } else if (needed_pages < mfd->page_count) {
        while (mfd->page_count > needed_pages) {
            void *frame = mfd->pages[--mfd->page_count];
            if (frame) {
                pmm_free_frame(frame);
                mfd->pages[mfd->page_count] = nullptr;
            }
        }
    }

    mfd->size = size;
    node->size = size;

    spinlock_release_irqrestore(&mfd->lock, flags);
    return 0;
}

static void memfd_vfs_close(VNode *node)
{
    if (!node || !node->fs_data)
        return;

    MemFd *mfd = static_cast<MemFd *>(node->fs_data);
    for (size_t i = 0; i < mfd->page_count; i++) {
        if (mfd->pages[i]) {
            pmm_free_frame(mfd->pages[i]);
            mfd->pages[i] = nullptr;
        }
    }

    free(mfd);
    node->fs_data = nullptr;
}

VNodeOps memfd_ops = {
    .read = memfd_vfs_read,
    .write = memfd_vfs_write,
    .readdir = nullptr,
    .lookup = nullptr,
    .create = nullptr,
    .mkdir = nullptr,
    .unlink = nullptr,
    .rename = nullptr,
    .truncate = memfd_vfs_truncate,
    .sync = nullptr,
    .close = memfd_vfs_close
};

VNode *memfd_create_vnode(const char *name)
{
    MemFd *mfd = static_cast<MemFd *>(malloc(sizeof(MemFd)));
    if (!mfd)
        return nullptr;

    kstring::zero_memory(mfd, sizeof(MemFd));
    kstring::strncpy(mfd->name, name ? name : "anon", 63);
    mfd->name[63] = '\0';
    mfd->lock = SPINLOCK_INIT;

    VNode *node = vfs_create_vnode(0, 0, false, &memfd_ops, mfd);
    if (!node) {
        free(mfd);
        return nullptr;
    }

    return node;
}

bool is_memfd_vnode(VNode *node)
{
    return node && node->ops == &memfd_ops;
}

void *memfd_get_page(struct VNode *node, size_t page_idx)
{
    if (!is_memfd_vnode(node))
        return nullptr;

    MemFd *mfd = static_cast<MemFd *>(node->fs_data);
    uint64_t flags = spinlock_acquire_irqsave(&mfd->lock);

    if (page_idx >= MEMFD_MAX_PAGES) {
        spinlock_release_irqrestore(&mfd->lock, flags);
        return nullptr;
    }

    // Allocate page if not present
    while (mfd->page_count <= page_idx) {
        void *new_frame = pmm_alloc_frame();
        if (!new_frame) {
            spinlock_release_irqrestore(&mfd->lock, flags);
            return nullptr;
        }
        kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(new_frame))), 4096);
        mfd->pages[mfd->page_count++] = new_frame;
    }

    void *frame = mfd->pages[page_idx];
    if (!frame) {
        frame = pmm_alloc_frame();
        if (frame) {
            kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(frame))), 4096);
            mfd->pages[page_idx] = frame;
        }
    }

    spinlock_release_irqrestore(&mfd->lock, flags);
    return frame;
}
