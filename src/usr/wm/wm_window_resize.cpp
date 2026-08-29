#include "wm_metrics.h"
#include "wm_present.h"
#include "wm_render.h"
#include "wm_window.h"

static uint32_t next_configure_serial(Window &w)
{
    uint32_t serial = (w.configure_serial == 0xFFFFFFFFu) ? 1u : w.configure_serial + 1u;
    w.configure_serial = serial;
    return serial;
}

// Publish the current target as a resize configure. Only called with no
// configure outstanding: resizes are serialized per window, so the client is
// never redrawing toward one size while being asked for another. The visible
// bounds stay at the last committed frame until the client acks, then flip
// to the pending geometry in one step.
bool post_window_resize_configure(Window &w)
{
    if (!w.entry || !(w.entry->flags & WIN_FLAG_RESIZABLE) || !w.owner_pid || w.target_w <= 0 || w.target_h <= 0)
        return false;
    if (w.resize_configure_pending)
        return false;

    // The buffer holds the last committed frame right now (no resize in
    // flight): copy it before the client starts redrawing, so composition can
    // keep presenting a stable frame for the whole redraw window.
    wm_resize_snapshot_capture(w);

    w.pending_configure_serial = next_configure_serial(w);
    w.entry_resize_serial = w.pending_configure_serial;
    w.resize_configure_pending = true;
    w.last_configure_ticks = get_ticks();
    w.pending_x = w.target_x;
    w.pending_y = w.target_y;
    w.pending_w = w.target_w;
    w.pending_h = w.target_h;

    w.entry->x = w.target_x;
    w.entry->y = w.target_y;
    w.entry->w = w.target_w;
    w.entry->h = w.target_h;
    w.entry->position_serial++;
    w.entry->resize_serial = w.pending_configure_serial;
    asm volatile("sfence" ::: "memory");

    Event resize_ev = {};
    resize_ev.type = EVT_WINDOW_RESIZE;
    resize_ev.resize.width = w.target_w;
    resize_ev.resize.height = w.target_h;
    resize_ev.resize.serial = w.pending_configure_serial;
    syscall2(SYS_POST_EVENT, w.owner_pid, (uint64_t)&resize_ev);
    return true;
}

// The client finished redrawing the outstanding configure: flip the visible
// bounds to the pending geometry — backing and bounds land in one frame —
// then keep the pipeline full if the target already moved on.
void apply_window_resize_flip(Window &w)
{
    if (!w.resize_configure_pending || w.pending_configure_serial == 0)
        return;
    g_frame_stats.resize_flips++;
    apply_window_bounds_now(w, w.pending_x, w.pending_y, w.pending_w, w.pending_h, false);
    w.resize_configure_pending = false;
    w.last_configure_ticks = 0;
    w.last_commit_ticks = get_ticks();
    // The buffer now holds the freshly committed frame: refresh the snapshot
    // so the next configure generation starts from it.
    wm_resize_snapshot_capture(w);
    if (w.target_x != w.x || w.target_y != w.y || w.target_w != w.w || w.target_h != w.h)
        post_window_resize_configure(w);
}

// Retransmit the outstanding configure unchanged (same serial, same
// geometry) after a client went quiet; no new generation, no snapshot.
void resend_window_resize_configure(Window &w)
{
    if (!w.entry || !w.owner_pid || !w.resize_configure_pending || w.pending_configure_serial == 0)
        return;
    w.last_configure_ticks = get_ticks();
    w.entry->resize_serial = w.pending_configure_serial;
    asm volatile("sfence" ::: "memory");

    Event resize_ev = {};
    resize_ev.type = EVT_WINDOW_RESIZE;
    resize_ev.resize.width = w.pending_w;
    resize_ev.resize.height = w.pending_h;
    resize_ev.resize.serial = w.pending_configure_serial;
    syscall2(SYS_POST_EVENT, w.owner_pid, (uint64_t)&resize_ev);
}

void wm_resize_snapshot_release(Window &w)
{
    gui_destroy_surface(&w.resize_snapshot);
    w.resize_snapshot_y0 = 0;
}

// Copy the window's backing into a WM-owned surface while the backing is
// stable (configure post / ack). During the following redraw window the
// compositor presents from this copy instead of the shared buffer the client
// is overwriting. Very tall content backings are captured as a band around
// the visible slice; a scroll outside the band falls back to the live buffer
// for that frame.
void wm_resize_snapshot_capture(Window &w)
{
    if (!w.buffer || w.buffer_w <= 0 || w.buffer_h <= 0 || w.buffer_w > 8192) {
        wm_resize_snapshot_release(w);
        return;
    }
    int cw = w.buffer_w;
    int ch = w.buffer_h;
    int y0 = 0;
    if (ch > 4096) {
        int band_h = w.h + 256;
        if (band_h > ch)
            band_h = ch;
        y0 = w.scroll_y - 128;
        if (y0 < 0)
            y0 = 0;
        if (y0 + band_h > ch)
            y0 = ch - band_h;
        ch = band_h;
    }
    if (!w.resize_snapshot.buffer || static_cast<int>(w.resize_snapshot.width) != cw ||
        static_cast<int>(w.resize_snapshot.height) != ch) {
        gui_destroy_surface(&w.resize_snapshot);
        w.resize_snapshot = gui_create_surface(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch));
        if (!w.resize_snapshot.buffer)
            return;
    }
    const uint32_t src_stride = static_cast<uint32_t>(w.buffer_w);
    const uint32_t dst_stride = w.resize_snapshot.pitch / 4;
    for (int y = 0; y < ch; y++) {
        memcpy(&w.resize_snapshot.buffer[static_cast<size_t>(y) * dst_stride],
               &w.buffer[static_cast<size_t>(y + y0) * src_stride], static_cast<size_t>(cw) * sizeof(uint32_t));
    }
    w.resize_snapshot_y0 = y0;
}
