#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/sysinfo.h>

#include "../../libc/unistd.h"
#include "../../libgui/gui.h"

static inline int iabs(int v)
{
    return v < 0 ? -v : v;
}

static inline float fabs_float(float x)
{
    return x < 0.0f ? -x : x;
}

static inline double fabs_double(double x)
{
    return x < 0.0 ? -x : x;
}

static float fast_sqrt(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    bits = (bits + 0x3f800000u) >> 1;
    float guess;
    memcpy(&guess, &bits, sizeof(guess));
    guess = 0.5f * (guess + x / guess);
    guess = 0.5f * (guess + x / guess);
    return guess;
}

static float fast_sin(float x)
{
    while (x > 3.14159265f)
        x -= 6.28318531f;
    while (x < -3.14159265f)
        x += 6.28318531f;

    if (x > 1.57079632f)
        x = 3.14159265f - x;
    else if (x < -1.57079632f)
        x = -3.14159265f - x;

    float xx = x * x;
    return x * (1.0f - xx * (0.16666667f - xx * (0.00833333f - xx * 0.00019841f)));
}

static float fast_cos(float x)
{
    return fast_sin(x + 1.57079637f);
}

static inline void blend_rgba_over_rgb(uint32_t *dst, uint32_t color, uint8_t coverage)
{
    uint32_t src_a = ((color >> 24) & 0xFFu) * coverage / 255u;
    if (src_a == 0)
        return;
    uint32_t d = *dst;
    if (src_a >= 255) {
        *dst = 0xFF000000u | (color & 0x00FFFFFFu);
        return;
    }

    uint32_t inv_a = 255u - src_a;
    uint32_t s_rb = color & 0x00FF00FFu;
    uint32_t d_rb = d & 0x00FF00FFu;
    uint32_t rb = (s_rb * src_a + d_rb * inv_a + 0x00800080u);
    rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
    rb &= 0x00FF00FFu;

    uint32_t s_g = (color >> 8) & 0xFFu;
    uint32_t d_g = (d >> 8) & 0xFFu;
    uint32_t g = (s_g * src_a + d_g * inv_a + 127u) / 255u;

    *dst = 0xFF000000u | rb | (g << 8);
}

static inline void clock_blend_pixel(Surface *win, int x, int y, uint32_t color, uint8_t alpha)
{
    if (x < 0 || y < 0 || x >= (int)win->width || y >= (int)win->height)
        return;
    blend_rgba_over_rgb(&win->buffer[y * (win->pitch / 4) + x], color, alpha);
}

static void draw_aa_line_thick(Surface *win, float x1, float y1, float x2, float y2, int thickness, uint32_t color)
{
    if (!win)
        return;

    int min_x = (int)(x1 < x2 ? x1 : x2);
    int max_x = (int)(x1 > x2 ? x1 : x2);
    int min_y = (int)(y1 < y2 ? y1 : y2);
    int max_y = (int)(y1 > y2 ? y1 : y2);

    int pad_int = thickness / 2 + 2;
    min_x -= pad_int;
    min_y -= pad_int;
    max_x += pad_int;
    max_y += pad_int;

    if (min_x < 0)
        min_x = 0;
    if (min_y < 0)
        min_y = 0;
    if (max_x >= (int)win->width)
        max_x = (int)win->width - 1;
    if (max_y >= (int)win->height)
        max_y = (int)win->height - 1;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len_sq = dx * dx + dy * dy;
    float len = fast_sqrt(len_sq);

    if (len < 0.5f) {
        if (thickness > 1)
            gui_fill_circle(win, (int)(x1 + 0.5f), (int)(y1 + 0.5f), thickness / 2, color);
        else
            clock_blend_pixel(win, (int)(x1 + 0.5f), (int)(y1 + 0.5f), color, 255);
        return;
    }

    float nx = dx / len;
    float ny = dy / len;
    float half_t = thickness * 0.5f;
    float pad = half_t + 1.0f;

    uint32_t pitch = win->pitch / 4;

    for (int py = min_y; py <= max_y; py++) {
        float wy = (float)py + 0.5f - y1;
        int start_x = min_x;
        int end_x = max_x;

        if (fabs_float(ny) > 0.01f) {
            float inv_ny = 1.0f / ny;
            float val1 = (wy * nx - pad) * inv_ny;
            float val2 = (wy * nx + pad) * inv_ny;
            float bound1 = x1 - 0.5f + val1;
            float bound2 = x1 - 0.5f + val2;
            int bmin = (int)(bound1 < bound2 ? bound1 : bound2) - (int)pad - 1;
            int bmax = (int)(bound1 > bound2 ? bound1 : bound2) + (int)pad + 1;
            if (bmin > start_x)
                start_x = bmin;
            if (bmax < end_x)
                end_x = bmax;
        }

        if (start_x < min_x)
            start_x = min_x;
        if (end_x > max_x)
            end_x = max_x;

        uint32_t *row = &win->buffer[py * pitch];

        for (int px = start_x; px <= end_x; px++) {
            float wx = (float)px + 0.5f - x1;
            float dot = wx * nx + wy * ny;
            float dist;

            if (dot <= 0.0f) {
                dist = fast_sqrt(wx * wx + wy * wy);
            } else if (dot >= len) {
                float ex = (float)px + 0.5f - x2;
                float ey = (float)py + 0.5f - y2;
                dist = fast_sqrt(ex * ex + ey * ey);
            } else {
                dist = fabs_float(wx * ny - wy * nx);
            }

            float coverage = half_t + 0.5f - dist;
            if (coverage <= 0.0f)
                continue;

            if (coverage >= 1.0f) {
                blend_rgba_over_rgb(&row[px], color, 255);
            } else {
                blend_rgba_over_rgb(&row[px], color, (uint8_t)(coverage * 255.0f));
            }
        }
    }
}

static void draw_clock_hand(Surface *win, int cx, int cy, float angle, int length, int tail_len, int thickness,
                            uint32_t color, uint32_t shadow_color)
{
    if (!win || length <= 0)
        return;

    float rad = angle - 1.57079637f;
    float dx = fast_cos(rad);
    float dy = fast_sin(rad);

    float x1 = (float)cx - dx * (float)tail_len;
    float y1 = (float)cy - dy * (float)tail_len;
    float x2 = (float)cx + dx * (float)length;
    float y2 = (float)cy + dy * (float)length;

    if (shadow_color != 0) {
        draw_aa_line_thick(win, x1 + 1.0f, y1 + 2.0f, x2 + 1.0f, y2 + 2.0f, thickness, shadow_color);
    }
    draw_aa_line_thick(win, x1, y1, x2, y2, thickness, color);
}

static void draw_tick_marks(Surface *win, int cx, int cy, int r)
{
    if (!win || r <= 0)
        return;

    uint32_t tick_color = g_gui_style.text_dim;
    uint32_t tick_major_color = g_gui_style.text;

    for (int i = 0; i < 60; i++) {
        float angle = (float)i * 0.10471976f;
        float dx = fast_cos(angle);
        float dy = fast_sin(angle);
        bool major = (i % 5) == 0;
        bool cardinal = (i % 15) == 0;

        if (major) {
            int tick_inner = cardinal ? r - gui_scaled_metric(14) : r - gui_scaled_metric(10);
            int tick_outer = r - gui_scaled_metric(2);
            float x1 = (float)cx + dx * tick_inner;
            float y1 = (float)cy + dy * tick_inner;
            float x2 = (float)cx + dx * tick_outer;
            float y2 = (float)cy + dy * tick_outer;
            draw_aa_line_thick(win, x1, y1, x2, y2, gui_scaled_metric(2), tick_major_color);
        } else {
            int tick_inner = r - gui_scaled_metric(4);
            int tick_outer = r - gui_scaled_metric(2);
            float x1 = (float)cx + dx * tick_inner;
            float y1 = (float)cy + dy * tick_inner;
            float x2 = (float)cx + dx * tick_outer;
            float y2 = (float)cy + dy * tick_outer;
            draw_aa_line_thick(win, x1, y1, x2, y2, gui_scaled_metric(1), tick_color);
        }
    }
}

static void draw_clock_face_static(Surface *win, int cx, int cy, int r)
{
    if (!win || r <= 0)
        return;

    uint32_t face_bg = g_gui_style.app_surface;
    uint32_t face_border = g_gui_style.border;

    gui_fill_circle(win, cx, cy, r, face_bg);
    gui_draw_circle_stroke(win, cx, cy, r, 2, face_border);
    if (r > 4)
        gui_draw_circle_stroke(win, cx, cy, r - 1, 1, face_border);

    draw_tick_marks(win, cx, cy, r);
}

static void draw_clock_face_hands(Surface *win, int cx, int cy, int r, double continuous_seconds)
{
    if (!win || r <= 0)
        return;

    uint32_t hand_hour = g_gui_style.text;
    uint32_t hand_min = g_gui_style.text_dim;
    uint32_t hand_sec = g_gui_style.danger;
    uint32_t center_color = g_gui_style.accent;
    uint32_t face_border = g_gui_style.border;
    uint32_t shadow = 0x20000000;

    float sec_angle = (float)continuous_seconds * 0.10471976f;
    float min_angle = (float)(continuous_seconds / 60.0) * 0.10471976f;
    float hour_angle = (float)(continuous_seconds / 3600.0) * 0.52359878f;

    int hour_len = r - gui_scaled_metric(28);
    int min_len = r - gui_scaled_metric(14);
    int sec_len = r - gui_scaled_metric(8);
    int tail_len = gui_scaled_metric(16);

    draw_clock_hand(win, cx, cy, hour_angle, hour_len, tail_len, gui_scaled_metric(3), hand_hour, shadow);
    draw_clock_hand(win, cx, cy, min_angle, min_len, tail_len, gui_scaled_metric(2), hand_min, shadow);
    draw_clock_hand(win, cx, cy, sec_angle, sec_len, tail_len + gui_scaled_metric(4), 1, hand_sec, 0);

    int center_r = gui_scaled_metric(4);
    gui_fill_circle(win, cx, cy, center_r, center_color);
    if (center_r > 1)
        gui_draw_circle_stroke(win, cx, cy, center_r, 1, face_border);
}

static double os_dial_seconds(const SysTime *t)
{
    return (double)(t->hour % 12) * 3600.0 + (double)t->minute * 60.0 + (double)t->second;
}

static bool clock_rtc_fallback(const SysTime *t)
{
    return t->year == 2026 && t->month == 1 && t->day == 1 && t->hour == 0 && t->minute == 0 && t->second == 0;
}

static inline uint64_t clock_read_tsc()
{
    uint32_t lo = 0, hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}

static double wrap_dial_seconds(double v)
{
    while (v < 0.0)
        v += 43200.0;
    while (v >= 43200.0)
        v -= 43200.0;
    return v;
}

static double dial_step(double from, double to)
{
    double d = to - from;
    if (d < -21600.0)
        d += 43200.0;
    if (d > 21600.0)
        d -= 43200.0;
    return d;
}

static int weekday(uint16_t year, uint8_t month, uint8_t day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = (int)year;
    int m = (int)month;
    int d = (int)day;
    y -= m < 3;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

struct ClockCache
{
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    int face_r;
    int cx;
    int cy;
    bool valid;
};

static void invalidate_cache(ClockCache *cache)
{
    if (cache)
        cache->valid = false;
}

static void rebuild_cache(ClockCache *cache, Surface *win, int cx, int cy, int face_r)
{
    if (!cache || !win)
        return;

    uint32_t needed = win->width * win->height;
    if (!cache->buffer || cache->width != win->width || cache->height != win->height) {
        free(cache->buffer);
        cache->buffer = (uint32_t *)malloc(needed * sizeof(uint32_t));
        cache->width = win->width;
        cache->height = win->height;
    }
    if (!cache->buffer) {
        cache->valid = false;
        return;
    }

    Surface cache_surf = {};
    cache_surf.buffer = cache->buffer;
    cache_surf.width = win->width;
    cache_surf.height = win->height;
    cache_surf.pitch = win->width * sizeof(uint32_t);
    cache_surf.owns_buffer = false;

    gui_fill_surface(&cache_surf, g_gui_style.app_bg);
    draw_clock_face_static(&cache_surf, cx, cy, face_r);

    cache->face_r = face_r;
    cache->cx = cx;
    cache->cy = cy;
    cache->valid = true;
}

static void blit_cache_to_window(ClockCache *cache, Surface *win)
{
    if (!cache || !cache->valid || !win || !win->buffer)
        return;

    uint32_t src_pitch = cache->width;
    uint32_t dst_pitch = win->pitch / 4;
    uint32_t h = cache->height < win->height ? cache->height : win->height;
    uint32_t w = cache->width < win->width ? cache->width : win->width;

    for (uint32_t y = 0; y < h; y++) {
        memcpy(&win->buffer[y * dst_pitch], &cache->buffer[y * src_pitch], w * sizeof(uint32_t));
    }
}

static void draw_clock_text(Surface *win, int cx, int cy, int face_r, const SysTime *time)
{
    if (!win || !time)
        return;

    int w = (int)win->width;
    int pad = gui_scaled_metric(16);

    const GuiFont *font = gui_font_title();
    char digital[32];
    snprintf(digital, sizeof(digital), "%02u:%02u:%02u", (unsigned)time->hour, (unsigned)time->minute,
             (unsigned)time->second);

    int dig_w = gui_measure_text(font, digital);
    int dig_x = cx - dig_w / 2;
    int dig_y = cy + face_r + gui_scaled_metric(10);
    gui_draw_text_clipped(win, font, dig_x, dig_y, w - pad * 2, digital, g_gui_style.text, g_gui_style.app_bg);

    const GuiFont *sfont = gui_font_default();
    char date_str[64];
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    int wd = weekday(time->year, time->month, time->day);
    snprintf(date_str, sizeof(date_str), "%s, %s %u, %u", weekdays[wd], months[(time->month - 1) % 12],
             (unsigned)time->day, (unsigned)time->year);

    int date_w = gui_measure_text(sfont, date_str);
    int date_x = cx - date_w / 2;
    int date_y = dig_y + gui_font_line_height(font) + gui_space_1();
    gui_draw_text_clipped(win, sfont, date_x, date_y, w - pad * 2, date_str, g_gui_style.text_dim, g_gui_style.app_bg);
}

extern "C" int main()
{
    int win_w = gui_scaled_metric(360);
    int win_h = gui_scaled_metric(420);
    Surface win = gui_register_window_ex("Clock", (uint32_t)win_w, (uint32_t)win_h, WIN_FLAG_RESIZABLE);
    if (!win.buffer)
        return 1;

    gui_window_set_min_size(gui_scaled_metric(280), gui_scaled_metric(320));
    gui_sync_theme_from_registry();
    gui_request_focus();

    uint32_t backbuffer_stride = win.pitch / 4;
    uint32_t *backbuffer_data = (uint32_t *)malloc(backbuffer_stride * win.height * sizeof(uint32_t));
    if (!backbuffer_data)
        return 1;
    Surface backbuffer = win;
    backbuffer.buffer = backbuffer_data;
    backbuffer.owns_buffer = false;

    SystemProfile profile = {};
    uint32_t nominal_tick_hz = 1000;
    if (get_sysinfo(&profile) == 0 && profile.timer_hz != 0)
        nominal_tick_hz = profile.timer_hz;

    SysTime current_os_time = {};

    // Smooth clock model. A monotonic counter (TSC when available: it keeps
    // running even while scheduler-timer IRQs are masked) free-runs the hands,
    // and an RTC phase servo trims it into alignment. Each RTC second boundary
    // feeds a rate-limited correction, so the hands sweep continuously and
    // never snap, freeze, or reverse.
    uint64_t tsc_mhz = get_tsc_freq();
    bool use_tsc = tsc_mhz != 0;
    bool rtc_valid = get_time(&current_os_time) == 0 && !clock_rtc_fallback(&current_os_time);

    double counter_hz = use_tsc ? (double)tsc_mhz * 1000000.0 : (double)nominal_tick_hz;
    uint64_t anchor_counter = use_tsc ? clock_read_tsc() : get_ticks();
    double anchor_seconds = rtc_valid ? os_dial_seconds(&current_os_time) : 0.0;
    bool anchored = rtc_valid;

    double correction = 0.0;
    double correction_target = 0.0;
    double prev_rtc_seconds = anchor_seconds;
    uint64_t prev_obs_counter = anchor_counter;
    uint64_t last_counter = anchor_counter;
    double candidate_seconds = -1.0;
    int candidate_reads = 0;

    double ticks_per_second = (double)nominal_tick_hz;
    bool ticks_rate_locked = false;
    int seed_rejects = 0;
    double relock_candidate = 0.0;
    int relock_reads = 0;
    bool have_cross_ticks = false;
    uint64_t last_cross_ticks = 0;

    uint64_t frame_ticks = (uint64_t)(ticks_per_second * 0.016);
    if (frame_ticks == 0)
        frame_ticks = 1;
    uint64_t next_frame_ticks = get_ticks() + frame_ticks;

    Registry *registry = gui_registry();
    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;

    ClockCache cache = {};
    int cx = 0, cy = 0, face_r = 0, w = 0, h = 0;
    bool needs_full_redraw = true;

    while (true) {
        Event ev = {};
        bool resized = false;

        while (poll_event(&ev) > 0) {
            if (ev.type == EVT_WINDOW_CLOSE) {
                free(backbuffer_data);
                return 0;
            }
            if (ev.type == EVT_WINDOW_RESIZE && gui_sync_window_size(&win) > 0) {
                resized = true;
                free(backbuffer_data);
                backbuffer_stride = win.pitch / 4;
                backbuffer_data = (uint32_t *)malloc(backbuffer_stride * win.height * sizeof(uint32_t));
                backbuffer = win;
                backbuffer.buffer = backbuffer_data;
                if (!backbuffer_data) {
                    free(cache.buffer);
                    return 1;
                }
                continue;
            }
        }

        registry = gui_registry();
        bool theme_changed = false;
        if (registry && registry->settings_generation != last_settings_generation) {
            last_settings_generation = registry->settings_generation;
            theme_changed = gui_sync_theme_from_registry();
        }

        uint64_t now_counter = use_tsc ? clock_read_tsc() : get_ticks();
        uint64_t now_ticks = use_tsc ? get_ticks() : now_counter;

        double dt = 0.0;
        if (now_counter > last_counter)
            dt = (double)(now_counter - last_counter) / counter_hz;
        last_counter = now_counter;

        bool rtc_ok = get_time(&current_os_time) == 0 && !clock_rtc_fallback(&current_os_time);
        uint64_t after_counter = use_tsc ? clock_read_tsc() : get_ticks();
        uint64_t obs_counter = now_counter + (after_counter - now_counter) / 2;

        if (rtc_ok) {
            double rtc_seconds = os_dial_seconds(&current_os_time);

            if (rtc_seconds == prev_rtc_seconds) {
                candidate_reads = 0;
            } else {
                double step = dial_step(prev_rtc_seconds, rtc_seconds);
                bool accept;

                if (step == 1.0) {
                    accept = true;
                } else {
                    // Missed boundaries (stalled frames) or a CMOS glitch: trust
                    // the value only after two consecutive matching reads.
                    if (rtc_seconds == candidate_seconds)
                        candidate_reads++;
                    else {
                        candidate_seconds = rtc_seconds;
                        candidate_reads = 1;
                    }
                    accept = candidate_reads >= 2;
                }

                if (accept) {
                    candidate_reads = 0;

                    // Scheduler ticks per RTC second, measured across boundary
                    // observations: paces the frame loop at the true rate (and is
                    // the tick-clock rate estimate when TSC is unavailable).
                    if (have_cross_ticks && step > 0.0 && now_ticks > last_cross_ticks) {
                        double per_second = (double)(now_ticks - last_cross_ticks) / step;
                        if (!ticks_rate_locked) {
                            if ((per_second >= ticks_per_second * 0.5 && per_second <= ticks_per_second * 2.0) ||
                                ++seed_rejects >= 3) {
                                ticks_per_second = per_second;
                                ticks_rate_locked = true;
                            }
                        } else if (per_second >= ticks_per_second * 0.75 && per_second <= ticks_per_second * 1.3334) {
                            ticks_per_second = ticks_per_second * 0.5 + per_second * 0.5;
                            relock_reads = 0;
                        } else {
                            // A single corrupted interval (e.g. scheduler ticks
                            // paused) must not re-lock the rate outright; require
                            // several consistent readings first.
                            if (relock_reads > 0 && fabs_double(per_second - relock_candidate) <= per_second * 0.15)
                                relock_reads++;
                            else {
                                relock_candidate = per_second;
                                relock_reads = 1;
                            }
                            if (relock_reads >= 3) {
                                ticks_per_second = per_second;
                                relock_reads = 0;
                            }
                        }
                        frame_ticks = (uint64_t)(ticks_per_second * 0.016);
                        if (frame_ticks == 0)
                            frame_ticks = 1;
                    }
                    have_cross_ticks = true;
                    last_cross_ticks = now_ticks;

                    if (!anchored) {
                        anchor_seconds = rtc_seconds;
                        anchor_counter = obs_counter;
                        anchored = true;
                        correction = 0.0;
                        correction_target = 0.0;
                        if (!use_tsc && ticks_per_second > 0.0)
                            counter_hz = ticks_per_second;
                    } else {
                        double display_at_obs =
                            anchor_seconds + ((double)obs_counter - (double)anchor_counter) / counter_hz + correction;
                        double err_now = dial_step(rtc_seconds, display_at_obs);

                        if (err_now > 2.0 || err_now < -2.0) {
                            // A genuine wall-clock discontinuity (RTC was set,
                            // resume from suspend): a single visible step is the
                            // only sane response.
                            anchor_seconds = rtc_seconds;
                            anchor_counter = obs_counter;
                            correction = 0.0;
                            correction_target = 0.0;
                            if (!use_tsc && ticks_per_second > 0.0)
                                counter_hz = ticks_per_second;
                        } else {
                            double bracket_seconds = (double)(obs_counter - prev_obs_counter) / counter_hz;
                            if (bracket_seconds > 0.0 && bracket_seconds <= 0.1 && step == 1.0) {
                                // The RTC boundary is bracketed by the previous and
                                // current observations; servo out the residual phase
                                // without any discontinuity.
                                double boundary_counter =
                                    (double)prev_obs_counter + (double)(obs_counter - prev_obs_counter) * 0.5;
                                double display_at_boundary = anchor_seconds +
                                                             (boundary_counter - (double)anchor_counter) / counter_hz +
                                                             correction;
                                double err = dial_step(rtc_seconds, display_at_boundary);
                                correction_target = correction - err;
                                if (correction_target > 5.0)
                                    correction_target = 5.0;
                                else if (correction_target < -5.0)
                                    correction_target = -5.0;
                            }
                            // Wide bracket (missed frames): the free-running counter
                            // already keeps true time and the boundary cannot be
                            // localized (err_now is contaminated by the sub-second
                            // phase), so leave the correction alone; the next tight
                            // bracket re-measures any residual.
                        }

                        {
                            // Re-anchor to the current counter so the elapsed math
                            // stays precise and the accumulated correction is folded
                            // into the anchor (it integrates any counter-rate bias,
                            // so it must not grow unbounded); continuity preserved.
                            double display_now = anchor_seconds +
                                                 ((double)now_counter - (double)anchor_counter) / counter_hz +
                                                 correction;
                            anchor_seconds = wrap_dial_seconds(display_now);
                            anchor_counter = now_counter;
                            correction_target -= correction;
                            correction = 0.0;
                            if (!use_tsc && ticks_per_second > 0.0)
                                counter_hz = ticks_per_second;
                        }
                    }

                    prev_rtc_seconds = rtc_seconds;
                }
            }
            prev_obs_counter = obs_counter;
        }

        // Apply the RTC correction at a bounded rate: hand speed stays within
        // [0.7x, 1.3x], so motion is always forward and smooth.
        double corr_diff = correction_target - correction;
        if (corr_diff != 0.0) {
            double rate = corr_diff * 2.0;
            if (rate > 0.3)
                rate = 0.3;
            else if (rate < -0.3)
                rate = -0.3;
            double dc = rate * dt;
            if (corr_diff > 0.0 && dc > corr_diff)
                dc = corr_diff;
            else if (corr_diff < 0.0 && dc < corr_diff)
                dc = corr_diff;
            correction += dc;
            if (correction_target - correction > -0.0001 && correction_target - correction < 0.0001)
                correction = correction_target;
        }

        double elapsed_seconds =
            now_counter >= anchor_counter ? (double)(now_counter - anchor_counter) / counter_hz : 0.0;
        double continuous_seconds = wrap_dial_seconds(anchor_seconds + elapsed_seconds + correction);

        w = (int)win.width;
        h = (int)win.height;
        int pad = gui_scaled_metric(16);

        face_r = (w < h - gui_scaled_metric(80)) ? (w / 2 - pad) : (h / 2 - gui_scaled_metric(52));
        if (face_r < gui_scaled_metric(60))
            face_r = gui_scaled_metric(60);

        cx = w / 2;
        cy = h / 2 - gui_scaled_metric(12);

        bool full_frame = needs_full_redraw || resized || theme_changed;
        if (full_frame) {
            rebuild_cache(&cache, &win, cx, cy, face_r);
            needs_full_redraw = false;
        }

        blit_cache_to_window(&cache, &backbuffer);
        draw_clock_face_hands(&backbuffer, cx, cy, face_r, continuous_seconds);
        draw_clock_text(&backbuffer, cx, cy, face_r, &current_os_time);

        if (full_frame) {
            memcpy(win.buffer, backbuffer.buffer, (size_t)backbuffer_stride * win.height * sizeof(uint32_t));
            gui_blit_to_screen_rect(&win, 0, 0, win.width, win.height);
        } else {
            // Only the face (hands sweep) and the text below it change frame
            // to frame: publish damage for those regions instead of the whole
            // window to keep the compositor load low at 60 fps.
            int fx0 = cx - face_r - 2;
            int fy0 = cy - face_r - 2;
            int fx1 = cx + face_r + 2;
            int fy1 = cy + face_r + 2;
            if (fx0 < 0)
                fx0 = 0;
            if (fy0 < 0)
                fy0 = 0;
            if (fx1 > (int)win.width)
                fx1 = (int)win.width;
            if (fy1 > (int)win.height)
                fy1 = (int)win.height;
            int text_top = cy + face_r + gui_scaled_metric(10);
            if (text_top < 0)
                text_top = 0;
            if (text_top > (int)win.height)
                text_top = (int)win.height;

            for (int y = fy0; y < fy1; y++) {
                memcpy(&win.buffer[(size_t)y * backbuffer_stride + fx0],
                       &backbuffer.buffer[(size_t)y * backbuffer_stride + fx0],
                       (size_t)(fx1 - fx0) * sizeof(uint32_t));
            }
            for (int y = text_top; y < (int)win.height; y++) {
                memcpy(&win.buffer[(size_t)y * backbuffer_stride], &backbuffer.buffer[(size_t)y * backbuffer_stride],
                       (size_t)backbuffer_stride * sizeof(uint32_t));
            }

            if (fx1 > fx0 && fy1 > fy0)
                gui_blit_to_screen_rect(&win, fx0, fy0, fx1 - fx0, fy1 - fy0);
            if (text_top < (int)win.height)
                gui_blit_to_screen_rect(&win, 0, text_top, win.width, win.height - text_top);
        }

        sleep_until_ticks(next_frame_ticks);
        uint64_t frame_now = get_ticks();
        next_frame_ticks += frame_ticks;
        if (frame_now > next_frame_ticks)
            next_frame_ticks = frame_now + frame_ticks;
    }
}