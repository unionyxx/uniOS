#include <drivers/bus/pci/pci.h>
#include <drivers/video/display.h>
#include <drivers/video/framebuffer.h>
#include <kernel/boot_display_timing.h>
#include <kernel/debug.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

extern uint64_t gui_get_wm_pid();

static constexpr uint32_t DEFAULT_FALLBACK_REFRESH_HZ = 60;
static constexpr uint32_t DEFAULT_FALLBACK_REFRESH_MILLIHZ = DEFAULT_FALLBACK_REFRESH_HZ * 1000u;
static constexpr uint32_t COPY_PRESENT_MAX_HZ = 240;

enum class EdidTimingSource : uint8_t
{
    BaseStandard = 0,
    BaseDetailed = 1,
    OtherDetailed = 2,
    CtaVic = 3,
    CtaDetailed = 4,
    DisplayIdFormula = 5,
    DisplayIdDetailed = 6
};

struct EdidTiming
{
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihz;
    uint32_t pixel_clock_khz;
    uint32_t htotal;
    uint32_t vtotal;
    uint32_t hblank_start;
    uint32_t hblank_end;
    uint32_t hsync_start;
    uint32_t hsync_end;
    uint32_t vblank_start;
    uint32_t vblank_end;
    uint32_t vsync_start;
    uint32_t vsync_end;
    EdidTimingSource source;
    bool interlaced;
    uint32_t order;
};

struct EdidRangeLimits
{
    uint32_t min_vertical_hz;
    uint32_t max_vertical_hz;
};

struct EdidTimingList
{
    EdidTiming timings[64];
    uint32_t count;
};

struct EdidRefreshDecision
{
    uint32_t refresh_hz;
    uint32_t refresh_millihz;
    EdidTimingSource source;
    bool used_range_limits;
    bool used_fallback;
    bool exact_match;
    uint32_t exact_candidate_count;
};

static DisplayDevice s_device = {};
static constexpr uint32_t MAX_DISPLAY_BUFFER_OBJECTS = 16;
static constexpr uint32_t MAX_DISPLAY_EVENTS = 64;
static constexpr uint32_t MAX_COMPOSE_BUFFERS = 3;
static constexpr uint64_t DISPLAY_BUFFER_MAP_BASE = 0x340000000ULL;
static constexpr uint64_t DISPLAY_BUFFER_MAP_SLOT_SIZE = 0x04000000ULL;

struct DisplayBufferObject
{
    bool used;
    DisplayBufferHandle handle;
    uint64_t owner_pid;
    DMAAllocation dma;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t flags;
    uint64_t mapped_pid;
    uint64_t mapped_user_addr;
    uint64_t mapped_user_size;
    bool wm_access;
};

static DisplayBufferObject s_display_buffers[MAX_DISPLAY_BUFFER_OBJECTS] = {};
static Spinlock s_display_buffer_lock = SPINLOCK_INIT;
static DisplayEvent s_display_events[MAX_DISPLAY_EVENTS] = {};
static Spinlock s_display_event_lock = SPINLOCK_INIT;
static WaitQueue s_display_event_wait_queue = {};
static uint32_t s_display_event_head = 0;
static uint32_t s_display_event_tail = 0;
struct ComposeBufferSlot
{
    DMAAllocation dma;
    DisplayBuffer buffer;
    uint32_t in_flight_sequence;
};
static ComposeBufferSlot s_compose_buffers[MAX_COMPOSE_BUFFERS] = {};
static uint32_t s_compose_active_index = 0;
static constexpr uint64_t k_cpu_render_buffer_flags = PTE_PRESENT | PTE_WRITABLE | PTE_NX;

static uint64_t display_buffer_bytes(uint32_t stride, uint32_t height)
{
    return (uint64_t)stride * (uint64_t)height * sizeof(uint32_t);
}

static bool display_is_copy_backend_active()
{
    const uint32_t flags = s_device.primary_head.caps.flags;
    return (flags & DISPLAY_FLAG_USES_COPY_PATH) != 0 && (flags & DISPLAY_FLAG_HAS_PAGE_FLIP) == 0;
}

static uint64_t display_buffer_kernel_page_flags(uint32_t buffer_flags)
{
    if (display_is_copy_backend_active())
        return k_cpu_render_buffer_flags;

    const bool cpu_render_target = (buffer_flags & DISPLAY_BUFFER_FLAG_CPU_VISIBLE) != 0 &&
                                   (buffer_flags & DISPLAY_BUFFER_FLAG_RENDER_TARGET) != 0;
    const bool scanout_capable = (buffer_flags & DISPLAY_BUFFER_FLAG_SCANOUT) != 0;
    if (cpu_render_target && !scanout_capable)
        return k_cpu_render_buffer_flags;

    return PTE_WC;
}

static uint64_t display_buffer_user_page_flags(const DisplayBufferObject &buffer)
{
    return display_buffer_kernel_page_flags(buffer.flags) | PTE_USER | PTE_SHARED;
}

static uint64_t display_compose_buffer_page_flags(const DisplayDevice *device)
{
    if (!device)
        return PTE_WC;

    const uint32_t flags = device->primary_head.caps.flags;
    if ((flags & DISPLAY_FLAG_USES_COPY_PATH) != 0 && (flags & DISPLAY_FLAG_HAS_PAGE_FLIP) == 0)
        return k_cpu_render_buffer_flags;

    return PTE_WC;
}

enum DisplayPresentPathKind : uint32_t
{
    DISPLAY_PRESENT_PATH_UNKNOWN = 0,
    DISPLAY_PRESENT_PATH_FLIP = 1,
    DISPLAY_PRESENT_PATH_COPY = 2,
    DISPLAY_PRESENT_PATH_CPU_COMPOSE = 3
};

static bool display_buffers_match(const DisplayBuffer &a, const DisplayBuffer &b)
{
    if (a.width != b.width || a.height != b.height || a.stride != b.stride)
        return false;
    if (a.phys_addr != 0 && b.phys_addr != 0)
        return a.phys_addr == b.phys_addr;
    return a.cpu_mapping == b.cpu_mapping;
}

static uint64_t display_buffer_map_base(DisplayBufferHandle handle)
{
    return DISPLAY_BUFFER_MAP_BASE + (uint64_t)(handle - 1u) * DISPLAY_BUFFER_MAP_SLOT_SIZE;
}

static DisplayBufferObject *display_find_buffer_locked(DisplayBufferHandle handle);
static DisplayBuffer display_buffer_view_from_object(const DisplayBufferObject &buffer);

static uint32_t display_next_auto_sequence(const DisplayHead *head)
{
    if (!head)
        return 0;

    uint32_t base = head->submitted_sequence;
    if (head->status.queued_sequence > base)
        base = head->status.queued_sequence;
    if (head->status.completed_sequence > base)
        base = head->status.completed_sequence;

    uint32_t next = base + 1u;
    return next == 0u ? 1u : next;
}

static bool display_buffer_owner_permits_access(const DisplayBufferObject &buffer, uint64_t owner_pid)
{
    if (owner_pid == 0 || buffer.owner_pid == owner_pid)
        return true;
    uint64_t wm_pid = gui_get_wm_pid();
    return buffer.wm_access && wm_pid != 0 && owner_pid == wm_pid;
}

static bool display_buffer_lookup_snapshot(DisplayBufferHandle handle, uint64_t owner_pid, DisplayBuffer *out_buffer)
{
    if (!out_buffer)
        return false;

    spinlock_acquire(&s_display_buffer_lock);
    const DisplayBufferObject *buffer = display_find_buffer_locked(handle);
    if (!buffer || !display_buffer_owner_permits_access(*buffer, owner_pid)) {
        spinlock_release(&s_display_buffer_lock);
        return false;
    }

    *out_buffer = display_buffer_view_from_object(*buffer);
    spinlock_release(&s_display_buffer_lock);
    return true;
}

bool display_import_surface(uint64_t owner_pid, const DisplaySurfaceImport &request, DisplaySurface *out_surface)
{
    if (!out_surface)
        return false;

    *out_surface = {};
    if (request.format != DISPLAY_PIXEL_FORMAT_XRGB8888)
        return false;

    if (request.backing_kind == DISPLAY_SURFACE_BACKING_DISPLAY_BUFFER) {
        DisplayBuffer buffer = {};
        if (request.display_handle == 0 || !display_buffer_lookup_snapshot(request.display_handle, owner_pid, &buffer))
            return false;

        out_surface->width = buffer.width;
        out_surface->height = buffer.height;
        out_surface->stride = buffer.stride;
        out_surface->format = request.format;
        out_surface->backing_kind = DISPLAY_SURFACE_BACKING_DISPLAY_BUFFER;
        out_surface->cpu_map_address = (uint64_t)buffer.cpu_mapping;
        out_surface->cpu_map_size = display_buffer_bytes(buffer.stride, buffer.height);
        out_surface->cpu_map_stride = buffer.stride;
        out_surface->cpu_map_flags = DISPLAY_BUFFER_FLAG_CPU_VISIBLE | DISPLAY_BUFFER_FLAG_LINEAR;
        out_surface->backend_handle = request.display_handle;
        out_surface->backend_private_handle = buffer.phys_addr != 0 ? buffer.phys_addr : request.display_handle;
        out_surface->dirty_generation = request.dirty_generation;
        out_surface->resize_generation = request.resize_generation;
        return true;
    }

    return false;
}

bool display_lookup_present_buffer(uint64_t owner_pid, DisplayBufferHandle handle, DisplayBuffer *out_buffer)
{
    return display_buffer_lookup_snapshot(handle, owner_pid, out_buffer);
}

static DisplayBufferObject *display_find_buffer_locked(DisplayBufferHandle handle)
{
    if (handle == 0)
        return nullptr;
    for (DisplayBufferObject &buffer : s_display_buffers) {
        if (buffer.used && buffer.handle == handle)
            return &buffer;
    }
    return nullptr;
}

static void display_push_event(uint32_t type, uint32_t sequence)
{
    spinlock_acquire(&s_display_event_lock);
    uint32_t next_tail = (s_display_event_tail + 1u) % MAX_DISPLAY_EVENTS;
    if (next_tail == s_display_event_head)
        s_display_event_head = (s_display_event_head + 1u) % MAX_DISPLAY_EVENTS;

    DisplayEvent &event = s_display_events[s_display_event_tail];
    event.type = type;
    event.connector_id = s_device.connector_count > 0 ? s_device.connectors[0].info.connector_id : 0u;
    event.sequence = sequence;
    event.reserved = 0;
    event.timestamp_ticks = timer_get_ticks();
    event.vblank_count = s_device.primary_head.status.vblank_count;
    s_display_event_tail = next_tail;
    spinlock_release(&s_display_event_lock);
    scheduler_wake_all(&s_display_event_wait_queue);
}

static bool display_pop_event_locked(DisplayEvent *out_event)
{
    if (!out_event)
        return false;
    if (s_display_event_head == s_display_event_tail)
        return false;

    *out_event = s_display_events[s_display_event_head];
    s_display_event_head = (s_display_event_head + 1u) % MAX_DISPLAY_EVENTS;
    return true;
}

static bool display_pop_event(DisplayEvent *out_event)
{
    if (!out_event)
        return false;

    spinlock_acquire(&s_display_event_lock);
    bool ok = display_pop_event_locked(out_event);
    spinlock_release(&s_display_event_lock);
    return ok;
}

DisplayDevice *display_device_instance(void)
{
    return &s_device;
}

DisplayHead *display_primary_head(void)
{
    return &s_device.primary_head;
}

bool display_clip_rect(Rect &rect, const DisplayCaps &caps)
{
    int64_t x = rect.x;
    int64_t y = rect.y;
    int64_t w = rect.w;
    int64_t h = rect.h;

    if (w <= 0 || h <= 0)
        return false;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    int64_t max_width = (int64_t)caps.width;
    int64_t max_height = (int64_t)caps.height;
    if (x >= max_width || y >= max_height)
        return false;
    if (x + w > max_width)
        w = max_width - x;
    if (y + h > max_height)
        h = max_height - y;
    if (w <= 0 || h <= 0)
        return false;

    rect.x = (int32_t)x;
    rect.y = (int32_t)y;
    rect.w = (int32_t)w;
    rect.h = (int32_t)h;
    return true;
}

uint32_t display_resolve_present_sequence(DisplayHead *head, const DisplayPresentRequest &request)
{
    if (!head)
        return request.frame_sequence;

    uint32_t next_sequence = request.frame_sequence;
    if (next_sequence == 0)
        next_sequence = display_next_auto_sequence(head);

    if (head->status.queued_sequence < next_sequence)
        head->status.queued_sequence = next_sequence;
    return next_sequence;
}

void display_complete_present(DisplayHead *head, uint32_t sequence, uint32_t result, uint64_t present_pixels)
{
    if (!head)
        return;
    if (head->pending_flip && head->pending_flip_sequence == sequence &&
        head->pending_buffer_index < head->buffer_count) {
        head->front_buffer_index = head->pending_buffer_index;
        head->buffer_in_flight_sequence[head->pending_buffer_index] = 0;
        head->pending_flip = false;
        head->pending_flip_sequence = 0;
    }
    for (uint32_t i = 0; i < head->buffer_count; i++) {
        if (head->buffer_in_flight_sequence[i] != 0 && head->buffer_in_flight_sequence[i] <= sequence)
            head->buffer_in_flight_sequence[i] = 0;
    }
    head->last_present_pixels = present_pixels;
    head->status.completed_sequence = sequence;
    head->status.last_present_ticks = timer_get_ticks();
    head->status.last_present_result = result;
    head->status.flags = 0;
#ifdef DEBUG
    uint32_t refresh_millihz = head->caps.measured_refresh_millihz ? head->caps.measured_refresh_millihz
                                                                   : head->current_mode.nominal_refresh_millihz;
    BOOT_LOG(
        "Display: complete seq=%u submitted=%u path=%u dma_wait_ticks=%llu vblank_wait_ticks=%llu missed_vblank=%llu "
        "active_refresh=%u.%03uHz layers=%u dirty_rects=%u dirty_area=%llu",
        sequence, head->submitted_sequence, head->last_present_path, head->last_dma_wait_ticks,
        head->last_vblank_wait_ticks, head->missed_target_vblank_count, refresh_millihz / 1000u,
        refresh_millihz % 1000u, head->last_submitted_layers, head->last_submitted_dirty_rects,
        head->last_submitted_dirty_area);
#endif
    display_push_event(DISPLAY_EVENT_FLIP_COMPLETE, sequence);
}

void display_copy_present(DisplayDevice *device, const DisplayPresentRequest &request)
{
    if (!device || !device->boot_framebuffer || !request.buffer || request.stride == 0 ||
        (request.rect_count != 0 && !request.rects)) {
        return;
    }

    DisplayHead *head = &device->primary_head;
    volatile uint32_t *dest = (volatile uint32_t *)device->boot_framebuffer->address;
    uint32_t dest_stride = head->caps.pitch / 4u;
    Rect full_rect = gui_rect_make(0, 0, (int32_t)head->caps.width, (int32_t)head->caps.height);
    const Rect *rects = request.rect_count ? request.rects : &full_rect;
    uint32_t rect_count = request.rect_count ? request.rect_count : 1u;

    for (uint32_t i = 0; i < rect_count; i++) {
        Rect rect = rects[i];
        if (!display_clip_rect(rect, head->caps))
            continue;

        int32_t src_x = rect.x - request.source_origin_x;
        int32_t src_y = rect.y - request.source_origin_y;
        if (src_x < 0 || src_y < 0 || src_x >= (int32_t)request.stride || src_y >= (int32_t)head->caps.height)
            continue;

        int32_t max_copy_width = (int32_t)request.stride - src_x;
        int32_t max_copy_height = (int32_t)head->caps.height - src_y;
        if (rect.w > max_copy_width)
            rect.w = max_copy_width;
        if (rect.h > max_copy_height)
            rect.h = max_copy_height;
        if (rect.w <= 0 || rect.h <= 0)
            continue;

        for (int32_t py = 0; py < rect.h; py++) {
            size_t dst_row_index = ((size_t)(rect.y + py) * dest_stride) + (size_t)rect.x;
            const uint32_t *src_row = &request.buffer[(uint32_t)(src_y + py) * request.stride + (uint32_t)src_x];
            if ((uint32_t)rect.w < 64u)
                gfx_copy_line((uint32_t *)&dest[dst_row_index], src_row, (uint32_t)rect.w);
            else
                gfx_copy_line_nt((uint32_t *)&dest[dst_row_index], src_row, (uint32_t)rect.w);
        }
    }

    asm volatile("sfence" ::: "memory");
}

static bool display_damage_is_full_frame(const Rect *rects, uint32_t rect_count, const DisplayCaps &caps)
{
    if (rect_count == 0 || !rects)
        return true;
    if (rect_count != 1)
        return false;

    Rect rect = rects[0];
    if (!display_clip_rect(rect, caps))
        return false;
    return rect.x == 0 && rect.y == 0 && rect.w == (int32_t)caps.width && rect.h == (int32_t)caps.height;
}

static void display_copy_buffer_rows(uint32_t *dst, uint32_t dst_stride, const uint32_t *src, uint32_t src_stride,
                                     uint32_t width, uint32_t height)
{
    if (!dst || !src || dst_stride == 0 || src_stride == 0 || width == 0 || height == 0)
        return;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t *dst_row = &dst[(size_t)y * dst_stride];
        const uint32_t *src_row = &src[(size_t)y * src_stride];
        if (width < 64u)
            gfx_copy_line(dst_row, src_row, width);
        else
            gfx_copy_line_nt(dst_row, src_row, width);
    }
}

static uint32_t display_fallback_refresh_hz(const DisplayHead *head)
{
    if (!head)
        return 0;

    uint32_t refresh_hz = head->caps.refresh_hz ? head->caps.refresh_hz : DEFAULT_FALLBACK_REFRESH_HZ;
    if ((head->caps.flags & DISPLAY_FLAG_USES_COPY_PATH) != 0 && refresh_hz > COPY_PRESENT_MAX_HZ) {
        refresh_hz = COPY_PRESENT_MAX_HZ;
    }
    uint32_t timer_hz = timer_get_frequency();
    if (timer_hz == 0)
        timer_hz = 1000;
    if (refresh_hz == 0)
        refresh_hz = DEFAULT_FALLBACK_REFRESH_HZ;
    if (refresh_hz > timer_hz)
        refresh_hz = timer_hz;
    return refresh_hz;
}

static uint64_t display_fallback_reserve_next_deadline(DisplayHead *head)
{
    if (!head)
        return 0;

    uint32_t refresh_hz = display_fallback_refresh_hz(head);
    if (refresh_hz == 0)
        return 0;
    uint32_t timer_hz = timer_get_frequency();
    if (timer_hz == 0)
        timer_hz = 1000;

    uint64_t now = timer_get_ticks();
    if (head->next_deadline_tick == 0 || now > head->next_deadline_tick + (uint64_t)timer_hz) {
        head->next_deadline_tick = now;
        head->deadline_remainder = 0;
    }

    head->deadline_remainder += timer_hz;
    uint64_t delta = head->deadline_remainder / refresh_hz;
    head->deadline_remainder %= refresh_hz;
    if (delta == 0)
        delta = 1;
    head->next_deadline_tick += delta;

    return head->next_deadline_tick;
}

static void display_fallback_finish_wait(DisplayHead *head, uint64_t target_tick)
{
    if (!head)
        return;

    uint64_t wait_start = timer_get_ticks();
    if (target_tick != 0 && wait_start > target_tick)
        head->missed_target_vblank_count++;
    if (target_tick > wait_start)
        scheduler_sleep(target_tick - wait_start);
    uint64_t wait_end = timer_get_ticks();
    head->last_vblank_wait_ticks = wait_end - wait_start;
    head->total_vblank_wait_ticks += head->last_vblank_wait_ticks;
    head->status.vblank_count++;
    head->status.last_vblank_ticks = wait_end;
    display_push_event(DISPLAY_EVENT_VBLANK, head->status.completed_sequence);
}

uint32_t display_fallback_wait_until_next_frame(DisplayHead *head)
{
    uint64_t target_tick = display_fallback_reserve_next_deadline(head);
    display_fallback_finish_wait(head, target_tick);
    return head->status.completed_sequence;
}

static uint64_t display_damage_area(const Rect *rects, uint32_t rect_count, const DisplayCaps &caps,
                                    uint32_t *out_count)
{
    uint64_t area = 0;
    uint32_t clipped_count = 0;
    if (rects) {
        for (uint32_t i = 0; i < rect_count; i++) {
            Rect rect = rects[i];
            if (!display_clip_rect(rect, caps))
                continue;
            area += (uint64_t)rect.w * (uint64_t)rect.h;
            clipped_count++;
        }
    }
    if (out_count)
        *out_count = clipped_count;
    return area;
}

void display_note_present_submitted(DisplayHead *head, uint32_t sequence, uint32_t path, uint32_t layers,
                                    const Rect *damage_rects, uint32_t damage_count)
{
    if (!head || sequence == 0)
        return;

    uint32_t clipped_damage_count = 0;
    uint64_t clipped_damage_area = display_damage_area(damage_rects, damage_count, head->caps, &clipped_damage_count);
    head->submitted_sequence = sequence;
    if (head->status.queued_sequence < sequence)
        head->status.queued_sequence = sequence;
    head->last_present_path = path;
    head->last_submitted_layers = layers;
    head->last_submitted_dirty_rects = clipped_damage_count;
    head->last_submitted_dirty_area = clipped_damage_area;
    if (path == DISPLAY_PRESENT_PATH_FLIP)
        head->flip_path_count++;
    else if (path == DISPLAY_PRESENT_PATH_COPY)
        head->copy_path_count++;
    else if (path == DISPLAY_PRESENT_PATH_CPU_COMPOSE)
        head->cpu_compose_path_count++;
#ifdef DEBUG
    uint32_t refresh_millihz = head->caps.measured_refresh_millihz ? head->caps.measured_refresh_millihz
                                                                   : head->current_mode.nominal_refresh_millihz;
    BOOT_LOG(
        "Display: submit seq=%u completed=%u path=%u layers=%u dirty_rects=%u dirty_area=%llu active_refresh=%u.%03uHz",
        sequence, head->status.completed_sequence, path, layers, clipped_damage_count, clipped_damage_area,
        refresh_millihz / 1000u, refresh_millihz % 1000u);
#endif
}

static void display_set_head_modes(DisplayHead *head, const DisplayMode &current_mode,
                                   const DisplayMode &preferred_mode)
{
    if (!head)
        return;

    head->current_mode = current_mode;
    head->current_mode.flags |= DISPLAY_MODE_FLAG_CURRENT;
    head->current_mode.flags &= ~(uint32_t)DISPLAY_MODE_FLAG_PREFERRED;
    head->preferred_mode = preferred_mode.width != 0 ? preferred_mode : current_mode;
    head->preferred_mode.flags |= DISPLAY_MODE_FLAG_PREFERRED;
    head->preferred_mode.flags &= ~(uint32_t)DISPLAY_MODE_FLAG_CURRENT;
    if (head->preferred_mode.measured_refresh_millihz != 0 && head->current_mode.measured_refresh_millihz == 0)
        head->preferred_mode.measured_refresh_millihz = 0;
}

static bool gop_get_caps(DisplayDevice *device, DisplayCaps *out_caps);
static bool gop_get_status(DisplayDevice *device, DisplayStatus *out_status);
static uint32_t gop_present(DisplayDevice *device, const DisplayPresentRequest &request);
static uint32_t gop_present_buffer(DisplayDevice *device, const DisplayBuffer &buffer, const Rect *damage_rects,
                                   uint32_t damage_count, uint32_t frame_sequence, uint32_t flags);
static uint32_t gop_wait(DisplayDevice *device);
static bool gop_set_mode(DisplayDevice *device, const DisplayMode &mode);
static bool gop_atomic_commit(DisplayDevice *device, const DisplayAtomicState &state, const DisplayMode &mode);

static const DisplayBackendOps g_gop_backend_ops = {
    "firmware-framebuffer", gop_get_caps, gop_get_status,    gop_present, gop_present_buffer,
    gop_wait,       gop_set_mode, gop_atomic_commit, nullptr,     nullptr,
    nullptr,        nullptr,      nullptr,           nullptr,
};

void display_force_gop_backend(DisplayDevice *device)
{
    if (!device)
        return;
    device->ops = &g_gop_backend_ops;
    device->backend_kind = DisplayBackendKind::FirmwareFramebuffer;
}

static uint16_t read_le16(const uint8_t *data)
{
    if (!data)
        return 0;
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static const char *edid_timing_source_name(EdidTimingSource source)
{
    switch (source) {
        case EdidTimingSource::BaseStandard:
            return "base-standard";
        case EdidTimingSource::BaseDetailed:
            return "base-detailed";
        case EdidTimingSource::OtherDetailed:
            return "other-detailed";
        case EdidTimingSource::CtaVic:
            return "cta-vic";
        case EdidTimingSource::CtaDetailed:
            return "cta-detailed";
        case EdidTimingSource::DisplayIdFormula:
            return "displayid-formula";
        case EdidTimingSource::DisplayIdDetailed:
            return "displayid-detailed";
        default:
            return "unknown";
    }
}

static uint32_t round_millihz_to_hz(uint32_t refresh_millihz)
{
    if (refresh_millihz == 0)
        return 0;
    return (refresh_millihz + 500u) / 1000u;
}

static uint32_t hz_to_millihz(uint32_t refresh_hz)
{
    return refresh_hz * 1000u;
}

static uint32_t abs_diff_u32(uint32_t a, uint32_t b)
{
    return a > b ? (a - b) : (b - a);
}

static uint32_t display_refresh_millihz_or_default(uint32_t refresh_millihz)
{
    return refresh_millihz ? refresh_millihz : DEFAULT_FALLBACK_REFRESH_MILLIHZ;
}

static const char *format_millihz_fraction(uint32_t refresh_millihz, char out[4])
{
    uint32_t fraction = refresh_millihz % 1000u;
    out[0] = (char)('0' + (fraction / 100u) % 10u);
    out[1] = (char)('0' + (fraction / 10u) % 10u);
    out[2] = (char)('0' + (fraction % 10u));
    out[3] = '\0';
    return out;
}

static bool edid_block_checksum_valid(const uint8_t *block)
{
    if (!block)
        return false;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 128; i++)
        sum += block[i];
    return (sum & 0xFFu) == 0;
}

static void add_timing_candidate(EdidTimingList *list, const EdidTiming &timing)
{
    if (!list || timing.width == 0 || timing.height == 0 || timing.refresh_millihz == 0)
        return;

    for (uint32_t i = 0; i < list->count; i++) {
        EdidTiming &existing = list->timings[i];
        if (existing.width != timing.width || existing.height != timing.height ||
            existing.refresh_millihz != timing.refresh_millihz || existing.interlaced != timing.interlaced) {
            continue;
        }
        if ((uint32_t)timing.source > (uint32_t)existing.source)
            existing.source = timing.source;
        return;
    }

    if (list->count >= (sizeof(list->timings) / sizeof(list->timings[0])))
        return;
    EdidTiming stored = timing;
    stored.order = list->count;
    list->timings[list->count++] = stored;
}

static bool timing_has_exact_geometry(const EdidTiming &timing)
{
    return timing.pixel_clock_khz != 0 && timing.htotal > timing.width && timing.vtotal > timing.height &&
           timing.hblank_start < timing.htotal && timing.hblank_end <= timing.htotal &&
           timing.hsync_start < timing.htotal && timing.hsync_end <= timing.htotal &&
           timing.vblank_start < timing.vtotal && timing.vblank_end <= timing.vtotal &&
           timing.vsync_start < timing.vtotal && timing.vsync_end <= timing.vtotal;
}

static bool parse_detailed_timing(const uint8_t *dtd, EdidTimingSource source, EdidTiming *out_timing)
{
    if (!dtd || !out_timing)
        return false;

    uint32_t pixel_clock_10khz = (uint32_t)dtd[0] | ((uint32_t)dtd[1] << 8);
    if (pixel_clock_10khz == 0)
        return false;

    uint32_t h_active = (uint32_t)dtd[2] | (((uint32_t)dtd[4] & 0xF0u) << 4);
    uint32_t h_blank = (uint32_t)dtd[3] | (((uint32_t)dtd[4] & 0x0Fu) << 8);
    uint32_t v_active = (uint32_t)dtd[5] | (((uint32_t)dtd[7] & 0xF0u) << 4);
    uint32_t v_blank = (uint32_t)dtd[6] | (((uint32_t)dtd[7] & 0x0Fu) << 8);
    uint32_t h_sync_offset = (uint32_t)dtd[8] | (((uint32_t)dtd[11] & 0xC0u) << 2);
    uint32_t h_sync_width = (uint32_t)dtd[9] | (((uint32_t)dtd[11] & 0x30u) << 4);
    uint32_t v_sync_offset = ((uint32_t)dtd[10] >> 4) | (((uint32_t)dtd[11] & 0x0Cu) << 2);
    uint32_t v_sync_width = ((uint32_t)dtd[10] & 0x0Fu) | (((uint32_t)dtd[11] & 0x03u) << 4);
    uint32_t h_total = h_active + h_blank;
    uint32_t v_total = v_active + v_blank;
    if (h_active == 0 || v_active == 0 || h_total == 0 || v_total == 0)
        return false;

    uint64_t pixel_clock_hz = (uint64_t)pixel_clock_10khz * 10000ULL;
    uint64_t total_pixels = (uint64_t)h_total * (uint64_t)v_total;
    bool interlaced = (dtd[17] & 0x80u) != 0;
    uint64_t numerator = pixel_clock_hz * 1000ULL;
    if (interlaced)
        numerator *= 2ULL;
    uint32_t refresh_millihz = (uint32_t)((numerator + (total_pixels / 2ULL)) / total_pixels);
    if (refresh_millihz == 0)
        return false;

    out_timing->width = h_active;
    out_timing->height = v_active;
    out_timing->refresh_millihz = refresh_millihz;
    out_timing->pixel_clock_khz = pixel_clock_10khz * 10u;
    out_timing->htotal = h_total;
    out_timing->vtotal = v_total;
    out_timing->hblank_start = h_active;
    out_timing->hblank_end = h_total;
    out_timing->hsync_start = h_active + h_sync_offset;
    out_timing->hsync_end = out_timing->hsync_start + h_sync_width;
    out_timing->vblank_start = v_active;
    out_timing->vblank_end = v_total;
    out_timing->vsync_start = v_active + v_sync_offset;
    out_timing->vsync_end = out_timing->vsync_start + v_sync_width;
    out_timing->source = source;
    out_timing->interlaced = interlaced;
    return true;
}

static bool parse_standard_timing(const uint8_t *descriptor, uint8_t edid_revision, EdidTiming *out_timing)
{
    if (!descriptor || !out_timing)
        return false;
    if (descriptor[0] == 0x01 && descriptor[1] == 0x01)
        return false;

    uint32_t width = ((uint32_t)descriptor[0] + 31u) * 8u;
    uint32_t aspect = descriptor[1] >> 6;
    uint32_t height = 0;
    switch (aspect) {
        case 0:
            height = (edid_revision < 3) ? width : (width * 10u) / 16u;
            break;
        case 1:
            height = (width * 3u) / 4u;
            break;
        case 2:
            height = (width * 4u) / 5u;
            break;
        case 3:
            height = (width * 9u) / 16u;
            break;
        default:
            return 0;
    }

    uint32_t refresh_hz = (descriptor[1] & 0x3Fu) + 60u;
    if (width == 0 || height == 0 || refresh_hz == 0)
        return false;

    out_timing->width = width;
    out_timing->height = height;
    out_timing->refresh_millihz = hz_to_millihz(refresh_hz);
    out_timing->pixel_clock_khz = 0;
    out_timing->htotal = 0;
    out_timing->vtotal = 0;
    out_timing->hblank_start = 0;
    out_timing->hblank_end = 0;
    out_timing->hsync_start = 0;
    out_timing->hsync_end = 0;
    out_timing->vblank_start = 0;
    out_timing->vblank_end = 0;
    out_timing->vsync_start = 0;
    out_timing->vsync_end = 0;
    out_timing->source = EdidTimingSource::BaseStandard;
    out_timing->interlaced = false;
    return true;
}

static bool parse_cta_vic_timing(uint8_t vic, EdidTiming *out_timing)
{
    if (!out_timing)
        return false;

    struct CtaVicTiming
    {
        uint16_t width;
        uint16_t height;
        uint16_t refresh_hz;
        uint8_t vic;
        bool interlaced;
    };

    static const CtaVicTiming k_cta_vics[] = {
        {640, 480, 60, 1, false},      {720, 480, 60, 2, false},      {720, 480, 60, 3, false},
        {1280, 720, 60, 4, false},     {1920, 1080, 60, 5, true},     {1920, 1080, 60, 16, false},
        {1920, 1080, 50, 31, false},   {1920, 1080, 24, 32, false},   {1920, 1080, 25, 33, false},
        {1920, 1080, 30, 34, false},   {1920, 1080, 120, 63, false},  {1920, 1080, 100, 64, false},
        {3840, 2160, 30, 95, false},   {3840, 2160, 25, 96, false},   {3840, 2160, 24, 97, false},
        {4096, 2160, 24, 98, false},   {3840, 2160, 60, 102, false},  {3840, 2160, 50, 103, false},
        {4096, 2160, 60, 104, false},  {4096, 2160, 50, 105, false},  {3840, 2160, 100, 117, false},
        {3840, 2160, 120, 118, false}, {3840, 2160, 100, 119, false}, {3840, 2160, 120, 120, false},
    };

    for (uint32_t i = 0; i < sizeof(k_cta_vics) / sizeof(k_cta_vics[0]); i++) {
        if (k_cta_vics[i].vic != vic)
            continue;
        out_timing->width = k_cta_vics[i].width;
        out_timing->height = k_cta_vics[i].height;
        out_timing->refresh_millihz = hz_to_millihz(k_cta_vics[i].refresh_hz);
        out_timing->pixel_clock_khz = 0;
        out_timing->htotal = 0;
        out_timing->vtotal = 0;
        out_timing->hblank_start = 0;
        out_timing->hblank_end = 0;
        out_timing->hsync_start = 0;
        out_timing->hsync_end = 0;
        out_timing->vblank_start = 0;
        out_timing->vblank_end = 0;
        out_timing->vsync_start = 0;
        out_timing->vsync_end = 0;
        out_timing->source = EdidTimingSource::CtaVic;
        out_timing->interlaced = k_cta_vics[i].interlaced;
        return true;
    }
    return false;
}

static bool parse_displayid_detailed_timing(const uint8_t *timing, bool type_7, EdidTiming *out_timing)
{
    if (!timing || !out_timing)
        return false;

    uint32_t pixel_clock_raw = (uint32_t)timing[0] | ((uint32_t)timing[1] << 8) | ((uint32_t)timing[2] << 16);
    if (pixel_clock_raw == 0)
        return false;

    uint32_t width = (uint32_t)read_le16(&timing[4]) + 1u;
    uint32_t h_blank = (uint32_t)read_le16(&timing[6]) + 1u;
    uint32_t height = (uint32_t)read_le16(&timing[12]) + 1u;
    uint32_t v_blank = (uint32_t)read_le16(&timing[14]) + 1u;
    uint32_t h_total = width + h_blank;
    uint32_t v_total = height + v_blank;
    if (width == 0 || height == 0 || h_total == 0 || v_total == 0)
        return false;

    uint64_t pixel_clock_hz = (uint64_t)pixel_clock_raw * (type_7 ? 1000ULL : 10000ULL);
    uint64_t total_pixels = (uint64_t)h_total * (uint64_t)v_total;
    uint32_t refresh_millihz = (uint32_t)(((pixel_clock_hz * 1000ULL) + (total_pixels / 2ULL)) / total_pixels);
    if (refresh_millihz == 0)
        return false;

    out_timing->width = width;
    out_timing->height = height;
    out_timing->refresh_millihz = refresh_millihz;
    out_timing->pixel_clock_khz = (uint32_t)(pixel_clock_hz / 1000ULL);
    out_timing->htotal = h_total;
    out_timing->vtotal = v_total;
    out_timing->hblank_start = width;
    out_timing->hblank_end = h_total;
    out_timing->hsync_start = 0;
    out_timing->hsync_end = 0;
    out_timing->vblank_start = height;
    out_timing->vblank_end = v_total;
    out_timing->vsync_start = 0;
    out_timing->vsync_end = 0;
    out_timing->source = EdidTimingSource::DisplayIdDetailed;
    out_timing->interlaced = false;
    return true;
}

static bool parse_displayid_formula_timing(const uint8_t *timing, EdidTiming *out_timing)
{
    if (!timing || !out_timing)
        return false;

    uint8_t timing_formula = timing[0] & 0x07u;
    if (timing_formula > 1u)
        return false;

    uint32_t width = (uint32_t)read_le16(&timing[1]) + 1u;
    uint32_t height = (uint32_t)read_le16(&timing[3]) + 1u;
    uint32_t refresh_hz = (uint32_t)timing[5] + 1u;
    if (width == 0 || height == 0 || refresh_hz == 0)
        return false;

    out_timing->width = width;
    out_timing->height = height;
    out_timing->refresh_millihz = hz_to_millihz(refresh_hz);
    out_timing->pixel_clock_khz = 0;
    out_timing->htotal = 0;
    out_timing->vtotal = 0;
    out_timing->hblank_start = 0;
    out_timing->hblank_end = 0;
    out_timing->hsync_start = 0;
    out_timing->hsync_end = 0;
    out_timing->vblank_start = 0;
    out_timing->vblank_end = 0;
    out_timing->vsync_start = 0;
    out_timing->vsync_end = 0;
    out_timing->source = EdidTimingSource::DisplayIdFormula;
    out_timing->interlaced = false;
    return true;
}

static void parse_displayid_extension(const uint8_t *ext, EdidTimingList *timings)
{
    if (!ext || !timings)
        return;

    uint32_t payload_len = ext[2];
    uint32_t offset = 5;
    uint32_t end = offset + payload_len;
    // DisplayID payload cannot exceed the 128-byte block (last byte is checksum)
    if (end > 126u)
        end = 126u;

    while (offset + 3u <= end) {
        uint8_t tag = ext[offset];
        uint32_t block_len = ext[offset + 2];
        uint32_t payload = offset + 3u;
        uint32_t next = payload + block_len;
        if (next > end)
            break;

        if ((tag == 0x03u || tag == 0x22u) && (block_len % 20u) == 0) {
            bool type_7 = tag == 0x22u;
            for (uint32_t i = 0; i < block_len; i += 20u) {
                EdidTiming timing = {};
                if (parse_displayid_detailed_timing(&ext[payload + i], type_7, &timing)) {
                    add_timing_candidate(timings, timing);
                }
            }
        } else if ((tag == 0x24u || tag == 0x25u) && (block_len % 6u) == 0) {
            for (uint32_t i = 0; i < block_len; i += 6u) {
                EdidTiming timing = {};
                if (parse_displayid_formula_timing(&ext[payload + i], &timing)) {
                    add_timing_candidate(timings, timing);
                }
            }
        }

        offset = next;
    }
}

static bool parse_monitor_range_limits(const uint8_t *descriptor, EdidRangeLimits *out_limits)
{
    if (!descriptor || !out_limits)
        return false;
    if (descriptor[0] != 0x00 || descriptor[1] != 0x00 || descriptor[2] != 0x00 || descriptor[3] != 0xFD) {
        return false;
    }

    uint32_t min_vertical_hz = descriptor[5];
    uint32_t max_vertical_hz = descriptor[6];
    if (min_vertical_hz == 0 || max_vertical_hz == 0 || min_vertical_hz > max_vertical_hz)
        return false;

    out_limits->min_vertical_hz = min_vertical_hz;
    out_limits->max_vertical_hz = max_vertical_hz;
    return true;
}

static void parse_descriptor_block(const uint8_t *block, uint32_t start_offset, uint32_t end_offset,
                                   EdidTimingSource detailed_source, EdidTimingList *timings,
                                   uint32_t *max_range_refresh)
{
    if (!block || !timings || !max_range_refresh)
        return;
    if (end_offset > 128u)
        end_offset = 128u;

    EdidTiming timing = {};
    EdidRangeLimits range_limits = {};
    for (uint32_t offset = start_offset; offset + 18u <= end_offset; offset += 18u) {
        const uint8_t *descriptor = &block[offset];
        if (parse_detailed_timing(descriptor, detailed_source, &timing)) {
            add_timing_candidate(timings, timing);
            continue;
        }
        if (parse_monitor_range_limits(descriptor, &range_limits) &&
            range_limits.max_vertical_hz > *max_range_refresh) {
            *max_range_refresh = range_limits.max_vertical_hz;
        }
    }
}

static uint32_t timing_source_preference(EdidTimingSource source)
{
    switch (source) {
        case EdidTimingSource::DisplayIdDetailed:
            return 6;
        case EdidTimingSource::CtaDetailed:
            return 5;
        case EdidTimingSource::BaseDetailed:
            return 4;
        case EdidTimingSource::OtherDetailed:
            return 3;
        case EdidTimingSource::DisplayIdFormula:
            return 2;
        case EdidTimingSource::CtaVic:
            return 1;
        case EdidTimingSource::BaseStandard:
        default:
            return 0;
    }
}

static bool exact_timing_better_than(const EdidTiming &candidate, const EdidTiming &best, bool have_best,
                                     uint32_t refresh_hint_millihz)
{
    if (!have_best)
        return true;

    if (refresh_hint_millihz != 0) {
        uint32_t candidate_delta = abs_diff_u32(candidate.refresh_millihz, refresh_hint_millihz);
        uint32_t best_delta = abs_diff_u32(best.refresh_millihz, refresh_hint_millihz);
        if (candidate_delta != best_delta)
            return candidate_delta < best_delta;
    }

    if (candidate.refresh_millihz != best.refresh_millihz)
        return candidate.refresh_millihz > best.refresh_millihz;

    uint32_t candidate_pref = timing_source_preference(candidate.source);
    uint32_t best_pref = timing_source_preference(best.source);
    if (candidate_pref != best_pref)
        return candidate_pref > best_pref;

    return candidate.order < best.order;
}

static bool select_best_exact_timing(const EdidTimingList &timings, uint32_t width, uint32_t height,
                                     uint32_t refresh_hint_millihz, EdidTiming *out_timing)
{
    if (!out_timing)
        return false;

    for (int pass = 0; pass < 2; pass++) {
        bool require_progressive = (pass == 0);
        bool found = false;
        EdidTiming best = {};

        for (uint32_t i = 0; i < timings.count; i++) {
            const EdidTiming &timing = timings.timings[i];
            if (timing.width != width || timing.height != height)
                continue;
            if (require_progressive && timing.interlaced)
                continue;
            if (exact_timing_better_than(timing, best, found, refresh_hint_millihz)) {
                best = timing;
                found = true;
            }
        }

        if (found) {
            *out_timing = best;
            return true;
        }
    }

    return false;
}

static uint32_t count_exact_timing_candidates(const EdidTimingList &timings, uint32_t width, uint32_t height,
                                              bool progressive_only)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < timings.count; i++) {
        const EdidTiming &timing = timings.timings[i];
        if (timing.width != width || timing.height != height)
            continue;
        if (progressive_only && timing.interlaced)
            continue;
        count++;
    }
    return count;
}

static bool collect_edid_timings(const uint8_t *edid, uint64_t edid_size, EdidTimingList *out_timings,
                                 uint32_t *out_max_range_refresh)
{
    if (!edid || !out_timings || !out_max_range_refresh || edid_size < 128)
        return false;

    static const uint8_t k_edid_header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (kstring::memcmp(edid, k_edid_header, sizeof(k_edid_header)) != 0)
        return false;

    uint32_t available_blocks = (uint32_t)(edid_size / 128u);
    uint32_t required_blocks = (uint32_t)edid[126] + 1u;
    if (required_blocks == 0 || required_blocks > available_blocks)
        return false;
    for (uint32_t block = 0; block < required_blocks; block++) {
        if (!edid_block_checksum_valid(&edid[block * 128u]))
            return false;
    }

    *out_timings = {};
    *out_max_range_refresh = 0;
    uint8_t edid_revision = edid[19];
    EdidTiming timing = {};
    parse_descriptor_block(edid, 54, 126, EdidTimingSource::BaseDetailed, out_timings, out_max_range_refresh);

    for (uint32_t offset = 38; offset + 1 < 54; offset += 2) {
        if (parse_standard_timing(&edid[offset], edid_revision, &timing)) {
            add_timing_candidate(out_timings, timing);
        }
    }

    for (uint32_t block = 1; block < required_blocks; block++) {
        const uint8_t *ext = &edid[block * 128u];
        if (ext[0] == 0x02) {
            uint32_t dtd_offset = ext[2];
            bool valid_cta_data_range = dtd_offset >= 4 && dtd_offset <= 127;
            if (!valid_cta_data_range)
                dtd_offset = 127;
            if (valid_cta_data_range) {
                for (uint32_t offset = 4; offset < dtd_offset;) {
                    uint8_t header = ext[offset];
                    uint32_t payload_len = header & 0x1Fu;
                    uint32_t next = offset + 1u + payload_len;
                    if (next > dtd_offset)
                        break;

                    uint8_t tag = header >> 5;
                    if (tag == 0x02) {
                        for (uint32_t i = offset + 1u; i < next; i++) {
                            uint8_t vic = ext[i] & 0x7Fu;
                            if (parse_cta_vic_timing(vic, &timing)) {
                                add_timing_candidate(out_timings, timing);
                            }
                        }
                    }
                    offset = next;
                }
            }
            parse_descriptor_block(ext, dtd_offset, 127, EdidTimingSource::CtaDetailed, out_timings,
                                   out_max_range_refresh);
            continue;
        }

        if (ext[0] == 0x70u) {
            parse_displayid_extension(ext, out_timings);
            continue;
        }

        parse_descriptor_block(ext, 0, 127, EdidTimingSource::OtherDetailed, out_timings, out_max_range_refresh);
    }

    return true;
}

static bool timing_matches_current_mode(const EdidTiming &timing, const DisplayMode *current_mode)
{
    if (!current_mode)
        return false;
    if (timing.width != current_mode->width || timing.height != current_mode->height)
        return false;

    uint32_t current_refresh = current_mode->nominal_refresh_millihz ? current_mode->nominal_refresh_millihz
                                                                     : hz_to_millihz(current_mode->refresh_hz);
    if (current_refresh == 0)
        return false;
    return abs_diff_u32(timing.refresh_millihz, current_refresh) <= 1500u;
}

static bool preferred_mode_timing_better(const EdidTiming &candidate, const EdidTiming &best, bool have_best)
{
    if (!have_best)
        return true;

    uint64_t candidate_pixels = (uint64_t)candidate.width * (uint64_t)candidate.height;
    uint64_t best_pixels = (uint64_t)best.width * (uint64_t)best.height;
    if (candidate_pixels != best_pixels)
        return candidate_pixels > best_pixels;

    if (candidate.interlaced != best.interlaced)
        return !candidate.interlaced;

    if (candidate.refresh_millihz != best.refresh_millihz)
        return candidate.refresh_millihz > best.refresh_millihz;

    uint32_t candidate_pref = timing_source_preference(candidate.source);
    uint32_t best_pref = timing_source_preference(best.source);
    if (candidate_pref != best_pref)
        return candidate_pref > best_pref;

    return candidate.order < best.order;
}

static void populate_mode_from_timing(const EdidTiming &timing, uint32_t mode_id, uint32_t flags,
                                      uint32_t measured_refresh_millihz, DisplayMode *out_mode)
{
    if (!out_mode)
        return;
    *out_mode = {};
    out_mode->mode_id = mode_id;
    out_mode->width = timing.width;
    out_mode->height = timing.height;
    out_mode->refresh_hz = round_millihz_to_hz(timing.refresh_millihz);
    out_mode->nominal_refresh_millihz = timing.refresh_millihz;
    out_mode->measured_refresh_millihz = (flags & DISPLAY_MODE_FLAG_CURRENT) ? measured_refresh_millihz : 0u;
    out_mode->pixel_clock_khz = timing.pixel_clock_khz;
    out_mode->htotal = timing.htotal;
    out_mode->vtotal = timing.vtotal;
    out_mode->hblank_start = timing.hblank_start;
    out_mode->hblank_end = timing.hblank_end;
    out_mode->hsync_start = timing.hsync_start;
    out_mode->hsync_end = timing.hsync_end;
    out_mode->vblank_start = timing.vblank_start;
    out_mode->vblank_end = timing.vblank_end;
    out_mode->vsync_start = timing.vsync_start;
    out_mode->vsync_end = timing.vsync_end;
    out_mode->flags = flags | (timing.interlaced ? (uint32_t)DISPLAY_MODE_FLAG_INTERLACED : 0u) |
                      (timing_has_exact_geometry(timing) ? (uint32_t)DISPLAY_MODE_FLAG_EXACT_TIMING : 0u);
}

static EdidRefreshDecision detect_refresh_from_edid(const uint8_t *edid, uint64_t edid_size, uint32_t width,
                                                    uint32_t height, uint32_t refresh_hint_millihz = 0)
{
    EdidRefreshDecision decision = {DEFAULT_FALLBACK_REFRESH_HZ,
                                    DEFAULT_FALLBACK_REFRESH_MILLIHZ,
                                    EdidTimingSource::BaseStandard,
                                    false,
                                    true,
                                    false,
                                    0};
    if (!edid || edid_size < 128 || width == 0 || height == 0)
        return decision;

    EdidTimingList timings = {};
    uint32_t max_range_refresh = 0;
    if (!collect_edid_timings(edid, edid_size, &timings, &max_range_refresh))
        return decision;

    EdidTiming best_exact = {};
    bool have_exact = select_best_exact_timing(timings, width, height, refresh_hint_millihz, &best_exact);
    if (have_exact) {
        uint32_t exact_candidate_count = count_exact_timing_candidates(timings, width, height, !best_exact.interlaced);
        decision.refresh_millihz = best_exact.refresh_millihz;
        decision.refresh_hz = round_millihz_to_hz(best_exact.refresh_millihz);
        decision.source = best_exact.source;
        decision.used_fallback = false;
        decision.exact_match = true;
        decision.exact_candidate_count = exact_candidate_count;
        return decision;
    }

    if (max_range_refresh >= 30 && max_range_refresh <= 360) {
        decision.refresh_hz = max_range_refresh;
        decision.refresh_millihz = hz_to_millihz(max_range_refresh);
        decision.used_range_limits = true;
        decision.used_fallback = false;
    }
    return decision;
}

uint32_t display_detect_refresh_millihz_from_edid(const uint8_t *edid, uint64_t edid_size, uint32_t width,
                                                  uint32_t height)
{
    return detect_refresh_from_edid(edid, edid_size, width, height).refresh_millihz;
}

uint32_t display_detect_refresh_hz_from_edid(const uint8_t *edid, uint64_t edid_size, uint32_t width, uint32_t height)
{
    return detect_refresh_from_edid(edid, edid_size, width, height).refresh_hz;
}

bool display_detect_exact_refresh_millihz_from_edid_hint(const uint8_t *edid, uint64_t edid_size, uint32_t width,
                                                         uint32_t height, uint32_t refresh_hint_millihz,
                                                         uint32_t *out_refresh_millihz)
{
    if (!out_refresh_millihz)
        return false;

    EdidRefreshDecision decision = detect_refresh_from_edid(edid, edid_size, width, height, refresh_hint_millihz);
    if (!decision.exact_match)
        return false;
    *out_refresh_millihz = decision.refresh_millihz;
    return true;
}

int display_detect_modes_from_edid(const uint8_t *edid, uint64_t edid_size, DisplayMode *out_modes, uint32_t max_modes,
                                   const DisplayMode *current_mode, uint32_t measured_refresh_millihz)
{
    if (!out_modes || max_modes == 0)
        return 0;

    EdidTimingList timings = {};
    uint32_t max_range_refresh = 0;
    if (!collect_edid_timings(edid, edid_size, &timings, &max_range_refresh))
        return 0;

    uint32_t count = 0;
    bool marked_preferred = false;
    int current_index = -1;
    uint32_t preferred_timing_index = 0;
    bool have_preferred_timing = false;
    for (uint32_t i = 0; i < timings.count; i++) {
        const EdidTiming &timing = timings.timings[i];
        if (preferred_mode_timing_better(timing, timings.timings[preferred_timing_index], have_preferred_timing)) {
            preferred_timing_index = i;
            have_preferred_timing = true;
        }
    }

    for (uint32_t i = 0; i < timings.count && count < max_modes; i++) {
        const EdidTiming &timing = timings.timings[i];
        uint32_t flags = 0;
        if (have_preferred_timing && i == preferred_timing_index) {
            flags |= DISPLAY_MODE_FLAG_PREFERRED;
            marked_preferred = true;
        }
        if (timing_matches_current_mode(timing, current_mode)) {
            flags |= DISPLAY_MODE_FLAG_CURRENT;
            current_index = (int)count;
        }
        populate_mode_from_timing(timing, count + 1u, flags, measured_refresh_millihz, &out_modes[count]);
        count++;
    }

    if (count == 0)
        return 0;

    if (!marked_preferred) {
        out_modes[0].flags |= DISPLAY_MODE_FLAG_PREFERRED;
        marked_preferred = true;
    }

    if (current_index < 0 && current_mode && count < max_modes && current_mode->width != 0 &&
        current_mode->height != 0) {
        DisplayMode synthetic_current = *current_mode;
        synthetic_current.mode_id = count + 1u;
        synthetic_current.flags |= DISPLAY_MODE_FLAG_CURRENT;
        synthetic_current.measured_refresh_millihz = measured_refresh_millihz;
        if ((synthetic_current.flags & DISPLAY_MODE_FLAG_PREFERRED) == 0 && !marked_preferred)
            synthetic_current.flags |= DISPLAY_MODE_FLAG_PREFERRED;
        out_modes[count] = synthetic_current;
        current_index = (int)count;
        count++;
    }

    if (current_index < 0) {
        out_modes[0].flags |= DISPLAY_MODE_FLAG_CURRENT;
        out_modes[0].measured_refresh_millihz = measured_refresh_millihz;
    }

    return (int)count;
}

static EdidRefreshDecision parse_edid_refresh_decision(const BootFramebuffer *fb)
{
    if (!fb)
        return {DEFAULT_FALLBACK_REFRESH_HZ,
                DEFAULT_FALLBACK_REFRESH_MILLIHZ,
                EdidTimingSource::BaseStandard,
                false,
                true,
                false,
                0};

    return detect_refresh_from_edid((const uint8_t *)fb->edid, fb->edid_size, (uint32_t)fb->width,
                                    (uint32_t)fb->height);
}

static void log_edid_refresh_decision(const BootFramebuffer *fb, const EdidRefreshDecision &decision)
{
    if (!fb)
        return;

    char frac_buf[4];
    if (decision.used_fallback) {
        if (fb->edid && fb->edid_size >= 128) {
            BOOT_LOG("Display: EDID did not provide an exact refresh for %ux%u; using default %u.%s Hz",
                     (uint32_t)fb->width, (uint32_t)fb->height, decision.refresh_millihz / 1000u,
                     format_millihz_fraction(decision.refresh_millihz, frac_buf));
        } else {
            BOOT_LOG("Display: exact refresh unavailable for %ux%u; using default %u.%s Hz", (uint32_t)fb->width,
                     (uint32_t)fb->height, decision.refresh_millihz / 1000u,
                     format_millihz_fraction(decision.refresh_millihz, frac_buf));
        }
        return;
    }

    if (decision.used_range_limits) {
        BOOT_LOG("Display: refresh estimated %u.%s Hz for %ux%u from panel limits", decision.refresh_millihz / 1000u,
                 format_millihz_fraction(decision.refresh_millihz, frac_buf), (uint32_t)fb->width,
                 (uint32_t)fb->height);
        return;
    }

    BOOT_LOG("Display: refresh inferred from exact %s timing = %u.%s Hz for %ux%u (%u candidate%s)",
             edid_timing_source_name(decision.source), decision.refresh_millihz / 1000u,
             format_millihz_fraction(decision.refresh_millihz, frac_buf), (uint32_t)fb->width, (uint32_t)fb->height,
             decision.exact_candidate_count, decision.exact_candidate_count == 1 ? "" : "s");
}

static bool resolve_exact_boot_handoff_refresh(const BootFramebuffer *fb, uint32_t *out_refresh_hz,
                                               uint32_t *out_refresh_millihz)
{
    if (!fb || !out_refresh_hz || !out_refresh_millihz)
        return false;

    BootDisplayTiming timing = {};
    if (!boot_display_timing_get(&timing))
        return false;
    if (timing.width != (uint32_t)fb->width || timing.height != (uint32_t)fb->height) {
        BOOT_WARN("Display: exact boot timing handoff %ux%u does not match framebuffer %ux%u; ignoring", timing.width,
                  timing.height, (uint32_t)fb->width, (uint32_t)fb->height);
        return false;
    }

    *out_refresh_millihz = timing.refresh_millihz;
    *out_refresh_hz = round_millihz_to_hz(timing.refresh_millihz);
    char frac_buf[4];
    BOOT_LOG("Display: using exact boot handoff refresh = %u.%s Hz for %ux%u", timing.refresh_millihz / 1000u,
             format_millihz_fraction(timing.refresh_millihz, frac_buf), timing.width, timing.height);
    return *out_refresh_hz != 0;
}

static bool resolve_exact_boot_handoff_mode(const BootFramebuffer *fb, DisplayMode *out_mode)
{
    if (!fb || !out_mode)
        return false;

    BootDisplayTiming timing = {};
    if (!boot_display_timing_get(&timing))
        return false;
    if (timing.width != (uint32_t)fb->width || timing.height != (uint32_t)fb->height)
        return false;

    out_mode->refresh_hz = round_millihz_to_hz(timing.refresh_millihz);
    out_mode->nominal_refresh_millihz = timing.refresh_millihz;
    out_mode->pixel_clock_khz = timing.pixel_clock_khz;
    out_mode->htotal = timing.h_total;
    out_mode->vtotal = timing.v_total;
    out_mode->hblank_start = 0;
    out_mode->hblank_end = 0;
    out_mode->hsync_start = 0;
    out_mode->hsync_end = 0;
    out_mode->vblank_start = 0;
    out_mode->vblank_end = 0;
    out_mode->vsync_start = 0;
    out_mode->vsync_end = 0;
    if (timing.interlaced)
        out_mode->flags |= DISPLAY_MODE_FLAG_INTERLACED;
    return true;
}

static void setup_boot_caps(DisplayCaps *caps, const BootFramebuffer *fb)
{
    if (!caps)
        return;
    *caps = {};
    if (!fb)
        return;

    caps->width = (uint32_t)fb->width;
    caps->height = (uint32_t)fb->height;
    caps->pitch = (uint32_t)fb->pitch;
    caps->bpp = (uint32_t)fb->bpp;
    caps->pixel_format = DISPLAY_PIXEL_FORMAT_XRGB8888;
    EdidRefreshDecision decision = parse_edid_refresh_decision(fb);
    bool applied_handoff = false;
    if (!decision.exact_match) {
        applied_handoff = resolve_exact_boot_handoff_refresh(fb, &caps->refresh_hz, &caps->refresh_millihz);
    }
    if (!applied_handoff) {
        log_edid_refresh_decision(fb, decision);
        caps->refresh_hz = decision.refresh_hz;
        caps->refresh_millihz = decision.refresh_millihz;
    }
    if (caps->refresh_hz == 0)
        caps->refresh_hz = DEFAULT_FALLBACK_REFRESH_HZ;
    caps->refresh_millihz = display_refresh_millihz_or_default(caps->refresh_millihz);
    caps->nominal_refresh_millihz = caps->refresh_millihz;
    caps->measured_refresh_millihz = 0;
    caps->flags = DISPLAY_FLAG_USES_COPY_PATH;
}

static inline DisplayHead *primary_head()
{
    return &s_device.primary_head;
}

static void sync_mode_from_caps(DisplayHead *head)
{
    if (!head)
        return;

    DisplayMode mode = {};
    bool preserve_exact_geometry =
        head->current_mode.width == head->caps.width && head->current_mode.height == head->caps.height &&
        head->current_mode.pixel_clock_khz != 0 && head->current_mode.htotal != 0 && head->current_mode.vtotal != 0;
    if (preserve_exact_geometry)
        mode = head->current_mode;
    mode.mode_id = 1;
    mode.width = head->caps.width;
    mode.height = head->caps.height;
    mode.refresh_hz = head->caps.refresh_hz;
    mode.nominal_refresh_millihz =
        head->caps.nominal_refresh_millihz ? head->caps.nominal_refresh_millihz : head->caps.refresh_millihz;
    mode.measured_refresh_millihz = head->caps.measured_refresh_millihz;
    if (!preserve_exact_geometry) {
        mode.htotal = head->caps.width;
        mode.vtotal = head->caps.height;
        mode.hblank_start = 0;
        mode.hblank_end = 0;
        mode.hsync_start = 0;
        mode.hsync_end = 0;
        mode.vblank_start = 0;
        mode.vblank_end = 0;
        mode.vsync_start = 0;
        mode.vsync_end = 0;
        mode.flags &= ~(uint32_t)DISPLAY_MODE_FLAG_EXACT_TIMING;
    }
    mode.flags |= DISPLAY_MODE_FLAG_CURRENT;
    resolve_exact_boot_handoff_mode(s_device.boot_framebuffer, &mode);
    display_set_head_modes(head, mode, mode);
}

static void sync_primary_topology(DisplayDevice *device)
{
    if (!device)
        return;

    sync_mode_from_caps(&device->primary_head);

    DisplayConnectorInfo preserved_connector_info = {};
    uint32_t preserved_encoder_index = 0u;
    DisplayEncoder preserved_encoder = {};
    bool have_preserved_connector = device->connector_count > 0;
    bool have_preserved_encoder = device->encoder_count > 0;
    if (have_preserved_connector) {
        preserved_connector_info = device->connectors[0].info;
        preserved_encoder_index = device->connectors[0].encoder_index;
    }
    if (have_preserved_encoder)
        preserved_encoder = device->encoders[0];

    device->connector_count = 1;
    DisplayConnector &connector = device->connectors[0];
    connector = {};
    connector.info = preserved_connector_info;
    if (connector.info.connector_id == 0)
        connector.info.connector_id = 1;
    if (connector.info.connector_type == 0)
        connector.info.connector_type = DISPLAY_CONNECTOR_TYPE_UNKNOWN;
    connector.info.status = DISPLAY_CONNECTOR_STATUS_CONNECTED;
    if (connector.info.name[0] == '\0')
        kstring::strncpy(connector.info.name, "display-0", sizeof(connector.info.name) - 1);
    int detected_modes = 0;
    if (device->boot_framebuffer && device->boot_framebuffer->edid && device->boot_framebuffer->edid_size >= 128u) {
        detected_modes = display_detect_modes_from_edid(
            (const uint8_t *)device->boot_framebuffer->edid, device->boot_framebuffer->edid_size, connector.modes,
            (uint32_t)(sizeof(connector.modes) / sizeof(connector.modes[0])), &device->primary_head.current_mode,
            device->primary_head.caps.measured_refresh_millihz);
    }
    DisplayMode current_mode = device->primary_head.current_mode;
    DisplayMode preferred_mode = device->primary_head.preferred_mode.width != 0 ? device->primary_head.preferred_mode
                                                                                : device->primary_head.current_mode;

    if (detected_modes <= 0) {
        connector.modes[0] = preferred_mode;
        connector.modes[0].mode_id = 1u;
        connector.modes[0].flags |= DISPLAY_MODE_FLAG_PREFERRED;
        connector.modes[0].measured_refresh_millihz = 0u;
        connector.mode_count = 1;
        connector.info.active_mode_id = current_mode.mode_id != 0 ? current_mode.mode_id : connector.modes[0].mode_id;
    } else {
        connector.mode_count = (uint32_t)detected_modes;
        uint32_t active_mode_id = 0u;
        uint32_t preferred_mode_id = 0u;
        uint32_t preferred_refresh = 0u;

        for (uint32_t i = 0; i < connector.mode_count; i++) {
            const DisplayMode &m = connector.modes[i];
            uint32_t m_refresh = m.nominal_refresh_millihz ? m.nominal_refresh_millihz : hz_to_millihz(m.refresh_hz);
            bool is_preferred = (m.flags & DISPLAY_MODE_FLAG_PREFERRED) != 0;
            bool is_current = (m.flags & DISPLAY_MODE_FLAG_CURRENT) != 0;

            if (is_current) {
                active_mode_id = m.mode_id;
                current_mode = m;
            }
            if (is_preferred && m_refresh >= preferred_refresh) {
                preferred_mode_id = m.mode_id;
                preferred_refresh = m_refresh;
                preferred_mode = m;
            }
        }

        if (active_mode_id == 0u)
            active_mode_id = current_mode.mode_id != 0u
                                 ? current_mode.mode_id
                                 : (preferred_mode_id != 0u ? preferred_mode_id : connector.modes[0].mode_id);
        connector.info.active_mode_id = active_mode_id;
    }
    connector.encoder_index = preserved_encoder_index;
    display_set_head_modes(&device->primary_head, current_mode, preferred_mode);

    device->encoder_count = 1;
    device->encoders[0] = preserved_encoder;
    if (device->encoders[0].encoder_id == 0)
        device->encoders[0].encoder_id = 1;

    device->crtc_count = 1;
    device->crtcs[0] = {};
    device->crtcs[0].crtc_id = 1;
    device->crtcs[0].current_mode = device->primary_head.current_mode;
    device->crtcs[0].last_vblank_ticks = device->primary_head.status.last_vblank_ticks;

    device->plane_count = 1;
    device->planes[0] = {};
    device->planes[0].info.plane_id = 1;
    device->planes[0].info.plane_type = DISPLAY_PLANE_TYPE_PRIMARY;
    device->planes[0].info.flags = 0;
    device->planes[0].buffer = device->primary_head.buffers[0];
    device->planes[0].src_rect =
        gui_rect_make(0, 0, (int32_t)device->primary_head.caps.width, (int32_t)device->primary_head.caps.height);
    device->planes[0].dst_rect = device->planes[0].src_rect;
}

static bool display_mode_matches_head(const DisplayHead *head, const DisplayMode &mode)
{
    if (!head)
        return false;
    return mode.width == head->caps.width && mode.height == head->caps.height &&
           mode.nominal_refresh_millihz ==
               (head->caps.nominal_refresh_millihz ? head->caps.nominal_refresh_millihz : head->caps.refresh_millihz);
}

static DisplayConnector *display_find_connector(DisplayDevice *device, uint32_t connector_id)
{
    if (!device)
        return nullptr;
    for (uint32_t i = 0; i < device->connector_count; i++) {
        if (device->connectors[i].info.connector_id == connector_id)
            return &device->connectors[i];
    }
    return nullptr;
}

static DisplayMode *display_find_mode(DisplayConnector *connector, const DisplayMode &requested)
{
    if (!connector)
        return nullptr;

    if (requested.mode_id != 0) {
        for (uint32_t i = 0; i < connector->mode_count; i++) {
            if (connector->modes[i].mode_id == requested.mode_id)
                return &connector->modes[i];
        }
    }

    for (uint32_t i = 0; i < connector->mode_count; i++) {
        DisplayMode &candidate = connector->modes[i];
        if (candidate.width != requested.width || candidate.height != requested.height)
            continue;
        if (requested.nominal_refresh_millihz != 0 &&
            candidate.nominal_refresh_millihz == requested.nominal_refresh_millihz)
            return &candidate;
        if (requested.refresh_hz != 0 && candidate.refresh_hz == requested.refresh_hz)
            return &candidate;
    }

    return nullptr;
}

static bool display_build_atomic_mode_state(DisplayDevice *device, const DisplayMode &requested,
                                            DisplayAtomicState *out_state, DisplayMode *out_mode)
{
    if (!device || !out_state || !out_mode)
        return false;

    for (uint32_t i = 0; i < device->connector_count; i++) {
        DisplayConnector &connector = device->connectors[i];
        if (connector.info.status != DISPLAY_CONNECTOR_STATUS_CONNECTED)
            continue;

        DisplayMode *resolved = display_find_mode(&connector, requested);
        if (!resolved)
            continue;

        *out_state = {};
        out_state->connector_id = connector.info.connector_id;
        out_state->mode_id = resolved->mode_id;
        out_state->modeset = true;
        out_state->page_flip = false;
        out_state->damage_bounds = gui_rect_make(0, 0, (int32_t)resolved->width, (int32_t)resolved->height);
        *out_mode = *resolved;
        return true;
    }

    return false;
}

static bool display_resolve_atomic_request(DisplayDevice *device, const DisplayAtomicRequest &request,
                                           DisplayAtomicState *out_state, DisplayMode *out_mode)
{
    if (!device || !out_state || !out_mode)
        return false;

    DisplayConnector *connector = display_find_connector(device, request.connector_id);
    if (!connector || connector->info.status != DISPLAY_CONNECTOR_STATUS_CONNECTED)
        return false;

    DisplayMode requested = {};
    requested.mode_id = request.mode_id;
    const DisplayMode *resolved = display_find_mode(connector, requested);
    if (!resolved)
        return false;

    *out_state = {};
    out_state->connector_id = connector->info.connector_id;
    out_state->mode_id = resolved->mode_id;
    out_state->modeset = (request.flags & DISPLAY_ATOMIC_MODESET) != 0;
    out_state->page_flip = (request.flags & DISPLAY_ATOMIC_PAGE_FLIP) != 0;
    out_state->damage_bounds = request.damage_bounds;
    *out_mode = *resolved;
    return true;
}

static void display_apply_committed_mode(DisplayDevice *device, const DisplayAtomicState &state,
                                         const DisplayMode &mode)
{
    if (!device)
        return;

    DisplayConnector *connector = display_find_connector(device, state.connector_id);
    DisplayMode current_mode = mode;
    DisplayMode preferred_mode =
        device->primary_head.preferred_mode.width != 0 ? device->primary_head.preferred_mode : mode;
    if (connector) {
        connector->info.active_mode_id = mode.mode_id;
        for (uint32_t i = 0; i < connector->mode_count; i++) {
            if (connector->modes[i].mode_id == mode.mode_id) {
                connector->modes[i] = mode;
                connector->modes[i].flags |= DISPLAY_MODE_FLAG_CURRENT;
            } else {
                connector->modes[i].flags &= ~(uint32_t)DISPLAY_MODE_FLAG_CURRENT;
            }
            if ((connector->modes[i].flags & DISPLAY_MODE_FLAG_PREFERRED) != 0)
                preferred_mode = connector->modes[i];
        }
    }

    display_set_head_modes(&device->primary_head, current_mode, preferred_mode);
    device->primary_head.caps.width = mode.width;
    device->primary_head.caps.height = mode.height;
    device->primary_head.caps.refresh_hz = mode.refresh_hz;
    device->primary_head.caps.refresh_millihz = mode.nominal_refresh_millihz;
    device->primary_head.caps.nominal_refresh_millihz = mode.nominal_refresh_millihz;
    device->primary_head.caps.measured_refresh_millihz = 0u;
    if (device->primary_head.caps.bpp != 0)
        device->primary_head.caps.pitch = mode.width * (device->primary_head.caps.bpp / 8u);

    if (device->crtc_count > 0) {
        device->crtcs[0].current_mode = mode;
        device->crtcs[0].last_vblank_ticks = device->primary_head.status.last_vblank_ticks;
    }
    if (device->plane_count > 0) {
        device->planes[0].src_rect = gui_rect_make(0, 0, (int32_t)mode.width, (int32_t)mode.height);
        device->planes[0].dst_rect = device->planes[0].src_rect;
    }
}

static bool display_set_mode_noop(DisplayDevice *device, const DisplayMode &mode)
{
    return device && display_mode_matches_head(&device->primary_head, mode);
}

static void retire_compose_buffers(DisplayDevice *device)
{
    if (!device)
        return;

    uint32_t completed = device->primary_head.status.completed_sequence;
    for (ComposeBufferSlot &slot : s_compose_buffers) {
        if (slot.in_flight_sequence != 0 && slot.in_flight_sequence <= completed)
            slot.in_flight_sequence = 0;
    }
}

static ComposeBufferSlot *acquire_compose_buffer(DisplayDevice *device)
{
    if (!device)
        return nullptr;

    uint32_t stride = device->primary_head.caps.pitch / 4u;
    uint64_t bytes = display_buffer_bytes(stride, device->primary_head.caps.height);
    retire_compose_buffers(device);

    for (uint32_t offset = 0; offset < MAX_COMPOSE_BUFFERS; offset++) {
        uint32_t index = (s_compose_active_index + offset) % MAX_COMPOSE_BUFFERS;
        ComposeBufferSlot &slot = s_compose_buffers[index];
        if (slot.in_flight_sequence != 0)
            continue;

        if (slot.dma.virt != 0 && slot.dma.size < bytes) {
            vmm_free_dma(slot.dma);
            slot.dma = {};
        }
        if (slot.dma.virt == 0) {
            uint64_t pages = (bytes + 4095u) / 4096u;
            slot.dma = vmm_alloc_dma_with_flags((size_t)pages, display_compose_buffer_page_flags(device));
            if (slot.dma.virt == 0)
                continue;
            kstring::zero_memory(reinterpret_cast<void *>(slot.dma.virt), slot.dma.size);
        }

        slot.buffer.cpu_mapping = reinterpret_cast<uint32_t *>(slot.dma.virt);
        slot.buffer.phys_addr = slot.dma.phys;
        slot.buffer.stride = stride;
        slot.buffer.width = device->primary_head.caps.width;
        slot.buffer.height = device->primary_head.caps.height;

        s_compose_active_index = (index + 1u) % MAX_COMPOSE_BUFFERS;
        return &slot;
    }

    return nullptr;
}

static DisplayBufferObject *display_buffer_lookup(DisplayBufferHandle handle, uint64_t owner_pid)
{
    spinlock_acquire(&s_display_buffer_lock);
    DisplayBufferObject *buffer = display_find_buffer_locked(handle);
    if (!buffer || (owner_pid != 0 && buffer->owner_pid != owner_pid)) {
        spinlock_release(&s_display_buffer_lock);
        return nullptr;
    }
    spinlock_release(&s_display_buffer_lock);
    return buffer;
}

static DisplayBuffer display_buffer_view_from_object(const DisplayBufferObject &buffer)
{
    DisplayBuffer view = {};
    view.cpu_mapping = reinterpret_cast<uint32_t *>(buffer.dma.virt);
    view.phys_addr = buffer.dma.phys;
    view.stride = buffer.stride;
    view.width = buffer.width;
    view.height = buffer.height;
    view.full_frame_valid = true;
    return view;
}

static uint32_t display_predict_present_sequence(DisplayHead *head, uint32_t requested_sequence)
{
    if (!head)
        return requested_sequence;
    return requested_sequence != 0 ? requested_sequence : display_next_auto_sequence(head);
}

static int display_assign_head_buffer_slot(DisplayHead *head, const DisplayBuffer &buffer)
{
    if (!head || !buffer.cpu_mapping || buffer.stride == 0)
        return -1;

    for (uint32_t i = 0; i < head->buffer_count; i++) {
        if (display_buffers_match(head->buffers[i], buffer)) {
            head->buffers[i] = buffer;
            return (int)i;
        }
    }

    if (head->buffer_count < (uint32_t)(sizeof(head->buffers) / sizeof(head->buffers[0]))) {
        uint32_t slot = head->buffer_count++;
        head->buffers[slot] = buffer;
        head->buffer_in_flight_sequence[slot] = 0;
        return (int)slot;
    }

    uint32_t slot = (head->front_buffer_index + 1u) % head->buffer_count;
    for (uint32_t attempt = 0; attempt < head->buffer_count; attempt++) {
        uint32_t index = (slot + attempt) % head->buffer_count;
        if (index == head->front_buffer_index)
            continue;
        if (head->pending_flip && index == head->pending_buffer_index)
            continue;
        if (head->buffer_in_flight_sequence[index] != 0 &&
            head->buffer_in_flight_sequence[index] > head->status.completed_sequence)
            continue;
        head->buffers[index] = buffer;
        head->buffer_in_flight_sequence[index] = 0;
        return (int)index;
    }

    return -1;
}

static void display_note_pending_present_buffer(DisplayDevice *device, const DisplayBuffer &buffer, uint32_t sequence)
{
    if (!device || sequence == 0)
        return;

    DisplayHead *head = &device->primary_head;
    int slot = display_assign_head_buffer_slot(head, buffer);
    if (slot < 0)
        return;

    head->pending_buffer_index = (uint32_t)slot;
    head->pending_flip_sequence = sequence;
    head->pending_flip = true;
    head->buffer_in_flight_sequence[head->pending_buffer_index] = sequence;
    if (device->plane_count > 0)
        device->planes[0].buffer = head->buffers[head->pending_buffer_index];
}

static void display_cancel_pending_present_buffer(DisplayHead *head, uint32_t sequence)
{
    if (!head)
        return;
    if (head->pending_flip && head->pending_flip_sequence == sequence) {
        if (head->pending_buffer_index < head->buffer_count &&
            head->buffer_in_flight_sequence[head->pending_buffer_index] == sequence)
            head->buffer_in_flight_sequence[head->pending_buffer_index] = 0;
        head->pending_flip = false;
        head->pending_flip_sequence = 0;
    }
}

static bool display_present_buffer_tracks_front(const DisplayDevice *device)
{
    if (!device || !device->ops || !device->ops->present_buffer)
        return false;

    const uint32_t flags = device->primary_head.caps.flags;
    return (flags & DISPLAY_FLAG_HAS_PAGE_FLIP) != 0 && (flags & DISPLAY_FLAG_USES_COPY_PATH) == 0;
}

static uint32_t display_present_buffer_internal(DisplayDevice *device, const DisplayBuffer &buffer,
                                                const Rect *damage_rects, uint32_t damage_count,
                                                uint32_t frame_sequence, uint32_t flags)
{
    if (!device || !device->ops || !buffer.cpu_mapping || buffer.stride == 0)
        return device ? device->primary_head.status.completed_sequence : 0;

    uint32_t predicted_sequence = display_predict_present_sequence(&device->primary_head, frame_sequence);
    bool tracks_front_buffer = display_present_buffer_tracks_front(device);
    if (tracks_front_buffer) {
        // Copy backends update the boot framebuffer, not the scanout pointer. Only real page
        // flips may advance front_buffer_index; otherwise partial composes can seed from stale
        // WM buffers and show colored stripe/grid artifacts.
        display_note_pending_present_buffer(device, buffer, predicted_sequence);
    }

    if (device->ops->present_buffer) {
        uint32_t submitted =
            device->ops->present_buffer(device, buffer, damage_rects, damage_count, frame_sequence, flags);
        if (tracks_front_buffer && submitted == 0) {
            display_cancel_pending_present_buffer(&device->primary_head, predicted_sequence);
        } else if (tracks_front_buffer && submitted != predicted_sequence) {
            device->primary_head.pending_flip_sequence = submitted;
        }
        return submitted;
    }

    Rect full_damage = gui_rect_make(0, 0, (int32_t)buffer.width, (int32_t)buffer.height);
    DisplayPresentRequest present = {};
    present.buffer = buffer.cpu_mapping;
    present.stride = buffer.stride;
    present.rects = damage_count ? damage_rects : &full_damage;
    present.rect_count = damage_count ? damage_count : 1u;
    present.frame_sequence = frame_sequence;
    present.flags = flags;
    present.source_origin_x = 0;
    present.source_origin_y = 0;
    uint32_t submitted = device->ops->present(device, present);
    if (tracks_front_buffer && submitted == 0) {
        display_cancel_pending_present_buffer(&device->primary_head, predicted_sequence);
    } else if (tracks_front_buffer && submitted != predicted_sequence) {
        device->primary_head.pending_flip_sequence = submitted;
    }
    return submitted;
}

static int display_buffer_copy_out(DisplayBufferObject *buffer, DisplayBufferCreate *request)
{
    if (!buffer || !request)
        return -1;
    request->stride = buffer->stride;
    request->handle = buffer->handle;
    request->size_bytes = display_buffer_bytes(buffer->stride, buffer->height);
    return 0;
}

static bool display_map_buffer_into_process(DisplayBufferObject *buffer, DisplayBufferMap *request)
{
    if (!buffer || !request)
        return false;

    Process *process = process_get_current();
    if (!process || !process->page_table)
        return false;

    uint64_t size = display_buffer_bytes(buffer->stride, buffer->height);
    if (size == 0 || size > DISPLAY_BUFFER_MAP_SLOT_SIZE)
        return false;

    uint64_t virt_start = display_buffer_map_base(buffer->handle);
    if (buffer->mapped_pid == process->pid && buffer->mapped_user_addr == virt_start) {
        request->address = virt_start;
        request->size_bytes = size;
        request->stride = buffer->stride;
        request->width = buffer->width;
        request->height = buffer->height;
        return true;
    }
    if (buffer->mapped_pid != 0 && buffer->mapped_pid != process->pid)
        return false;

    spinlock_acquire(&process->vma_lock);
    const VMA *existing = vma_find(process->vma_list, virt_start);
    if (existing) {
        bool same_mapping =
            existing->start == virt_start && existing->end >= virt_start + size && (existing->flags & PTE_SHARED);
        spinlock_release(&process->vma_lock);
        if (!same_mapping)
            return false;
        buffer->mapped_pid = process->pid;
        buffer->mapped_user_addr = virt_start;
        buffer->mapped_user_size = size;
        request->address = virt_start;
        request->size_bytes = size;
        request->stride = buffer->stride;
        request->width = buffer->width;
        request->height = buffer->height;
        return true;
    }
    spinlock_release(&process->vma_lock);

    uint64_t flags = display_buffer_user_page_flags(*buffer);
    for (uint64_t mapped = 0; mapped < size; mapped += 4096u) {
        uint64_t phys = buffer->dma.phys + mapped;
        pmm_refcount_inc(reinterpret_cast<void *>(phys));
        if (!vmm_map_page_in(process->page_table, virt_start + mapped, phys, flags).ok()) {
            pmm_refcount_dec(reinterpret_cast<void *>(phys));
            for (uint64_t rollback = 0; rollback < mapped; rollback += 4096u) {
                vmm_unmap_page_in(process->page_table, virt_start + rollback);
                pmm_refcount_dec(reinterpret_cast<void *>(buffer->dma.phys + rollback));
            }
            asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
            return false;
        }
    }

    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    spinlock_acquire(&process->vma_lock);
    bool vma_ok = vma_add(&process->vma_list, virt_start, virt_start + size, flags, VMAType::Shared) != nullptr;
    spinlock_release(&process->vma_lock);
    if (!vma_ok) {
        for (uint64_t rollback = 0; rollback < size; rollback += 4096u) {
            vmm_unmap_page_in(process->page_table, virt_start + rollback);
            pmm_refcount_dec(reinterpret_cast<void *>(buffer->dma.phys + rollback));
        }
        asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
        return false;
    }

    buffer->mapped_pid = process->pid;
    buffer->mapped_user_addr = virt_start;
    buffer->mapped_user_size = size;
    request->address = virt_start;
    request->size_bytes = size;
    request->stride = buffer->stride;
    request->width = buffer->width;
    request->height = buffer->height;
    return true;
}

static void display_unmap_buffer_from_current_process(DisplayBufferObject *buffer)
{
    if (!buffer)
        return;

    Process *process = process_get_current();
    if (!process || !process->page_table || buffer->mapped_pid != process->pid || buffer->mapped_user_addr == 0)
        return;

    uint64_t size = buffer->mapped_user_size;
    uint64_t virt_start = buffer->mapped_user_addr;
    spinlock_acquire(&process->vma_lock);
    vma_remove(&process->vma_list, virt_start, virt_start + size);
    spinlock_release(&process->vma_lock);

    for (uint64_t i = 0; i < size; i += 4096u) {
        vmm_unmap_page_in(process->page_table, virt_start + i);
        pmm_refcount_dec(reinterpret_cast<void *>(buffer->dma.phys + i));
    }
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    buffer->mapped_pid = 0;
    buffer->mapped_user_addr = 0;
    buffer->mapped_user_size = 0;
}

static uint32_t compose_alpha_scale(uint32_t alpha, uint32_t global_alpha)
{
    return (alpha * global_alpha + 127u) / 255u;
}

static uint32_t compose_blend_pixel(uint32_t dst, uint32_t src, uint32_t global_alpha, bool opaque)
{
    uint32_t src_alpha = opaque ? 255u : ((src >> 24) & 0xFFu);
    src_alpha = compose_alpha_scale(src_alpha, global_alpha);
    if (src_alpha >= 255u)
        return 0xFF000000u | (src & 0x00FFFFFFu);
    if (src_alpha == 0u)
        return dst;

    uint32_t dst_r = (dst >> 16) & 0xFFu;
    uint32_t dst_g = (dst >> 8) & 0xFFu;
    uint32_t dst_b = dst & 0xFFu;
    uint32_t src_r = (src >> 16) & 0xFFu;
    uint32_t src_g = (src >> 8) & 0xFFu;
    uint32_t src_b = src & 0xFFu;
    uint32_t inv_alpha = 255u - src_alpha;

    uint32_t out_r = (src_r * src_alpha + dst_r * inv_alpha + 127u) / 255u;
    uint32_t out_g = (src_g * src_alpha + dst_g * inv_alpha + 127u) / 255u;
    uint32_t out_b = (src_b * src_alpha + dst_b * inv_alpha + 127u) / 255u;
    return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
}

static uint32_t compose_fast_single_layer(DisplayDevice *device, const DisplayComposeRequest &request,
                                          const Rect *damage_rects, uint32_t damage_count)
{
    if (!device || !request.layers || request.layer_count != 1 || request.cursor_buffer_handle != 0)
        return 0;

    const DisplayComposeLayer &layer = request.layers[0];
    if (layer.buffer_handle == 0 || (layer.flags & DISPLAY_COMPOSE_LAYER_CURSOR) != 0 || layer.alpha != 255u ||
        (layer.flags & DISPLAY_COMPOSE_LAYER_OPAQUE) == 0) {
        return 0;
    }
    if (layer.dst_rect.x != 0 || layer.dst_rect.y != 0 ||
        layer.dst_rect.w != (int32_t)device->primary_head.caps.width ||
        layer.dst_rect.h != (int32_t)device->primary_head.caps.height) {
        return 0;
    }

    Process *process = process_get_current();
    uint64_t owner_pid = process ? process->pid : 0;
    DisplayBuffer present_buffer = {};
    if (!display_buffer_lookup_snapshot(layer.buffer_handle, owner_pid, &present_buffer))
        return 0;

    if (layer.src_rect.x != 0 || layer.src_rect.y != 0 || layer.src_rect.w != (int32_t)present_buffer.width ||
        layer.src_rect.h != (int32_t)present_buffer.height) {
        return 0;
    }

    return display_present_buffer_internal(device, present_buffer, damage_rects, damage_count, request.frame_sequence,
                                           request.flags);
}

static uint32_t compose_submit_backend(DisplayDevice *device, const DisplayComposeRequest &request,
                                   const Rect *damage_rects, uint32_t damage_count, PresentBackendResult *out_result)
{
    if (out_result)
        *out_result = PresentBackendResult::PRESENT_CPU_FALLBACK;
    if (!device || !device->ops || !device->ops->backend_compose_begin || !device->ops->backend_compose_end ||
        !device->ops->backend_compose_submit) {
        return 0;
    }
    if (request.cursor_buffer_handle != 0 && !device->ops->backend_compose_cursor)
        return 0;

    device->primary_head.backend_compose_attempt_count++;

    ComposeBufferSlot *compose_slot = acquire_compose_buffer(device);
    if (!compose_slot) {
        device->primary_head.backend_compose_failure_count++;
        return 0;
    }

    compose_slot->buffer.cpu_mapping = reinterpret_cast<uint32_t *>(compose_slot->dma.virt);
    compose_slot->buffer.stride = device->primary_head.caps.pitch / 4u;
    compose_slot->buffer.width = device->primary_head.caps.width;
    compose_slot->buffer.height = device->primary_head.caps.height;
    compose_slot->buffer.full_frame_valid = true;

    Process *process = process_get_current();
    DisplayComposeFrameContext frame_ctx = {};
    frame_ctx.owner_pid = process ? process->pid : 0;
    frame_ctx.frame_sequence = request.frame_sequence;
    frame_ctx.flags = request.flags;
    frame_ctx.layer_count = request.layer_count;
    frame_ctx.target_buffer = compose_slot->buffer;
    frame_ctx.damage_rects = damage_rects;
    frame_ctx.damage_rect_count = damage_count;
    frame_ctx.cursor_buffer_handle = request.cursor_buffer_handle;
    frame_ctx.cursor_x = request.cursor_x;
    frame_ctx.cursor_y = request.cursor_y;

    if (!device->ops->backend_compose_begin(device, &compose_slot->buffer, frame_ctx)) {
        device->primary_head.backend_compose_failure_count++;
        return 0;
    }

    Rect full_rect = gui_rect_make(0, 0, (int32_t)compose_slot->buffer.width, (int32_t)compose_slot->buffer.height);
    if (device->ops->backend_compose_clear)
        device->ops->backend_compose_clear(device, 0xFF000000u, full_rect);

    for (uint32_t i = 0; i < request.layer_count; i++) {
        if (device->ops->backend_compose_layer && !device->ops->backend_compose_layer(device, request.layers[i])) {
            device->primary_head.backend_compose_failure_count++;
            return 0;
        }
    }

    if (request.cursor_buffer_handle != 0 && device->ops->backend_compose_cursor &&
        !device->ops->backend_compose_cursor(device, request.cursor_buffer_handle, request.cursor_x, request.cursor_y)) {
        device->primary_head.backend_compose_failure_count++;
        return 0;
    }

    if (!device->ops->backend_compose_end(device)) {
        device->primary_head.backend_compose_failure_count++;
        return 0;
    }

    PresentBackendResult backend_result = PresentBackendResult::PRESENT_CPU_FALLBACK;
    uint32_t submitted = device->ops->backend_compose_submit(device, &backend_result);
    if (submitted != 0 && backend_result == PresentBackendResult::PRESENT_BACKEND_COMPOSED) {
        compose_slot->in_flight_sequence = submitted;
        device->primary_head.backend_compose_path_count++;
    } else {
        device->primary_head.backend_compose_failure_count++;
    }
    if (out_result)
        *out_result = backend_result;
    return submitted;
}

static void compose_cursor_cpu(DisplayDevice *device, uint32_t *dst, uint32_t dst_stride, const Rect &damage,
                               DisplayBufferHandle cursor_handle, int32_t cursor_x, int32_t cursor_y,
                               uint64_t owner_pid)
{
    if (!device || !dst || dst_stride == 0 || cursor_handle == 0)
        return;

    DisplayBuffer cursor = {};
    if (!display_buffer_lookup_snapshot(cursor_handle, owner_pid, &cursor))
        return;
    if (!cursor.cpu_mapping || cursor.width == 0 || cursor.height == 0 || cursor.stride == 0)
        return;

    Rect cursor_rect = gui_rect_make(cursor_x, cursor_y, (int32_t)cursor.width, (int32_t)cursor.height);
    if (!display_clip_rect(cursor_rect, device->primary_head.caps))
        return;

    int32_t ox1 = cursor_rect.x > damage.x ? cursor_rect.x : damage.x;
    int32_t oy1 = cursor_rect.y > damage.y ? cursor_rect.y : damage.y;
    int32_t ox2 = (cursor_rect.x + cursor_rect.w) < (damage.x + damage.w) ? (cursor_rect.x + cursor_rect.w)
                                                                           : (damage.x + damage.w);
    int32_t oy2 = (cursor_rect.y + cursor_rect.h) < (damage.y + damage.h) ? (cursor_rect.y + cursor_rect.h)
                                                                           : (damage.y + damage.h);
    if (ox2 <= ox1 || oy2 <= oy1)
        return;

    const uint32_t *src = reinterpret_cast<const uint32_t *>(cursor.cpu_mapping);
    for (int32_t y = oy1; y < oy2; y++) {
        uint32_t *dst_row = &dst[(uint32_t)y * dst_stride];
        const uint32_t *src_row = &src[(uint32_t)(y - cursor_y) * cursor.stride];
        for (int32_t x = ox1; x < ox2; x++) {
            uint32_t pixel = src_row[(uint32_t)(x - cursor_x)];
            uint8_t alpha = (uint8_t)(pixel >> 24);
            if (alpha == 0)
                continue;
            if (alpha == 255)
                dst_row[(uint32_t)x] = 0xFF000000u | (pixel & 0x00FFFFFFu);
            else
                dst_row[(uint32_t)x] = compose_blend_pixel(dst_row[(uint32_t)x], pixel, 255u, false);
        }
    }
}

static uint32_t compose_submit_cpu(DisplayDevice *device, const DisplayComposeRequest &request,
                                   const Rect *damage_rects, uint32_t damage_count)
{
    if (!device || !device->ops)
        return device ? device->primary_head.status.completed_sequence : 0;

    device->primary_head.cpu_compose_path_count++;
    device->primary_head.last_present_path = DISPLAY_PRESENT_PATH_CPU_COMPOSE;
    device->primary_head.last_submitted_layers = request.layer_count;
    device->primary_head.last_submitted_dirty_area = display_damage_area(
        damage_rects, damage_count, device->primary_head.caps, &device->primary_head.last_submitted_dirty_rects);

    ComposeBufferSlot *compose_slot = acquire_compose_buffer(device);
    if (!compose_slot)
        return device->primary_head.status.completed_sequence;

    uint32_t *dst = reinterpret_cast<uint32_t *>(compose_slot->dma.virt);
    uint32_t dst_stride = device->primary_head.caps.pitch / 4u;
    Rect full_damage =
        gui_rect_make(0, 0, (int32_t)device->primary_head.caps.width, (int32_t)device->primary_head.caps.height);
    const Rect *rects = damage_count ? damage_rects : &full_damage;
    uint32_t rect_count = damage_count ? damage_count : 1u;
    bool seeded_from_front_buffer = false;

    if (!display_damage_is_full_frame(rects, rect_count, device->primary_head.caps)) {
        DisplayHead *head = &device->primary_head;
        if (head->front_buffer_index < head->buffer_count) {
            const DisplayBuffer &front = head->buffers[head->front_buffer_index];
            if (front.cpu_mapping && front.stride != 0 && front.width == device->primary_head.caps.width &&
                front.height == device->primary_head.caps.height && front.full_frame_valid) {
                if (front.cpu_mapping != dst) {
                    // CPU composition submits a page-flippable full buffer. Seed it from the current
                    // front buffer before repainting only dirty rects so unchanged pixels stay stable.
                    display_copy_buffer_rows(dst, dst_stride, front.cpu_mapping, front.stride,
                                             device->primary_head.caps.width, device->primary_head.caps.height);
                }
                seeded_from_front_buffer = true;
            }
        }
        if (!seeded_from_front_buffer) {
            rects = &full_damage;
            rect_count = 1u;
        }
    }

    Process *process = process_get_current();
    uint64_t owner_pid = process ? process->pid : 0;

    for (uint32_t r = 0; r < rect_count; r++) {
        Rect damage = rects[r];
        if (!display_clip_rect(damage, device->primary_head.caps))
            continue;

        for (int32_t y = damage.y; y < damage.y + damage.h; y++) {
            uint32_t *dst_row = &dst[(uint32_t)y * dst_stride + (uint32_t)damage.x];
            for (int32_t x = 0; x < damage.w; x++)
                dst_row[x] = 0xFF000000u;
        }

        for (uint32_t i = 0; i < request.layer_count; i++) {
            const DisplayComposeLayer &layer = request.layers[i];
            DisplayBuffer buffer = {};
            if (!display_buffer_lookup_snapshot(layer.buffer_handle, owner_pid, &buffer))
                continue;

            Rect overlap = layer.dst_rect;
            if (!display_clip_rect(overlap, device->primary_head.caps))
                continue;

            int32_t ox1 = overlap.x > damage.x ? overlap.x : damage.x;
            int32_t oy1 = overlap.y > damage.y ? overlap.y : damage.y;
            int32_t ox2 =
                (overlap.x + overlap.w) < (damage.x + damage.w) ? (overlap.x + overlap.w) : (damage.x + damage.w);
            int32_t oy2 =
                (overlap.y + overlap.h) < (damage.y + damage.h) ? (overlap.y + overlap.h) : (damage.y + damage.h);
            if (ox2 <= ox1 || oy2 <= oy1)
                continue;

            const uint32_t *src = reinterpret_cast<const uint32_t *>(buffer.cpu_mapping);
            bool opaque = (layer.flags & DISPLAY_COMPOSE_LAYER_OPAQUE) != 0;
            bool direct_copy = opaque && layer.alpha == 255u && layer.dst_rect.w == layer.src_rect.w &&
                               layer.dst_rect.h == layer.src_rect.h;
            if (direct_copy) {
                for (int32_t y = oy1; y < oy2; y++) {
                    int32_t src_y = layer.src_rect.y + (y - layer.dst_rect.y);
                    int32_t src_x = layer.src_rect.x + (ox1 - layer.dst_rect.x);
                    if (src_x < 0 || src_y < 0 || src_x + (ox2 - ox1) > (int32_t)buffer.width ||
                        src_y >= (int32_t)buffer.height)
                        continue;
                    uint32_t *dst_row = &dst[(uint32_t)y * dst_stride + (uint32_t)ox1];
                    const uint32_t *src_row = &src[(uint32_t)src_y * buffer.stride + (uint32_t)src_x];
                    gfx_copy_line(dst_row, src_row, (uint32_t)(ox2 - ox1));
                }
                continue;
            }

            for (int32_t y = oy1; y < oy2; y++) {
                uint32_t *dst_row = &dst[(uint32_t)y * dst_stride];
                for (int32_t x = ox1; x < ox2; x++) {
                    int32_t rel_x = x - layer.dst_rect.x;
                    int32_t rel_y = y - layer.dst_rect.y;
                    int32_t src_x = layer.src_rect.x;
                    int32_t src_y = layer.src_rect.y;
                    if (layer.dst_rect.w > 0)
                        src_x += (rel_x * layer.src_rect.w) / layer.dst_rect.w;
                    if (layer.dst_rect.h > 0)
                        src_y += (rel_y * layer.src_rect.h) / layer.dst_rect.h;
                    if (src_x < 0 || src_y < 0 || src_x >= (int32_t)buffer.width || src_y >= (int32_t)buffer.height)
                        continue;

                    uint32_t src_pixel = src[(uint32_t)src_y * buffer.stride + (uint32_t)src_x];
                    dst_row[(uint32_t)x] = compose_blend_pixel(dst_row[(uint32_t)x], src_pixel, layer.alpha, opaque);
                }
            }
        }
        compose_cursor_cpu(device, dst, dst_stride, damage, request.cursor_buffer_handle, request.cursor_x,
                           request.cursor_y, owner_pid);
    }

    compose_slot->buffer.cpu_mapping = dst;
    compose_slot->buffer.stride = dst_stride;
    compose_slot->buffer.width = device->primary_head.caps.width;
    compose_slot->buffer.height = device->primary_head.caps.height;
    compose_slot->buffer.full_frame_valid =
        seeded_from_front_buffer || display_damage_is_full_frame(rects, rect_count, device->primary_head.caps);
    uint32_t submitted = display_present_buffer_internal(device, compose_slot->buffer, rects, rect_count,
                                                         request.frame_sequence, request.flags);
    if (submitted != 0)
        compose_slot->in_flight_sequence = submitted;
    return submitted;
}

static bool gop_get_caps(DisplayDevice *device, DisplayCaps *out_caps)
{
    if (!device || !out_caps || !device->boot_framebuffer)
        return false;
    *out_caps = device->primary_head.caps;
    return true;
}

static bool gop_get_status(DisplayDevice *device, DisplayStatus *out_status)
{
    if (!device || !out_status || !device->boot_framebuffer)
        return false;
    *out_status = device->primary_head.status;
    return true;
}

static uint32_t gop_present(DisplayDevice *device, const DisplayPresentRequest &request)
{
    if (!device || !device->boot_framebuffer || !request.buffer || request.stride == 0) {
        return device ? device->primary_head.status.completed_sequence : 0;
    }

    DisplayHead *head = &device->primary_head;
    bool wait_for_vblank = (request.flags & DISPLAY_PRESENT_VBLANK) != 0;
    uint64_t target_tick = wait_for_vblank ? display_fallback_reserve_next_deadline(head) : 0;
    uint32_t sequence = display_resolve_present_sequence(head, request);
    uint64_t present_pixels = 0;
    Rect full_rect = gui_rect_make(0, 0, (int32_t)head->caps.width, (int32_t)head->caps.height);
    const Rect *rects = request.rect_count ? request.rects : &full_rect;
    uint32_t rect_count = request.rect_count ? request.rect_count : 1u;

    for (uint32_t i = 0; i < rect_count; i++) {
        Rect rect = rects[i];
        if (!display_clip_rect(rect, head->caps))
            continue;
        present_pixels += (uint64_t)rect.w * (uint64_t)rect.h;
    }

    display_note_present_submitted(head, sequence, DISPLAY_PRESENT_PATH_COPY, 1u, rects, rect_count);

    if (wait_for_vblank)
        display_fallback_finish_wait(head, target_tick);
    else
        head->last_vblank_wait_ticks = 0;

    uint64_t copy_start = timer_get_ticks();
    display_copy_present(device, request);
    uint64_t copy_end = timer_get_ticks();
    head->last_dma_wait_ticks = copy_end - copy_start;
    head->total_dma_wait_ticks += head->last_dma_wait_ticks;
    display_complete_present(head, sequence, DISPLAY_PRESENT_RESULT_OK, present_pixels);
    return sequence;
}

static uint32_t gop_present_buffer(DisplayDevice *device, const DisplayBuffer &buffer, const Rect *damage_rects,
                                   uint32_t damage_count, uint32_t frame_sequence, uint32_t flags)
{
    Rect full_damage = gui_rect_make(0, 0, (int32_t)buffer.width, (int32_t)buffer.height);
    DisplayPresentRequest request = {};
    request.buffer = buffer.cpu_mapping;
    request.stride = buffer.stride;
    request.rects = damage_count ? damage_rects : &full_damage;
    request.rect_count = damage_count ? damage_count : 1u;
    request.frame_sequence = frame_sequence;
    request.flags = flags;
    request.source_origin_x = 0;
    request.source_origin_y = 0;
    return gop_present(device, request);
}

static uint32_t gop_wait(DisplayDevice *device)
{
    if (!device || !device->boot_framebuffer)
        return 0;
    return display_fallback_wait_until_next_frame(&device->primary_head);
}

static bool gop_set_mode(DisplayDevice *device, const DisplayMode &mode)
{
    return display_set_mode_noop(device, mode);
}

static bool gop_atomic_commit(DisplayDevice *device, const DisplayAtomicState &state, const DisplayMode &mode)
{
    (void)state;
    return gop_set_mode(device, mode);
}

void display_init(const BootFramebuffer *fb)
{
    s_device = {};
    kstring::zero_memory(s_display_buffers, sizeof(s_display_buffers));
    kstring::zero_memory(s_display_events, sizeof(s_display_events));
    kstring::zero_memory(s_compose_buffers, sizeof(s_compose_buffers));
    s_display_event_head = 0;
    s_display_event_tail = 0;
    s_compose_active_index = 0;
    s_device.boot_framebuffer = fb;
    s_device.ops = &g_gop_backend_ops;
    s_device.backend_kind = DisplayBackendKind::FirmwareFramebuffer;

    if (!fb)
        return;

    setup_boot_caps(&primary_head()->caps, fb);
    primary_head()->status = {};
    primary_head()->buffer_count = 1;
    primary_head()->buffers[0].cpu_mapping = (uint32_t *)fb->address;
    primary_head()->buffers[0].phys_addr = 0;
    primary_head()->buffers[0].stride = primary_head()->caps.pitch / 4;
    primary_head()->buffers[0].width = primary_head()->caps.width;
    primary_head()->buffers[0].height = primary_head()->caps.height;
    primary_head()->buffers[0].full_frame_valid = true;
    primary_head()->buffer_in_flight_sequence[0] = 0;
    primary_head()->caps.flags |= DISPLAY_FLAG_HAS_COMPOSITOR;
    sync_primary_topology(&s_device);
}

void display_late_init(void)
{
    if (!s_device.boot_framebuffer)
        return;

    PciDevice display_device = {};
    if (!pci_find_display(&display_device)) {
        BOOT_WARN("Display: no PCI display device found, keeping firmware framebuffer backend");
        return;
    }

    s_device.has_pci_device = true;
    s_device.pci_device = display_device;
    s_device.detected_vendor_id = display_device.vendor_id;
    s_device.detected_device_id = display_device.device_id;

    BOOT_LOG("Display: using firmware framebuffer backend for PCI display %x:%x", s_device.detected_vendor_id,
             s_device.detected_device_id);
    s_device.backend_kind = DisplayBackendKind::FirmwareFramebuffer;
    sync_primary_topology(&s_device);
}

bool display_get_caps(DisplayCaps *out_caps)
{
    if (!s_device.ops)
        return false;
    return s_device.ops->get_caps(&s_device, out_caps);
}

bool display_get_status(DisplayStatus *out_status)
{
    if (!s_device.ops)
        return false;
    return s_device.ops->get_status(&s_device, out_status);
}

int display_query_connectors(DisplayConnectorInfo *out, uint32_t max_count)
{
    if (!out && max_count != 0)
        return -1;
    uint32_t count = s_device.connector_count;
    if (out && max_count != 0) {
        if (max_count < count)
            count = max_count;
        for (uint32_t i = 0; i < count; i++)
            out[i] = s_device.connectors[i].info;
    }
    return (int)count;
}

int display_get_modes(uint32_t connector_id, DisplayMode *out, uint32_t max_count)
{
    for (uint32_t i = 0; i < s_device.connector_count; i++) {
        const DisplayConnector &connector = s_device.connectors[i];
        if (connector.info.connector_id != connector_id)
            continue;

        uint32_t count = connector.mode_count;
        if (out && max_count != 0) {
            if (max_count < count)
                count = max_count;
            for (uint32_t j = 0; j < count; j++)
                out[j] = connector.modes[j];
        }
        return (int)count;
    }
    return -1;
}

bool display_atomic_commit(const DisplayAtomicRequest &request)
{
    DisplayAtomicState state = {};
    DisplayMode resolved_mode = {};
    if (!display_resolve_atomic_request(&s_device, request, &state, &resolved_mode))
        return false;

    bool committed = false;
    if (s_device.ops && s_device.ops->atomic_commit)
        committed = s_device.ops->atomic_commit(&s_device, state, resolved_mode);
    else if (s_device.ops && s_device.ops->set_mode)
        committed = s_device.ops->set_mode(&s_device, resolved_mode);
    else
        committed = display_mode_matches_head(&s_device.primary_head, resolved_mode);

    if (!committed)
        return false;

    display_apply_committed_mode(&s_device, state, resolved_mode);
    return true;
}

bool display_set_mode(const DisplayMode &mode)
{
    DisplayAtomicState state = {};
    DisplayMode resolved_mode = {};
    if (!display_build_atomic_mode_state(&s_device, mode, &state, &resolved_mode))
        return false;

    DisplayAtomicRequest request = {};
    request.connector_id = state.connector_id;
    request.mode_id = state.mode_id;
    request.flags = DISPLAY_ATOMIC_MODESET;
    request.damage_bounds = state.damage_bounds;
    return display_atomic_commit(request);
}

int display_buffer_create(DisplayBufferCreate *request)
{
    if (!request || request->width == 0 || request->height == 0 ||
        request->pixel_format != DISPLAY_PIXEL_FORMAT_XRGB8888)
        return -1;

    Process *process = process_get_current();
    if (!process)
        return -1;

    uint32_t stride = request->stride ? request->stride : request->width;
    if (stride < request->width)
        return -1;

    uint64_t size_bytes = display_buffer_bytes(stride, request->height);
    if (size_bytes == 0 || size_bytes > DISPLAY_BUFFER_MAP_SLOT_SIZE)
        return -1;

    spinlock_acquire(&s_display_buffer_lock);
    DisplayBufferObject *slot = nullptr;
    DisplayBufferHandle next_handle = 1;
    for (DisplayBufferObject &buffer : s_display_buffers) {
        if (!buffer.used && !slot)
            slot = &buffer;
        if (buffer.used && buffer.handle >= next_handle)
            next_handle = buffer.handle + 1u;
    }
    if (!slot) {
        spinlock_release(&s_display_buffer_lock);
        return -1;
    }
    spinlock_release(&s_display_buffer_lock);

    DMAAllocation dma = vmm_alloc_dma_with_flags((size_t)((size_bytes + 4095u) / 4096u),
                                                 display_buffer_kernel_page_flags(request->flags));
    if (dma.virt == 0)
        return -1;
    kstring::zero_memory(reinterpret_cast<void *>(dma.virt), dma.size);

    spinlock_acquire(&s_display_buffer_lock);
    slot->used = true;
    slot->handle = next_handle;
    slot->owner_pid = process->pid;
    slot->dma = dma;
    slot->width = request->width;
    slot->height = request->height;
    slot->stride = stride;
    slot->pixel_format = request->pixel_format;
    slot->flags = request->flags | DISPLAY_BUFFER_FLAG_CPU_VISIBLE | DISPLAY_BUFFER_FLAG_LINEAR;
    slot->mapped_pid = 0;
    slot->mapped_user_addr = 0;
    slot->mapped_user_size = 0;
    slot->wm_access = false;
    spinlock_release(&s_display_buffer_lock);

    return display_buffer_copy_out(slot, request);
}

int display_buffer_map(DisplayBufferMap *request)
{
    if (!request || request->handle == 0)
        return -1;

    Process *process = process_get_current();
    if (!process)
        return -1;

    spinlock_acquire(&s_display_buffer_lock);
    DisplayBufferObject *buffer = display_find_buffer_locked(request->handle);
    if (!buffer || buffer->owner_pid != process->pid) {
        spinlock_release(&s_display_buffer_lock);
        return -1;
    }
    spinlock_release(&s_display_buffer_lock);

    return display_map_buffer_into_process(buffer, request) ? 0 : -1;
}

bool display_buffer_set_wm_access(DisplayBufferHandle handle, bool allow)
{
    Process *process = process_get_current();
    if (!process || handle == 0)
        return false;

    spinlock_acquire(&s_display_buffer_lock);
    DisplayBufferObject *buffer = display_find_buffer_locked(handle);
    if (!buffer || buffer->owner_pid != process->pid) {
        spinlock_release(&s_display_buffer_lock);
        return false;
    }
    buffer->wm_access = allow;
    spinlock_release(&s_display_buffer_lock);
    return true;
}

bool display_buffer_destroy(DisplayBufferHandle handle)
{
    Process *process = process_get_current();
    if (!process || handle == 0)
        return false;

    spinlock_acquire(&s_display_buffer_lock);
    DisplayBufferObject *buffer = display_find_buffer_locked(handle);
    if (!buffer || buffer->owner_pid != process->pid) {
        spinlock_release(&s_display_buffer_lock);
        return false;
    }

    DisplayBuffer view = display_buffer_view_from_object(*buffer);
    const DisplayHead *head = &s_device.primary_head;
    for (uint32_t i = 0; i < head->buffer_count; ++i) {
        if (!display_buffers_match(head->buffers[i], view))
            continue;
        if (i == head->front_buffer_index || (head->pending_flip && i == head->pending_buffer_index) ||
            (head->buffer_in_flight_sequence[i] != 0 &&
             head->buffer_in_flight_sequence[i] > head->status.completed_sequence)) {
            spinlock_release(&s_display_buffer_lock);
            return false;
        }
    }

    DisplayBufferObject local = *buffer;
    *buffer = {};
    spinlock_release(&s_display_buffer_lock);

    display_unmap_buffer_from_current_process(&local);
    vmm_free_dma(local.dma);
    return true;
}

uint32_t display_present(const DisplayPresentRequest &request)
{
    if (!s_device.ops)
        return primary_head()->status.completed_sequence;
    return s_device.ops->present(&s_device, request);
}

uint32_t display_compose_submit(const DisplayComposeRequest &request)
{
    if (!s_device.ops || !request.layers || request.layer_count == 0)
        return primary_head()->status.completed_sequence;

    static constexpr uint32_t kMaxLocalDamageRects = 128u;
    Rect local_damage[kMaxLocalDamageRects] = {};
    uint32_t damage_count = request.damage_rect_count;

    if (damage_count == 0 || !request.damage_rects) {
        local_damage[0] = gui_rect_make(0, 0, (int32_t)primary_head()->caps.width, (int32_t)primary_head()->caps.height);
        damage_count = 1u;
    } else if (damage_count > kMaxLocalDamageRects) {
        // Never truncate damage. Collapse excessive damage into one safe clipped
        // bounding box so every dirty pixel is still presented.
        bool have_bounds = false;
        int32_t x0 = (int32_t)primary_head()->caps.width;
        int32_t y0 = (int32_t)primary_head()->caps.height;
        int32_t x1 = 0;
        int32_t y1 = 0;
        for (uint32_t i = 0; i < damage_count; i++) {
            Rect r = request.damage_rects[i];
            if (!display_clip_rect(r, primary_head()->caps))
                continue;
            if (!have_bounds) {
                x0 = r.x;
                y0 = r.y;
                x1 = r.x + r.w;
                y1 = r.y + r.h;
                have_bounds = true;
            } else {
                if (r.x < x0)
                    x0 = r.x;
                if (r.y < y0)
                    y0 = r.y;
                if (r.x + r.w > x1)
                    x1 = r.x + r.w;
                if (r.y + r.h > y1)
                    y1 = r.y + r.h;
            }
        }
        if (!have_bounds)
            return primary_head()->status.completed_sequence;
        local_damage[0] = gui_rect_make(x0, y0, x1 - x0, y1 - y0);
        damage_count = 1u;
    } else {
        uint32_t out_count = 0;
        for (uint32_t i = 0; i < damage_count; i++) {
            Rect r = request.damage_rects[i];
            if (!display_clip_rect(r, primary_head()->caps))
                continue;
            local_damage[out_count++] = r;
        }
        damage_count = out_count;
        if (damage_count == 0)
            return primary_head()->status.completed_sequence;
    }

    uint32_t fast_submitted = compose_fast_single_layer(&s_device, request, local_damage, damage_count);
    if (fast_submitted != 0)
        return fast_submitted;

    PresentBackendResult backend_result = PresentBackendResult::PRESENT_CPU_FALLBACK;
    uint32_t backend_submitted = compose_submit_backend(&s_device, request, local_damage, damage_count, &backend_result);
    if (backend_submitted != 0 && backend_result != PresentBackendResult::PRESENT_CPU_FALLBACK)
        return backend_submitted;

    return compose_submit_cpu(&s_device, request, local_damage, damage_count);
}

bool display_event_wait(DisplayEvent *out_event, bool block)
{
    if (!out_event)
        return false;

    spinlock_acquire(&s_display_event_lock);
    for (;;) {
        if (display_pop_event_locked(out_event)) {
            spinlock_release(&s_display_event_lock);
            return true;
        }
        if (!block) {
            spinlock_release(&s_display_event_lock);
            return false;
        }

        // scheduler_wait atomically drops the lock and sleeps.
        // It returns with the lock re-acquired.
        scheduler_wait(&s_display_event_wait_queue, &s_display_event_lock);
    }
}

uint32_t display_wait(void)
{
    if (!s_device.ops)
        return primary_head()->status.completed_sequence;
    return s_device.ops->wait(&s_device);
}

uint32_t display_get_completed_sequence(void)
{
    return primary_head()->status.completed_sequence;
}
