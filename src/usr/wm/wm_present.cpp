#include "wm_core.h"

// Frame sequence state: the last submitted present sequence and the sequence
// the next present will use. Owned here so bootstrap, frame build, and the
// submit/wait path all observe one source of truth.
static uint32_t g_last_seq = 0;
static uint32_t g_frame_seq = 1;

// Non-blocking present-wait timer; backs off so a wedged display does not
// log-flood.
static uint64_t g_wait_start_ticks = 0;
static uint64_t g_wait_warn_interval = 250;

void wm_present_init_sequences(uint32_t first_submitted)
{
    g_last_seq = first_submitted;
    g_frame_seq = (first_submitted ? first_submitted : 1) + 1;
}

uint32_t wm_present_last_sequence()
{
    return g_last_seq;
}

uint32_t wm_present_next_sequence()
{
    return g_frame_seq;
}

void wm_present_note_submitted(uint32_t sequence)
{
    g_last_seq = sequence;
    g_frame_seq = sequence + 1;
}

void apply_display_event(const DisplayEvent &event)
{
    if (event.type == DISPLAY_EVENT_FLIP_COMPLETE && event.sequence > g_display_queue.completed_sequence) {
        g_display_queue.completed_sequence = event.sequence;
    }
    if ((event.type == DISPLAY_EVENT_FLIP_COMPLETE || event.type == DISPLAY_EVENT_VBLANK) &&
        event.timestamp_ticks > g_display_queue.last_vblank_ticks) {
        g_display_queue.last_vblank_ticks = event.timestamp_ticks;
    }
    if (event.vblank_count > g_display_queue.vblank_count) {
        g_display_queue.vblank_count = event.vblank_count;
    }
}

static void retire_completed_present_buffers()
{
    for (uint32_t i = 0; i < g_presentbuffer_slot_count; i++) {
        if (g_presentbuffer_slots[i].in_flight_sequence &&
            g_presentbuffer_slots[i].in_flight_sequence <= g_display_queue.completed_sequence) {
            g_presentbuffer_slots[i].in_flight_sequence = 0;
        }
    }
}

void sync_presentbuffer_alias_from_active_slot()
{
    if (g_presentbuffer_slot_count == 0 || g_presentbuffer_active_slot >= g_presentbuffer_slot_count) {
        g_presentbuffer = {};
        g_presentbuffer_handle = 0;
        return;
    }
    g_presentbuffer = g_presentbuffer_slots[g_presentbuffer_active_slot].surface;
    g_presentbuffer_handle = g_presentbuffer_slots[g_presentbuffer_active_slot].handle;
}

static void clear_presentbuffer_slot_stale(PresentBufferSlot &slot)
{
    slot.stale_count = 0;
}

static void mark_presentbuffer_slot_stale(PresentBufferSlot &slot, const DirtyRect &dirty)
{
    DirtyRect clipped = dirty;
    if (!clip_dirty_rect_to_screen(clipped))
        return;

    wm::DirtyRect rects[MAX_DIRTY_RECTS];
    int count = clamp_dirty_rect_count(slot.stale_count);
    for (int i = 0; i < count; i++) {
        rects[i] = to_policy_rect(slot.stale_rects[i]);
    }

    wm::enqueue_damage_rect(rects, &count, MAX_DIRTY_RECTS, static_cast<int>(g_screen.width),
                            static_cast<int>(g_screen.height), to_policy_rect(clipped));

    slot.stale_count = clamp_dirty_rect_count(count);
    for (int i = 0; i < slot.stale_count; i++) {
        slot.stale_rects[i] = from_policy_rect(rects[i]);
    }
}

void mark_presentbuffer_slots_stale(const DirtyRect &dirty)
{
    for (uint32_t i = 0; i < g_presentbuffer_slot_count; i++) {
        mark_presentbuffer_slot_stale(g_presentbuffer_slots[i], dirty);
    }
}

void mark_other_presentbuffer_slots_stale(const DirtyRect *rects, int rect_count, uint32_t fresh_slot)
{
    if (!rects || rect_count <= 0)
        return;
    rect_count = clamp_dirty_rect_count(rect_count);
    if (rect_count == 0)
        return;

    wm::DirtyRect new_rects[MAX_DIRTY_RECTS];
    int new_count = 0;
    for (int r = 0; r < rect_count; r++) {
        DirtyRect clipped = rects[r];
        if (!clip_dirty_rect_to_screen(clipped))
            continue;
        new_rects[new_count++] = to_policy_rect(clipped);
    }
    if (new_count == 0)
        return;

    for (uint32_t i = 0; i < g_presentbuffer_slot_count; i++) {
        if (i == fresh_slot)
            continue;
        PresentBufferSlot &slot = g_presentbuffer_slots[i];

        wm::DirtyRect merged[MAX_DIRTY_RECTS];
        int merged_count = clamp_dirty_rect_count(slot.stale_count);
        for (int j = 0; j < merged_count; j++) {
            merged[j] = to_policy_rect(slot.stale_rects[j]);
        }

        for (int r = 0; r < new_count; r++) {
            wm::enqueue_damage_rect(merged, &merged_count, MAX_DIRTY_RECTS, static_cast<int>(g_screen.width),
                                    static_cast<int>(g_screen.height), new_rects[r]);
        }

        slot.stale_count = clamp_dirty_rect_count(merged_count);
        for (int j = 0; j < slot.stale_count; j++) {
            slot.stale_rects[j] = from_policy_rect(merged[j]);
        }
    }
}

static bool sync_presentbuffer_slot_from_active(uint32_t slot_index, bool overwrite_full_frame)
{
    if (slot_index >= g_presentbuffer_slot_count)
        return false;

    PresentBufferSlot &dst = g_presentbuffer_slots[slot_index];
    if (!dst.surface.buffer)
        return false;

    if (overwrite_full_frame) {
        clear_presentbuffer_slot_stale(dst);
        return true;
    }

    int stale_count = clamp_dirty_rect_count(dst.stale_count);

    // Dynamic Pruning: Discard stale repair rects that will be completely overwritten by the new frame's dirty
    // regions. Bypass during active window manipulation to prevent aggressive pruning from dropping essential
    // shadow/border repair rects.
    bool manip = g_input.pointer_down && g_input.drag_index >= 2;
    if (!manip) {
        for (int i = 0; i < stale_count; i++) {
            for (int j = 0; j < g_dirty_count; j++) {
                if (rect_contains(g_dirty_rects[j], dst.stale_rects[i])) {
                    dst.stale_rects[i] = dst.stale_rects[stale_count - 1];
                    stale_count--;
                    dst.stale_count = stale_count;
                    i--;
                    break;
                }
            }
        }
    }

    wm_stats_note_stale_repair(stale_count);

    DirtyRect cursor_rect = {};
    gui_get_cursor_bounds(g_input.cursor_kind, g_input.mouse_x, g_input.mouse_y, &cursor_rect.x, &cursor_rect.y,
                          &cursor_rect.w, &cursor_rect.h);
    bool cursor_on_screen = clip_dirty_rect_to_screen(cursor_rect);
    bool cursor_erased = false;

    DirtyRect clipped_stale[MAX_DIRTY_RECTS] = {};
    int clipped_stale_count = 0;
    DirtyRect bounds = {};
    uint64_t stale_area = 0;
    for (int i = 0; i < stale_count; i++) {
        DirtyRect stale = dst.stale_rects[i];
        if (!clip_dirty_rect_to_screen(stale))
            continue;
        clipped_stale[clipped_stale_count++] = stale;
        bounds = clipped_stale_count == 1 ? stale : rect_union(bounds, stale);
        stale_area += static_cast<uint64_t>(stale.w) * static_cast<uint64_t>(stale.h);
    }

    uint64_t bounds_area = static_cast<uint64_t>(bounds.w) * static_cast<uint64_t>(bounds.h);
    bool batch_repair = clipped_stale_count > 4 && stale_area != 0 && bounds_area * 2u <= stale_area * 3u;
    if (batch_repair) {
        gui_blit_rect(&dst.surface, &g_backbuffer, bounds.x, bounds.y, bounds.x, bounds.y, bounds.w, bounds.h);
        cursor_erased = cursor_on_screen && rect_intersection(bounds, cursor_rect, nullptr);
    } else {
        for (int i = 0; i < clipped_stale_count; i++) {
            DirtyRect stale = clipped_stale[i];
            gui_blit_rect(&dst.surface, &g_backbuffer, stale.x, stale.y, stale.x, stale.y, stale.w, stale.h);
            if (cursor_on_screen && rect_intersection(stale, cursor_rect, nullptr))
                cursor_erased = true;
        }
    }

    if (cursor_erased)
        enqueue_damage_rect(cursor_rect.x, cursor_rect.y, cursor_rect.w, cursor_rect.h);
    clear_presentbuffer_slot_stale(dst);
    return true;
}

bool select_presentbuffer_slot_for_frame()
{
    retire_completed_present_buffers();
    if (g_presentbuffer_slot_count == 0)
        return false;

    bool overwrite_full_frame = dirty_set_is_single_fullscreen_rect();
    for (uint32_t offset = 0; offset < g_presentbuffer_slot_count; offset++) {
        uint32_t index = (g_presentbuffer_active_slot + offset) % g_presentbuffer_slot_count;
        if (g_presentbuffer_slots[index].in_flight_sequence != 0)
            continue;
        if (!sync_presentbuffer_slot_from_active(index, overwrite_full_frame))
            continue;

        g_presentbuffer_active_slot = index;
        sync_presentbuffer_alias_from_active_slot();
        return true;
    }

    sync_presentbuffer_alias_from_active_slot();
    return false;
}

void wm_drain_display_events()
{
    DisplayEvent event = {};
    while (display_poll_event(&event) == 0) {
        apply_display_event(event);
    }
    retire_completed_present_buffers();
}

void refresh_display_queue_from_status()
{
    DisplayStatus status = {};
    if (display_get_status(&status) == 0) {
        if (status.completed_sequence > g_display_queue.completed_sequence)
            g_display_queue.completed_sequence = status.completed_sequence;
        if (status.last_vblank_ticks > g_display_queue.last_vblank_ticks)
            g_display_queue.last_vblank_ticks = status.last_vblank_ticks;
        if (status.vblank_count > g_display_queue.vblank_count)
            g_display_queue.vblank_count = status.vblank_count;
        retire_completed_present_buffers();
    }
}

uint32_t present_frame(const Surface *source, const DirtyRect *rects, int rect_count, uint32_t frame_sequence,
                       DisplayBufferHandle cursor_handle, int cursor_x, int cursor_y)
{
    rect_count = clamp_dirty_rect_count(rect_count);
    if (!source || !source->buffer || rect_count <= 0)
        return 0;

    Rect present_rects[MAX_DIRTY_RECTS];
    int present_count = 0;
    for (int i = 0; i < rect_count; i++) {
        DirtyRect clipped = rects[i];
        if (clip_dirty_rect_to_screen(clipped)) {
            present_rects[present_count++] = gui_rect_make(clipped.x, clipped.y, clipped.w, clipped.h);
        }
    }
    if (present_count == 0)
        return 0;

    if (g_presentbuffer_handle != 0) {
        DisplayComposeLayer layer = {.buffer_handle = g_presentbuffer_handle,
                                     .src_rect = gui_rect_make(0, 0, source->width, source->height),
                                     .dst_rect = gui_rect_make(0, 0, source->width, source->height),
                                     .flags = DISPLAY_COMPOSE_LAYER_OPAQUE,
                                     .alpha = 255u};

        DisplayComposeRequest req = {.layers = &layer,
                                     .layer_count = 1,
                                     .damage_rects = present_rects,
                                     .damage_rect_count = static_cast<uint32_t>(present_count),
                                     .frame_sequence = frame_sequence,
                                     .flags = DISPLAY_PRESENT_VBLANK,
                                     .cursor_buffer_handle = cursor_handle,
                                     .cursor_x = cursor_x,
                                     .cursor_y = cursor_y};
        return display_compose_submit(&req);
    }

    DisplayPresentRequest req = {.buffer = source->buffer,
                                 .stride = source->pitch / 4,
                                 .rects = present_rects,
                                 .rect_count = static_cast<uint32_t>(present_count),
                                 .frame_sequence = frame_sequence,
                                 .flags = DISPLAY_PRESENT_VBLANK};
    return display_present(&req);
}

bool wm_present_end_frame(Registry *registry, bool manip, bool inter, uint32_t limit, uint64_t frame_tsc_start)
{
    uint32_t pending = wm::pending_presents(g_last_seq, g_display_queue.completed_sequence);
    if (pending) {
        refresh_display_queue_from_status();
        pending = wm::pending_presents(g_last_seq, g_display_queue.completed_sequence);
    }

    wm::PresentPolicyDecision action =
        wm::choose_present_policy({pending, limit, (g_display_caps.flags & DISPLAY_FLAG_STRICT_SYNC_ONLY) != 0, inter,
                                   g_display_copy_path, manip});

    if (g_dirty_frame_ready && action == wm::PresentPolicyDecision::Submit) {
        asm volatile("sfence" ::: "memory");
        uint64_t present_tsc_start = wm_tsc_now();
        DisplayBufferHandle frame_cursor_handle = 0;
        int frame_cursor_x = 0;
        int frame_cursor_y = 0;
        wm_cursor_frame_plane(&frame_cursor_handle, &frame_cursor_x, &frame_cursor_y);
        uint32_t sub = present_frame(&g_presentbuffer, g_dirty_rects, clamp_dirty_rect_count(g_dirty_count),
                                     g_frame_seq, frame_cursor_handle, frame_cursor_x, frame_cursor_y);
        uint64_t present_tsc_end = wm_tsc_now();
        if (sub) {
            g_frame_stats.last_present_ticks = present_tsc_end - present_tsc_start;
            g_frame_stats.total_present_ticks += g_frame_stats.last_present_ticks;
            g_frame_stats.last_frame_ticks = present_tsc_end - frame_tsc_start;
            if (g_frame_stats.last_frame_ticks > g_frame_stats.max_frame_ticks)
                g_frame_stats.max_frame_ticks = g_frame_stats.last_frame_ticks;
            if (g_frame_stats.last_input_ticks != 0)
                g_frame_stats.last_input_to_submit_ticks = present_tsc_end - g_frame_stats.last_input_ticks;
            if (g_presentbuffer_slot_count) {
                g_presentbuffer_slots[g_presentbuffer_active_slot].in_flight_sequence = sub;
            }
            mark_other_presentbuffer_slots_stale(g_dirty_rects, clamp_dirty_rect_count(g_dirty_count),
                                                 g_presentbuffer_active_slot);
            for (int i = 0; i < g_window_count; i++) {
                g_windows[i].last_rendered_x = g_windows[i].x;
                g_windows[i].last_rendered_y = g_windows[i].y;
                g_windows[i].last_rendered_w = g_windows[i].w;
                g_windows[i].last_rendered_h = g_windows[i].h;
            }
            g_frame_stats.frames_submitted++;
#ifdef DEBUG
            if ((g_frame_stats.frames_submitted % 120u) == 0) {
                LOG_INFO("wm",
                         "stats: sub=%llu skip=%llu comp=%lluus pres=%lluus frame=%lluus max=%lluus "
                         "dmg=%llukpx in>sub=%lluus",
                         static_cast<unsigned long long>(g_frame_stats.frames_submitted),
                         static_cast<unsigned long long>(g_frame_stats.frames_skipped),
                         static_cast<unsigned long long>(wm_tsc_to_us(g_frame_stats.last_compose_ticks)),
                         static_cast<unsigned long long>(wm_tsc_to_us(g_frame_stats.last_present_ticks)),
                         static_cast<unsigned long long>(wm_tsc_to_us(g_frame_stats.last_frame_ticks)),
                         static_cast<unsigned long long>(wm_tsc_to_us(g_frame_stats.max_frame_ticks)),
                         static_cast<unsigned long long>(g_frame_stats.last_dirty_area / 1000u),
                         static_cast<unsigned long long>(wm_tsc_to_us(g_frame_stats.last_input_to_submit_ticks)));
            }
#endif
            wm_present_note_submitted(sub);
            g_dirty_count = 0;
            g_dirty_frame_ready = false;
            g_wait_start_ticks = 0; // Reset wait timer
            g_wait_warn_interval = 250;
        }
    } else if (!g_dirty_frame_ready || action == wm::PresentPolicyDecision::Skip) {
        if (action == wm::PresentPolicyDecision::Skip)
            g_frame_stats.frames_skipped++;
        // Flush deferred settings persist during idle to avoid blocking I/O during compositing.
        flush_pending_settings_persist(registry);
        if (g_dirty_count == 0 && g_last_seq <= g_display_queue.completed_sequence) {
            // Fully idle: a 1 ms loop woke the compositor ~1000x/second
            // for no work (power/thermal cost on real laptops). The only
            // genuinely periodic duties are 1 Hz (clock) and toast expiry.
            sleep_ms(16);
        } else {
            yield();
        }
    } else {
        // Swapchain present-wait: park on the display event queue instead
        // of polling, so completion wakes us within a tick instead of
        // after an arbitrary 1 ms poll interval.
        if (g_wait_start_ticks == 0) {
            g_wait_start_ticks = get_ticks();
        }
        if (get_ticks() - g_wait_start_ticks > g_wait_warn_interval) {
            LOG_ERROR("wm", "Display driver has not completed the pending frame");
            g_wait_start_ticks = get_ticks();
            if (g_wait_warn_interval < 4000)
                g_wait_warn_interval *= 2;
        }
        DisplayEvent wait_event = {};
        if (display_wait_event_timeout_ms(&wait_event, 4) == 0)
            apply_display_event(wait_event);
        return true; // Wait without releasing ownership of an in-flight present buffer.
    }
    return false;
}
