#include "wm_core.h"

StoragePromptLayout storage_prompt_layout()
{
    StoragePromptLayout L = {};
    int bw = gui_scaled_metric(540);
    int bh = gui_scaled_metric(276);
    int btn_w = gui_scaled_metric(136);
    int btn_h = gui_app_control_h();
    int gap = gui_space_1();
    int outer_pad = gui_space_2();
    L.box = {(int)(g_screen.width - bw) / 2, (int)(g_screen.height - bh) / 2, bw, bh};
    int by = L.box.y + L.box.h - outer_pad - btn_h;
    L.writable_button = {L.box.x + L.box.w - outer_pad - btn_w, by, btn_w, btn_h};
    L.readonly_button = {L.writable_button.x - gap - btn_w, by, btn_w, btn_h};
    L.off_button = {L.readonly_button.x - gap - btn_w, by, btn_w, btn_h};
    return L;
}

void sync_storage_prompt_state(bool force)
{
    int mode = get_storage_mode();
    bool was = g_storage_prompt.visible;
    if (mode == STORAGE_MODE_WRITABLE && !force) {
        g_storage_prompt.visible = false;
        g_storage_prompt.dismissed = false;
        g_storage_prompt.hovered_button = -1;
    } else if (force || !g_storage_prompt.dismissed) {
        g_storage_prompt.visible = true;
        g_storage_prompt.hovered_button = -1;
    }
    if (g_storage_prompt.visible)
        clear_hover_feedback_state();
    if (was != g_storage_prompt.visible || force)
        enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
}

void open_storage_prompt()
{
    close_context_menu();
    g_storage_prompt.dismissed = false;
    sync_storage_prompt_state(true);
}

void dismiss_storage_prompt()
{
    if (!g_storage_prompt.visible)
        return;
    g_storage_prompt.visible = false;
    g_storage_prompt.dismissed = true;
    g_storage_prompt.hovered_button = -1;
    enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
}

void update_storage_prompt_hover(int mx, int my)
{
    if (!g_storage_prompt.visible)
        return;
    StoragePromptLayout L = storage_prompt_layout();
    int hov =
        point_in_rect(L.off_button, mx, my)
            ? 0
            : (point_in_rect(L.readonly_button, mx, my) ? 1 : (point_in_rect(L.writable_button, mx, my) ? 2 : -1));
    if (hov != g_storage_prompt.hovered_button) {
        g_storage_prompt.hovered_button = hov;
        enqueue_damage_rect(L.box.x, L.box.y, L.box.w, L.box.h);
    }
}

static void ensure_default_storage_file(const char *path, const char *fallback_path)
{
    if (!path || !fallback_path)
        return;
    int probe = open(path, O_RDONLY);
    if (probe >= 0) {
        close(probe);
        return;
    }

    int in = open(fallback_path, O_RDONLY);
    if (in < 0)
        return;
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return;
    }

    char buf[512];
    bool ok = true;
    while (true) {
        int n = read(in, buf, sizeof(buf));
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        if (write(out, buf, (size_t)n) != n) {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);
    if (!ok)
        unlink(path);
}

void ensure_default_user_storage_layout()
{
    if (get_storage_mode() != STORAGE_MODE_WRITABLE)
        return;
    VNodeStat st = {};
    if (stat("/data", &st) != 0 || !st.is_dir)
        return;
    struct StandardDir
    {
        const char *c;
    } dirs[] = {{"/data/Desktop"}, {"/data/Documents"}, {"/data/Downloads"}, {"/data/Pictures"}};
    for (auto d : dirs) {
        if (stat(d.c, &st) == 0 && st.is_dir)
            continue;
        mkdir(d.c);
    }
    ensure_default_storage_file(SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH);
    ensure_default_storage_file(WALLPAPER_CONFIG_PATH, WALLPAPER_BOOTSTRAP_CONFIG_PATH);
}

bool apply_storage_mode_request(Registry *registry, int mode)
{
    if (!registry || mode < STORAGE_MODE_OFF || mode > STORAGE_MODE_WRITABLE || set_storage_mode(mode) != 0)
        return false;
    if (mode == STORAGE_MODE_WRITABLE)
        ensure_default_user_storage_layout();
    registry->storage_mode = mode;
    registry->storage_request_mode = mode;
    asm volatile("sfence" ::: "memory");
    sync_storage_prompt_state(false);
    return true;
}

bool activate_storage_prompt_button(Registry *registry, int mx, int my)
{
    if (!g_storage_prompt.visible)
        return false;
    StoragePromptLayout L = storage_prompt_layout();
    if (point_in_rect(L.off_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_OFF);
        dismiss_storage_prompt();
        return true;
    }
    if (point_in_rect(L.readonly_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_READ_ONLY);
        dismiss_storage_prompt();
        return true;
    }
    if (point_in_rect(L.writable_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_WRITABLE);
        dismiss_storage_prompt();
        return true;
    }
    return false;
}

static uint32_t storage_prompt_scrim_color()
{
    uint32_t base = g_gui_style.app_bg ? g_gui_style.app_bg : g_gui_chrome.desktop_bg;
    bool light = color_luma(base) >= 128;
    uint32_t material = mix_rgb(base, g_gui_style.chrome_bg, light ? 14 : 8);
    uint8_t alpha = light ? 96 : 156;
    return ((uint32_t)alpha << 24) | (material & 0x00FFFFFFu);
}


void draw_storage_prompt_overlay_clipped(const DirtyRect &clip)
{
    if (!g_storage_prompt.visible || !g_backbuffer.buffer)
        return;
    DirtyRect screen = {0, 0, (int)g_backbuffer.width, (int)g_backbuffer.height};
    DirtyRect dim = {};
    if (!rect_intersection(screen, clip, &dim))
        return;

    uint32_t stride = g_backbuffer.pitch / 4;
    uint32_t scrim = storage_prompt_scrim_color();

    // Pre-calculate scrim blending constants outside the loop
    uint32_t scrim_a = scrim >> 24;
    uint32_t inv_sa = 255u - scrim_a;
    uint32_t scrim_r = ((scrim >> 16) & 0xFFu) * scrim_a;
    uint32_t scrim_g = ((scrim >> 8) & 0xFFu) * scrim_a;
    uint32_t scrim_b = (scrim & 0xFFu) * scrim_a;

    for (int y = dim.y; y < dim.y + dim.h; y++) {
        uint32_t *row = &g_backbuffer.buffer[(size_t)y * stride + dim.x];
        for (int x = 0; x < dim.w; x++) {
            uint32_t dst = row[x];
            uint32_t dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
            uint32_t out_r = (scrim_r + dr * inv_sa) >> 8;
            uint32_t out_g = (scrim_g + dg * inv_sa) >> 8;
            uint32_t out_b = (scrim_b + db * inv_sa) >> 8;
            row[x] = 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
        }
    }

    StoragePromptLayout layout = storage_prompt_layout();
    if (!rect_intersection(clip, layout.box, nullptr))
        return;

    int box_r = gui_radius_xl();

    gui_draw_panel_shadow(&g_backbuffer, layout.box.x, layout.box.y, layout.box.w, layout.box.h, box_r);

    gui_draw_chrome_frame(&g_backbuffer, layout.box.x, layout.box.y, layout.box.w, layout.box.h, box_r,
                          g_gui_style.app_surface, true);
    gui_draw_card_header_ext(&g_backbuffer, layout.box.x + 1, layout.box.y + 1, layout.box.w - 2, box_r - 1,
                             "Storage Mode", "Choose how uniOS should expose AHCI and ATA storage");

    int text_x = layout.box.x + gui_space_2();
    int content_y = layout.box.y + gui_card_header_h() + gui_space_2();
    int text_w = layout.box.w - gui_space_4();

    content_y +=
        gui_draw_wrapped_value(&g_backbuffer, text_x, content_y, text_w,
                               "Off hides persistent storage from apps. Read-Only allows browsing without disk writes. "
                               "Writable enables normal file changes and seeds standard user folders in /data.",
                               g_gui_style.text, g_gui_style.app_surface);

    int note_y = content_y + gui_space_1_5();
    int note_h = gui_app_row_tall_h();
    if (note_y + note_h < layout.off_button.y - gui_space_1()) {
        gui_fill_rounded_rect(&g_backbuffer, text_x, note_y, text_w, note_h, gui_radius_md(), g_gui_style.chrome_bg);
        gui_draw_rounded_rect(&g_backbuffer, text_x, note_y, text_w, note_h, gui_radius_md(), g_gui_style.border);
        gui_draw_badge(&g_backbuffer, text_x + gui_space_1(), note_y + (note_h - gui_badge_h()) / 2, "CAUTION",
                       g_gui_style.warning, g_gui_style.app_surface);
        int note_text_x = text_x + gui_scaled_metric(92);
        gui_draw_wrapped_value(
            &g_backbuffer, note_text_x, note_y + gui_scaled_metric(8), text_w - (note_text_x - text_x) - gui_space_1(),
            "Choose Writable only if you are intentionally testing on hardware you are prepared to modify.",
            g_gui_style.text_dim, g_gui_style.chrome_bg);
    }

    int footer_y = layout.off_button.y - gui_space_1();
    gui_draw_separator_h(&g_backbuffer, layout.box.x + 1, footer_y, layout.box.w - 2, g_gui_style.chrome_edge);

    gui_app_draw_button(&g_backbuffer, layout.off_button.x, layout.off_button.y, layout.off_button.w,
                        layout.off_button.h, "Off", false, false, g_storage_prompt.hovered_button == 0);
    gui_app_draw_button(&g_backbuffer, layout.readonly_button.x, layout.readonly_button.y, layout.readonly_button.w,
                        layout.readonly_button.h, "Read-Only", false, false, g_storage_prompt.hovered_button == 1);
    gui_app_draw_button(&g_backbuffer, layout.writable_button.x, layout.writable_button.y, layout.writable_button.w,
                        layout.writable_button.h, "Writable", true, false, g_storage_prompt.hovered_button == 2);
}

void draw_storage_prompt_overlay()
{
    DirtyRect full = {0, 0, (int)g_backbuffer.width, (int)g_backbuffer.height};
    draw_storage_prompt_overlay_clipped(full);
}

