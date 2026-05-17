#include "wm_core.h"

static uint32_t storage_prompt_scrim_color()
{
    uint32_t base = g_gui_style.app_bg ? g_gui_style.app_bg : g_gui_chrome.desktop_bg;
    bool light = color_luma(base) >= 128;
    uint32_t material = mix_rgb(base, g_gui_style.chrome_bg, light ? 14 : 8);
    uint8_t alpha = light ? 96 : 156;
    return ((uint32_t)alpha << 24) | (material & 0x00FFFFFFu);
}

void draw_context_menu_overlay(const Registry *registry)
{
    if (!g_context_menu.open)
        return;
    GuiMenuItem items[8];
    int count = build_context_menu_items(registry, items, 8);
    if (count > 0)
        gui_draw_popup_menu(&g_backbuffer, g_context_menu.x, g_context_menu.y, g_context_menu.w, items, count,
                            g_context_menu.hovered_index);
}

void draw_context_menu_overlay_clipped(const DirtyRect &clip, const Registry *registry)
{
    if (!g_context_menu.open)
        return;
    DirtyRect bounds = context_menu_bounds();
    if (!rect_intersection(clip, bounds, nullptr))
        return;
    GuiMenuItem items[8];
    int count = build_context_menu_items(registry, items, 8);
    if (count > 0)
        gui_draw_popup_menu(&g_backbuffer, g_context_menu.x, g_context_menu.y, g_context_menu.w, items, count,
                            g_context_menu.hovered_index);
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

    int box_r = gui_scaled_metric(20);

    // Soft shadow.
    int shadow_1 = gui_scaled_metric(10);
    int shadow_2 = gui_scaled_metric(5);
    int shadow_3 = gui_scaled_metric(2);
    gui_fill_rounded_rect(&g_backbuffer, layout.box.x + 1, layout.box.y + shadow_1, layout.box.w - 2, layout.box.h,
                          box_r, 0x08000000u);
    gui_fill_rounded_rect(&g_backbuffer, layout.box.x, layout.box.y + shadow_2, layout.box.w, layout.box.h, box_r,
                          0x0C000000u);
    gui_fill_rounded_rect(&g_backbuffer, layout.box.x, layout.box.y + shadow_3, layout.box.w, layout.box.h, box_r,
                          0x14000000u);

    gui_draw_panel_inset_ext(&g_backbuffer, layout.box.x, layout.box.y, layout.box.w, layout.box.h, box_r,
                             g_gui_style.app_surface, g_gui_style.border, g_gui_style.chrome_bg_alt);
    gui_draw_card_header_ext(&g_backbuffer, layout.box.x + 1, layout.box.y + 1, layout.box.w - 2, box_r, "Storage Mode",
                             "Choose how uniOS should expose AHCI and ATA storage");

    int text_x = layout.box.x + gui_space_2();
    int content_y = layout.box.y + gui_card_header_h() + gui_space_2();
    int text_w = layout.box.w - gui_space_4();

    content_y +=
        gui_draw_wrapped_value(&g_backbuffer, text_x, content_y, text_w,
                               "Off hides persistent storage from apps. Read-Only allows browsing without disk writes. "
                               "Writable enables normal file changes and seeds standard user folders in /data.",
                               g_gui_style.text, g_gui_style.app_surface);

    int note_y = content_y + gui_space_1_5();
    int note_h = gui_scaled_metric(52);
    if (note_y + note_h < layout.off_button.y - gui_space_1()) {
        gui_fill_rounded_rect(&g_backbuffer, text_x, note_y, text_w, note_h, gui_radius_md(), g_gui_style.chrome_bg);
        gui_draw_rounded_rect(&g_backbuffer, text_x, note_y, text_w, note_h, gui_radius_md(), g_gui_style.border);
        gui_draw_badge(&g_backbuffer, text_x + gui_space_1(), note_y + gui_scaled_metric(8), "CAUTION",
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

static int wm_index_result_item_h()
{
    int h = gui_scaled_metric(52);
    return h < gui_scaled_metric(40) ? gui_scaled_metric(40) : h;
}

static DirtyRect wm_index_search_bounds()
{
    DirtyRect box = index_overlay_bounds();
    int pad = gui_space_2();
    int h = gui_scaled_metric(44);
    return {box.x + pad, box.y + pad, box.w - pad * 2, h};
}

static int wm_index_results_start_y()
{
    DirtyRect search = wm_index_search_bounds();
    return search.y + search.h + gui_space_1();
}

void draw_index_overlay_clipped(const DirtyRect &clip, const Registry *registry)
{
    (void)registry;
    if (!g_index.active || !g_backbuffer.buffer)
        return;

    DirtyRect box = index_overlay_bounds();
    DirtyRect damage = rect_expand(box, gui_scaled_metric(14));
    if (!rect_intersection(clip, damage, nullptr))
        return;

    int radius = gui_scaled_metric(20);

    // Soft shadow.
    int shadow_1 = gui_scaled_metric(8);
    int shadow_2 = gui_scaled_metric(4);
    int shadow_3 = gui_scaled_metric(2);
    gui_fill_rounded_rect(&g_backbuffer, box.x + 1, box.y + shadow_1, box.w - 2, box.h, radius, 0x08000000u);
    gui_fill_rounded_rect(&g_backbuffer, box.x, box.y + shadow_2, box.w, box.h, radius, 0x0C000000u);
    gui_fill_rounded_rect(&g_backbuffer, box.x, box.y + shadow_3, box.w, box.h, radius, 0x10000000u);

    gui_draw_panel_inset_ext(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface,
                             g_gui_style.border_focus, g_gui_style.chrome_bg_alt);

    DirtyRect search = wm_index_search_bounds();
    const char *query = g_index.query_len > 0 ? g_index.query : "";
    gui_app_draw_text_field(&g_backbuffer, search.x, search.y, search.w, search.h, query, g_index.query_len > 0, false);
    if (g_index.query_len == 0) {
        int placeholder_y = gui_align_text_y(gui_font_default(), search.y, search.h);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), search.x + gui_space_1(), placeholder_y,
                              search.w - gui_space_2(), "Search apps, commands, settings", g_gui_style.text_muted,
                              g_gui_style.app_surface);
    }

    int hint_w = gui_measure_text(gui_font_default(), "Enter");
    int hint_x = search.x + search.w - hint_w - gui_space_1();
    if (hint_x > search.x + search.w / 2) {
        int hint_y = gui_align_text_y(gui_font_default(), search.y, search.h);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), hint_x, hint_y,
                              search.x + search.w - hint_x - gui_space_1(), "Enter", g_gui_style.text_muted,
                              g_gui_style.app_surface);
    }

    int pad = gui_space_2();
    int row_y = wm_index_results_start_y();
    int row_h = wm_index_result_item_h();
    int row_gap = gui_scaled_metric(2);
    int bottom = box.y + box.h - pad;

    if (g_index.result_count <= 0) {
        int empty_y = row_y + gui_space_2();
        gui_draw_text_clipped(&g_backbuffer, gui_font_title(), box.x + pad, empty_y, box.w - pad * 2, "No matches",
                              g_gui_style.text_dim, g_gui_style.app_surface);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), box.x + pad,
                              empty_y + gui_line_height() + gui_scaled_metric(4), box.w - pad * 2,
                              "Try an app name, setting, or command.", g_gui_style.text_muted, g_gui_style.app_surface);
        return;
    }

    for (int i = 0; i < g_index.result_count; i++) {
        if (row_y + row_h > bottom)
            break;
        bool selected = i == g_index.selected_index;
        bool hovered = i == g_index.hovered_index;
        const IndexResult &result = g_index.results[i];
        const char *badge = result.is_app ? "APP" : "CMD";
        const char *detail = result.detail[0] ? result.detail : result.path;
        gui_app_draw_list_row(&g_backbuffer, box.x + pad, row_y, box.w - pad * 2, row_h, badge, result.title, detail,
                              selected, hovered, false);
        row_y += row_h + row_gap;
    }
}

// Layout functions are defined in wm_logic.cpp and declared in wm_core.h:
// control_panel_card_h(), control_panel_item_rect()

static void draw_control_toggle(ControlPanelItem item, const char *label, const char *detail, bool on)
{
    DirtyRect r = control_panel_item_rect(item);
    gui_app_draw_toggle_row(&g_backbuffer, r.x, r.y, r.w, r.h, label, detail, on, false,
                            g_control_center.hovered_item == item);
}

static void draw_control_volume_card()
{
    DirtyRect r = control_panel_item_rect(CONTROL_ITEM_VOLUME);
    bool hovered = g_control_center.hovered_item == CONTROL_ITEM_VOLUME || g_control_center.volume_dragging;
    uint32_t bg = hovered ? g_gui_style.app_surface_alt : g_gui_style.app_surface;
    int card_r = gui_radius_md();
    gui_fill_rounded_rect(&g_backbuffer, r.x, r.y, r.w, r.h, card_r, bg);
    gui_draw_rounded_rect(&g_backbuffer, r.x, r.y, r.w, r.h, card_r,
                          hovered ? g_gui_style.border_hover : g_gui_style.border);

    char value[16];
    snprintf(value, sizeof(value), "%u%%", (unsigned)g_control_center.volume);
    int pad = gui_space_1_5();
    int label_y = r.y + gui_space_1();
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), r.x + pad, label_y, r.w / 2, "Volume", g_gui_style.text,
                          bg);
    int value_w = gui_measure_text(gui_font_default(), value);
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), r.x + r.w - pad - value_w, label_y, value_w, value,
                          g_gui_style.text_dim, bg);

    int track_h = gui_scaled_metric(16);
    int track_x = r.x + pad;
    int track_y = r.y + r.h - pad - track_h;
    int track_w = r.w - pad * 2;
    int track_r = gui_corner_radius(track_w, track_h, track_h / 2);
    gui_fill_rounded_rect(&g_backbuffer, track_x, track_y, track_w, track_h, track_r, g_gui_style.chrome_bg_alt);
    gui_draw_rounded_rect(&g_backbuffer, track_x, track_y, track_w, track_h, track_r, g_gui_style.border);
    int fill_w = (int)((uint64_t)track_w * g_control_center.volume / 100u);
    if (fill_w > 0)
        gui_fill_rounded_rect(&g_backbuffer, track_x, track_y, fill_w, track_h, track_r, g_gui_style.accent);
    int knob_d = track_h + gui_scaled_metric(4);
    int knob_x = track_x + fill_w - knob_d / 2;
    if (knob_x < track_x)
        knob_x = track_x;
    if (knob_x + knob_d > track_x + track_w)
        knob_x = track_x + track_w - knob_d;
    gui_fill_rounded_rect(&g_backbuffer, knob_x, track_y - gui_scaled_metric(2), knob_d, knob_d, knob_d / 2,
                          g_gui_style.app_surface);
    gui_draw_rounded_rect(&g_backbuffer, knob_x, track_y - gui_scaled_metric(2), knob_d, knob_d, knob_d / 2,
                          g_gui_style.border_focus);
}

void draw_control_center_overlay_clipped(const DirtyRect &clip)
{
    if (!g_control_center.open || !g_backbuffer.buffer)
        return;

    DirtyRect box = control_center_bounds();
    DirtyRect damage = rect_expand(box, gui_scaled_metric(14));
    if (!rect_intersection(clip, damage, nullptr))
        return;

    int radius = gui_scaled_metric(20);

    // Soft shadow.
    int shadow_1 = gui_scaled_metric(8);
    int shadow_2 = gui_scaled_metric(4);
    int shadow_3 = gui_scaled_metric(2);
    gui_fill_rounded_rect(&g_backbuffer, box.x + 1, box.y + shadow_1, box.w - 2, box.h, radius, 0x08000000u);
    gui_fill_rounded_rect(&g_backbuffer, box.x, box.y + shadow_2, box.w, box.h, radius, 0x0C000000u);
    gui_fill_rounded_rect(&g_backbuffer, box.x, box.y + shadow_3, box.w, box.h, radius, 0x10000000u);

    // Panel surface.
    gui_draw_panel_inset_ext(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface,
                             g_gui_style.border, g_gui_style.chrome_bg_alt);

    // Card header.
    gui_draw_card_header_ext(&g_backbuffer, box.x + 1, box.y + 1, box.w - 2, radius, "Control Panel", "uniOS");

    draw_control_toggle(CONTROL_ITEM_NETWORK, "Network", g_control_center.network_enabled ? "Ethernet" : "Disconnected",
                        g_control_center.network_enabled);
    draw_control_toggle(CONTROL_ITEM_DARK_MODE, "Dark", g_control_center.dark_mode ? "On" : "Off",
                        g_control_center.dark_mode);
    draw_control_toggle(CONTROL_ITEM_DESKTOP_GRID, "Grid", g_control_center.desktop_grid ? "Shown" : "Hidden",
                        g_control_center.desktop_grid);
    draw_control_toggle(CONTROL_ITEM_CLOCK_SECONDS, "Seconds", g_control_center.clock_seconds ? "Show" : "Hide",
                        g_control_center.clock_seconds);
    draw_control_toggle(CONTROL_ITEM_ANIMATIONS, "Motion", g_control_center.animations_enabled ? "On" : "Off",
                        g_control_center.animations_enabled);
    draw_control_toggle(CONTROL_ITEM_TRANSPARENCY, "Transparency",
                        g_control_center.transparency_level < 255 ? "On" : "Off",
                        g_control_center.transparency_level < 255);
    draw_control_volume_card();

    DirtyRect storage = control_panel_item_rect(CONTROL_ITEM_STORAGE);
    DirtyRect settings = control_panel_item_rect(CONTROL_ITEM_SETTINGS);
    gui_app_draw_button(&g_backbuffer, storage.x, storage.y, storage.w, storage.h, "Storage", false, false,
                        g_control_center.hovered_item == CONTROL_ITEM_STORAGE);
    gui_app_draw_button(&g_backbuffer, settings.x, settings.y, settings.w, settings.h, "Settings", true, false,
                        g_control_center.hovered_item == CONTROL_ITEM_SETTINGS);

    int notif_y = box.y + box.h + gui_space_2();
    draw_notification_center_clipped(clip, notif_y);
}

void draw_toast_overlay_clipped(const DirtyRect &clip)
{
    if (!g_backbuffer.buffer || g_notifications.count == 0)
        return;

    int toast_w = gui_scaled_metric(320);
    int toast_h = gui_scaled_metric(76);
    int margin = gui_space_2();
    int toast_x = g_backbuffer.width - toast_w - margin;
    int toast_y = wm_menubar_h() + margin;

    DirtyRect toast_box = {toast_x, toast_y, toast_w, toast_h};
    DirtyRect damage = rect_expand(toast_box, gui_scaled_metric(14));

    if (!rect_intersection(clip, damage, nullptr))
        return;

    uint64_t now = get_ticks();
    Notification *active_toast = nullptr;

    int idx = g_notifications.head - 1;
    if (idx < 0)
        idx = MAX_NOTIFICATIONS - 1;
    for (int i = 0; i < g_notifications.count; i++) {
        Notification &notif = g_notifications.history[idx];
        if (notif.active_toast) {
            if (now - notif.timestamp_ticks > TOAST_DURATION_TICKS) {
                notif.active_toast = false;
                enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
            } else {
                active_toast = &notif;
                break;
            }
        }
        idx--;
        if (idx < 0)
            idx = MAX_NOTIFICATIONS - 1;
    }

    if (!active_toast)
        return;

    int radius = gui_scaled_metric(16);

    int shadow_1 = gui_scaled_metric(8);
    int shadow_2 = gui_scaled_metric(4);
    int shadow_3 = gui_scaled_metric(2);
    gui_fill_rounded_rect(&g_backbuffer, toast_box.x, toast_box.y + shadow_2, toast_box.w, toast_box.h, radius,
                          0x0C000000u);
    gui_fill_rounded_rect(&g_backbuffer, toast_box.x, toast_box.y + shadow_3, toast_box.w, toast_box.h, radius,
                          0x14000000u);

    gui_draw_panel_inset_ext(&g_backbuffer, toast_box.x, toast_box.y, toast_box.w, toast_box.h, radius,
                             g_gui_style.app_surface, g_gui_style.border, g_gui_style.chrome_bg_alt);

    int accent_w = gui_scaled_metric(4);
    int accent_h = toast_box.h - gui_scaled_metric(24);
    gui_fill_rounded_rect(&g_backbuffer, toast_box.x + gui_scaled_metric(10), toast_box.y + gui_scaled_metric(12),
                          accent_w, accent_h, accent_w / 2, g_gui_style.accent);

    int text_x = toast_box.x + gui_scaled_metric(22);
    int text_y = toast_box.y + gui_scaled_metric(16);

    gui_draw_text_clipped(&g_backbuffer, gui_font_title(), text_x, text_y, toast_box.w - gui_scaled_metric(30),
                          active_toast->title, g_gui_style.text, g_gui_style.app_surface);
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), text_x, text_y + gui_line_height() + gui_scaled_metric(4),
                          toast_box.w - gui_scaled_metric(30), active_toast->message, g_gui_style.text_dim,
                          g_gui_style.app_surface);
}

void draw_notification_center_clipped(const DirtyRect &clip, int start_y)
{
    if (g_notifications.count == 0)
        return;

    DirtyRect cc_box = control_center_bounds();
    DirtyRect box = {cc_box.x, start_y, cc_box.w, gui_scaled_metric(240)};
    DirtyRect damage = rect_expand(box, gui_scaled_metric(14));

    if (!rect_intersection(clip, damage, nullptr))
        return;

    int radius = gui_scaled_metric(20);

    gui_fill_rounded_rect(&g_backbuffer, box.x, box.y + gui_scaled_metric(2), box.w, box.h, radius, 0x12000000u);
    gui_draw_panel_inset_ext(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface,
                             g_gui_style.border, g_gui_style.chrome_bg_alt);
    gui_draw_card_header_ext(&g_backbuffer, box.x + 1, box.y + 1, box.w - 2, radius, "Notifications", "Recent");

    int item_y = box.y + gui_card_header_h() + gui_space_2();
    int index = g_notifications.head - 1;
    if (index < 0)
        index = MAX_NOTIFICATIONS - 1;

    uint64_t now = get_ticks();
    for (int i = 0; i < g_notifications.count && i < 3; i++) {
        Notification &notif = g_notifications.history[index];

        int card_x = box.x + gui_space_1_5();
        int card_w = box.w - gui_space_3();
        int card_h = gui_scaled_metric(52);
        int card_r = gui_scaled_metric(12);

        // Individual notification card background
        gui_fill_rounded_rect(&g_backbuffer, card_x, item_y, card_w, card_h, card_r, g_gui_style.app_surface_alt);
        gui_draw_rounded_rect(&g_backbuffer, card_x, item_y, card_w, card_h, card_r, g_gui_style.border);

        int text_x = card_x + gui_space_1_5();
        int text_y = item_y + gui_scaled_metric(8);

        gui_draw_text_clipped(&g_backbuffer, gui_font_title(), text_x, text_y, card_w - gui_space_3(), notif.title,
                              g_gui_style.text, g_gui_style.app_surface_alt);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), text_x, text_y + gui_line_height() + 2,
                              card_w - gui_space_3(), notif.message, g_gui_style.text_dim, g_gui_style.app_surface_alt);

        // Relative timestamp
        uint64_t diff = now - notif.timestamp_ticks;
        char time_str[32];
        if (diff < 60000) {
            snprintf(time_str, sizeof(time_str), "Now");
        } else {
            snprintf(time_str, sizeof(time_str), "%u min ago", (unsigned)(diff / 60000));
        }
        int time_w = gui_measure_text(gui_font_default(), time_str);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), card_x + card_w - gui_space_1_5() - time_w, text_y + 1,
                              time_w, time_str, g_gui_style.text_dim, g_gui_style.app_surface_alt);

        item_y += card_h + gui_space_1();
        index--;
        if (index < 0)
            index = MAX_NOTIFICATIONS - 1;
    }
}
