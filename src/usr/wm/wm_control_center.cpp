#include "wm_core.h"

ControlCenterState g_control_center = {false, CONTROL_ITEM_NONE, 75, true, true, true, false, true, 180, false};

DirtyRect control_center_bounds()
{
    int margin = gui_space_1();
    int max_w = (int)g_screen.width - margin * 2;
    int max_h = (int)g_screen.height - wm_menubar_h() - margin * 2;
    int min_w = gui_scaled_metric(280);
    int min_h = gui_scaled_metric(300);
    int bw = gui_scaled_metric(348);
    int bh = gui_scaled_metric(366);
    if (max_w > 0 && bw > max_w)
        bw = max_w;
    if (max_h > 0 && bh > max_h)
        bh = max_h;
    if (bw < min_w && max_w >= min_w)
        bw = min_w;
    if (bh < min_h && max_h >= min_h)
        bh = min_h;
    if (bw <= 0)
        bw = (int)g_screen.width;
    if (bh <= 0)
        bh = (int)g_screen.height;
    int x = (int)g_screen.width - bw - margin;
    int y = wm_menubar_h() + margin;
    if (x < margin)
        x = margin;
    if (y < margin)
        y = margin;
    return {x, y, bw, bh};
}

static DirtyRect control_center_panel_damage_bounds()
{
    return rect_expand(control_center_bounds(), gui_scaled_metric(14));
}

static DirtyRect control_center_damage_bounds()
{
    DirtyRect cc = control_center_bounds();
    DirtyRect damage = rect_expand(cc, gui_scaled_metric(14));
    if (g_notifications.count > 0) {
        int notif_h = gui_scaled_metric(240);
        int notif_y = cc.y + cc.h + gui_space_2();
        DirtyRect notif_damage = rect_expand({cc.x, notif_y, cc.w, notif_h}, gui_scaled_metric(14));
        damage = rect_union(damage, notif_damage);
    }
    return damage;
}

int control_panel_card_h()
{
    return gui_app_row_tall_h();
}

DirtyRect control_panel_item_rect(ControlPanelItem item)
{
    DirtyRect box = control_center_bounds();
    int pad = gui_space_1_5();
    int gap = gui_space_1();
    int header_h = gui_card_header_h();
    int card_h = control_panel_card_h();
    int half_w = (box.w - pad * 2 - gap) / 2;
    int y = box.y + header_h + pad;

    if (item == CONTROL_ITEM_NETWORK)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_DARK_MODE)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_DESKTOP_GRID)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_CLOCK_SECONDS)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_ANIMATIONS)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_TRANSPARENCY)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_VOLUME)
        return {box.x + pad, y, box.w - pad * 2, gui_app_slider_h()};

    int action_h = gui_app_control_h();
    int action_y = box.y + box.h - pad - action_h;
    int action_w = (box.w - pad * 2 - gap) / 2;
    if (item == CONTROL_ITEM_STORAGE)
        return {box.x + pad, action_y, action_w, action_h};
    if (item == CONTROL_ITEM_SETTINGS)
        return {box.x + pad + action_w + gap, action_y, action_w, action_h};

    return {0, 0, 0, 0};
}

static ControlPanelItem control_panel_item_at(int mouse_x, int mouse_y)
{
    if (!point_in_rect(control_center_bounds(), mouse_x, mouse_y))
        return CONTROL_ITEM_NONE;
    ControlPanelItem items[] = {CONTROL_ITEM_NETWORK,       CONTROL_ITEM_DARK_MODE,  CONTROL_ITEM_DESKTOP_GRID,
                                CONTROL_ITEM_CLOCK_SECONDS, CONTROL_ITEM_ANIMATIONS, CONTROL_ITEM_TRANSPARENCY,
                                CONTROL_ITEM_VOLUME,        CONTROL_ITEM_STORAGE,    CONTROL_ITEM_SETTINGS};
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++)
        if (point_in_rect(control_panel_item_rect(items[i]), mouse_x, mouse_y))
            return items[i];
    return CONTROL_ITEM_NONE;
}

static DirtyRect control_panel_volume_track_rect()
{
    DirtyRect card = control_panel_item_rect(CONTROL_ITEM_VOLUME);
    Rect track = gui_app_slider_track_rect(card.x, card.y, card.w, card.h);
    return {track.x, track.y, track.w, track.h};
}

static bool set_control_center_volume_from_x(int mouse_x)
{
    DirtyRect track = control_panel_volume_track_rect();
    if (track.w <= 0)
        return false;
    Rect track_rect = gui_rect_make(track.x, track.y, track.w, track.h);
    uint32_t next = gui_app_slider_value_from_x(mouse_x, &track_rect, 100);
    if (next == g_control_center.volume)
        return true;
    g_control_center.volume = next;
    Registry *registry = gui_registry();
    if (registry) {
        registry->volume_level = next;
        publish_settings_changed(registry);
    }
    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
    return true;
}

void sync_control_center_state_from_registry(const Registry *registry)
{
    if (!registry)
        return;
    g_control_center.dark_mode = registry->theme_mode != GUI_THEME_LIGHT;
    g_control_center.desktop_grid = (registry->system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0;
    g_control_center.clock_seconds = (registry->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0;
    g_control_center.network_enabled = registry->ethernet_enabled;
    g_control_center.animations_enabled = registry->animations_enabled;
    g_control_center.transparency_level = registry->transparency_level;
    g_control_center.volume = registry->volume_level <= 100 ? registry->volume_level : 100;
}

void toggle_control_center()
{
    if (g_control_center.open) {
        close_control_center();
        return;
    }
    close_index();
    close_context_menu();
    sync_control_center_state_from_registry(gui_registry());
    g_control_center.open = true;
    Registry *reg = gui_registry();
    if (reg) {
        reg->cp_open = true;
        asm volatile("sfence" ::: "memory");
    }
    g_control_center.hovered_item = CONTROL_ITEM_NONE;
    g_control_center.volume_dragging = false;
    DirtyRect damage = control_center_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

void close_control_center()
{
    if (!g_control_center.open)
        return;
    DirtyRect damage = control_center_damage_bounds();
    g_control_center.open = false;
    Registry *reg = gui_registry();
    if (reg) {
        reg->cp_open = false;
        asm volatile("sfence" ::: "memory");
    }
    g_control_center.hovered_item = CONTROL_ITEM_NONE;
    g_control_center.volume_dragging = false;
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

static void set_theme_from_control_panel(Registry *registry, bool dark)
{
    if (!registry)
        return;
    registry->theme_mode = dark ? GUI_THEME_DARK : GUI_THEME_LIGHT;
    g_control_center.dark_mode = dark;
    publish_settings_changed(registry);
    enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
}

static void set_system_flag_from_control_panel(Registry *registry, uint32_t flag, bool enabled)
{
    if (!registry)
        return;
    if (enabled)
        registry->system_flags |= flag;
    else
        registry->system_flags &= ~flag;
    g_system_flags = registry->system_flags;
    sync_control_center_state_from_registry(registry);
    publish_settings_changed(registry);
    enqueue_damage_rect(0, 0, (int)g_screen.width, wm_menubar_h());
    if (registry->window_count > 1)
        enqueue_damage_rect(registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                            registry->windows[1].h);
}

static void publish_control_center_settings(Registry *registry)
{
    if (!registry) {
        persist_wm_settings();
        return;
    }
    registry->ethernet_enabled = g_control_center.network_enabled;
    registry->animations_enabled = g_control_center.animations_enabled;
    registry->transparency_level = g_control_center.transparency_level;
    registry->volume_level = g_control_center.volume;
    publish_settings_changed(registry);
}

bool handle_control_center_pointer_down(Registry *registry, int mouse_x, int mouse_y)
{
    if (!g_control_center.open)
        return false;
    DirtyRect cc_box = control_center_bounds();
    if (!point_in_rect(cc_box, mouse_x, mouse_y))
        return false;

    ControlPanelItem hit = control_panel_item_at(mouse_x, mouse_y);
    g_control_center.hovered_item = hit;
    if (hit == CONTROL_ITEM_NETWORK) {
        g_control_center.network_enabled = !g_control_center.network_enabled;
        publish_control_center_settings(registry);
    } else if (hit == CONTROL_ITEM_DARK_MODE) {
        set_theme_from_control_panel(registry, !g_control_center.dark_mode);
    } else if (hit == CONTROL_ITEM_DESKTOP_GRID) {
        set_system_flag_from_control_panel(registry, SYSTEM_FLAG_SHOW_DESKTOP_GRID, !g_control_center.desktop_grid);
    } else if (hit == CONTROL_ITEM_CLOCK_SECONDS) {
        set_system_flag_from_control_panel(registry, SYSTEM_FLAG_CLOCK_SHOW_SECONDS, !g_control_center.clock_seconds);
        persist_wm_settings();
    } else if (hit == CONTROL_ITEM_ANIMATIONS) {
        g_control_center.animations_enabled = !g_control_center.animations_enabled;
        publish_control_center_settings(registry);
    } else if (hit == CONTROL_ITEM_TRANSPARENCY) {
        g_control_center.transparency_level = (g_control_center.transparency_level > 200) ? 180 : 255;
        publish_control_center_settings(registry);
        recapture_shell_blur_sources(registry);
    } else if (hit == CONTROL_ITEM_VOLUME) {
        g_control_center.volume_dragging = true;
        set_control_center_volume_from_x(mouse_x);
        publish_control_center_settings(registry);
    } else if (hit == CONTROL_ITEM_STORAGE) {
        close_control_center();
        open_storage_prompt();
        return true;
    } else if (hit == CONTROL_ITEM_SETTINGS) {
        close_control_center();
        launch_or_focus_app(registry, "Settings", "/bin/preferences.elf");
        return true;
    }

    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
    return true;
}

void handle_control_center_pointer_up()
{
    if (!g_control_center.volume_dragging)
        return;
    g_control_center.volume_dragging = false;
    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

void update_control_center_hover(int mouse_x, int mouse_y)
{
    if (!g_control_center.open)
        return;
    ControlPanelItem hit = control_panel_item_at(mouse_x, mouse_y);
    if (hit == g_control_center.hovered_item)
        return;
    g_control_center.hovered_item = hit;
    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

bool update_control_center_drag(int mouse_x, int mouse_y)
{
    (void)mouse_y;
    if (!g_control_center.open || !g_control_center.volume_dragging)
        return false;
    return set_control_center_volume_from_x(mouse_x);
}

bool handle_control_center_scroll(Registry *registry, int mouse_x, int mouse_y, int scroll_y)
{
    if (!g_control_center.open || !point_in_rect(control_center_bounds(), mouse_x, mouse_y))
        return false;
    if (control_panel_item_at(mouse_x, mouse_y) == CONTROL_ITEM_VOLUME) {
        int delta = scroll_y > 0 ? 5 : (scroll_y < 0 ? -5 : 0);
        int next = (int)g_control_center.volume + delta;
        if (next < 0)
            next = 0;
        if (next > 100)
            next = 100;
        if ((uint32_t)next != g_control_center.volume) {
            g_control_center.volume = (uint32_t)next;
            publish_control_center_settings(registry);
            DirtyRect damage = control_center_panel_damage_bounds();
            enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
        }
    }
    return true;
}

