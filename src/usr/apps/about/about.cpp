#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/display.h>
#include <uapi/event.h>
#include <uapi/sysinfo.h>

#include "../../libc/unistd.h"
#include "../../libgui/gui.h"

static void format_size_kb(uint64_t kb, char *out, size_t out_size)
{
    if (kb >= 1024ull * 1024ull) {
        uint64_t gib10 = (kb * 10ull) / (1024ull * 1024ull);
        snprintf(out, out_size, "%llu.%llu GiB", (unsigned long long)(gib10 / 10ull),
                 (unsigned long long)(gib10 % 10ull));
        return;
    }

    uint64_t mib10 = (kb * 10ull) / 1024ull;
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
    if (flags & DISPLAY_FLAG_HAS_VBLANK)
        strncat(out, out[0] ? ", VBlank" : "VBlank", out_size - strlen(out) - 1);
    if (flags & DISPLAY_FLAG_HAS_PAGE_FLIP)
        strncat(out, out[0] ? ", Page Flip" : "Page Flip", out_size - strlen(out) - 1);
    if (flags & DISPLAY_FLAG_HAS_CURSOR_PLANE)
        strncat(out, out[0] ? ", Cursor Plane" : "Cursor Plane", out_size - strlen(out) - 1);
    if (flags & DISPLAY_FLAG_HAS_OVERLAY)
        strncat(out, out[0] ? ", Overlay" : "Overlay", out_size - strlen(out) - 1);
    if (flags & DISPLAY_FLAG_HAS_COMPOSITOR)
        strncat(out, out[0] ? ", Compositor" : "Compositor", out_size - strlen(out) - 1);
    if (flags & DISPLAY_FLAG_USES_COPY_PATH)
        strncat(out, out[0] ? ", Copy Path" : "Copy Path", out_size - strlen(out) - 1);
    if (flags & DISPLAY_FLAG_STRICT_SYNC_ONLY)
        strncat(out, out[0] ? ", Strict Sync" : "Strict Sync", out_size - strlen(out) - 1);
    if (!out[0])
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
    *(uint32_t *)&out[0] = ebx;
    *(uint32_t *)&out[4] = edx;
    *(uint32_t *)&out[8] = ecx;
    out[12] = '\0';
}

static void draw_section(Surface *win, int x, int y, int w, int h, const char *title)
{
    gui_draw_panel_inset(win, x, y, w, h, g_gui_style.app_surface, g_gui_style.border, g_gui_style.chrome_bg_alt);
    gui_draw_card_header(win, x + 1, y + 1, w - 2, title, nullptr);
}

struct DetailItem
{
    const char *label;
    const char *value;
};

static int about_row_h()
{
    int compact_row_h = gui_line_height() + gui_space_1() / 2;
    if (compact_row_h < gui_scaled_metric(20))
        compact_row_h = gui_scaled_metric(20);
    return compact_row_h;
}

static int about_summary_h()
{
    int title_line_h = gui_font_line_height(gui_font_title());
    int badge_h = gui_badge_h();
    int line_h = gui_line_height();
    int content_h =
        ((title_line_h > badge_h) ? title_line_h : badge_h) + gui_space_1() / 2 + line_h + gui_space_1() / 3 + line_h;
    return gui_card_header_h() + gui_space_2() + content_h + gui_space_1();
}

static void draw_detail_cell(Surface *win, int x, int y, int w, int h, const DetailItem &item, uint32_t bg)
{
    if (!item.label || !item.value || w <= 0 || h <= 0)
        return;
    int text_y = y + (h - gui_line_height()) / 2;
    int gap = gui_space_1();
    int min_label_w = gui_scaled_metric(96);
    int min_value_w = gui_scaled_metric(88);
    int max_value_w = w - min_label_w - gap;
    if (max_value_w < min_value_w)
        max_value_w = w / 2;
    if (max_value_w < 0)
        max_value_w = 0;

    int val_w = gui_measure_text(gui_font_default(), item.value);
    if (val_w > max_value_w)
        val_w = max_value_w;

    int val_x = x + w - val_w;
    int label_w = val_x - x - gap;
    if (label_w < 0)
        label_w = 0;

    gui_draw_text_clipped(win, gui_font_default(), x, text_y, label_w, item.label, g_gui_style.text_dim, bg);
    if (val_w > 0) {
        gui_draw_text_clipped(win, gui_font_default(), val_x, text_y, val_w, item.value, g_gui_style.text, bg);
    }
}

static void draw_detail_row(Surface *win, int x, int y, int w, int h, const DetailItem &item, uint32_t bg)
{
    draw_detail_cell(win, x, y, w, h, item, bg);
}

static int panel_content_top(int y)
{
    return y + gui_card_header_h() + gui_space_2();
}

struct AboutSnapshot
{
    SystemProfile profile;
    MemInfo mem;
    DisplayCaps caps;
    SysTime now;
    int proc_count;
    char vendor[13];
    uint64_t uptime_seconds;
};

static void collect_about_snapshot(AboutSnapshot *snapshot)
{
    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof(*snapshot));
    cpu_vendor(snapshot->vendor);
    get_sysinfo(&snapshot->profile);
    get_meminfo(&snapshot->mem);
    display_get_caps(&snapshot->caps);
    get_time(&snapshot->now);
    ProcessInfo procs[64];
    snapshot->proc_count = get_procs(procs, 64);
    if (snapshot->proc_count < 0)
        snapshot->proc_count = 0;
    snapshot->uptime_seconds = get_uptime();
}

static uint32_t hash_cstr(const char *s)
{
    uint32_t hash = 2166136261u;
    if (!s)
        return hash;
    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t about_signature(const AboutSnapshot *snapshot)
{
    if (!snapshot)
        return 0;
    const SystemProfile *profile = &snapshot->profile;
    const MemInfo *mem = &snapshot->mem;
    const DisplayCaps *caps = &snapshot->caps;
    const SysTime *now = &snapshot->now;

    uint32_t sig = 0xA80Fu;
    sig = sig * 33u + (uint32_t)profile->kernel_build_debug;
    sig = sig * 33u + (uint32_t)mem->used_kb;
    sig = sig * 33u + (uint32_t)mem->free_kb;
    sig = sig * 33u + (uint32_t)mem->heap_used_kb;
    sig = sig * 33u + (uint32_t)mem->heap_total_kb;
    sig = sig * 33u + (uint32_t)caps->width;
    sig = sig * 33u + (uint32_t)caps->height;
    sig = sig * 33u + (uint32_t)caps->bpp;
    sig = sig * 33u + caps->flags;
    sig = sig * 33u + caps->nominal_refresh_millihz;
    sig = sig * 33u + caps->measured_refresh_millihz;
    sig = sig * 33u + (uint32_t)snapshot->proc_count;
    sig = sig * 33u + (uint32_t)now->year;
    sig = sig * 33u + (uint32_t)now->month;
    sig = sig * 33u + (uint32_t)now->day;
    sig = sig * 33u + (uint32_t)now->hour;
    sig = sig * 33u + (uint32_t)now->minute;
    sig = sig * 33u + (uint32_t)now->second;
    sig ^= hash_cstr(profile->kernel_commit);
    sig ^= hash_cstr(profile->bootloader_name);
    sig ^= hash_cstr(profile->bootloader_version);
    return sig;
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
    int summary_y = content_y + title_line_h + gui_space_1() / 2;
    int subtitle_y = summary_y + line_h + gui_space_1() / 3;
    gui_draw_text_clipped(win, gui_font_default(), content_x, summary_y, w - gui_space_4(), summary,
                          g_gui_style.text_dim, g_gui_style.app_surface);
    gui_draw_text_clipped(win, gui_font_default(), content_x, subtitle_y, w - gui_space_4(),
                          "Core system profile and runtime state", g_gui_style.text_muted, g_gui_style.app_surface);
}

static int get_panel_height(int rows)
{
    return gui_card_header_h() + gui_space_2() + (rows * about_row_h()) + gui_space_1();
}

static void draw_runtime_panel(Surface *win, int x, int y, int w, int h, const char *time_buf, const char *uptime_buf,
                               const char *proc_buf)
{
    draw_section(win, x, y, w, h, "Runtime");
    int inner_y = panel_content_top(y);
    DetailItem items[3] = {{"Local Time", time_buf}, {"Uptime", uptime_buf}, {"Processes", proc_buf}};
    for (int i = 0; i < 3; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * about_row_h(), w - gui_space_3(), about_row_h(), items[i],
                        g_gui_style.app_surface);
    }
}

static void draw_display_panel(Surface *win, int x, int y, int w, int h, const char *resolution, const char *nominal,
                               const char *measured, const char *depth, const char *flags)
{
    draw_section(win, x, y, w, h, "Display");
    int inner_y = panel_content_top(y);
    DetailItem items[5] = {
        {"Resolution", resolution}, {"Target Refresh", nominal}, {"Actual Refresh", measured}, {"Color Depth", depth},
        {"Flags", flags},
    };
    for (int i = 0; i < 5; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * about_row_h(), w - gui_space_3(), about_row_h(), items[i],
                        g_gui_style.app_surface);
    }
}

static void draw_memory_panel(Surface *win, int x, int y, int w, int h, const char *total, const char *used,
                              const char *free_kb, const char *heap_total, const char *heap_used)
{
    draw_section(win, x, y, w, h, "Memory");
    int inner_y = panel_content_top(y);
    DetailItem items[5] = {
        {"Total", total}, {"Used", used}, {"Free", free_kb}, {"Heap Total", heap_total}, {"Heap Used", heap_used}};
    for (int i = 0; i < 5; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * about_row_h(), w - gui_space_3(), about_row_h(), items[i],
                        g_gui_style.app_surface);
    }
}

static void draw_platform_panel(Surface *win, int x, int y, int w, int h, const char *vendor, const char *timer_hz,
                                const char *bootloader)
{
    draw_section(win, x, y, w, h, "Platform");
    int inner_y = panel_content_top(y);
    DetailItem items[3] = {{"CPU Vendor", vendor}, {"Timer", timer_hz}, {"Bootloader", bootloader}};
    for (int i = 0; i < 3; i++) {
        draw_detail_row(win, x + gui_space_2(), inner_y + i * about_row_h(), w - gui_space_3(), about_row_h(), items[i],
                        g_gui_style.app_surface);
    }
}

static int compute_about_content_height(const Surface *win, int content_w)
{
    (void)win;
    int gap = gui_app_section_gap();
    int h_summary = about_summary_h();
    int h_runtime = get_panel_height(3);
    int h_display = get_panel_height(5);
    int h_memory = get_panel_height(5);
    int h_platform = get_panel_height(3);

    int min_panel_w = gui_scaled_metric(280);
    int total = h_summary + gap;
    if (content_w >= min_panel_w * 2 + gap) {
        int left_h = h_runtime + gap + h_memory;
        int right_h = h_display + gap + h_platform;
        total += (left_h > right_h ? left_h : right_h);
    } else {
        total += h_runtime + gap + h_display + gap + h_memory + gap + h_platform;
    }
    return total;
}

static void draw_about(Surface *win, const AboutSnapshot *snapshot)
{
    if (!win || !snapshot)
        return;

    const SystemProfile &profile = snapshot->profile;
    const MemInfo &mem = snapshot->mem;
    const DisplayCaps &caps = snapshot->caps;
    const SysTime &now = snapshot->now;
    char uptime_buf[64];
    char proc_buf[32];
    char total[32], used[32], free_kb[32], heap_total[32], heap_used[32];
    char resolution[32], depth[32], nominal_refresh[32], measured_refresh[32], flags[96], bootloader[96], timer_hz[32],
        time_buf[64];

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
    snprintf(time_buf, sizeof(time_buf), "%04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day, now.hour,
             now.minute, now.second);

    GuiAppLayout layout = gui_app_begin(win);
    int view_w = layout.outer_w + layout.outer_x * 2;
    int content_w = layout.body_rect.w;
    int bottom_pad = gui_app_outer_padding();
    int content_total = compute_about_content_height(win, content_w) + layout.body_rect.y + bottom_pad;
    gui_set_content_size(win, view_w, content_total);
    const int outer = layout.body_rect.x;
    const int gap = gui_app_section_gap();

    int h_runtime = get_panel_height(3);
    int h_display = get_panel_height(5);
    int h_memory = get_panel_height(5);
    int h_platform = get_panel_height(3);
    int y = layout.body_rect.y;

    int summary_h = about_summary_h();
    draw_summary(win, outer, y, content_w, summary_h, profile.kernel_commit, profile.kernel_build_debug != 0);
    y += summary_h + gap;

    int min_panel_w = gui_scaled_metric(280);
    if (content_w >= min_panel_w * 2 + gap) {
        int col_w = (content_w - gap) / 2;
        int left_x = outer;
        int right_x = outer + col_w + gap;

        draw_runtime_panel(win, left_x, y, col_w, h_runtime, time_buf, uptime_buf, proc_buf);
        draw_display_panel(win, right_x, y, col_w, h_display, resolution, nominal_refresh, measured_refresh, depth,
                           flags);

        draw_memory_panel(win, left_x, y + h_runtime + gap, col_w, h_memory, total, used, free_kb, heap_total,
                          heap_used);
        draw_platform_panel(win, right_x, y + h_display + gap, col_w, h_platform, snapshot->vendor, timer_hz,
                            bootloader);
    } else {
        draw_runtime_panel(win, outer, y, content_w, h_runtime, time_buf, uptime_buf, proc_buf);
        y += h_runtime + gap;
        draw_display_panel(win, outer, y, content_w, h_display, resolution, nominal_refresh, measured_refresh, depth,
                           flags);
        y += h_display + gap;
        draw_memory_panel(win, outer, y, content_w, h_memory, total, used, free_kb, heap_total, heap_used);
        y += h_memory + gap;
        draw_platform_panel(win, outer, y, content_w, h_platform, snapshot->vendor, timer_hz, bootloader);
    }

    gui_app_draw_header(win, &layout, "About uniOS", "Hardware, runtime, and display overview", nullptr);
    gui_blit_to_screen_rect(win, 0, 0, win->width, win->height);
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
    uint64_t last_refresh_tick = 0;
    bool needs_redraw = true;
    uint32_t last_signature = 0;
    Registry *registry = gui_registry();
    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;

    while (true) {
        Event ev = {};
        while (poll_event(&ev) > 0) {
            if (ev.type == EVT_WINDOW_CLOSE)
                return 0;
            if (ev.type == EVT_WINDOW_RESIZE && gui_sync_window_size(&win) > 0)
                needs_redraw = true;
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
            uint32_t current_signature = about_signature(&snapshot);
            if (needs_redraw || current_signature != last_signature) {
                draw_about(&win, &snapshot);
                last_signature = current_signature;
            }
            last_refresh_tick = now_tick;
            needs_redraw = false;
        }

        sleep_ms(50);
    }
}
