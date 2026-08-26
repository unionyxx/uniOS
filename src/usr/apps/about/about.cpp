#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/display.h>
#include <uapi/event.h>
#include <uapi/sysinfo.h>

#include "../../libc/unistd.h"
#include "../../libgui/gui.h"

static void format_size_kb(uint64_t kb, char *out, size_t out_size)
{
    if (kb >= 1048576ull) {
        uint64_t gib10 = (kb * 10ull) >> 20;
        snprintf(out, out_size, "%llu.%llu GiB", (unsigned long long)(gib10 / 10ull),
                 (unsigned long long)(gib10 % 10ull));
        return;
    }

    uint64_t mib10 = (kb * 10ull) >> 10;
    snprintf(out, out_size, "%llu.%llu MiB", (unsigned long long)(mib10 / 10ull), (unsigned long long)(mib10 % 10ull));
}

static void format_uptime(uint64_t uptime, char *out, size_t out_size)
{
    uint64_t days = uptime / 86400ull;
    uint64_t hours = (uptime / 3600ull) % 24ull;
    uint64_t mins = (uptime / 60ull) % 60ull;
    uint64_t secs = uptime % 60ull;
    if (days > 0) {
        snprintf(out, out_size, "%llud %02lluh %02llum %02llus", (unsigned long long)days, (unsigned long long)hours,
                 (unsigned long long)mins, (unsigned long long)secs);
    } else {
        snprintf(out, out_size, "%02lluh %02llum %02llus", (unsigned long long)hours, (unsigned long long)mins,
                 (unsigned long long)secs);
    }
}

static void format_display_flags(uint32_t flags, char *out, size_t out_size)
{
    out[0] = '\0';
    char *p = out;
    size_t rem = out_size;

    auto append = [&](const char *str) {
        if (p != out && rem > 2) {
            *p++ = ',';
            *p++ = ' ';
            rem -= 2;
        }
        size_t len = strlen(str);
        if (len >= rem)
            len = rem - 1;
        memcpy(p, str, len);
        p += len;
        rem -= len;
        *p = '\0';
    };

    if (flags & DISPLAY_FLAG_HAS_VBLANK)
        append("VBlank");
    if (flags & DISPLAY_FLAG_HAS_PAGE_FLIP)
        append("Page Flip");
    if (flags & DISPLAY_FLAG_HAS_CURSOR_PLANE)
        append("Cursor Plane");
    if (flags & DISPLAY_FLAG_HAS_OVERLAY)
        append("Overlay");
    if (flags & DISPLAY_FLAG_HAS_COMPOSITOR)
        append("Compositor");
    if (flags & DISPLAY_FLAG_USES_COPY_PATH)
        append("Copy Path");
    if (flags & DISPLAY_FLAG_STRICT_SYNC_ONLY)
        append("Strict Sync");
    if (p == out)
        snprintf(out, out_size, "Basic");
}

static void format_refresh_value(uint32_t refresh_millihz, char *out, size_t out_size)
{
    if (refresh_millihz == 0) {
        snprintf(out, out_size, "Unavailable");
        return;
    }
    snprintf(out, out_size, "%u.%03u Hz", refresh_millihz / 1000u, refresh_millihz % 1000u);
}

static void cpu_vendor(char *out)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *reinterpret_cast<uint32_t *>(&out[0]) = ebx;
    *reinterpret_cast<uint32_t *>(&out[4]) = edx;
    *reinterpret_cast<uint32_t *>(&out[8]) = ecx;
    out[12] = '\0';
}

static void draw_section(Surface *win, int x, int y, int w, int h, const char *title)
{
    gui_draw_card(win, x, y, w, h, title);
}

struct DetailItem
{
    const char *label;
    const char *value;
};

static inline int about_row_h()
{
    int compact_row_h = gui_line_height() + gui_space_0_5();
    if (compact_row_h < gui_scaled_metric(20))
        compact_row_h = gui_scaled_metric(20);
    return compact_row_h;
}

static inline int about_summary_h()
{
    int title_line_h = gui_font_line_height(gui_font_title());
    int badge_h = gui_badge_h();
    int line_h = gui_line_height();
    int content_h =
        ((title_line_h > badge_h) ? title_line_h : badge_h) + gui_space_0_5() + line_h + gui_space_0_5() + line_h;
    return gui_card_header_h() + gui_space_2() + content_h + gui_space_1();
}

static void draw_detail_row(Surface *win, int x, int y, int w, int h, const DetailItem &item, uint32_t bg)
{
    (void)bg;
    if (!item.label || !item.value || w <= 0 || h <= 0)
        return;
    int text_y = y + (h - gui_line_height()) / 2;
    gui_draw_metric_row(win, x, text_y, w, item.label, item.value, false);
}

static inline int panel_content_top(int y)
{
    return y + gui_card_header_h() + gui_space_2();
}

struct AboutSnapshot
{
    SystemProfile profile;
    MemInfo mem;
    DisplayCaps caps;
    SysTime now;
    uint64_t uptime_seconds;
    int proc_count;
    char vendor[16];
};

static void collect_about_snapshot(AboutSnapshot *snapshot)
{
    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof(*snapshot));

    static char s_vendor[16] = {0};
    static SystemProfile s_profile = {};
    static bool s_static_loaded = false;

    if (!s_static_loaded) {
        cpu_vendor(s_vendor);
        get_sysinfo(&s_profile);
        s_static_loaded = true;
    }

    memcpy(snapshot->vendor, s_vendor, sizeof(snapshot->vendor));
    snapshot->profile = s_profile;

    get_meminfo(&snapshot->mem);
    display_get_caps(&snapshot->caps);
    get_time(&snapshot->now);

    ProcessInfo procs[64];
    snapshot->proc_count = get_procs(procs, 64);
    if (snapshot->proc_count < 0)
        snapshot->proc_count = 0;

    snapshot->uptime_seconds = get_uptime();
}

// Structural signature: excludes the fields that tick every second (time,
// uptime, process count) so the periodic refresh only repaints the runtime
// panel instead of the whole window.
static uint32_t about_static_signature(const AboutSnapshot *snapshot)
{
    if (!snapshot)
        return 0;
    AboutSnapshot copy = *snapshot;
    memset(&copy.now, 0, sizeof(copy.now));
    copy.uptime_seconds = 0;
    copy.proc_count = 0;
    uint32_t sig = 2166136261u;
    const uint8_t *data = reinterpret_cast<const uint8_t *>(&copy);
    for (size_t i = 0; i < sizeof(AboutSnapshot); i++) {
        sig ^= data[i];
        sig *= 16777619u;
    }
    return sig;
}

static void about_copy_rect(const Surface *backbuffer, Surface *win, const Rect &rect)
{
    if (gui_rect_is_empty(rect))
        return;
    int x = rect.x > 0 ? rect.x : 0;
    int y = rect.y > 0 ? rect.y : 0;
    int x2 = rect.x + rect.w < (int)win->width ? rect.x + rect.w : (int)win->width;
    int y2 = rect.y + rect.h < (int)win->height ? rect.y + rect.h : (int)win->height;
    if (x2 <= x || y2 <= y)
        return;
    uint32_t stride = win->pitch / 4;
    for (int row = y; row < y2; row++) {
        memcpy(&win->buffer[(size_t)row * stride + x], &backbuffer->buffer[(size_t)row * stride + x],
               (size_t)(x2 - x) * sizeof(uint32_t));
    }
}

static void draw_summary(Surface *win, int x, int y, int w, int h, const char *kernel_commit, bool debug_build)
{
    draw_section(win, x, y, w, h, "Overview");
    int content_x = x + gui_space_2();
    int content_y = panel_content_top(y);
    int title_h = gui_font_line_height(gui_font_title());
    int line_h = gui_line_height();
    const char *build_label = debug_build ? "Debug Build" : "Release Build";
    int badge_w = gui_measure_text(gui_font_default(), build_label) + gui_badge_pad_x() * 2;
    int badge_x = x + w - gui_space_2() - badge_w;
    if (badge_x < content_x + gui_scaled_metric(120))
        badge_x = content_x + gui_scaled_metric(120);
    int title_line_h = (title_h > gui_badge_h()) ? title_h : gui_badge_h();
    int title_y = content_y + (title_line_h - title_h) / 2;
    int badge_y = content_y + (title_line_h - gui_badge_h()) / 2;
    int title_w = badge_x - content_x - gui_space_1();
    if (title_w < gui_scaled_metric(72))
        title_w = gui_scaled_metric(72);

    gui_draw_text_clipped(win, gui_font_title(), content_x, title_y, title_w, "uniOS", g_gui_style.text,
                          g_gui_style.app_surface);
    gui_draw_badge(win, badge_x, badge_y, build_label, g_gui_style.accent_soft, g_gui_style.text);

    char summary[128];
    snprintf(summary, sizeof(summary), "Commit %s  |  x86_64",
             kernel_commit && kernel_commit[0] ? kernel_commit : "unknown");
    int summary_y = content_y + title_line_h + gui_space_0_5();
    int subtitle_y = summary_y + line_h + gui_space_0_5();
    gui_draw_text_clipped(win, gui_font_default(), content_x, summary_y, w - gui_space_4(), summary,
                          g_gui_style.text_dim, g_gui_style.app_surface);
    gui_draw_text_clipped(win, gui_font_default(), content_x, subtitle_y, w - gui_space_4(),
                          "Core system profile and runtime state", g_gui_style.text_muted, g_gui_style.app_surface);
}

static inline int get_panel_height(int rows, int row_h)
{
    return gui_card_header_h() + gui_space_2() + (rows * row_h) + gui_space_1();
}

static void draw_runtime_panel(Surface *win, int x, int y, int w, int h, int row_h, const char *time_buf,
                               const char *uptime_buf, const char *proc_buf)
{
    draw_section(win, x, y, w, h, "Runtime");
    int inner_y = panel_content_top(y);
    DetailItem items[3] = {{"Local Time", time_buf}, {"Uptime", uptime_buf}, {"Processes", proc_buf}};
    for (int i = 0; i < 3; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * row_h, w - gui_space_3(), row_h, items[i],
                        g_gui_style.app_surface);
    }
}

static void draw_display_panel(Surface *win, int x, int y, int w, int h, int row_h, const char *resolution,
                               const char *nominal, const char *measured, const char *depth, const char *flags)
{
    draw_section(win, x, y, w, h, "Display");
    int inner_y = panel_content_top(y);
    DetailItem items[5] = {
        {"Resolution", resolution}, {"Target Refresh", nominal}, {"Actual Refresh", measured}, {"Color Depth", depth},
        {"Flags", flags},
    };
    for (int i = 0; i < 5; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * row_h, w - gui_space_3(), row_h, items[i],
                        g_gui_style.app_surface);
    }
}

static void draw_memory_panel(Surface *win, int x, int y, int w, int h, int row_h, const char *total, const char *used,
                              const char *free_kb, const char *heap_total, const char *heap_used)
{
    draw_section(win, x, y, w, h, "Memory");
    int inner_y = panel_content_top(y);
    DetailItem items[5] = {
        {"Total", total}, {"Used", used}, {"Free", free_kb}, {"Heap Total", heap_total}, {"Heap Used", heap_used}};
    for (int i = 0; i < 5; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * row_h, w - gui_space_3(), row_h, items[i],
                        g_gui_style.app_surface);
    }
}

static void draw_platform_panel(Surface *win, int x, int y, int w, int h, int row_h, const char *vendor,
                                const char *cores, const char *timer_hz, const char *bootloader)
{
    draw_section(win, x, y, w, h, "Platform");
    int inner_y = panel_content_top(y);
    DetailItem items[4] = {
        {"CPU Vendor", vendor}, {"CPU Cores", cores}, {"Timer", timer_hz}, {"Bootloader", bootloader}};
    for (int i = 0; i < 4; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * row_h, w - gui_space_3(), row_h, items[i],
                        g_gui_style.app_surface);
    }
}

static int compute_about_content_height(int content_w, int row_h, int summary_h)
{
    int gap = gui_app_section_gap();
    int h_runtime = get_panel_height(3, row_h);
    int h_display = get_panel_height(5, row_h);
    int h_memory = get_panel_height(5, row_h);
    int h_platform = get_panel_height(4, row_h);

    int min_panel_w = gui_scaled_metric(280);
    int total = summary_h + gap;
    if (content_w >= min_panel_w * 2 + gap) {
        int left_h = h_runtime + gap + h_memory;
        int right_h = h_display + gap + h_platform;
        total += (left_h > right_h ? left_h : right_h);
    } else {
        total += h_runtime + gap + h_display + gap + h_memory + gap + h_platform;
    }
    return total;
}

struct AboutPanelRects
{
    Rect runtime;
    Rect memory;
};

static void draw_about(Surface *window, uint32_t **back_data, size_t *back_capacity, Surface *win,
                       const AboutSnapshot *snapshot, AboutPanelRects *panel_rects)
{
    if (!window || !back_data || !back_capacity || !win || !snapshot)
        return;
    if (panel_rects)
        *panel_rects = {};

    const SystemProfile &profile = snapshot->profile;
    const MemInfo &mem = snapshot->mem;
    const DisplayCaps &caps = snapshot->caps;
    const SysTime &now = snapshot->now;

    char uptime_buf[64];
    char proc_buf[32];
    char total[32], used[32], free_kb[32], heap_total[32], heap_used[32];
    char resolution[32], depth[32], nominal_refresh[32], measured_refresh[32], flags[96], bootloader[96], timer_hz[32],
        cores_buf[16], time_buf[64];

    format_uptime(snapshot->uptime_seconds, uptime_buf, sizeof(uptime_buf));
    snprintf(proc_buf, sizeof(proc_buf), "%d", snapshot->proc_count);
    format_size_kb(mem.total_kb, total, sizeof(total));
    format_size_kb(mem.used_kb, used, sizeof(used));
    format_size_kb(mem.free_kb, free_kb, sizeof(free_kb));
    format_size_kb(mem.heap_total_kb, heap_total, sizeof(heap_total));
    format_size_kb(mem.heap_used_kb, heap_used, sizeof(heap_used));
    snprintf(resolution, sizeof(resolution), "%ux%u", caps.width, caps.height);
    snprintf(depth, sizeof(depth), "%u-bit", caps.bpp);

    uint32_t nominal_refresh_millihz = caps.nominal_refresh_millihz
                                           ? caps.nominal_refresh_millihz
                                           : (caps.refresh_millihz ? caps.refresh_millihz : caps.refresh_hz * 1000u);
    format_refresh_value(nominal_refresh_millihz, nominal_refresh, sizeof(nominal_refresh));
    format_refresh_value(caps.measured_refresh_millihz, measured_refresh, sizeof(measured_refresh));
    format_display_flags(caps.flags, flags, sizeof(flags));

    if (profile.bootloader_version[0]) {
        snprintf(bootloader, sizeof(bootloader), "%s %s",
                 profile.bootloader_name[0] ? profile.bootloader_name : "Unknown", profile.bootloader_version);
    } else {
        snprintf(bootloader, sizeof(bootloader), "%s",
                 profile.bootloader_name[0] ? profile.bootloader_name : "Unknown");
    }
    snprintf(timer_hz, sizeof(timer_hz), "%u Hz", profile.timer_hz);
    snprintf(cores_buf, sizeof(cores_buf), "%u", profile.cpu_count ? profile.cpu_count : 1u);
    snprintf(time_buf, sizeof(time_buf), "%04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day, now.hour,
             now.minute, now.second);

    GuiAppLayout layout = gui_app_begin(win);
    int view_w = layout.outer_w + layout.outer_x * 2;
    int content_w = layout.body_rect.w;
    int bottom_pad = gui_app_outer_padding();

    int row_h = about_row_h();
    int summary_h = about_summary_h();

    int content_total = compute_about_content_height(content_w, row_h, summary_h) + layout.body_rect.y + bottom_pad;
    // Content size and backing growth must act on the real window surface,
    // never on the private backbuffer (a resize would swap its buffer pointer
    // and strand the malloc'd pixels).
    gui_set_content_size(window, view_w, content_total);

    // The backbuffer spans the whole scrollable content, not just the view.
    size_t needed = (size_t)(window->pitch / 4) * window->height;
    if (needed > *back_capacity) {
        uint32_t *grown = (uint32_t *)malloc(needed * sizeof(uint32_t));
        if (!grown)
            return;
        if (*back_data)
            memcpy(grown, *back_data, *back_capacity * sizeof(uint32_t));
        free(*back_data);
        *back_data = grown;
        *back_capacity = needed;
    }
    *win = *window;
    win->buffer = *back_data;
    win->owns_buffer = false;
    layout = gui_app_begin(win);

    const int outer = layout.body_rect.x;
    const int gap = gui_app_section_gap();

    int h_runtime = get_panel_height(3, row_h);
    int h_display = get_panel_height(5, row_h);
    int h_memory = get_panel_height(5, row_h);
    int h_platform = get_panel_height(4, row_h);
    int y = layout.body_rect.y;

    draw_summary(win, outer, y, content_w, summary_h, profile.kernel_commit, profile.kernel_build_debug != 0);
    y += summary_h + gap;

    int min_panel_w = gui_scaled_metric(280);
    if (content_w >= min_panel_w * 2 + gap) {
        int col_w = (content_w - gap) / 2;
        int left_x = outer;
        int right_x = outer + col_w + gap;

        if (panel_rects)
            panel_rects->runtime = gui_rect_make(left_x, y, col_w, h_runtime);
        draw_runtime_panel(win, left_x, y, col_w, h_runtime, row_h, time_buf, uptime_buf, proc_buf);
        draw_display_panel(win, right_x, y, col_w, h_display, row_h, resolution, nominal_refresh, measured_refresh,
                           depth, flags);

        if (panel_rects)
            panel_rects->memory = gui_rect_make(left_x, y + h_runtime + gap, col_w, h_memory);
        draw_memory_panel(win, left_x, y + h_runtime + gap, col_w, h_memory, row_h, total, used, free_kb, heap_total,
                          heap_used);
        draw_platform_panel(win, right_x, y + h_display + gap, col_w, h_platform, row_h, snapshot->vendor, cores_buf,
                            timer_hz, bootloader);
    } else {
        if (panel_rects)
            panel_rects->runtime = gui_rect_make(outer, y, content_w, h_runtime);
        draw_runtime_panel(win, outer, y, content_w, h_runtime, row_h, time_buf, uptime_buf, proc_buf);
        y += h_runtime + gap;
        draw_display_panel(win, outer, y, content_w, h_display, row_h, resolution, nominal_refresh, measured_refresh,
                           depth, flags);
        y += h_display + gap;
        if (panel_rects)
            panel_rects->memory = gui_rect_make(outer, y, content_w, h_memory);
        draw_memory_panel(win, outer, y, content_w, h_memory, row_h, total, used, free_kb, heap_total, heap_used);
        y += h_memory + gap;
        draw_platform_panel(win, outer, y, content_w, h_platform, row_h, snapshot->vendor, cores_buf, timer_hz,
                            bootloader);
    }

    gui_app_draw_header(win, &layout, "About uniOS", "Hardware, runtime, and display overview", nullptr);
}

// The only entry is the reserved About command, which the menubar handles
// itself (it re-focuses this window), so no app-side dispatch is needed.
static void about_publish_menus()
{
    MenuModel model;
    gui_menu_model_reset(&model);

    int help = gui_menu_model_add_menu(&model, "Help");
    gui_menu_model_add_item(&model, help, "About uniOS", MENU_CMD_ABOUT_UNIOS, 0, nullptr);

    gui_menu_publish(&model);
}

extern "C" int main()
{
    Surface win = gui_register_window_ex("About uniOS", (uint32_t)gui_scaled_metric(720),
                                         (uint32_t)gui_scaled_metric(440), WIN_FLAG_RESIZABLE);
    if (!win.buffer)
        return 1;
    gui_window_set_min_size(gui_scaled_metric(560), gui_scaled_metric(420));

    gui_sync_theme_from_registry();
    gui_request_focus();
    about_publish_menus();

    // Double buffer: draw off-screen, then publish whole regions at once so
    // the compositor never samples a half-drawn frame. The buffer is grown by
    // draw_about as the scrollable content height changes.
    size_t back_capacity = (size_t)(win.pitch / 4) * win.height;
    uint32_t *back_data = (uint32_t *)malloc(back_capacity * sizeof(uint32_t));
    if (!back_data)
        return 1;
    Surface backbuffer = win;
    backbuffer.buffer = back_data;
    backbuffer.owns_buffer = false;

    uint64_t last_refresh_tick = 0;
    bool needs_redraw = true;
    uint32_t last_static_signature = 0;
    Registry *registry = gui_registry();
    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;

    while (true) {
        Event ev = {};
        while (poll_event(&ev) > 0) {
            if (ev.type == EVT_WINDOW_CLOSE) {
                free(back_data);
                return 0;
            }
            if (ev.type == EVT_FOCUS) {
                about_publish_menus();
                continue;
            }
            if (ev.type == EVT_WINDOW_RESIZE && gui_sync_window_size(&win) > 0) {
                // draw_about grows the backbuffer to the new content size.
                needs_redraw = true;
            }
        }

        registry = gui_registry();
        if (registry && registry->settings_generation != last_settings_generation) {
            last_settings_generation = registry->settings_generation;
            if (gui_sync_theme_from_registry())
                needs_redraw = true;
        }

        uint64_t now_tick = get_ticks();
        if (needs_redraw || now_tick - last_refresh_tick >= 1000) {
            AboutSnapshot snapshot = {};
            collect_about_snapshot(&snapshot);
            uint32_t static_sig = about_static_signature(&snapshot);
            bool full = needs_redraw || static_sig != last_static_signature;

            AboutPanelRects panels = {};
            draw_about(&win, &back_data, &back_capacity, &backbuffer, &snapshot, &panels);

            if (full) {
                memcpy(win.buffer, backbuffer.buffer, back_capacity * sizeof(uint32_t));
                gui_blit_to_screen_rect(&win, 0, 0, win.width, win.height);
                last_static_signature = static_sig;
            } else {
                // Only time/uptime/process count changed: republish the two
                // panels that display them.
                about_copy_rect(&backbuffer, &win, panels.runtime);
                about_copy_rect(&backbuffer, &win, panels.memory);
                if (!gui_rect_is_empty(panels.runtime))
                    gui_blit_to_screen_rect(&win, panels.runtime.x, panels.runtime.y, panels.runtime.w,
                                            panels.runtime.h);
                if (!gui_rect_is_empty(panels.memory))
                    gui_blit_to_screen_rect(&win, panels.memory.x, panels.memory.y, panels.memory.w, panels.memory.h);
            }
            last_refresh_tick = now_tick;
            needs_redraw = false;
        }

        sleep_ms(50);
    }
}