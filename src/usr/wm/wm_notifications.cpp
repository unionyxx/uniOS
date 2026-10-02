#include "wm_damage.h"
#include "wm_metrics.h"
#include "wm_overlays.h"
#include "wm_present.h"

NotificationCenterState g_notifications = {};

int notification_pill_h(void)
{
    return gui_scaled_metric(56);
}

int notification_center_panel_h(void)
{
    int n = g_notifications.count < 3 ? g_notifications.count : 3;
    if (n <= 0)
        return 0;
    int pad = gui_space_1_5();
    int card_gap = gui_space_1();
    return gui_card_header_h() + pad + n * notification_pill_h() + (n - 1) * card_gap + pad;
}

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
    int toast_h = notification_pill_h();
    int margin = gui_space_2();
    int toast_x = g_screen.width - toast_w - margin;
    int toast_y = wm_menubar_h() + margin;

    int shadow_pad = gui_panel_shadow_pad();
    enqueue_damage_rect(toast_x - shadow_pad, toast_y - shadow_pad, toast_w + shadow_pad * 2, toast_h + shadow_pad * 2);
}

void wm_pump_notification_expiry(void)
{
    if (g_notifications.count == 0 || !g_backbuffer.buffer)
        return;
    uint64_t now = get_ticks();
    bool expired = false;
    int idx = (g_notifications.head - 1 + MAX_NOTIFICATIONS) % MAX_NOTIFICATIONS;
    for (int i = 0; i < g_notifications.count; i++) {
        Notification &notif = g_notifications.history[idx];
        if (notif.active_toast && (now - notif.timestamp_ticks > TOAST_DURATION_TICKS)) {
            notif.active_toast = false;
            expired = true;
        }
        idx = (idx - 1 + MAX_NOTIFICATIONS) % MAX_NOTIFICATIONS;
    }
    if (expired) {
        int toast_w = gui_scaled_metric(320);
        int toast_h = notification_pill_h();
        int margin = gui_space_2();
        DirtyRect toast_box = {static_cast<int>(g_backbuffer.width) - toast_w - margin, wm_menubar_h() + margin,
                               toast_w, toast_h};
        // Match the expansion used when the toast is drawn so the whole
        // shadow region is repainted at any UI scale.
        DirtyRect expired_rect = rect_expand(toast_box, gui_panel_shadow_pad());
        enqueue_damage_rect(expired_rect.x, expired_rect.y, expired_rect.w, expired_rect.h);
    }
}

// Shared notification card content, used identically by the live toast and the
// Notification Center rows: an accent glyph well on the left, then a title and
// a message line vertically centered as one block, and an optional trailing
// string (the relative timestamp in the list) aligned with the title. Keeping
// the metrics in one place guarantees the toast and the list read the same.
static void draw_notification_card_content(int x, int y, int w, int h, const char *title, const char *message,
                                           const char *trailing)
{
    int icon_size = gui_glyph_std_size() + gui_scaled_metric(4);
    int icon_x = x + gui_space_1_5();
    int icon_y = y + (h - icon_size) / 2;
    gui_fill_rounded_rect(&g_backbuffer, icon_x, icon_y, icon_size, icon_size, gui_radius_sm(),
                          g_gui_style.accent_soft);
    int glyph_size = gui_glyph_std_size();
    gui_draw_glyph(&g_backbuffer, icon_x + (icon_size - glyph_size) / 2, icon_y + (icon_size - glyph_size) / 2,
                   glyph_size, GUI_GLYPH_INFO, g_gui_style.accent);

    int text_x = icon_x + icon_size + gui_space_1_5();
    int right_pad = gui_space_1_5();
    int trailing_w = (trailing && trailing[0]) ? gui_measure_text(gui_font_default(), trailing) : 0;
    int trailing_gap = trailing_w > 0 ? gui_space_1() : 0;
    int text_w = x + w - right_pad - trailing_w - trailing_gap - text_x;
    if (text_w < 0)
        text_w = 0;

    int title_line = gui_font_line_height(gui_font_title());
    int row_gap = gui_space_0_5();
    int block_h = title_line + row_gap + gui_line_height();
    int title_y = y + (h - block_h) / 2;

    gui_draw_text_clipped(&g_backbuffer, gui_font_title(), text_x, title_y, text_w, title, g_gui_style.text, 0);
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), text_x, title_y + title_line + row_gap, text_w, message,
                          g_gui_style.text_muted, 0);
    if (trailing_w > 0)
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), x + w - right_pad - trailing_w, title_y, trailing_w,
                              trailing, g_gui_style.text_muted, 0);
}

void draw_toast_overlay_clipped(const DirtyRect &clip)
{
    if (!g_backbuffer.buffer || g_notifications.count == 0)
        return;

    // Suppress the toast while the control center is open: the notification
    // center list already shows recent notifications, so a live toast would
    // only overlap the panel. New arrivals appear at the top of the list
    // instead; a still-live toast reappears once the center closes.
    if (g_control_center.open)
        return;

    int toast_w = gui_scaled_metric(320);
    int toast_h = notification_pill_h();
    int margin = gui_space_2();
    int toast_x = g_backbuffer.width - toast_w - margin;
    int toast_y = wm_menubar_h() + margin;

    DirtyRect toast_box = {toast_x, toast_y, toast_w, toast_h};
    DirtyRect damage = rect_expand(toast_box, gui_panel_shadow_pad());

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

    gui_draw_panel_shadow_clipped(&g_backbuffer, toast_box.x, toast_box.y, toast_box.w, toast_box.h, radius, clip.x,
                                  clip.y, clip.w, clip.h);

    gui_draw_window_frame(&g_backbuffer, toast_box.x, toast_box.y, toast_box.w, toast_box.h, radius,
                          g_gui_style.app_surface);

    draw_notification_card_content(toast_box.x, toast_box.y, toast_box.w, toast_box.h, active_toast->title,
                                   active_toast->message, nullptr);
}

void draw_notification_center_clipped(const DirtyRect &clip, int start_y)
{
    if (g_notifications.count == 0)
        return;

    DirtyRect cc_box = control_center_bounds();
    DirtyRect box = {cc_box.x, start_y, cc_box.w, notification_center_panel_h()};
    DirtyRect damage = rect_expand(box, gui_scaled_metric(14));

    if (!rect_intersection(clip, damage, nullptr))
        return;

    int radius = gui_radius_xl();

    gui_draw_panel_shadow_clipped(&g_backbuffer, box.x, box.y, box.w, box.h, radius, clip.x, clip.y, clip.w, clip.h);
    // Draw the frame without the light rim, then the header, then the rim on top,
    // so the rim stays visible along the top edge instead of being buried under
    // the header fill.
    gui_draw_window_frame_no_rim(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface);
    gui_draw_card_header_ext(&g_backbuffer, box.x + 1, box.y + 1, box.w - 2, radius - 1, "Notifications", "Recent");
    gui_draw_window_rim(&g_backbuffer, box.x, box.y, box.w, box.h, radius);

    int pad = gui_space_1_5();
    int card_gap = gui_space_1();
    int shown = g_notifications.count < 3 ? g_notifications.count : 3;
    int card_h = notification_pill_h();
    int card_x = box.x + pad;
    int card_w = box.w - pad * 2;
    int card_r = gui_radius_sm() + gui_scaled_metric(2);

    int item_y = box.y + gui_card_header_h() + pad;
    int index = g_notifications.head - 1;
    if (index < 0)
        index = MAX_NOTIFICATIONS - 1;

    uint64_t now = get_ticks();
    for (int i = 0; i < shown; i++) {
        Notification &notif = g_notifications.history[index];

        // Individual flat card on a subtle wash (no nested frame); the content
        // layout is byte-for-byte the same as the live toast.
        gui_fill_rounded_rect(&g_backbuffer, card_x, item_y, card_w, card_h, card_r, gui_subtle_card_wash_color());

        // Relative timestamp on the title line's right gutter.
        uint64_t diff = now - notif.timestamp_ticks;
        char time_str[32];
        if (diff < 60000) {
            snprintf(time_str, sizeof(time_str), "Now");
        } else {
            snprintf(time_str, sizeof(time_str), "%u min ago", (unsigned)(diff / 60000));
        }

        draw_notification_card_content(card_x, item_y, card_w, card_h, notif.title, notif.message, time_str);

        item_y += card_h + card_gap;
        index--;
        if (index < 0)
            index = MAX_NOTIFICATIONS - 1;
    }
}
