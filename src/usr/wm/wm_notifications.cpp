#include "wm_damage.h"
#include "wm_metrics.h"
#include "wm_overlays.h"
#include "wm_present.h"

NotificationCenterState g_notifications = {};

void wm_push_notification(const char *title, const char *message)
{
    int index = g_notifications.head;
    Notification &notif = g_notifications.history[index];

    strncpy(notif.title, title, sizeof(notif.title) - 1);
    notif.title[sizeof(notif.title) - 1] = '\0';

    strncpy(notif.message, message, sizeof(notif.message) - 1);
    notif.message[sizeof(notif.message) - 1] = '\0';

    notif.timestamp_ticks = get_ticks();
    notif.read = false;
    notif.active_toast = true;

    g_notifications.head = (g_notifications.head + 1) % MAX_NOTIFICATIONS;
    if (g_notifications.count < MAX_NOTIFICATIONS) {
        g_notifications.count++;
    }

    int toast_w = gui_scaled_metric(320);
    int toast_h = gui_scaled_metric(76);
    int margin = gui_space_2();
    int toast_x = g_screen.width - toast_w - margin;
    int toast_y = wm_menubar_h() + margin;

    enqueue_damage_rect(toast_x - 16, toast_y - 16, toast_w + 32, toast_h + 32);
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

    int radius = gui_radius_xl();

    gui_draw_panel_shadow(&g_backbuffer, toast_box.x, toast_box.y, toast_box.w, toast_box.h, radius);

    gui_draw_chrome_frame(&g_backbuffer, toast_box.x, toast_box.y, toast_box.w, toast_box.h, radius,
                          g_gui_style.app_surface, true);

    int accent_x = toast_box.x + gui_space_1();
    int accent_w = gui_scaled_metric(4);
    int accent_h = toast_box.h - gui_space_3();
    gui_fill_rounded_rect(&g_backbuffer, accent_x, toast_box.y + gui_space_1_5(), accent_w, accent_h, accent_w / 2,
                          g_gui_style.accent);

    int text_x = accent_x + accent_w + gui_space_1_5();
    int text_y = toast_box.y + gui_space_2();

    gui_draw_text_clipped(&g_backbuffer, gui_font_title(), text_x, text_y, toast_box.w - gui_scaled_metric(30),
                          active_toast->title, g_gui_style.text, g_gui_style.app_surface);
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), text_x, text_y + gui_line_height() + gui_space_0_5(),
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

    int radius = gui_radius_xl();

    gui_draw_panel_shadow(&g_backbuffer, box.x, box.y, box.w, box.h, radius);
    gui_draw_chrome_frame(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface, true);
    gui_draw_card_header_ext(&g_backbuffer, box.x + 1, box.y + 1, box.w - 2, radius - 1, "Notifications", "Recent");

    int item_y = box.y + gui_card_header_h() + gui_space_2();
    int index = g_notifications.head - 1;
    if (index < 0)
        index = MAX_NOTIFICATIONS - 1;

    uint64_t now = get_ticks();
    for (int i = 0; i < g_notifications.count && i < 3; i++) {
        Notification &notif = g_notifications.history[index];

        int card_x = box.x + gui_space_1_5();
        int card_w = box.w - gui_space_3();
        int card_h = gui_app_row_tall_h();
        int card_r = gui_radius_lg();

        // Individual notification card background
        gui_fill_rounded_rect(&g_backbuffer, card_x, item_y, card_w, card_h, card_r, g_gui_style.app_surface_alt);
        gui_draw_rounded_rect(&g_backbuffer, card_x, item_y, card_w, card_h, card_r, g_gui_style.border);

        int text_x = card_x + gui_space_1_5();
        int text_y = item_y + gui_scaled_metric(8);

        gui_draw_text_clipped(&g_backbuffer, gui_font_title(), text_x, text_y, card_w - gui_space_3(), notif.title,
                              g_gui_style.text, g_gui_style.app_surface_alt);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), text_x, text_y + gui_line_height() + gui_space_0_5(),
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
