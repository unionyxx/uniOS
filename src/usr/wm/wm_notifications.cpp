#include "wm_core.h"

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

