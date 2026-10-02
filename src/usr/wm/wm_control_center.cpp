#include "wm_damage.h"
#include "wm_metrics.h"
#include "wm_overlays.h"
#include "wm_present.h"
#include "wm_settings.h"
#include "wm_window.h"

ControlCenterState g_control_center = {false, CONTROL_ITEM_NONE, 75, true, true, true, false, true, 180, false};

// Control center geometry: a 300px panel, 10px from the screen edges, holding
// a 2x3 quick-toggle tile grid, a capsule volume slider and two actions.
static int cc_pad()
{
    return gui_scaled_metric(12);
}
static int cc_tile_h()
{
    return gui_scaled_metric(44);
}
static int cc_tile_gap()
{
    return gui_scaled_metric(8);
}
static int cc_slider_h()
{
    return gui_scaled_metric(26);
}
static int cc_radius()
{
    return gui_radius_xl();
}

DirtyRect control_center_bounds()
{
    int margin = gui_scaled_metric(10);
    int max_w = (int)g_screen.width - margin * 2;
    int max_h = (int)g_screen.height - wm_menubar_h() - margin * 2;
    int min_w = gui_scaled_metric(280);
    int min_h = gui_scaled_metric(240);
    int bw = gui_scaled_metric(300);
    int pad = cc_pad();
    int gap = cc_tile_gap();
    int bh = pad + 3 * cc_tile_h() + 2 * gap + pad + cc_slider_h() + pad + gui_app_control_h() + gui_space_1() +
             gui_app_control_h() + pad;
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
        int notif_h = notification_center_panel_h();
        int notif_y = cc.y + cc.h + gui_space_1();
        DirtyRect notif_damage = rect_expand({cc.x, notif_y, cc.w, notif_h}, gui_scaled_metric(14));
        damage = rect_union(damage, notif_damage);
    }
    return damage;
}

DirtyRect control_panel_item_rect(ControlPanelItem item)
{
    DirtyRect box = control_center_bounds();
    int pad = cc_pad();
    int gap = cc_tile_gap();
    int tile_h = cc_tile_h();
    int tile_w = (box.w - pad * 2 - gap) / 2;
    int y = box.y + pad;

    // 2x3 quick-toggle tile grid (row-major).
    int toggle_index = -1;
    if (item == CONTROL_ITEM_NETWORK)
        toggle_index = 0;
    else if (item == CONTROL_ITEM_DARK_MODE)
        toggle_index = 1;
    else if (item == CONTROL_ITEM_DESKTOP_GRID)
        toggle_index = 2;
    else if (item == CONTROL_ITEM_CLOCK_SECONDS)
        toggle_index = 3;
    else if (item == CONTROL_ITEM_ANIMATIONS)
        toggle_index = 4;
    else if (item == CONTROL_ITEM_TRANSPARENCY)
        toggle_index = 5;
    if (toggle_index >= 0) {
        int col = toggle_index % 2;
        int row = toggle_index / 2;
        int w = (col == 1) ? (box.x + box.w - pad - (box.x + pad + tile_w + gap)) : tile_w;
        return {box.x + pad + col * (tile_w + gap), y + row * (tile_h + gap), w, tile_h};
    }

    y += 3 * tile_h + 2 * gap + pad;
    if (item == CONTROL_ITEM_VOLUME)
        return {box.x + pad, y, box.w - pad * 2, cc_slider_h()};

    int action_h = gui_app_control_h();
    int action_gap = gui_space_1();
    int action_w = box.w - pad * 2;
    // Storage and Settings are full-width rows stacked above the panel's
    // bottom edge, Storage above Settings.
    if (item == CONTROL_ITEM_STORAGE)
        return {box.x + pad, box.y + box.h - pad - action_h * 2 - action_gap, action_w, action_h};
    if (item == CONTROL_ITEM_SETTINGS)
        return {box.x + pad, box.y + box.h - pad - action_h, action_w, action_h};

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
    // The whole row is the capsule track; the speaker glyph sits inside it.
    return control_panel_item_rect(CONTROL_ITEM_VOLUME);
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

// One quick-toggle tile: compact card with a glyph on the left and a
// title/status pair on the right. Active tiles fill with the accent color.
static void draw_control_tile(ControlPanelItem item, GuiGlyphKind icon, const char *title, const char *status, bool on)
{
    DirtyRect r = control_panel_item_rect(item);
    if (r.w <= 0 || r.h <= 0)
        return;
    bool hovered = g_control_center.hovered_item == item;
    int tile_r = gui_radius_sm() + gui_scaled_metric(2);

    // Tile background: subtle card wash, slightly brighter on hover. The active
    // state highlights the *icon* with an accent circle — the tile itself never
    // turns solid blue.
    uint32_t bg = hovered ? gui_hover_wash_color() : gui_subtle_card_wash_color();
    gui_fill_rounded_rect(&g_backbuffer, r.x, r.y, r.w, r.h, tile_r, bg);

    uint32_t title_fg = g_gui_style.text;
    uint32_t status_fg = g_gui_style.text_muted;

    int icon_size = gui_glyph_std_size();
    int icon_x = r.x + gui_space_1_5();
    int icon_y = r.y + (r.h - icon_size) / 2;
    // Active: accent-tinted circle behind the glyph.
    if (on) {
        int well = icon_size + gui_scaled_metric(6);
        int wx = icon_x - gui_scaled_metric(3);
        int wy = r.y + (r.h - well) / 2;
        gui_fill_rounded_rect(&g_backbuffer, wx, wy, well, well, well / 2, g_gui_style.accent);
    }
    uint32_t icon_fg = on ? COLOR_WHITE : g_gui_style.text_dim;
    gui_draw_glyph(&g_backbuffer, icon_x, icon_y, icon_size, icon, icon_fg);

    int text_x = icon_x + icon_size + gui_space_1();
    int text_w = r.x + r.w - gui_space_1() - text_x;
    int line_h = gui_line_height();
    int title_y = r.y + (r.h - line_h * 2 - gui_scaled_metric(2)) / 2;
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), text_x, title_y, text_w, title, title_fg, 0);
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), text_x, title_y + line_h + gui_scaled_metric(2), text_w,
                          status, status_fg, 0);
}

static void draw_control_volume_slider()
{
    DirtyRect r = control_panel_item_rect(CONTROL_ITEM_VOLUME);
    if (r.w <= 0 || r.h <= 0)
        return;
    int track_r = r.h / 2;

    // Capsule track filling the whole row, accent fill up to the volume.
    gui_fill_rounded_rect(&g_backbuffer, r.x, r.y, r.w, r.h, track_r, gui_inset_wash_color());
    uint64_t fill_w64 = ((uint64_t)g_control_center.volume * r.w + 50u) / 100u;
    int fill_w = (int)fill_w64;
    if (g_control_center.volume > 0 && fill_w < r.h)
        fill_w = r.h;
    if (fill_w > r.w)
        fill_w = r.w;
    if (fill_w > 0)
        gui_fill_rounded_rect(&g_backbuffer, r.x, r.y, fill_w, r.h, track_r, g_gui_style.accent);

    // Speaker glyph sits inside the left end of the capsule and turns white once
    // the accent fill reaches it.
    int icon_size = gui_glyph_std_size();
    int icon_x = r.x + gui_space_1();
    int icon_center_dx = icon_x + icon_size / 2 - r.x;
    uint32_t icon_fg = (fill_w >= icon_center_dx) ? COLOR_WHITE : g_gui_style.text_muted;
    gui_draw_glyph(&g_backbuffer, icon_x, r.y + (r.h - icon_size) / 2, icon_size, GUI_GLYPH_VOLUME, icon_fg);
}

void draw_control_center_overlay_clipped(const DirtyRect &clip)
{
    if (!g_control_center.open || !g_backbuffer.buffer)
        return;

    DirtyRect box = control_center_bounds();
    DirtyRect damage = rect_expand(box, gui_scaled_metric(14));
    if (!rect_intersection(clip, damage, nullptr))
        return;

    int radius = cc_radius();

    gui_draw_panel_shadow_clipped(&g_backbuffer, box.x, box.y, box.w, box.h, radius, clip.x, clip.y, clip.w, clip.h);

    // Panel surface: the window outline recipe (opaque body + 1 px hairline).
    gui_draw_window_frame(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface);

    // 2x3 quick-toggle tile grid.
    draw_control_tile(CONTROL_ITEM_NETWORK, GUI_GLYPH_NETWORK, "Network",
                      g_control_center.network_enabled ? "Ethernet" : "Disconnected", g_control_center.network_enabled);
    draw_control_tile(CONTROL_ITEM_DARK_MODE, GUI_GLYPH_APPEARANCE, "Dark Mode",
                      g_control_center.dark_mode ? "On" : "Off", g_control_center.dark_mode);
    draw_control_tile(CONTROL_ITEM_DESKTOP_GRID, GUI_GLYPH_GRID, "Grid",
                      g_control_center.desktop_grid ? "Shown" : "Hidden", g_control_center.desktop_grid);
    draw_control_tile(CONTROL_ITEM_CLOCK_SECONDS, GUI_GLYPH_CLOCK, "Seconds",
                      g_control_center.clock_seconds ? "Shown" : "Hidden", g_control_center.clock_seconds);
    draw_control_tile(CONTROL_ITEM_ANIMATIONS, GUI_GLYPH_ANIMATION, "Motion",
                      g_control_center.animations_enabled ? "On" : "Off", g_control_center.animations_enabled);
    draw_control_tile(CONTROL_ITEM_TRANSPARENCY, GUI_GLYPH_TRANSPARENCY, "Glass",
                      g_control_center.transparency_level < 255 ? "On" : "Off",
                      g_control_center.transparency_level < 255);

    draw_control_volume_slider();

    DirtyRect storage = control_panel_item_rect(CONTROL_ITEM_STORAGE);
    DirtyRect settings = control_panel_item_rect(CONTROL_ITEM_SETTINGS);
    gui_app_draw_button(&g_backbuffer, storage.x, storage.y, storage.w, storage.h, "Storage", false, false,
                        g_control_center.hovered_item == CONTROL_ITEM_STORAGE);
    gui_app_draw_button(&g_backbuffer, settings.x, settings.y, settings.w, settings.h, "Settings", true, false,
                        g_control_center.hovered_item == CONTROL_ITEM_SETTINGS);

    int notif_y = box.y + box.h + gui_space_1();
    draw_notification_center_clipped(clip, notif_y);
}
