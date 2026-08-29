#include "wm_core.h"

static uint64_t g_tsc_freq_cached = 0;

uint64_t wm_tsc_now(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint64_t wm_tsc_to_us(uint64_t cycles)
{
    if (g_tsc_freq_cached == 0)
        g_tsc_freq_cached = get_tsc_freq(); // MHz (SYS_GET_TSC_FREQ contract)
    if (g_tsc_freq_cached == 0)
        return 0;
    return cycles / g_tsc_freq_cached;
}

static uint64_t dirty_area_sum(const DirtyRect *rects, int rect_count)
{
    uint64_t area = 0;
    rect_count = clamp_dirty_rect_count(rect_count);
    for (int i = 0; i < rect_count; i++) {
        DirtyRect clipped = rects[i];
        if (!clip_dirty_rect_to_screen(clipped))
            continue;
        area += static_cast<uint64_t>(clipped.w) * static_cast<uint64_t>(clipped.h);
    }
    return area;
}

void wm_stats_note_dirty_set(const DirtyRect *rects, int rect_count)
{
    rect_count = clamp_dirty_rect_count(rect_count);
    uint64_t area = dirty_area_sum(rects, rect_count);
    g_frame_stats.last_dirty_rects = static_cast<uint32_t>(rect_count);
    g_frame_stats.last_dirty_area = area;
    g_frame_stats.dirty_area_accum += area;

    if (static_cast<uint32_t>(rect_count) > g_frame_stats.max_dirty_rects)
        g_frame_stats.max_dirty_rects = static_cast<uint32_t>(rect_count);
    if (area > g_frame_stats.max_dirty_area)
        g_frame_stats.max_dirty_area = area;

    if (rect_count == 1 && area == static_cast<uint64_t>(g_screen.width) * static_cast<uint64_t>(g_screen.height)) {
        g_frame_stats.full_repaints++;
    } else {
        g_frame_stats.clipped_repaints++;
    }
}

void wm_stats_note_stale_repair(int rect_count)
{
    if (rect_count > 0)
        g_frame_stats.stale_slot_repairs += static_cast<uint64_t>(rect_count);
}

static void wm_bench_finish(Registry *registry)
{
    if (g_bench.win_index >= 0 && g_bench.win_index < g_window_count) {
        Window &w = g_windows[g_bench.win_index];
        if (w.entry) {
            set_window_bounds(w, g_bench.origin_x, g_bench.origin_y, g_bench.origin_w, g_bench.origin_h);
        } else {
            Window old = w;
            w.x = g_bench.origin_x;
            w.y = g_bench.origin_y;
            w.w = g_bench.origin_w;
            w.h = g_bench.origin_h;
            mark_window_transition_damage(old, w);
        }
    }
    uint64_t frames = g_frame_stats.frames_submitted - g_bench.start_submitted;
    uint64_t compose_ticks = g_frame_stats.total_compose_ticks - g_bench.start_compose_total;
    uint64_t present_ticks = g_frame_stats.total_present_ticks - g_bench.start_present_total;
    uint64_t dirty_area = g_frame_stats.dirty_area_accum - g_bench.start_dirty_area_accum;
    uint64_t flips = g_frame_stats.resize_flips - g_bench.start_resize_flips;
    uint64_t stale_acks = g_frame_stats.resize_stale_acks - g_bench.start_resize_stale_acks;
    LOG_INFO("wm",
             "bench %s done: %llu frames, compose avg %llu us, present avg %llu us, frame max %llu us, "
             "dirty avg %llu kpx, resize flips %llu stale %llu",
             g_bench.resize_mode ? "resize" : "drag", static_cast<unsigned long long>(frames),
             static_cast<unsigned long long>(frames ? wm_tsc_to_us(compose_ticks) / frames : 0),
             static_cast<unsigned long long>(frames ? wm_tsc_to_us(present_ticks) / frames : 0),
             static_cast<unsigned long long>(wm_tsc_to_us(g_frame_stats.max_frame_ticks)),
             static_cast<unsigned long long>(frames ? dirty_area / frames / 1000u : 0),
             static_cast<unsigned long long>(flips), static_cast<unsigned long long>(stale_acks));
    registry->system_flags &= ~(SYSTEM_FLAG_WM_BENCH_DRAG | SYSTEM_FLAG_WM_BENCH_RESIZE);
    smp_wmb();
    g_bench = {};
}

void wm_bench_tick(Registry *registry)
{
    if (!registry)
        return;
    bool drag_flag = (registry->system_flags & SYSTEM_FLAG_WM_BENCH_DRAG) != 0;
    bool resize_flag = (registry->system_flags & SYSTEM_FLAG_WM_BENCH_RESIZE) != 0;

    if (!g_bench.active) {
        if (!drag_flag && !resize_flag)
            return;
        int idx = -1;
        for (int i = WM_FIRST_USER_WINDOW; i < g_window_count; i++) {
            if (is_window_visible(g_windows[i]) && g_windows[i].buffer) {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            return; // Keep the flag set; the benchmark starts once a window exists.
        Window &w = g_windows[idx];
        g_bench = {};
        g_bench.active = true;
        g_bench.resize_mode = resize_flag;
        g_bench.win_index = idx;
        g_bench.origin_x = w.x;
        g_bench.origin_y = w.y;
        g_bench.origin_w = w.w;
        g_bench.origin_h = w.h;
        g_bench.frames_target = 1000;
        g_bench.start_submitted = g_frame_stats.frames_submitted;
        g_bench.start_compose_total = g_frame_stats.total_compose_ticks;
        g_bench.start_present_total = g_frame_stats.total_present_ticks;
        g_bench.start_max_frame_ticks = g_frame_stats.max_frame_ticks;
        g_bench.start_dirty_area_accum = g_frame_stats.dirty_area_accum;
        g_bench.start_resize_flips = g_frame_stats.resize_flips;
        g_bench.start_resize_stale_acks = g_frame_stats.resize_stale_acks;
        LOG_INFO("wm", "bench: starting %s benchmark (%llu frames)", resize_flag ? "resize" : "drag",
                 static_cast<unsigned long long>(g_bench.frames_target));
        return;
    }

    uint64_t done = g_frame_stats.frames_submitted - g_bench.start_submitted;
    if (done >= g_bench.frames_target || g_bench.win_index < 0 || g_bench.win_index >= g_window_count) {
        wm_bench_finish(registry);
        return;
    }

    Window &w = g_windows[g_bench.win_index];
    Window old = w;
    if (!g_bench.resize_mode) {
        int step = static_cast<int>(done % 512u);
        int phase = step < 256 ? step : 512 - step;
        int span_x = static_cast<int>(g_screen.width) - w.w;
        int span_y = static_cast<int>(g_screen.height) - w.h;
        w.x = span_x > 0 ? (phase * span_x) / 255 : w.x;
        w.y = span_y > 0 ? (phase * span_y) / 255 : w.y;
    } else {
        int step = static_cast<int>(done % 256u);
        int phase = step < 128 ? step : 256 - step;
        int max_w = static_cast<int>(g_screen.width);
        int max_h = static_cast<int>(g_screen.height) - wm_menubar_h();
        int min_w = w.min_w > 0 ? w.min_w : 240;
        int min_h = w.min_h > 0 ? w.min_h : 160;
        int new_w = min_w + ((max_w > min_w ? max_w - min_w : 0) * phase) / 127;
        int new_h = min_h + ((max_h > min_h ? max_h - min_h : 0) * phase) / 127;
        // Route through the real resize pipeline (configure post, client
        // redraw, buffer flip, geometry flip on ack) so the benchmark
        // measures exactly what an interactive edge drag exercises.
        (void)old;
        set_window_bounds(w, g_bench.origin_x, g_bench.origin_y, new_w, new_h);
        return;
    }
    if (w.x != old.x || w.y != old.y || w.w != old.w || w.h != old.h) {
        mark_window_transition_damage(old, w);
        invalidate_window_visibility_cache();
    }
}
void wm_stats_overlay_bounds(DirtyRect *out_box, DirtyRect *out_damage)
{
    int w = gui_scaled_metric(330);
    int h = gui_scaled_metric(112);
    int margin = gui_space_2();
    DirtyRect box = {margin, wm_menubar_h() + margin, w, h};
    if (out_box)
        *out_box = box;
    if (out_damage)
        *out_damage = rect_expand(box, gui_scaled_metric(10));
}

static void stats_format_ms(char *out, size_t size, uint64_t cycles)
{
    uint64_t us = wm_tsc_to_us(cycles);
    snprintf(out, size, "%llu.%02llu", static_cast<unsigned long long>(us / 1000u),
             static_cast<unsigned long long>((us % 1000u) / 10u));
}

void draw_stats_overlay_clipped(const DirtyRect &clip)
{
    if (!g_backbuffer.buffer || (g_system_flags & SYSTEM_FLAG_SHOW_DEBUG_STATS) == 0)
        return;

    DirtyRect box = {};
    DirtyRect damage = {};
    wm_stats_overlay_bounds(&box, &damage);
    if (!rect_intersection(clip, damage, nullptr))
        return;

    DisplayStatus status = {};
    display_get_status(&status);

    int radius = gui_radius_md();
    gui_draw_panel_shadow(&g_backbuffer, box.x, box.y, box.w, box.h, radius);
    gui_draw_chrome_frame(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface, true);

    const GuiFont *mono = gui_font_mono();
    int lh = gui_font_line_height(mono) + 2;
    int text_x = box.x + gui_space_1_5();
    int text_y = box.y + gui_space_1();
    int max_w = box.w - gui_space_3();
    char line[96];
    char a[24];
    char b[24];

    stats_format_ms(a, sizeof(a), g_frame_stats.last_frame_ticks);
    stats_format_ms(b, sizeof(b), g_frame_stats.max_frame_ticks);
    snprintf(line, sizeof(line), "frame %sms max %sms", a, b);
    gui_draw_text_clipped(&g_backbuffer, mono, text_x, text_y, max_w, line, g_gui_style.text, g_gui_style.app_surface);
    text_y += lh;

    stats_format_ms(a, sizeof(a), g_frame_stats.last_compose_ticks);
    stats_format_ms(b, sizeof(b), g_frame_stats.last_present_ticks);
    snprintf(line, sizeof(line), "comp %sms pres %sms skip %llu", a, b,
             static_cast<unsigned long long>(g_frame_stats.frames_skipped));
    gui_draw_text_clipped(&g_backbuffer, mono, text_x, text_y, max_w, line, g_gui_style.text, g_gui_style.app_surface);
    text_y += lh;

    stats_format_ms(a, sizeof(a), g_frame_stats.last_input_to_submit_ticks);
    snprintf(line, sizeof(line), "in>sub %sms dmg %llukpx %ur", a,
             static_cast<unsigned long long>(g_frame_stats.last_dirty_area / 1000u), g_frame_stats.last_dirty_rects);
    gui_draw_text_clipped(&g_backbuffer, mono, text_x, text_y, max_w, line, g_gui_style.text, g_gui_style.app_surface);
    text_y += lh;

    snprintf(line, sizeof(line), "vram %llu px %llu ticks", static_cast<unsigned long long>(status.last_present_pixels),
             static_cast<unsigned long long>(status.last_vram_copy_ticks));
    gui_draw_text_clipped(&g_backbuffer, mono, text_x, text_y, max_w, line, g_gui_style.text_dim,
                          g_gui_style.app_surface);
    text_y += lh;

    snprintf(line, sizeof(line), "sub %llu built %llu swcur %llu",
             static_cast<unsigned long long>(g_frame_stats.frames_submitted),
             static_cast<unsigned long long>(g_frame_stats.frames_built),
             static_cast<unsigned long long>(g_frame_stats.cursor_software_frames));
    gui_draw_text_clipped(&g_backbuffer, mono, text_x, text_y, max_w, line, g_gui_style.text_dim,
                          g_gui_style.app_surface);
}

