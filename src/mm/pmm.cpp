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
#ifdef DEBUG
// Frame ownership ledger: the allocation site (return address) currently
// holding each frame, 0 when free. Catches double-allocation at the moment a
// still-owned frame is handed out again.
static uint64_t *g_pmm_owners = nullptr;
#endif

// Zone descriptors: a frame-range classification over the whole bitmap.
// ZONE_DMA32 = frames below 4 GiB (the only memory 32-bit-only DMA devices
// can reach), ZONE_NORMAL = the rest. Multiple USABLE boot regions can each
// contribute parts of both zones; reserved frames inside a range stay marked
// in-use in the bitmap and are simply never allocated. Phase 4 extends these
// with per-zone locks and per-CPU magazines.
PmmZone g_pmm_zone_dma32 = {0, 0, "ZONE_DMA32"};
PmmZone g_pmm_zone_normal = {0, 0, "ZONE_NORMAL"};

// Bottom-up first-fit cursors, one per zone. Bits below a zone's cursor are
// (mostly) in-use; a freed frame lowers its zone's cursor so the hole is the
// first candidate of the next scan in that zone.
static size_t g_dma32_cursor = 1; // frame 0 is permanently reserved
static size_t g_normal_cursor = 0;

static uint64_t g_total_memory = 0;
static uint64_t g_free_memory = 0;
static uint64_t g_highest_page = 0;

static constexpr uint64_t k_frame_size = 4096;
static constexpr uint64_t k_dma32_limit_frame = 0x100000; // 4 GiB / 4096
static constexpr size_t k_not_found = static_cast<size_t>(-1);

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

[[nodiscard]] static bool get_aligned_usable_range(const BootMemoryMapEntry *entry, uint64_t *base_out,
                                                   uint64_t *length_out)
{
    if (!entry || entry->length == 0)
        return false;

    uint64_t end;
    if (__builtin_add_overflow(entry->base, entry->length, &end))
        return false;

    uint64_t base = (entry->base + 0xFFFULL) & ~0xFFFULL;
    uint64_t aligned_end = end & ~0xFFFULL;

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
        const BootMemoryMapEntry *entry = &boot_info->memory_map[i];
        if (entry->type == BOOT_MEM_USABLE || entry->type == BOOT_MEM_BOOTLOADER_RECLAIMABLE) {
            uint64_t end;
            if (__builtin_add_overflow(entry->base, entry->length, &end))
                continue;
            if (end > highest_phys_addr)
                highest_phys_addr = end;
        }
    }

    if (highest_phys_addr > UINT64_MAX - 4095ULL)
        panic("pmm: physical memory range is too large");

    uint64_t max_frames = (highest_phys_addr + 4095ULL) / 4096ULL;
    if (max_frames > static_cast<uint64_t>(SIZE_MAX))
        panic("pmm: PMM bitmap exceeds addressable size");
    g_bitmap_bits = static_cast<size_t>(max_frames);

    uint64_t bitmap_size_bytes = (max_frames + 7) / 8;
    if (bitmap_size_bytes > UINT64_MAX - 4095ULL)
        panic("pmm: bitmap size overflow");
    bitmap_size_bytes = (bitmap_size_bytes + 4095) & ~4095ULL;

    uint64_t refcount_raw_bytes;
    if (__builtin_mul_overflow(max_frames, sizeof(uint16_t), &refcount_raw_bytes) ||
        refcount_raw_bytes > UINT64_MAX - 4095ULL) {
        panic("pmm: refcount size overflow");
    }
    uint64_t refcounts_size_bytes = (refcount_raw_bytes + 4095) & ~4095ULL;

#ifdef DEBUG
    uint64_t owners_raw_bytes;
    if (__builtin_mul_overflow(max_frames, sizeof(uint64_t), &owners_raw_bytes) ||
        owners_raw_bytes > UINT64_MAX - 4095ULL) {
        panic("pmm: owner ledger size overflow");
    }
    uint64_t owners_size_bytes = (owners_raw_bytes + 4095) & ~4095ULL;
#else
    uint64_t owners_size_bytes = 0;
#endif

    uint64_t bitmap_phys_addr = 0;
    uint64_t refcounts_phys_addr = 0;
    uint64_t owners_phys_addr = 0;
    bool bitmap_phys_found = false;
    bool refcounts_phys_found = false;

    for (uint64_t i = 0; i < boot_info->memory_map_count; i++) {
        const BootMemoryMapEntry *entry = &boot_info->memory_map[i];
        if (entry->type == BOOT_MEM_USABLE) {
            uint64_t base, length;
            if (!get_aligned_usable_range(entry, &base, &length))
                continue;

            if (!bitmap_phys_found && length >= bitmap_size_bytes) {
                bitmap_phys_addr = base;
                bitmap_phys_found = true;
                uint64_t metadata_size;
                if (!refcounts_phys_found &&
                    !__builtin_add_overflow(bitmap_size_bytes, refcounts_size_bytes + owners_size_bytes,
                                            &metadata_size) &&
                    length >= metadata_size) {
                    refcounts_phys_addr = base + bitmap_size_bytes;
                    refcounts_phys_found = true;
                    owners_phys_addr = base + bitmap_size_bytes + refcounts_size_bytes;
                }
            } else if (!refcounts_phys_found && length >= refcounts_size_bytes + owners_size_bytes) {
                refcounts_phys_addr = base;
                refcounts_phys_found = true;
                owners_phys_addr = base + refcounts_size_bytes;
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
    kstring::zero_memory(g_pmm_refcounts, g_bitmap_bits * sizeof(uint16_t));
#ifdef DEBUG
    g_pmm_owners = reinterpret_cast<uint64_t *>(owners_phys_addr + vmm_get_hhdm_offset());
    kstring::zero_memory(g_pmm_owners, g_bitmap_bits * sizeof(uint64_t));
#endif

    g_pmm_bitmap.set_range(0, g_bitmap_bits, true);

    for (uint64_t i = 0; i < boot_info->memory_map_count; i++) {
        const BootMemoryMapEntry *entry = &boot_info->memory_map[i];

        if (entry->type == BOOT_MEM_USABLE) {
            uint64_t base, length;
            if (!get_aligned_usable_range(entry, &base, &length))
                continue;

            uint64_t start_frame = base / k_frame_size;
            uint64_t frames = length / k_frame_size;

            if (start_frame < g_bitmap_bits) {
                if (start_frame + frames > g_bitmap_bits)
                    frames = g_bitmap_bits - start_frame;

                g_pmm_bitmap.set_range(start_frame, frames, false);
                g_free_memory += frames * k_frame_size;
                g_total_memory += frames * k_frame_size;

                if (start_frame + frames - 1 > g_highest_page) {
                    g_highest_page = start_frame + frames - 1;
                }
            }
        }
    }

    // Derive the zones from the same walk: every USABLE region contributes its
    // below-4-GiB frames to ZONE_DMA32 and the rest to ZONE_NORMAL, so a zone
    // is a range over the whole bitmap rather than a per-region structure.
    const uint64_t dma32_frames = (static_cast<uint64_t>(g_bitmap_bits) < k_dma32_limit_frame)
                                      ? static_cast<uint64_t>(g_bitmap_bits)
                                      : k_dma32_limit_frame;
    g_pmm_zone_dma32 = PmmZone{0, dma32_frames, "ZONE_DMA32"};
    g_pmm_zone_normal =
        PmmZone{k_dma32_limit_frame, static_cast<uint64_t>(g_bitmap_bits) - dma32_frames, "ZONE_NORMAL"};
    g_dma32_cursor = 1;
    g_normal_cursor = static_cast<size_t>(g_pmm_zone_normal.base_frame);
    DEBUG_INFO("pmm: %s frames [0x%lx, 0x%lx), %s frames [0x%lx, 0x%lx)", g_pmm_zone_dma32.name,
               g_pmm_zone_dma32.base_frame, g_pmm_zone_dma32.base_frame + g_pmm_zone_dma32.frame_count,
               g_pmm_zone_normal.name, g_pmm_zone_normal.base_frame,
               g_pmm_zone_normal.base_frame + g_pmm_zone_normal.frame_count);

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

#ifdef DEBUG
    uint64_t owners_start_frame = owners_phys_addr / k_frame_size;
    uint64_t owners_frame_count = owners_size_bytes / k_frame_size;
    g_pmm_bitmap.set_range(owners_start_frame, owners_frame_count, true);
    for (size_t i = 0; i < owners_frame_count; i++)
        g_pmm_refcounts[owners_start_frame + i] = 1;
    g_free_memory -= owners_size_bytes;
#endif

    reserve_frame_permanently(0);
}

// Bottom-up first-fit scan inside one zone, honoring (and advancing) the
// zone's cursor with a zone-local wraparound. Returns k_not_found when the
// zone holds no free frame. g_pmm_lock must be held.
[[nodiscard]] static size_t zone_scan_single(const PmmZone &zone, size_t &cursor)
{
    const size_t base = static_cast<size_t>(zone.base_frame);
    const size_t end = static_cast<size_t>(zone.base_frame + zone.frame_count);

    size_t start = cursor;
    if (start < base || start > end)
        start = base;

    size_t idx = g_pmm_bitmap.find_first_free(start, end);
    if (idx == k_not_found && start != base)
        idx = g_pmm_bitmap.find_first_free(base, start);
    if (idx == k_not_found)
        return k_not_found;

    if (idx == 0) {
        // Physical frame 0 is never handed out: a stray free of it must not
        // turn into an allocation of physical address zero.
        reserve_frame_permanently(0);
        idx = g_pmm_bitmap.find_first_free(1, end);
        if (idx == k_not_found)
            return k_not_found;
    }

    cursor = idx + 1;
    return idx;
}

// Claims one frame after a successful zone scan: owners ledger, bitmap bit,
// refcount, free counter, then lock release and zeroing outside the lock.
// g_pmm_lock must be held; it is released before returning.
[[nodiscard]] static void *commit_single_frame(size_t frame_idx, uint64_t irq_flags)
{
#ifdef DEBUG
    if (g_pmm_owners && g_pmm_owners[frame_idx] != 0) {
        uint64_t held_by = g_pmm_owners[frame_idx];
        spinlock_release_irqrestore(&g_pmm_lock, irq_flags);
        KLOG(LogModule::Mem, LogLevel::Fatal, "pmm: double-alloc of frame 0x%lx (held by %p, re-requested by %p)",
             frame_idx * k_frame_size, reinterpret_cast<void *>(held_by), __builtin_return_address(0));
        panic("pmm: double-alloc of frame (details in log)");
    }
    if (g_pmm_owners)
        g_pmm_owners[frame_idx] = reinterpret_cast<uint64_t>(__builtin_return_address(0));
#endif
    g_pmm_bitmap.set(frame_idx, true);
    g_pmm_refcounts[frame_idx] = 1;
    g_free_memory -= k_frame_size;
    spinlock_release_irqrestore(&g_pmm_lock, irq_flags);

    void *phys_ptr = reinterpret_cast<void *>(frame_idx * k_frame_size);
    void *virt_ptr = reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(phys_ptr)));
    kstring::zero_memory(virt_ptr, k_frame_size);
    return phys_ptr;
}

// Claims a contiguous run after a successful zone scan. g_pmm_lock must be
// held; it is released before returning. Zeroing happens outside the lock,
// matching commit_single_frame(): callers (kernel stacks, DMA buffers handed
// to devices) must never observe stale contents from a previous owner.
[[nodiscard]] static void *commit_frame_range(size_t frame_idx, size_t count, uint64_t irq_flags)
{
#ifdef DEBUG
    if (g_pmm_owners) {
        for (size_t i = 0; i < count; i++) {
            if (g_pmm_owners[frame_idx + i] != 0) {
                uint64_t held_by = g_pmm_owners[frame_idx + i];
                spinlock_release_irqrestore(&g_pmm_lock, irq_flags);
                KLOG(LogModule::Mem, LogLevel::Fatal,
                     "pmm: double-alloc of frame 0x%lx in range (held by %p, re-requested by %p)",
                     (frame_idx + i) * k_frame_size, reinterpret_cast<void *>(held_by), __builtin_return_address(0));
                panic("pmm: double-alloc of frame in range (details in log)");
            }
        }
        for (size_t i = 0; i < count; i++)
            g_pmm_owners[frame_idx + i] = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    }
#endif
    g_pmm_bitmap.set_range(frame_idx, count, true);
    for (size_t i = 0; i < count; i++) {
        g_pmm_refcounts[frame_idx + i] = 1;
    }
    g_free_memory -= (k_frame_size * count);

    spinlock_release_irqrestore(&g_pmm_lock, irq_flags);

    void *phys_ptr = reinterpret_cast<void *>(frame_idx * k_frame_size);
    void *virt_ptr = reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(phys_ptr)));
    kstring::zero_memory(virt_ptr, count * k_frame_size);
    return phys_ptr;
}

// Pushes a zone cursor past a freshly allocated range so the next bottom-up
// scan in that zone does not re-walk the frames the range just consumed.
static void advance_cursor_over(size_t &cursor, size_t frame_idx, size_t count)
{
    if (cursor >= frame_idx && cursor < frame_idx + count)
        cursor = frame_idx + count;
}

void *pmm_alloc_frame()
{
    uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);

    size_t frame_idx = zone_scan_single(g_pmm_zone_normal, g_normal_cursor);
    if (frame_idx == k_not_found) {
        // Order-0 only: a single frame may borrow from ZONE_DMA32 when NORMAL
        // is exhausted; contiguous allocation may not (pmm_alloc_frames).
        frame_idx = zone_scan_single(g_pmm_zone_dma32, g_dma32_cursor);
    }

    if (frame_idx == k_not_found) {
        spinlock_release_irqrestore(&g_pmm_lock, flags);
        return nullptr;
    }

    return commit_single_frame(frame_idx, flags);
}

void *pmm_alloc_frame_dma32()
{
    uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);

    size_t frame_idx = zone_scan_single(g_pmm_zone_dma32, g_dma32_cursor);

    if (frame_idx == k_not_found) {
        // ZONE_DMA32 is a scarce resource owned by 32-bit-only devices:
        // exhaustion is reported to the caller, never papered over with
        // memory the device cannot address.
        spinlock_release_irqrestore(&g_pmm_lock, flags);
        return nullptr;
    }

    return commit_single_frame(frame_idx, flags);
}

void *pmm_alloc_frames(size_t count)
{
    if (count == 0 || count > (static_cast<uint64_t>(SIZE_MAX) / k_frame_size))
        return nullptr;
    if (count == 1)
        return pmm_alloc_frame();

    uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);

    // Contiguous allocation is ZONE_NORMAL's job: while RAM above 4 GiB
    // exists, DMA32 is never a fallback for multi-frame kernel requests
    // (kernel stacks, heap growth) - it stays reserved for 32-bit DMA
    // devices. On a machine with no RAM above 4 GiB NORMAL is empty and
    // DMA32 is the entire zone set, so scanning it is not a fallback.
    const bool from_normal = g_pmm_zone_normal.frame_count != 0;
    const PmmZone &zone = from_normal ? g_pmm_zone_normal : g_pmm_zone_dma32;
    const size_t zone_base = static_cast<size_t>(zone.base_frame);
    const size_t zone_end = static_cast<size_t>(zone.base_frame + zone.frame_count);

    size_t frame_idx = g_pmm_bitmap.find_last_free_sequence(count, zone_base, zone_end);

    if (frame_idx == k_not_found || count > g_bitmap_bits - frame_idx ||
        (static_cast<uint64_t>(frame_idx) + count - 1) > g_highest_page) {
        spinlock_release_irqrestore(&g_pmm_lock, flags);
        if (from_normal) {
            DEBUG_ERROR("pmm: no contiguous run of %zu frames in ZONE_NORMAL; refusing ZONE_DMA32 fallback", count);
        }
        return nullptr;
    }

    advance_cursor_over(from_normal ? g_normal_cursor : g_dma32_cursor, frame_idx, count);

    return commit_frame_range(frame_idx, count, flags);
}

void *pmm_alloc_frames_dma32(size_t count)
{
    if (count == 0 || count > (static_cast<uint64_t>(SIZE_MAX) / k_frame_size))
        return nullptr;
    if (count == 1)
        return pmm_alloc_frame_dma32();

    uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);

    const size_t zone_base = static_cast<size_t>(g_pmm_zone_dma32.base_frame);
    const size_t zone_end = static_cast<size_t>(g_pmm_zone_dma32.base_frame + g_pmm_zone_dma32.frame_count);

    size_t frame_idx = g_pmm_bitmap.find_first_free_sequence(count, zone_base, zone_end);

    if (frame_idx == k_not_found || count > g_bitmap_bits - frame_idx ||
        (static_cast<uint64_t>(frame_idx) + count - 1) > g_highest_page) {
        spinlock_release_irqrestore(&g_pmm_lock, flags);
        return nullptr;
    }

    advance_cursor_over(g_dma32_cursor, frame_idx, count);

    return commit_frame_range(frame_idx, count, flags);
}

bool pmm_reserve_range(uint64_t phys, size_t pages)
{
    if (pages == 0 || (phys & 0xFFFULL) != 0)
        return false;

    const size_t first = static_cast<size_t>(phys / k_frame_size);
    if (first >= g_bitmap_bits || pages > g_bitmap_bits - first)
        return false;

    const uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);

    for (size_t i = 0; i < pages; i++) {
        // Accept frames the firmware already reserves (marked in-use with no
        // owner) so the kernel can pin special-purpose pages like the SMP
        // trampoline out of reserved low RAM. Frames with a live owner are
        // still rejected.
        if (g_pmm_bitmap[first + i] && g_pmm_refcounts[first + i] != 0) {
            spinlock_release_irqrestore(&g_pmm_lock, flags);
            return false;
        }
    }

    for (size_t i = 0; i < pages; i++) {
        const size_t idx = first + i;
        const bool was_free = !g_pmm_bitmap[idx];
        g_pmm_bitmap.set(idx, true);
        if (g_pmm_refcounts[idx] == 0)
            g_pmm_refcounts[idx] = 1;
        if (was_free)
            g_free_memory -= k_frame_size;
    }

    spinlock_release_irqrestore(&g_pmm_lock, flags);
    return true;
}

void pmm_refcount_inc(const void *frame)
{
    if (!frame)
        return;

    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);

    if (frame_idx < g_bitmap_bits) {
        // Referencing a frame nobody owns desyncs the bitmap and the
        // refcount and later frees the frame out from under the caller.
        if (g_pmm_refcounts[frame_idx] == 0) {
            spinlock_release_irqrestore(&g_pmm_lock, flags);
            panic("pmm: refcount_inc on free frame");
        }
        if (g_pmm_refcounts[frame_idx] == UINT16_MAX) {
            spinlock_release_irqrestore(&g_pmm_lock, flags);
            panic("pmm: refcount overflow");
        }
        g_pmm_refcounts[frame_idx]++;
    }

    spinlock_release_irqrestore(&g_pmm_lock, flags);
}

void pmm_refcount_dec(void *frame)
{
    if (!frame)
        return;

    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);

    if (frame_idx >= g_bitmap_bits || g_pmm_refcounts[frame_idx] == 0) {
        spinlock_release_irqrestore(&g_pmm_lock, flags);
        // A free of a frame with no live owner is a double-free or a wild
        // pointer; swallowing it silently let real bugs masquerade as
        // working code. Surface it loudly in debug builds.
        DEBUG_ERROR("pmm: invalid free of frame 0x%lx (double free or stray ref)", frame_idx * k_frame_size);
        return;
    }

    if (--g_pmm_refcounts[frame_idx] == 0) {
        g_pmm_bitmap.set(frame_idx, false);
        // Lower the freed frame's zone cursor (by physical zone boundary,
        // independent of any test-time zone override) so the hole is the
        // first candidate of the next bottom-up scan in that zone.
        if (frame_idx < k_dma32_limit_frame) {
            if (frame_idx < g_dma32_cursor)
                g_dma32_cursor = static_cast<size_t>(frame_idx);
        } else if (frame_idx < g_normal_cursor) {
            g_normal_cursor = static_cast<size_t>(frame_idx);
        }
        g_free_memory += k_frame_size;
#ifdef DEBUG
        if (g_pmm_owners)
            g_pmm_owners[frame_idx] = 0;
#endif
    }

    spinlock_release_irqrestore(&g_pmm_lock, flags);
}

uint16_t pmm_get_refcount(const void *frame)
{
    if (!frame)
        return 0;

    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    uint64_t flags = spinlock_acquire_irqsave(&g_pmm_lock);
    uint16_t count = (frame_idx < g_bitmap_bits) ? g_pmm_refcounts[frame_idx] : 0;
    spinlock_release_irqrestore(&g_pmm_lock, flags);

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

bool pmm_is_managed(const void *frame)
{
    if (!frame)
        return false;

    uint64_t frame_idx = reinterpret_cast<uint64_t>(frame) / k_frame_size;
    return (frame_idx < g_bitmap_bits);
}
