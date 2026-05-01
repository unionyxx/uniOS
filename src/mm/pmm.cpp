#include <boot/boot_info.h>
#include <kernel/debug.h>
#include <kernel/mm/bitmap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>
#include <kernel/sync/spinlock.h>
#include <libk/kstring.h>

static Spinlock g_pmm_lock = SPINLOCK_INIT;

static uint8_t *g_pmm_bitmap_buffer = nullptr;
static Bitmap g_pmm_bitmap;
static uint16_t *g_pmm_refcounts = nullptr;
static size_t g_bitmap_bits = 0;

static uint64_t g_total_memory = 0;
static uint64_t g_free_memory = 0;
static uint64_t g_highest_page = 0;

static constexpr uint64_t k_frame_size = 4096;

static void reserve_frame_permanently(size_t frame_idx)
{
    if (frame_idx >= g_bitmap_bits)
        return;

    const bool was_free = !g_pmm_bitmap[frame_idx];
    g_pmm_bitmap.set(frame_idx, true);
    if (g_pmm_refcounts[frame_idx] == 0)
        g_pmm_refcounts[frame_idx] = 1;

    if (was_free && g_free_memory >= k_frame_size)
        g_free_memory -= k_frame_size;
}

[[nodiscard]] static bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    return __builtin_add_overflow(a, b, out);
}

[[nodiscard]] static bool get_aligned_usable_range(const BootMemoryMapEntry *entry, uint64_t *base_out,
                                                   uint64_t *length_out)
{
    if (!entry || entry->length == 0)
        return false;

    uint64_t end = 0;
    if (add_overflow_u64(entry->base, entry->length, &end))
        return false;

    uint64_t base = entry->base;
    if (base & 0xFFFULL) {
        if (add_overflow_u64(base, 4095ULL, &base))
            return false;
        base &= ~4095ULL;
    }

    uint64_t aligned_end = end & ~4095ULL;
    if (base >= aligned_end)
        return false;

    *base_out = base;
    *length_out = aligned_end - base;
    return true;
}

void pmm_init()
{
    const BootInfo *boot_info = boot_get_info();
    if (!boot_info || !boot_info->memory_map || boot_info->memory_map_count == 0)
        return;

    g_total_memory = 0;
    g_free_memory = 0;
    g_highest_page = 0;

    uint64_t highest_phys_addr = 0;

    for (uint64_t i = 0; i < boot_info->memory_map_count; i++) {
        BootMemoryMapEntry *entry = &boot_info->memory_map[i];
        // Only consider usable/reclaimable memory for bitmap sizing.
        // UEFI may have MMIO regions at extremely high physical addresses (e.g., >4TiB)
        // that we don't need to track in the PMM bitmap.
        if (entry->type == BOOT_MEM_USABLE || entry->type == BOOT_MEM_BOOTLOADER_RECLAIMABLE) {
            uint64_t end = 0;
            if (add_overflow_u64(entry->base, entry->length, &end))
                continue;
            if (end > highest_phys_addr)
                highest_phys_addr = end;
        }
    }

    if (highest_phys_addr > UINT64_MAX - 4095ULL)
        panic("pmm: physical memory range is too large");

    uint64_t max_frames = (highest_phys_addr + 4095ULL) / 4096ULL;
    if (max_frames > (uint64_t)SIZE_MAX)
        panic("pmm: PMM bitmap exceeds addressable size");
    g_bitmap_bits = (size_t)max_frames;

    uint64_t bitmap_size_bytes = (max_frames + 7) / 8;
    if (bitmap_size_bytes > UINT64_MAX - 4095ULL)
        panic("pmm: bitmap size overflow");
    bitmap_size_bytes = (bitmap_size_bytes + 4095) & ~4095ULL;

    uint64_t refcount_raw_bytes = 0;
    if (__builtin_mul_overflow(max_frames, (uint64_t)sizeof(uint16_t), &refcount_raw_bytes) ||
        refcount_raw_bytes > UINT64_MAX - 4095ULL) {
        panic("pmm: refcount size overflow");
    }
    uint64_t refcounts_size_bytes = (refcount_raw_bytes + 4095) & ~4095ULL;

    uint64_t bitmap_phys_addr = 0;
    uint64_t refcounts_phys_addr = 0;
    bool bitmap_phys_found = false;
    bool refcounts_phys_found = false;

    for (uint64_t i = 0; i < boot_info->memory_map_count; i++) {
        BootMemoryMapEntry *entry = &boot_info->memory_map[i];
        if (entry->type == BOOT_MEM_USABLE) {
            uint64_t base = 0;
            uint64_t length = 0;
            if (!get_aligned_usable_range(entry, &base, &length))
                continue;

            if (!bitmap_phys_found && length >= bitmap_size_bytes) {
                bitmap_phys_addr = base;
                bitmap_phys_found = true;
                uint64_t metadata_size = 0;
                if (!refcounts_phys_found &&
                    !__builtin_add_overflow(bitmap_size_bytes, refcounts_size_bytes, &metadata_size) &&
                    length >= metadata_size) {
                    refcounts_phys_addr = base + bitmap_size_bytes;
                    refcounts_phys_found = true;
                }
            } else if (!refcounts_phys_found && length >= refcounts_size_bytes) {
                refcounts_phys_addr = base;
                refcounts_phys_found = true;
            }

            if (bitmap_phys_found && refcounts_phys_found)
                break;
        }
    }

    if (!bitmap_phys_found || !refcounts_phys_found) {
        panic("pmm: failed to find memory for bitmap or refcounts");
    }

    g_pmm_bitmap_buffer = reinterpret_cast<uint8_t *>(bitmap_phys_addr + vmm_get_hhdm_offset());
    g_pmm_bitmap.init(g_pmm_bitmap_buffer, g_bitmap_bits);

    g_pmm_refcounts = reinterpret_cast<uint16_t *>(refcounts_phys_addr + vmm_get_hhdm_offset());
    for (size_t i = 0; i < g_bitmap_bits; i++)
        g_pmm_refcounts[i] = 0;

    g_pmm_bitmap.set_range(0, g_bitmap_bits, true);

    uint64_t usable_memory = 0;
    uint64_t reserved_memory = 0;

    for (uint64_t i = 0; i < boot_info->memory_map_count; i++) {
        BootMemoryMapEntry *entry = &boot_info->memory_map[i];

        if (entry->type == BOOT_MEM_USABLE) {
            usable_memory += entry->length;

            uint64_t base = 0;
            uint64_t length = 0;
            if (!get_aligned_usable_range(entry, &base, &length))
                continue;

            for (uint64_t j = 0; j < length; j += k_frame_size) {
                uint64_t addr = base + j;
                uint64_t frame_idx = addr / k_frame_size;

                if (frame_idx < g_bitmap_bits) {
                    g_pmm_bitmap.set(frame_idx, false);
                    g_free_memory += k_frame_size;
                    g_total_memory += k_frame_size;
                    if (frame_idx > g_highest_page)
                        g_highest_page = frame_idx;
                }
            }
        } else {
            reserved_memory += entry->length;
        }
    }

    DEBUG_INFO("Memory Map: %d entries, Usable: %lu MB, Reserved: %lu MB", boot_info->memory_map_count,
               usable_memory / 1024 / 1024, reserved_memory / 1024 / 1024);

    uint64_t bitmap_start_frame = bitmap_phys_addr / k_frame_size;
    uint64_t bitmap_frame_count = bitmap_size_bytes / k_frame_size;
    g_pmm_bitmap.set_range(bitmap_start_frame, bitmap_frame_count, true);
    for (size_t i = 0; i < bitmap_frame_count; i++)
        g_pmm_refcounts[bitmap_start_frame + i] = 1;
    g_free_memory -= bitmap_size_bytes;

    uint64_t refcounts_start_frame = refcounts_phys_addr / k_frame_size;
    uint64_t refcounts_frame_count = refcounts_size_bytes / k_frame_size;
    g_pmm_bitmap.set_range(refcounts_start_frame, refcounts_frame_count, true);
    for (size_t i = 0; i < refcounts_frame_count; i++)
        g_pmm_refcounts[refcounts_start_frame + i] = 1;
    g_free_memory -= refcounts_size_bytes;

    // Physical frame 0 aliases nullptr in the PMM API, so never hand it out.
    reserve_frame_permanently(0);
    if (g_bitmap_bits > 1 && g_pmm_bitmap.get_hint() == 0)
        g_pmm_bitmap.update_hint(1);

    DEBUG_SUCCESS("Physical Memory Manager Initialized");
}

void *pmm_alloc_frame()
{
    spinlock_acquire(&g_pmm_lock);

    size_t frame_idx = g_pmm_bitmap.find_first_free();
    if (frame_idx == 0) {
        reserve_frame_permanently(0);
        frame_idx = g_pmm_bitmap.find_first_free(1);
    }
    if (frame_idx != static_cast<size_t>(-1) && frame_idx < g_bitmap_bits) {
        g_pmm_bitmap.set(frame_idx, true);
        g_pmm_refcounts[frame_idx] = 1;
        g_free_memory -= k_frame_size;
        spinlock_release(&g_pmm_lock);

        void *phys_ptr = reinterpret_cast<void *>(frame_idx * k_frame_size);
        void *virt_ptr = reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(phys_ptr)));
        kstring::zero_memory(virt_ptr, k_frame_size);
        return phys_ptr;
    }

    spinlock_release(&g_pmm_lock);
    return nullptr;
}

void *pmm_alloc_frames(size_t count)
{
    if (count == 0 || count > ((uint64_t)SIZE_MAX / k_frame_size))
        return nullptr;
    if (count == 1)
        return pmm_alloc_frame();

    spinlock_acquire(&g_pmm_lock);

    // Allocate contiguous frames from the TOP of memory to avoid fragmentation with single pages
    size_t frame_idx = g_pmm_bitmap.find_last_free_sequence(count);

    if (frame_idx != static_cast<size_t>(-1) && count <= g_bitmap_bits - frame_idx &&
        (uint64_t)frame_idx + (uint64_t)count - 1 <= g_highest_page) {
        g_pmm_bitmap.set_range(frame_idx, count, true);
        for (size_t i = 0; i < count; i++)
            g_pmm_refcounts[frame_idx + i] = 1;
        g_free_memory -= (k_frame_size * (uint64_t)count);

        // Update hint IF we just allocated at the bottom part (unlikely with find_last)
        if (frame_idx <= g_pmm_bitmap.get_hint() && (frame_idx + count) > g_pmm_bitmap.get_hint()) {
            g_pmm_bitmap.update_hint(frame_idx + count);
        }

        spinlock_release(&g_pmm_lock);

        void *phys_ptr = reinterpret_cast<void *>(frame_idx * k_frame_size);
        // The caller is responsible for zeroing if needed, as this is a physical allocator.
        // void* virt_ptr = reinterpret_cast<void*>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(phys_ptr)));
        // kstring::zero_memory(virt_ptr, 4096 * count);
        return phys_ptr;
    }

    spinlock_release(&g_pmm_lock);
    DEBUG_ERROR("pmm: failed to allocate %llu contiguous frames (Fragmentation?)", (unsigned long long)count);
    return nullptr;
}

void pmm_refcount_inc(void *frame)
{
    if (!frame)
        return;

    spinlock_acquire(&g_pmm_lock);
    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    if (frame_idx < g_bitmap_bits) {
        if (g_pmm_refcounts[frame_idx] == UINT16_MAX) {
            spinlock_release(&g_pmm_lock);
            panic("pmm: refcount overflow");
        }
        g_pmm_refcounts[frame_idx]++;
    }
    spinlock_release(&g_pmm_lock);
}

void pmm_refcount_dec(void *frame)
{
    if (!frame)
        return;

    spinlock_acquire(&g_pmm_lock);
    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    if (frame_idx >= g_bitmap_bits) {
        spinlock_release(&g_pmm_lock);
        return;
    }

    if (g_pmm_refcounts[frame_idx] == 0) {
        DEBUG_WARN("pmm: double free or stale refcount for frame %p", frame);
        spinlock_release(&g_pmm_lock);
        return;
    }

    g_pmm_refcounts[frame_idx]--;
    if (g_pmm_refcounts[frame_idx] == 0) {
        g_pmm_bitmap.set(frame_idx, false);
        g_pmm_bitmap.update_hint(frame_idx);
        g_free_memory += k_frame_size;
    }
    spinlock_release(&g_pmm_lock);
}

uint16_t pmm_get_refcount(void *frame)
{
    if (!frame)
        return 0;

    spinlock_acquire(&g_pmm_lock);
    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    uint16_t count = (frame_idx < g_bitmap_bits) ? g_pmm_refcounts[frame_idx] : 0;
    spinlock_release(&g_pmm_lock);
    return count;
}

void pmm_free_frame(void *frame)
{
    pmm_refcount_dec(frame);
}

uint64_t pmm_get_free_memory()
{
    return g_free_memory;
}

uint64_t pmm_get_total_memory()
{
    return g_total_memory;
}

bool pmm_is_managed(void *frame)
{
    if (!frame)
        return false;

    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    return (frame_idx < g_bitmap_bits);
}
