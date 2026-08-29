#include "wm_core.h"

static int resolve_context_menu_target_index()
{
    if (!g_context_menu.open || g_context_menu.kind != CONTEXT_MENU_WINDOW)
        return -1;
    if (g_context_menu.target_entry) {
        int idx = find_window_by_entry(g_context_menu.target_entry);
        if (idx >= WM_FIRST_USER_WINDOW && idx < g_window_count && is_window_visible(g_windows[idx]) &&
            g_windows[idx].entry == g_context_menu.target_entry) {
            g_context_menu.target_index = idx;
            return idx;
        }
    }
    if (g_context_menu.target_index >= WM_FIRST_USER_WINDOW && g_context_menu.target_index < g_window_count) {
        const Window &w = g_windows[g_context_menu.target_index];
        if (w.entry && is_window_visible(w)) {
            g_context_menu.target_entry = w.entry;
            return g_context_menu.target_index;
        }
    }
    return -1;
}

static bool context_menu_targets_window_entry(const WindowEntry *entry)
{
    return entry && g_context_menu.open && g_context_menu.kind == CONTEXT_MENU_WINDOW &&
           g_context_menu.target_entry == entry;
}

static bool ensure_context_menu_target_valid()
{
    if (!g_context_menu.open || g_context_menu.kind != CONTEXT_MENU_WINDOW)
        return true;
    if (resolve_context_menu_target_index() >= WM_FIRST_USER_WINDOW)
        return true;
    close_context_menu();
    return false;
}

int build_context_menu_items(const Registry *registry, GuiMenuItem *items, int max_items)
{
    (void)registry;
    if (!items || max_items <= 0 || !g_context_menu.open)
        return 0;
    if (g_context_menu.kind == CONTEXT_MENU_DESKTOP && max_items >= 5) {
        items[0] = {"Open Terminal", true, false};
        items[1] = {"Open Files", true, false};
        items[2] = {"Settings", true, false};
        items[3] = {"Storage Mode", true, false};
        items[4] = {"Refresh Desktop", true, false};
        return 5;
    }
    if (g_context_menu.kind == CONTEXT_MENU_WINDOW && max_items >= 5) {
        int target_index = resolve_context_menu_target_index();
        bool can_act = target_index >= WM_FIRST_USER_WINDOW && g_windows[target_index].entry;
        bool maxed = can_act && g_windows[target_index].entry->state == WIN_MAXIMIZED;
        items[0] = {maxed ? "Restore Window" : "Maximize Window", can_act, false};
        items[1] = {"Minimize Window", can_act, false};
        items[2] = {"Close Window", can_act, false};
        items[3] = {"Settings", true, false};
        items[4] = {"Storage Mode", true, false};
        return 5;
    }
    return 0;
}

void open_context_menu(const Registry *registry, ContextMenuKind kind, int target_index, int anchor_x, int anchor_y)
{
    if (!registry || kind == CONTEXT_MENU_NONE)
        return;
    if (g_context_menu.open) {
        DirtyRect prev = context_menu_bounds();
        enqueue_damage_rect(prev.x, prev.y, prev.w, prev.h);
    }
    clear_hover_feedback_state();

    g_context_menu = {};
    g_context_menu.open = true;
    g_context_menu.kind = kind;
    g_context_menu.target_index = target_index;
    g_context_menu.target_entry =
        (kind == CONTEXT_MENU_WINDOW && target_index >= WM_FIRST_USER_WINDOW && target_index < g_window_count)
            ? g_windows[target_index].entry
            : nullptr;

    GuiMenuItem items[8];
    int count = build_context_menu_items(registry, items, 8);
    if (count <= 0) {
        g_context_menu = {};
        return;
    }
    g_context_menu.w = gui_popup_menu_width(items, count, gui_scaled_metric(176));
    g_context_menu.h = gui_popup_menu_height(items, count);
    g_context_menu.x = anchor_x;
    g_context_menu.y = anchor_y;
    if (g_context_menu.x + g_context_menu.w > (int)g_screen.width)
        g_context_menu.x = g_screen.width - g_context_menu.w - gui_scaled_metric(8);
    if (g_context_menu.y + g_context_menu.h > (int)g_screen.height)
        g_context_menu.y = g_screen.height - g_context_menu.h - gui_scaled_metric(8);
    if (g_context_menu.x < 0)
        g_context_menu.x = 0;
    if (g_context_menu.y < 0)
        g_context_menu.y = 0;
    g_context_menu.hovered_index =
        gui_popup_menu_hit_test(items, count, g_context_menu.x, g_context_menu.y, g_context_menu.w, anchor_x, anchor_y);
    enqueue_damage_rect(g_context_menu.x, g_context_menu.y, g_context_menu.w, g_context_menu.h);
}

void close_context_menu()
{
    if (!g_context_menu.open)
        return;
    DirtyRect bounds = context_menu_bounds();
    enqueue_damage_rect(bounds.x, bounds.y, bounds.w, bounds.h);
    g_context_menu = {};
}

void update_context_menu_hover(const Registry *registry, int mx, int my)
{
    if (!g_context_menu.open)
        return;
    if (!ensure_context_menu_target_valid())
        return;
    GuiMenuItem items[8];
    int count = build_context_menu_items(registry, items, 8);
    if (count <= 0) {
        close_context_menu();
        return;
    }
    int hov = gui_popup_menu_hit_test(items, count, g_context_menu.x, g_context_menu.y, g_context_menu.w, mx, my);
    if (hov != g_context_menu.hovered_index) {
        g_context_menu.hovered_index = hov;
        DirtyRect b = context_menu_bounds();
        enqueue_damage_rect(b.x, b.y, b.w, b.h);
    }
}

bool activate_context_menu_item(Registry *registry, int index)
{
    if (!registry || !g_context_menu.open || index < 0)
        return false;
    if (g_context_menu.kind == CONTEXT_MENU_DESKTOP) {
        if (index == 0)
            launch_or_focus_app(registry, "Terminal", "/bin/terminal.elf");
        else if (index == 1)
            launch_or_focus_app(registry, "Files", "/bin/files.elf");
        else if (index == WM_FIRST_USER_WINDOW)
            launch_or_focus_app(registry, "Settings", "/bin/preferences.elf");
        else if (index == 3)
            open_storage_prompt();
        else if (index == 4)
            enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
        else
            return false;
        close_context_menu();
        return true;
    }
    if (g_context_menu.kind == CONTEXT_MENU_WINDOW) {
        int target_index = resolve_context_menu_target_index();
        if (target_index < WM_FIRST_USER_WINDOW) {
            close_context_menu();
            return false;
        }
        const Window &t = g_windows[target_index];
        if (!t.entry) {
            close_context_menu();
            return false;
        }
        t.entry->request_focus = true;
        if (index == 0) {
            if (t.entry->state == WIN_MAXIMIZED)
                t.entry->request_restore = true;
            else
                t.entry->request_maximize = true;
        } else if (index == 1)
            t.entry->request_minimize = true;
        else if (index == WM_FIRST_USER_WINDOW)
            t.entry->request_close = true;
        else if (index == 3)
            launch_or_focus_app(registry, "Settings", "/bin/preferences.elf");
        else if (index == 4)
            open_storage_prompt();
        else
            return false;
        asm volatile("sfence" ::: "memory");
        close_context_menu();
        return true;
    }
    return false;
}

DirtyRect context_menu_bounds()
{
    if (!g_context_menu.open)
        return {0, 0, 0, 0};
    int shadow_pad_x = gui_scaled_metric(8);
    int shadow_pad_y = gui_scaled_metric(12);
    return {g_context_menu.x - shadow_pad_x, g_context_menu.y, g_context_menu.w + shadow_pad_x * 2,
            g_context_menu.h + shadow_pad_y};
}

