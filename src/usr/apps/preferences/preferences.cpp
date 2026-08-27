#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/gui.h>

#include "../../libapp/app.h"
#include "../../libapp/widgets.h"
#include "../../libc/config_utils.h"
#include "../../libc/log.h"
#include "../../libc/unistd.h"
#include "../../libc/wallpaper_defaults.h"

static constexpr const char *SYSTEM_CONFIG_PATH = "/data/SYSTEM.CFG";
static constexpr const char *SYSTEM_BOOTSTRAP_CONFIG_PATH = "/etc/system.conf";
static constexpr const char *WALLPAPER_CONFIG_PATH = "/data/WALLPAPR.CFG";
static constexpr const char *WALLPAPER_BOOTSTRAP_CONFIG_PATH = "/etc/wallpaper.conf";

// Menubar command IDs (dispatched through WindowEntry.menu_command_id).
enum
{
    PREF_MENU_HELP = 0x80,
};

enum PrefSection
{
    PREF_SECTION_APPEARANCE = 0,
    PREF_SECTION_DESKTOP = 1,
    PREF_SECTION_NETWORK = 2,
    PREF_SECTION_SYSTEM = 3,
    PREF_SECTION_COUNT = 4,
};

struct PreferencesState
{
    GuiThemeMode theme_mode;
    uint32_t system_flags;
    int storage_mode;
    bool ethernet_enabled;
    bool ethernet_use_dhcp;
    bool animations_enabled;
    uint32_t transparency_level;
    uint32_t volume_level;
    char wallpaper_path[256];
    char status[128];
    int section;
};

struct PreferencesApp
{
    PreferencesState state;
    Rect nav[PREF_SECTION_COUNT];
    Rect wallpaper_rect;
    int nav_hover;
    WidgetSegment theme;
    WidgetSegment storage;
    WidgetField wallpaper;
    WidgetButton apply;
    WidgetButton def;
    WidgetToggle animations;
    WidgetToggle transparency;
    WidgetToggle grid;
    WidgetToggle seconds;
    WidgetToggle ethernet;
    WidgetToggle dhcp;
    WidgetToggle terminal;
    WidgetSlider volume;
    WidgetHelp help;
};

static bool cfg_value_enabled(const char *value, bool fallback)
{
    if (!value || !*value)
        return fallback;
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0 || strcmp(value, "no") == 0)
        return false;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0 || strcmp(value, "yes") == 0)
        return true;
    return fallback;
}

static void safe_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src)
        src = "";
    size_t i = 0;
    for (; i + 1 < dst_size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void load_preferences_state(PreferencesState *state, Registry *registry)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->theme_mode = GUI_THEME_DARK;
    state->system_flags = SYSTEM_FLAG_SHOW_DESKTOP_GRID;
    state->storage_mode = STORAGE_MODE_READ_ONLY;
    state->ethernet_enabled = true;
    state->ethernet_use_dhcp = true;
    state->animations_enabled = true;
    state->transparency_level = 180;
    state->volume_level = 75;
    state->section = PREF_SECTION_APPEARANCE;
    safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                   wallpaper_default_path_for_theme(state->theme_mode));

    char config[512];
    const char *system_candidates[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(system_candidates, sizeof(system_candidates) / sizeof(system_candidates[0]),
                                      config, sizeof(config))) {
        char value[64];
        if (cfg_line_value(config, "theme", value, sizeof(value))) {
            state->theme_mode = (strcmp(value, "light") == 0) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
        }
        if (cfg_line_value(config, "show_desktop_grid", value, sizeof(value))) {
            if (cfg_value_enabled(value, (state->system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0))
                state->system_flags |= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
            else
                state->system_flags &= ~SYSTEM_FLAG_SHOW_DESKTOP_GRID;
        }
        if (cfg_line_value(config, "clock_show_seconds", value, sizeof(value))) {
            if (cfg_value_enabled(value, (state->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0))
                state->system_flags |= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
            else
                state->system_flags &= ~SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
        }
        if (cfg_line_value(config, "launch_terminal_on_boot", value, sizeof(value))) {
            if (cfg_value_enabled(value, (state->system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) != 0))
                state->system_flags |= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
            else
                state->system_flags &= ~SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
        }
        if (cfg_line_value(config, "ethernet_enabled", value, sizeof(value)))
            state->ethernet_enabled = cfg_value_enabled(value, state->ethernet_enabled);
        if (cfg_line_value(config, "ethernet_use_dhcp", value, sizeof(value)))
            state->ethernet_use_dhcp = cfg_value_enabled(value, state->ethernet_use_dhcp);
        if (cfg_line_value(config, "animations_enabled", value, sizeof(value)))
            state->animations_enabled = cfg_value_enabled(value, state->animations_enabled);
        if (cfg_line_value(config, "transparency_level", value, sizeof(value)))
            state->transparency_level = (uint32_t)atoi(value);
        if (cfg_line_value(config, "volume_level", value, sizeof(value))) {
            int volume = atoi(value);
            if (volume < 0)
                volume = 0;
            if (volume > 100)
                volume = 100;
            state->volume_level = (uint32_t)volume;
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                       wallpaper_default_path_for_theme(state->theme_mode));
    }

    const char *wallpaper_candidates[] = {WALLPAPER_CONFIG_PATH, WALLPAPER_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(wallpaper_candidates,
                                      sizeof(wallpaper_candidates) / sizeof(wallpaper_candidates[0]), config,
                                      sizeof(config))) {
        char value[256];
        if (cfg_line_value(config, "", value, sizeof(value))) {
            safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), value);
        } else {
            for (size_t i = 0; config[i]; i++) {
                if (config[i] == '\n' || config[i] == '\r') {
                    config[i] = '\0';
                    break;
                }
            }
            if (config[0]) {
                safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), config);
            }
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                       wallpaper_default_path_for_theme(state->theme_mode));
    }

    if (registry) {
        state->theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
        state->system_flags = registry->system_flags;
        state->ethernet_enabled = registry->ethernet_enabled;
        state->ethernet_use_dhcp = registry->ethernet_use_dhcp;
        state->animations_enabled = registry->animations_enabled;
        state->transparency_level = registry->transparency_level;
        state->volume_level = registry->volume_level <= 100 ? registry->volume_level : 100;
        if (registry->storage_mode <= STORAGE_MODE_WRITABLE)
            state->storage_mode = (int)registry->storage_mode;
        if (registry->wallpaper_active[0]) {
            safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), registry->wallpaper_active);
        } else if (registry->wallpaper_requested[0]) {
            safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), registry->wallpaper_requested);
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                       wallpaper_default_path_for_theme(state->theme_mode));
    }
    int storage_mode = get_storage_mode();
    if (storage_mode >= STORAGE_MODE_OFF && storage_mode <= STORAGE_MODE_WRITABLE)
        state->storage_mode = storage_mode;
    snprintf(state->status, sizeof(state->status), "Ready");
}

static bool storage_is_persist_writable(const PreferencesState &state)
{
    return state.storage_mode == STORAGE_MODE_WRITABLE;
}

static void set_session_only_status(PreferencesState *state, const char *session_only_status)
{
    if (!state)
        return;
    if (state->storage_mode == STORAGE_MODE_OFF) {
        snprintf(state->status, sizeof(state->status), "%s (storage is off)", session_only_status);
    } else if (state->storage_mode == STORAGE_MODE_READ_ONLY) {
        snprintf(state->status, sizeof(state->status), "%s (storage is read-only)", session_only_status);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", session_only_status);
    }
}

static bool request_storage_mode_change(Registry *registry, int new_mode)
{
    if (!registry)
        return false;
    if (new_mode < STORAGE_MODE_OFF || new_mode > STORAGE_MODE_WRITABLE)
        return false;
    registry->storage_request_mode = (uint32_t)new_mode;
    asm volatile("sfence" ::: "memory");
    registry->storage_request_generation = registry->storage_request_generation + 1u;
    asm volatile("sfence" ::: "memory");
    return true;
}

static bool persist_system_settings(const PreferencesState &state)
{
    char config[512];
    snprintf(config, sizeof(config),
             "theme=%s\n"
             "show_desktop_grid=%d\n"
             "clock_show_seconds=%d\n"
             "launch_terminal_on_boot=%d\n"
             "ethernet_enabled=%d\n"
             "ethernet_use_dhcp=%d\n"
             "animations_enabled=%d\n"
             "transparency_level=%u\n"
             "volume_level=%u\n",
             state.theme_mode == GUI_THEME_LIGHT ? "light" : "dark",
             (state.system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) ? 1 : 0,
             (state.system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) ? 1 : 0,
             (state.system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) ? 1 : 0, state.ethernet_enabled ? 1 : 0,
             state.ethernet_use_dhcp ? 1 : 0, state.animations_enabled ? 1 : 0, state.transparency_level,
             state.volume_level <= 100 ? state.volume_level : 100);
    return cfg_write_text_file(SYSTEM_CONFIG_PATH, config);
}

static void publish_system_settings(const PreferencesState &state, Registry *registry)
{
    if (!registry)
        return;
    registry->theme_mode = (uint32_t)state.theme_mode;
    registry->system_flags = state.system_flags;
    registry->ethernet_enabled = state.ethernet_enabled;
    registry->ethernet_use_dhcp = state.ethernet_use_dhcp;
    registry->animations_enabled = state.animations_enabled;
    registry->transparency_level = state.transparency_level;
    registry->volume_level = state.volume_level <= 100 ? state.volume_level : 100;
    asm volatile("sfence" ::: "memory");
    registry->settings_generation = registry->settings_generation + 1u;
    asm volatile("sfence" ::: "memory");
}

static void apply_system_settings(PreferencesState *state, Registry *registry, const char *persisted_status,
                                  const char *session_only_status)
{
    if (!state)
        return;
    publish_system_settings(*state, registry);
    if (!storage_is_persist_writable(*state)) {
        set_session_only_status(state, session_only_status);
        return;
    }
    if (persist_system_settings(*state)) {
        snprintf(state->status, sizeof(state->status), "%s", persisted_status);
    } else {
        LOG_ERROR("preferences", "failed to persist %s to %s", persisted_status, SYSTEM_CONFIG_PATH);
        snprintf(state->status, sizeof(state->status), "%s", session_only_status);
    }
}

static void apply_network_settings(PreferencesState *state, Registry *registry, const char *persisted_status,
                                   const char *session_only_status)
{
    if (!state)
        return;
    publish_system_settings(*state, registry);
    if (!storage_is_persist_writable(*state)) {
        set_session_only_status(state, session_only_status);
        return;
    }
    if (persist_system_settings(*state)) {
        snprintf(state->status, sizeof(state->status), "%s", persisted_status);
    } else {
        LOG_ERROR("preferences", "failed to persist %s to %s", persisted_status, SYSTEM_CONFIG_PATH);
        snprintf(state->status, sizeof(state->status), "%s", session_only_status);
    }
}

static bool apply_wallpaper(PreferencesState *state, Registry *registry, const char *path)
{
    if (!state || !path || !*path)
        return false;
    char resolved_path[256];
    const char *requested_path = wallpaper_resolve_path_for_theme(path, state->theme_mode);
    safe_copy_text(resolved_path, sizeof(resolved_path), requested_path);
    Surface image = {};
    if (!gui_load_uowp(resolved_path, wallpaper_uowp_variant_for_theme(state->theme_mode), 0, 0, &image)) {
        snprintf(state->status, sizeof(state->status), "Wallpaper is not a readable UOWP");
        return false;
    }
    gui_destroy_surface(&image);

    safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), resolved_path);
    if (registry) {
        safe_copy_text(registry->wallpaper_requested, sizeof(registry->wallpaper_requested), resolved_path);
        registry->wallpaper_generation = registry->wallpaper_generation + 1u;
        registry->wallpaper_reload_requested = true;
        asm volatile("sfence" ::: "memory");
    }
    if (!storage_is_persist_writable(*state)) {
        set_session_only_status(state, "Wallpaper applied for this session");
        return true;
    }
    char config[320];
    snprintf(config, sizeof(config), "%s\n", resolved_path);
    if (cfg_write_text_file(WALLPAPER_CONFIG_PATH, config)) {
        snprintf(state->status, sizeof(state->status), "Wallpaper updated");
    } else {
        LOG_ERROR("preferences", "failed to persist wallpaper to %s", WALLPAPER_CONFIG_PATH);
        snprintf(state->status, sizeof(state->status), "Wallpaper applied for this session");
    }
    return true;
}

static int compute_preferences_content_height(PreferencesState *state, int detail_w)
{
    int header_h = gui_card_header_h();
    int gap = gui_space_2();
    int section_h = header_h + gap * 2;

    if (state->section == PREF_SECTION_APPEARANCE) {
        section_h += gui_line_height() + gui_space_0_5() + gui_app_control_h();
        section_h += gui_space_3();
        section_h += gui_line_height() + gui_space_0_5();
        bool stacked = detail_w < gui_scaled_metric(420);
        if (stacked)
            section_h += gui_app_control_h() * 2 + gui_space_1();
        else
            section_h += gui_app_control_h();
        section_h += gui_space_3();
        section_h += gui_app_row_tall_h() * 2 + gui_space_1_5();
        section_h += gui_space_2() + gui_line_height();
    } else if (state->section == PREF_SECTION_DESKTOP) {
        int row_h = gui_app_row_tall_h();
        section_h += row_h * 2 + gui_app_slider_h() + gui_space_1_5() * 2;
    } else if (state->section == PREF_SECTION_NETWORK) {
        int row_h = gui_app_row_tall_h();
        section_h += row_h * 2 + gui_space_1_5();
        section_h += gui_space_2() + gui_line_height() * 2;
    } else {
        section_h += gui_app_row_tall_h();
        section_h += gui_space_3() + gui_line_height() + gui_space_0_5() + gui_app_control_h();
        section_h += gui_space_2() + gui_line_height();
    }
    return section_h;
}

static void preferences_menus(App *app)
{
    (void)app;
    MenuModel model;
    gui_menu_model_reset(&model);

    app_menus_add_help(&model, PREF_MENU_HELP);

    gui_menu_publish(&model);
}

static void draw_preferences(App *app, Surface *win)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    PreferencesState *state = &st->state;
    if (!win)
        return;
    GuiAppLayout layout = gui_app_begin(win);
    int view_w = layout.outer_w + layout.outer_x * 2;
    int view_h = layout.outer_h + layout.outer_y + gui_app_outer_padding();

    int nav_w = layout.body_rect.w >= gui_scaled_metric(680) ? gui_scaled_metric(180) : gui_scaled_metric(150);
    int nav_item_h = gui_app_nav_h();
    bool stacked_nav = layout.body_rect.w < gui_scaled_metric(620);
    int nav_content_h = stacked_nav ? nav_item_h : (nav_item_h + gui_space_1()) * PREF_SECTION_COUNT;
    int detail_w = stacked_nav ? layout.body_rect.w : layout.body_rect.w - nav_w - gui_app_section_gap();
    int detail_content_h = compute_preferences_content_height(state, detail_w);

    int body_content_h = (stacked_nav) ? (nav_content_h + gui_app_section_gap() + detail_content_h)
                                       : (nav_content_h > detail_content_h ? nav_content_h : detail_content_h);
    int content_total = layout.body_rect.y + body_content_h + gui_app_outer_padding();
    app_set_content_size(app, view_w, content_total);

    memset(st->nav, 0, sizeof(st->nav));

    int nav_x = layout.body_rect.x;
    int nav_y = layout.body_rect.y;
    int scroll_y = app_scroll_y(app);
    if (stacked_nav)
        nav_w = layout.body_rect.w;

    int nav_h = stacked_nav ? nav_content_h : layout.body_rect.h;
    if (!stacked_nav && nav_content_h > nav_h)
        nav_h = nav_content_h;

    int detail_x = stacked_nav ? nav_x : nav_x + nav_w + gui_app_section_gap();
    int detail_y = stacked_nav ? nav_y + nav_h + gui_app_section_gap() : nav_y;
    int detail_h = stacked_nav ? detail_content_h : layout.body_rect.h;
    if (!stacked_nav && detail_content_h > detail_h)
        detail_h = detail_content_h;

    int sticky_nav_y = nav_y + scroll_y;
    int sticky_detail_y = detail_y + scroll_y;
    int card_header_h = gui_card_header_h();

    gui_draw_panel_inset(win, detail_x, detail_y, detail_w, detail_h, g_gui_style.app_surface, g_gui_style.border,
                         g_gui_style.chrome_bg_alt);

    int content_x = detail_x + gui_space_2();
    int content_y = detail_y + card_header_h + gui_space_2();
    int content_w = detail_w - gui_space_4();

    if (state->section == PREF_SECTION_APPEARANCE) {
        gui_draw_string(win, content_x, content_y, "Theme", g_gui_style.text_dim, g_gui_style.app_surface);
        st->theme.rect = gui_rect_make(content_x, content_y + gui_line_height() + gui_space_0_5(),
                                       gui_scaled_metric(180), gui_app_control_h());
        const char *theme_labels[2] = {"Dark", "Light"};
        widget_segment_draw(win, &st->theme, theme_labels, 2, state->theme_mode == GUI_THEME_LIGHT ? 1 : 0);

        int wallpaper_y = st->theme.rect.y + st->theme.rect.h + gui_space_3();
        gui_draw_string(win, content_x, wallpaper_y, "Wallpaper", g_gui_style.text_dim, g_gui_style.app_surface);
        int controls_y = wallpaper_y + gui_line_height() + gui_space_0_5();
        int apply_w = gui_scaled_metric(72);
        int default_w = gui_scaled_metric(92);
        bool stacked_controls = content_w < gui_scaled_metric(420);
        if (stacked_controls) {
            st->wallpaper_rect = gui_rect_make(content_x, controls_y, content_w, gui_app_control_h());
            st->apply.rect = gui_rect_make(content_x, st->wallpaper_rect.y + st->wallpaper_rect.h + gui_space_1(),
                                           (content_w - gui_space_1()) / 2, gui_app_control_h());
            st->def.rect = gui_rect_make(st->apply.rect.x + st->apply.rect.w + gui_space_1(), st->apply.rect.y,
                                         content_x + content_w - (st->apply.rect.x + st->apply.rect.w + gui_space_1()),
                                         gui_app_control_h());
        } else {
            st->wallpaper_rect = gui_rect_make(
                content_x, controls_y, content_w - (apply_w + default_w + gui_space_1() * 2), gui_app_control_h());
            st->apply.rect = gui_rect_make(st->wallpaper_rect.x + st->wallpaper_rect.w + gui_space_1(),
                                           st->wallpaper_rect.y, apply_w, gui_app_control_h());
            st->def.rect = gui_rect_make(st->apply.rect.x + st->apply.rect.w + gui_space_1(), st->apply.rect.y,
                                         default_w, gui_app_control_h());
        }
        widget_field_draw(win, &st->wallpaper, st->wallpaper_rect.x, st->wallpaper_rect.y, st->wallpaper_rect.w,
                          st->wallpaper_rect.h);
        widget_button_draw(win, &st->apply, "Apply", true, false);
        widget_button_draw(win, &st->def, "Default", false, false);

        int anim_y = st->def.rect.y + st->def.rect.h + gui_space_3();
        st->animations.rect = gui_rect_make(content_x, anim_y, content_w, gui_app_row_tall_h());
        widget_toggle_draw(win, &st->animations, "Motion", "Animate window and system transitions",
                           state->animations_enabled);

        int trans_y = st->animations.rect.y + st->animations.rect.h + gui_space_1_5();
        st->transparency.rect = gui_rect_make(content_x, trans_y, content_w, gui_app_row_tall_h());
        widget_toggle_draw(win, &st->transparency, "Transparency", "Use transparent menu bar and Dock surfaces",
                           state->transparency_level < 255);

        gui_draw_string(win, content_x, st->transparency.rect.y + st->transparency.rect.h + gui_space_2(),
                        state->status, g_gui_style.text_muted, g_gui_style.app_surface);
    } else if (state->section == PREF_SECTION_DESKTOP) {
        int row_h = gui_app_row_tall_h();
        st->grid.rect = gui_rect_make(content_x, content_y, content_w, row_h);
        st->seconds.rect = gui_rect_make(content_x, content_y + row_h + gui_space_1_5(), content_w, row_h);
        int slider_y = st->seconds.rect.y + st->seconds.rect.h + gui_space_1_5();
        st->volume.rect = gui_rect_make(content_x, slider_y, content_w, gui_app_slider_h());
        widget_toggle_draw(win, &st->grid, "Show desktop grid", nullptr,
                           (state->system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0);
        widget_toggle_draw(win, &st->seconds, "Show seconds in menu bar clock", nullptr,
                           (state->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0);
        widget_slider_draw(win, &st->volume, "Volume", 100);
    } else if (state->section == PREF_SECTION_NETWORK) {
        int row_h = gui_app_row_tall_h();
        st->ethernet.rect = gui_rect_make(content_x, content_y, content_w, row_h);
        st->dhcp.rect = gui_rect_make(content_x, content_y + row_h + gui_space_1_5(), content_w, row_h);
        widget_toggle_draw(win, &st->ethernet, "Ethernet",
                           "Use the wired Ethernet stack when a supported NIC is present", state->ethernet_enabled);
        widget_toggle_draw(win, &st->dhcp, "DHCP", "Request address, gateway, and DNS over Ethernet",
                           state->ethernet_use_dhcp);
        int note_y = st->dhcp.rect.y + st->dhcp.rect.h + gui_space_2();
        gui_draw_string(win, content_x, note_y, "Ethernet is the only supported network transport in this build.",
                        g_gui_style.text_muted, g_gui_style.app_surface);
        gui_draw_string(win, content_x, note_y + gui_line_height(), state->status, g_gui_style.text_muted,
                        g_gui_style.app_surface);
    } else {
        st->terminal.rect = gui_rect_make(content_x, content_y, content_w, gui_app_row_tall_h());
        widget_toggle_draw(win, &st->terminal, "Open Terminal at startup", nullptr,
                           (state->system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) != 0);
        int storage_y = st->terminal.rect.y + st->terminal.rect.h + gui_space_3();
        gui_draw_string(win, content_x, storage_y, "Storage Mode", g_gui_style.text_dim, g_gui_style.app_surface);
        st->storage.rect = gui_rect_make(content_x, storage_y + gui_line_height() + gui_space_0_5(),
                                         gui_scaled_metric(260), gui_app_control_h());
        const char *storage_labels[3] = {"Off", "Read-Only", "Writable"};
        widget_segment_draw(win, &st->storage, storage_labels, 3, state->storage_mode);
        gui_draw_string(win, content_x, st->storage.rect.y + st->storage.rect.h + gui_space_2(), state->status,
                        g_gui_style.text_muted, g_gui_style.app_surface);
    }

    if (!stacked_nav) {
        gui_draw_panel_inset(win, nav_x, sticky_nav_y, nav_w, layout.body_rect.h, g_gui_style.app_surface,
                             g_gui_style.border, g_gui_style.chrome_bg_alt);
    } else {
        gui_draw_panel_inset(win, nav_x, sticky_nav_y, nav_w, nav_h, g_gui_style.app_surface, g_gui_style.border,
                             g_gui_style.chrome_bg_alt);
    }

    const char *nav_labels[PREF_SECTION_COUNT] = {"Appearance", "Desktop", "Network", "System"};
    const char *nav_details[PREF_SECTION_COUNT] = {"Theme and wallpaper", "Desktop", "Ethernet", "Startup and storage"};
    for (int i = 0; i < PREF_SECTION_COUNT; i++) {
        int item_x = nav_x + 1;
        int item_y = sticky_nav_y + 1 + i * (nav_item_h + gui_space_1());
        int item_w = nav_w - 2;
        if (stacked_nav) {
            int gap = gui_space_1();
            int slot_w = (nav_w - gap * (PREF_SECTION_COUNT - 1)) / PREF_SECTION_COUNT;
            item_x = nav_x + i * (slot_w + gap);
            item_y = sticky_nav_y + 1;
            item_w = (i == PREF_SECTION_COUNT - 1) ? (nav_x + nav_w - item_x - 1) : slot_w;
        }
        st->nav[i] = gui_rect_make(item_x, item_y, item_w, nav_item_h);
        gui_app_draw_nav_item(win, st->nav[i].x, st->nav[i].y, st->nav[i].w, st->nav[i].h, nav_labels[i],
                              nav_details[i], state->section == i, st->nav_hover == i);
    }

    if (!stacked_nav) {
        gui_draw_panel_inset(win, detail_x, sticky_detail_y, detail_w, card_header_h + 1, g_gui_style.app_surface,
                             g_gui_style.border, g_gui_style.chrome_bg_alt);
        gui_draw_card_header(win, detail_x + 1, sticky_detail_y + 1, detail_w - 2, nav_labels[state->section],
                             nav_details[state->section]);
    } else {
        gui_draw_card_header(win, detail_x + 1, detail_y + 1, detail_w - 2, nav_labels[state->section],
                             nav_details[state->section]);
    }

    gui_app_draw_header(win, &layout, "Settings", "Appearance, desktop, network, and system", nullptr);

    if (st->help.open) {
        static const char *tips[] = {
            "Pick a section on the left to change its settings",
            "Theme, volume and toggles apply immediately",
            "Wallpaper Apply needs a readable .uowp path",
            "Storage mode controls whether changes persist to /data",
            "Settings marked session-only reset on the next boot",
        };
        widget_help_draw(win, view_w, view_h, scroll_y, "Settings Help", tips, 5);
    }
}

static void preferences_draw(App *app, Surface *canvas)
{
    draw_preferences(app, canvas);
}

static void preferences_sync_from_registry(PreferencesApp *st, Registry *registry)
{
    if (!registry)
        return;
    PreferencesState *state = &st->state;
    state->theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
    state->system_flags = registry->system_flags;
    state->ethernet_enabled = registry->ethernet_enabled;
    state->ethernet_use_dhcp = registry->ethernet_use_dhcp;
    state->animations_enabled = registry->animations_enabled;
    state->transparency_level = registry->transparency_level;
    state->volume_level = registry->volume_level <= 100 ? registry->volume_level : 100;
    st->volume.value = state->volume_level;
    if (registry->wallpaper_active[0])
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), registry->wallpaper_active);
    widget_field_set(&st->wallpaper, state->wallpaper_path);
}

static void preferences_settings(App *app)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    preferences_sync_from_registry(st, gui_registry());
    app_invalidate_all(app);
}

static void preferences_menu(App *app, uint32_t cmd)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    if (cmd == PREF_MENU_HELP) {
        st->help.open = true;
        app_invalidate_all(app);
    }
}

static void preferences_clear_hover(PreferencesApp *st)
{
    st->nav_hover = -1;
    widget_toggle_reset(&st->animations);
    widget_toggle_reset(&st->transparency);
    widget_toggle_reset(&st->grid);
    widget_toggle_reset(&st->seconds);
    widget_toggle_reset(&st->ethernet);
    widget_toggle_reset(&st->dhcp);
    widget_toggle_reset(&st->terminal);
    widget_slider_reset(&st->volume);
    widget_segment_reset(&st->theme);
    widget_segment_reset(&st->storage);
    widget_button_reset(&st->apply);
    widget_button_reset(&st->def);
    st->wallpaper.hovered = false;
}

static void preferences_event(App *app, const Event *ev)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    PreferencesState *state = &st->state;
    Registry *registry = gui_registry();

    switch (ev->type) {
        case EVT_UNFOCUS:
        case EVT_MOUSE_LEAVE:
            preferences_clear_hover(st);
            app_invalidate_all(app);
            break;

        case EVT_MOUSE_MOVE: {
            if (st->volume.dragging) {
                if (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) {
                    state->volume_level = st->volume.value;
                    publish_system_settings(*state, registry);
                }
                app_invalidate_all(app);
                break;
            }
            int previous_nav = st->nav_hover;
            st->nav_hover = widget_hit_rects(st->nav, PREF_SECTION_COUNT, ev->mouse.x, ev->mouse.y);
            bool changed = st->nav_hover != previous_nav;
            changed |= (widget_segment_event(&st->theme, ev, 2, nullptr) & WIDGET_CHANGED) != 0;
            changed |= (widget_segment_event(&st->storage, ev, 3, nullptr) & WIDGET_CHANGED) != 0;
            changed |= (widget_field_event(&st->wallpaper, st->wallpaper_rect, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_button_event(&st->apply, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_button_event(&st->def, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_toggle_event(&st->animations, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_toggle_event(&st->transparency, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_toggle_event(&st->grid, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_toggle_event(&st->seconds, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_toggle_event(&st->ethernet, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_toggle_event(&st->dhcp, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_toggle_event(&st->terminal, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) != 0;
            if (changed)
                app_invalidate_all(app);
            break;
        }

        case EVT_MOUSE_DOWN: {
            if (ev->mouse.button != 1)
                break;
            if (st->help.open) {
                if (widget_help_event(&st->help, ev))
                    app_invalidate_all(app);
                break;
            }

            if (widget_toggle_event(&st->animations, ev) & WIDGET_CLICKED) {
                state->animations_enabled = !state->animations_enabled;
                apply_system_settings(state, registry, "Animations updated", "Animations applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (widget_toggle_event(&st->transparency, ev) & WIDGET_CLICKED) {
                state->transparency_level = (state->transparency_level > 200) ? 180 : 255;
                apply_system_settings(state, registry, "Transparency updated", "Transparency applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (widget_toggle_event(&st->grid, ev) & WIDGET_CLICKED) {
                state->system_flags ^= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
                apply_system_settings(state, registry, "Desktop setting updated",
                                      "Desktop setting applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (widget_toggle_event(&st->seconds, ev) & WIDGET_CLICKED) {
                state->system_flags ^= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
                apply_system_settings(state, registry, "Clock setting updated",
                                      "Clock setting applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (widget_toggle_event(&st->ethernet, ev) & WIDGET_CLICKED) {
                state->ethernet_enabled = !state->ethernet_enabled;
                apply_network_settings(state, registry, "Ethernet updated",
                                       "Ethernet setting applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (widget_toggle_event(&st->dhcp, ev) & WIDGET_CLICKED) {
                state->ethernet_use_dhcp = !state->ethernet_use_dhcp;
                apply_network_settings(state, registry, "DHCP updated",
                                       "Ethernet DHCP setting applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (widget_toggle_event(&st->terminal, ev) & WIDGET_CLICKED) {
                state->system_flags ^= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
                apply_system_settings(state, registry, "Startup setting updated",
                                      "Startup setting applied for this session");
                app_invalidate_all(app);
                break;
            }

            int seg_index = -1;
            if (widget_segment_event(&st->theme, ev, 2, &seg_index) & WIDGET_CLICKED) {
                bool wallpaper_tracks_theme = wallpaper_is_default_family_path(state->wallpaper_path);
                state->theme_mode = (seg_index == 1) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
                if (wallpaper_tracks_theme) {
                    safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                                   wallpaper_default_path_for_theme(state->theme_mode));
                    widget_field_set(&st->wallpaper, state->wallpaper_path);
                }
                apply_system_settings(state, registry, "Theme updated", "Theme applied for this session");
                gui_sync_theme_from_registry();
                app_invalidate_all(app);
                break;
            }
            if (widget_segment_event(&st->storage, ev, 3, &seg_index) & WIDGET_CLICKED) {
                if (request_storage_mode_change(registry, seg_index))
                    snprintf(state->status, sizeof(state->status), "Storage Mode update requested");
                else
                    snprintf(state->status, sizeof(state->status), "Failed to update Storage Mode");
                app_invalidate_all(app);
                break;
            }

            if (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) {
                state->volume_level = st->volume.value;
                publish_system_settings(*state, registry);
                app_invalidate_all(app);
                break;
            }

            if (widget_button_event(&st->apply, ev) & WIDGET_CHANGED)
                app_invalidate_all(app);
            if (widget_button_event(&st->def, ev) & WIDGET_CHANGED)
                app_invalidate_all(app);

            int field_rc = widget_field_event(&st->wallpaper, st->wallpaper_rect, ev);
            if (field_rc & WIDGET_CHANGED)
                app_invalidate_all(app);

            int nav_index = widget_hit_rects(st->nav, PREF_SECTION_COUNT, ev->mouse.x, ev->mouse.y);
            if (nav_index >= 0 && nav_index != state->section) {
                state->section = nav_index;
                app_invalidate_all(app);
            }
            break;
        }

        case EVT_MOUSE_UP: {
            if (ev->mouse.button != 1)
                break;
            if (st->volume.dragging) {
                if (widget_slider_event(&st->volume, ev, 100) & WIDGET_CLICKED)
                    apply_system_settings(state, registry, "Volume updated", "Volume applied for this session");
                app_invalidate_all(app);
                break;
            }
            // Release-to-apply: the wallpaper change fires on mouse-up so a
            // drag away cancels it.
            if (widget_button_event(&st->apply, ev) & WIDGET_CLICKED) {
                apply_wallpaper(state, registry, state->wallpaper_path);
                widget_field_set(&st->wallpaper, state->wallpaper_path);
                app_invalidate_all(app);
                break;
            }
            if (widget_button_event(&st->def, ev) & WIDGET_CLICKED) {
                apply_wallpaper(state, registry, wallpaper_default_path_for_theme(state->theme_mode));
                widget_field_set(&st->wallpaper, state->wallpaper_path);
                app_invalidate_all(app);
                break;
            }
            break;
        }

        case EVT_KEY_DOWN: {
            if (st->help.open) {
                if (widget_help_event(&st->help, ev))
                    app_invalidate_all(app);
                break;
            }
            int field_rc = widget_field_event(&st->wallpaper, st->wallpaper_rect, ev);
            if (field_rc & WIDGET_FIELD_ENTER) {
                apply_wallpaper(state, registry, state->wallpaper_path);
                widget_field_set(&st->wallpaper, state->wallpaper_path);
                app_invalidate_all(app);
            } else if (field_rc & WIDGET_CHANGED) {
                app_invalidate_all(app);
            }
            break;
        }

        default:
            break;
    }
}

static void preferences_idle(App *app)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    Registry *registry = gui_registry();
    int current_storage_mode =
        registry && registry->storage_mode <= STORAGE_MODE_WRITABLE ? (int)registry->storage_mode : get_storage_mode();
    if (current_storage_mode >= STORAGE_MODE_OFF && current_storage_mode <= STORAGE_MODE_WRITABLE &&
        current_storage_mode != st->state.storage_mode) {
        st->state.storage_mode = current_storage_mode;
        app_invalidate_all(app);
    }
}

extern "C" int main()
{
    static PreferencesApp st = {};
    st.nav_hover = -1;
    load_preferences_state(&st.state, nullptr);
    widget_field_init(&st.wallpaper, st.state.wallpaper_path, sizeof(st.state.wallpaper_path));

    AppConfig config = {};
    config.title = "Settings";
    config.width = gui_scaled_metric(760);
    config.height = gui_scaled_metric(460);
    config.min_width = gui_scaled_metric(560);
    config.min_height = gui_scaled_metric(420);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 10;
    config.on_draw = preferences_draw;
    config.on_event = preferences_event;
    config.on_menu = preferences_menu;
    config.on_menus = preferences_menus;
    config.on_settings = preferences_settings;
    config.on_idle = preferences_idle;

    App *app = app_create(&config, &st);
    if (!app)
        return 1;

    // The registry exists once the window is registered: seed widget state
    // from it (theme, volume, wallpaper) before the first frame.
    preferences_sync_from_registry(&st, gui_registry());
    gui_apply_theme(st.state.theme_mode);

    app_invalidate_all(app);
    app_commit(app);
    while (app_pump(app)) {
        app_commit(app);
        if (!app_needs_draw(app) && !st.volume.dragging)
            sleep_ms(config.idle_ms);
    }
    app_destroy(app);
    return 0;
}
