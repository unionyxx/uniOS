#include "wm_damage.h"
#include "wm_input.h"
#include "wm_metrics.h"
#include "wm_present.h"

namespace {

struct CursorPresentBuffer
{
    DisplayBufferHandle handle;
    Surface surface;
    GuiCursorKind kind;
    int hot_x;
    int hot_y;
    bool valid;
};

CursorPresentBuffer g_cursor_present_buffers[GUI_CURSOR_RESIZE_D2 + 1] = {};
bool g_cursor_backend_disabled = false;
DisplayBufferHandle g_frame_cursor_handle = 0;
int g_frame_cursor_x = 0;
int g_frame_cursor_y = 0;
bool g_prev_frame_sw_cursor = false;
DirtyRect g_prev_sw_cursor_rect = {};

CursorPresentBuffer *ensure_cursor_present_buffer(GuiCursorKind kind)
{
    int index = static_cast<int>(kind);
    if (index < 0 || index > static_cast<int>(GUI_CURSOR_RESIZE_D2) || !wm_cursor_backend_allowed())
        return nullptr;

    CursorPresentBuffer &slot = g_cursor_present_buffers[index];
    if (slot.valid && slot.handle != 0 && slot.surface.buffer)
        return &slot;

    int32_t bx = 0, by = 0, bw = 0, bh = 0;
    gui_get_cursor_bounds(kind, 0, 0, &bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0 || bw > CURSOR_MAX_SIZE || bh > CURSOR_MAX_SIZE)
        return nullptr;

    DisplayBufferCreate create = {};
    create.width = static_cast<uint32_t>(bw);
    create.height = static_cast<uint32_t>(bh);
    create.pixel_format = DISPLAY_PIXEL_FORMAT_XRGB8888;
    create.flags = DISPLAY_BUFFER_FLAG_CPU_VISIBLE | DISPLAY_BUFFER_FLAG_LINEAR | DISPLAY_BUFFER_FLAG_RENDER_TARGET;

    if (display_buffer_create(&create) != 0 || create.handle == 0)
        return nullptr;

    DisplayBufferMap map = {};
    map.handle = create.handle;
    if (display_buffer_map(&map) != 0 || map.address == 0 || map.stride < static_cast<uint32_t>(bw)) {
        display_buffer_destroy(create.handle);
        return nullptr;
    }

    memset(&slot, 0, sizeof(slot));
    slot.handle = create.handle;
    slot.surface.width = static_cast<uint32_t>(bw);
    slot.surface.height = static_cast<uint32_t>(bh);
    slot.surface.pitch = map.stride * 4u;
    slot.surface.buffer = reinterpret_cast<uint32_t *>(map.address);
    slot.kind = kind;
    gui_get_cursor_hotspot(kind, &slot.hot_x, &slot.hot_y);
    gui_fill_rect(&slot.surface, 0, 0, bw, bh, 0x00000000u);
    gui_draw_cursor_kind(&slot.surface, slot.hot_x, slot.hot_y, kind);
    smp_wmb();
    slot.valid = true;
    return &slot;
}

} // namespace

bool wm_cursor_backend_allowed()
{
    // Re-enable cursor backend if display copy path is no longer active
    if (g_cursor_backend_disabled && !g_display_copy_path &&
        (g_display_caps.flags & DISPLAY_FLAG_HAS_COMPOSITOR) != 0 &&
        (g_display_caps.flags & DISPLAY_FLAG_HAS_PAGE_FLIP) != 0 && g_presentbuffer_handle != 0) {
        g_cursor_backend_disabled = false;
    }
    return !g_cursor_backend_disabled && !g_display_copy_path &&
           (g_display_caps.flags & DISPLAY_FLAG_HAS_COMPOSITOR) != 0 &&
           (g_display_caps.flags & DISPLAY_FLAG_HAS_PAGE_FLIP) != 0 && g_presentbuffer_handle != 0;
}

bool prepare_cursor_overlay_damage(bool interactive, DirtyRect *cursor_rect_out, bool track_damage)
{
    if (!cursor_rect_out)
        return false;

    DirtyRect cursor_rect = {};
    gui_get_cursor_bounds(g_input.cursor_kind, g_input.mouse_x, g_input.mouse_y, &cursor_rect.x, &cursor_rect.y,
                          &cursor_rect.w, &cursor_rect.h);
    if (!clip_dirty_rect_to_screen(cursor_rect))
        return false;

    // Only the software cursor bakes pixels into the frame; the hardware
    // plane draws it out of band, so cursor damage there would just inflate
    // the dirty set (and trip the resize dirty-collapse heuristic).
    if (track_damage && !dirty_set_contains_rect(cursor_rect)) {
        enqueue_damage_rect(cursor_rect.x, cursor_rect.y, cursor_rect.w, cursor_rect.h);
        normalize_dirty_rects(interactive);
        // Re-get bounds after potential normalization
        gui_get_cursor_bounds(g_input.cursor_kind, g_input.mouse_x, g_input.mouse_y, &cursor_rect.x, &cursor_rect.y,
                              &cursor_rect.w, &cursor_rect.h);
        if (!clip_dirty_rect_to_screen(cursor_rect))
            return false;
    }

    *cursor_rect_out = cursor_rect;
    return true;
}

void wm_cursor_begin_frame()
{
    g_frame_cursor_handle = 0;
    g_frame_cursor_x = 0;
    g_frame_cursor_y = 0;
}

void wm_cursor_erase_previous_software(bool hw_cursor_allowed, bool interactive)
{
    // Switching from a software cursor to the hardware plane: the
    // last baked cursor pixels must be erased once.
    if (hw_cursor_allowed && g_prev_frame_sw_cursor) {
        enqueue_damage_rect(g_prev_sw_cursor_rect.x, g_prev_sw_cursor_rect.y, g_prev_sw_cursor_rect.w,
                            g_prev_sw_cursor_rect.h);
        normalize_dirty_rects(interactive);
    }
}

bool wm_cursor_select_plane(bool hw_cursor_allowed, const DirtyRect &cursor_rect, bool interactive)
{
    bool draw_software_cursor = true;
    if (hw_cursor_allowed) {
        CursorPresentBuffer *cursor_buffer = ensure_cursor_present_buffer(g_input.cursor_kind);
        if (cursor_buffer && cursor_buffer->handle != 0) {
            g_frame_cursor_handle = cursor_buffer->handle;
            // Hardware cursor expects HOTSPOT position (mouse position), not bounds top-left
            g_frame_cursor_x = g_input.mouse_x;
            g_frame_cursor_y = g_input.mouse_y;
            draw_software_cursor = false;
            g_cursor_backend_disabled = false; // Re-enable if it works
        } else {
            g_cursor_backend_disabled = true;
            g_frame_cursor_handle = 0; // Clear stale handle
            // Falling back to the software cursor this frame: the
            // cursor rect was not damaged, so add it now.
            enqueue_damage_rect(cursor_rect.x, cursor_rect.y, cursor_rect.w, cursor_rect.h);
            normalize_dirty_rects(interactive);
            draw_software_cursor = true;
        }
    } else {
        // Force software cursor when backend is disabled or no cursor to draw
        g_frame_cursor_handle = 0; // Clear stale handle
    }
    return draw_software_cursor;
}

void wm_cursor_finish_frame(bool draw_cursor, bool software, const DirtyRect &cursor_rect)
{
    g_prev_frame_sw_cursor = draw_cursor && software;
    g_prev_sw_cursor_rect = cursor_rect;
}

void wm_cursor_frame_plane(DisplayBufferHandle *handle, int *x, int *y)
{
    if (handle)
        *handle = g_frame_cursor_handle;
    if (x)
        *x = g_frame_cursor_x;
    if (y)
        *y = g_frame_cursor_y;
}
