#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/gui.h>

#include "../../libc/config_utils.h"
#include "../../libc/log.h"
#include "../../libc/unistd.h"
#include "../../libc/wallpaper_defaults.h"
#include "../../libgui/gui.h"

static constexpr const char *SYSTEM_CONFIG_PATH = "/data/SYSTEM.CFG";
static constexpr const char *SYSTEM_BOOTSTRAP_CONFIG_PATH = "/etc/system.conf";
static constexpr const char *WALLPAPER_CONFIG_PATH = "/data/WALLPAPR.CFG";
static constexpr const char *WALLPAPER_BOOTSTRAP_CONFIG_PATH = "/etc/wallpaper.conf";

enum PrefSection
{
    PREF_SECTION_APPEARANCE = 0,
    PREF_SECTION_DESKTOP = 1,
    PREF_SECTION_SYSTEM = 2,
    PREF_SECTION_COUNT = 3,
};

enum HoverTarget
{
    HOVER_NONE = 0,
    HOVER_NAV_APPEARANCE,
    HOVER_NAV_DESKTOP,
    HOVER_NAV_SYSTEM,
    HOVER_THEME_DARK,
    HOVER_THEME_LIGHT,
    HOVER_WALLPAPER_FIELD,
    HOVER_WALLPAPER_APPLY,
    HOVER_WALLPAPER_DEFAULT,
    HOVER_GRID_TOGGLE,
    HOVER_SECONDS_TOGGLE,
    HOVER_TERMINAL_TOGGLE,
    HOVER_STORAGE_OFF,
    HOVER_STORAGE_READ_ONLY,
    HOVER_STORAGE_WRITABLE,
};

struct PreferencesState
{
    GuiThemeMode theme_mode;
    uint32_t system_flags;
    int storage_mode;
    char wallpaper_path[256];
    char status[128];
    int section;
    HoverTarget hovered;
    bool field_focused;
};

struct PreferencesRects
{
    Rect nav[PREF_SECTION_COUNT];
    Rect theme_segment;
    Rect wallpaper_field;
    Rect wallpaper_apply;
    Rect wallpaper_default_btn;
    Rect grid_toggle;
    Rect seconds_toggle;
    Rect terminal_toggle;
    Rect storage_segment;
};

static Rect inflate_rect(Rect rect, int pad)
{
    if (gui_rect_is_empty(rect))
        return rect;
    return gui_rect_make(rect.x - pad, rect.y - pad, rect.w + pad * 2, rect.h + pad * 2);
}

static Rect merge_rects(Rect a, Rect b)
{
    if (gui_rect_is_empty(a))
        return b;
    if (gui_rect_is_empty(b))
        return a;
    return gui_rect_union(a, b);
}

static bool point_in_rect(const Rect &rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static void load_preferences_state(PreferencesState *state, Registry *registry)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->theme_mode = GUI_THEME_DARK;
    state->system_flags = SYSTEM_FLAG_SHOW_DESKTOP_GRID;
    state->storage_mode = STORAGE_MODE_READ_ONLY;
    state->section = PREF_SECTION_APPEARANCE;
    state->hovered = HOVER_NONE;
    strncpy(state->wallpaper_path, wallpaper_default_path_for_theme(state->theme_mode),
            sizeof(state->wallpaper_path) - 1);

    char config[512];
    const char *system_candidates[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(system_candidates, sizeof(system_candidates) / sizeof(system_candidates[0]),
                                      config, sizeof(config))) {
        char value[64];
        if (cfg_line_value(config, "theme", value, sizeof(value))) {
            state->theme_mode = (strcmp(value, "light") == 0) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
        }
        if (cfg_line_value(config, "show_desktop_grid", value, sizeof(value))) {
            if (value[0] == '0')
                state->system_flags &= ~SYSTEM_FLAG_SHOW_DESKTOP_GRID;
        }
        if (cfg_line_value(config, "clock_show_seconds", value, sizeof(value))) {
            if (value[0] != '0')
                state->system_flags |= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
        }
        if (cfg_line_value(config, "launch_terminal_on_boot", value, sizeof(value))) {
            if (value[0] == '0')
                state->system_flags &= ~SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
            else
                state->system_flags |= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        strncpy(state->wallpaper_path, wallpaper_default_path_for_theme(state->theme_mode),
                sizeof(state->wallpaper_path) - 1);
        state->wallpaper_path[sizeof(state->wallpaper_path) - 1] = '\0';
    }

    const char *wallpaper_candidates[] = {WALLPAPER_CONFIG_PATH, WALLPAPER_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(wallpaper_candidates,
                                      sizeof(wallpaper_candidates) / sizeof(wallpaper_candidates[0]), config,
                                      sizeof(config))) {
        char value[256];
        if (cfg_line_value(config, "", value, sizeof(value))) {
            strncpy(state->wallpaper_path, value, sizeof(state->wallpaper_path) - 1);
        } else {
            for (size_t i = 0; config[i]; i++) {
                if (config[i] == '\n' || config[i] == '\r') {
                    config[i] = '\0';
                    break;
                }
            }
            if (config[0]) {
                strncpy(state->wallpaper_path, config, sizeof(state->wallpaper_path) - 1);
            }
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        strncpy(state->wallpaper_path, wallpaper_default_path_for_theme(state->theme_mode),
                sizeof(state->wallpaper_path) - 1);
        state->wallpaper_path[sizeof(state->wallpaper_path) - 1] = '\0';
    }

    if (registry) {
        state->theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
        state->system_flags = registry->system_flags;
        if (registry->storage_mode <= STORAGE_MODE_WRITABLE)
            state->storage_mode = (int)registry->storage_mode;
        if (registry->wallpaper_active[0]) {
            strncpy(state->wallpaper_path, registry->wallpaper_active, sizeof(state->wallpaper_path) - 1);
            state->wallpaper_path[sizeof(state->wallpaper_path) - 1] = '\0';
        } else if (registry->wallpaper_requested[0]) {
            strncpy(state->wallpaper_path, registry->wallpaper_requested, sizeof(state->wallpaper_path) - 1);
            state->wallpaper_path[sizeof(state->wallpaper_path) - 1] = '\0';
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        strncpy(state->wallpaper_path, wallpaper_default_path_for_theme(state->theme_mode),
                sizeof(state->wallpaper_path) - 1);
        state->wallpaper_path[sizeof(state->wallpaper_path) - 1] = '\0';
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
    char config[256];
    snprintf(config, sizeof(config),
             "theme=%s\nshow_desktop_grid=%d\nclock_show_seconds=%d\nlaunch_terminal_on_boot=%d\n",
             state.theme_mode == GUI_THEME_LIGHT ? "light" : "dark",
             (state.system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) ? 1 : 0,
             (state.system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) ? 1 : 0,
             (state.system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) ? 1 : 0);
    return cfg_write_text_file(SYSTEM_CONFIG_PATH, config);
}

static void publish_system_settings(const PreferencesState &state, Registry *registry)
{
    if (!registry)
        return;
    registry->theme_mode = (uint32_t)state.theme_mode;
    registry->system_flags = state.system_flags;
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

static bool apply_wallpaper(PreferencesState *state, Registry *registry, const char *path)
{
    if (!state || !path || !*path)
        return false;
    char resolved_path[256];
    const char *requested_path = wallpaper_resolve_path_for_theme(path, state->theme_mode);
    strncpy(resolved_path, requested_path, sizeof(resolved_path) - 1);
    resolved_path[sizeof(resolved_path) - 1] = '\0';
    Surface image = {};
    if (!gui_load_uowp(resolved_path, wallpaper_uowp_variant_for_theme(state->theme_mode), 0, 0, &image)) {
        snprintf(state->status, sizeof(state->status), "Wallpaper is not a readable UOWP");
        return false;
    }
    gui_destroy_surface(&image);

    strncpy(state->wallpaper_path, resolved_path, sizeof(state->wallpaper_path) - 1);
    state->wallpaper_path[sizeof(state->wallpaper_path) - 1] = '\0';
    if (registry) {
        strncpy(registry->wallpaper_requested, resolved_path, sizeof(registry->wallpaper_requested) - 1);
        registry->wallpaper_requested[sizeof(registry->wallpaper_requested) - 1] = '\0';
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

static HoverTarget hovered_nav_target(int index)
{
    if (index == PREF_SECTION_APPEARANCE)
        return HOVER_NAV_APPEARANCE;
    if (index == PREF_SECTION_DESKTOP)
        return HOVER_NAV_DESKTOP;
    return HOVER_NAV_SYSTEM;
}

static Rect hover_target_rect(const PreferencesRects &rects, int section, HoverTarget target)
{
    switch (target) {
        case HOVER_NAV_APPEARANCE:
            return inflate_rect(rects.nav[PREF_SECTION_APPEARANCE], 1);
        case HOVER_NAV_DESKTOP:
            return inflate_rect(rects.nav[PREF_SECTION_DESKTOP], 1);
        case HOVER_NAV_SYSTEM:
            return inflate_rect(rects.nav[PREF_SECTION_SYSTEM], 1);
        case HOVER_THEME_DARK: {
            Rect segment = rects.theme_segment;
            if (gui_rect_is_empty(segment))
                return segment;
            return inflate_rect(gui_rect_make(segment.x, segment.y, segment.w / 2, segment.h), 1);
        }
        case HOVER_THEME_LIGHT: {
            Rect segment = rects.theme_segment;
            if (gui_rect_is_empty(segment))
                return segment;
            int half_w = segment.w / 2;
            return inflate_rect(gui_rect_make(segment.x + half_w, segment.y, segment.w - half_w, segment.h), 1);
        }
        case HOVER_WALLPAPER_FIELD:
            return inflate_rect(rects.wallpaper_field, 1);
        case HOVER_WALLPAPER_APPLY:
            return inflate_rect(rects.wallpaper_apply, 1);
        case HOVER_WALLPAPER_DEFAULT:
            return inflate_rect(rects.wallpaper_default_btn, 1);
        case HOVER_GRID_TOGGLE:
            return section == PREF_SECTION_DESKTOP ? inflate_rect(rects.grid_toggle, 1) : gui_rect_make(0, 0, 0, 0);
        case HOVER_SECONDS_TOGGLE:
            return section == PREF_SECTION_DESKTOP ? inflate_rect(rects.seconds_toggle, 1) : gui_rect_make(0, 0, 0, 0);
        case HOVER_TERMINAL_TOGGLE:
            return section == PREF_SECTION_SYSTEM ? inflate_rect(rects.terminal_toggle, 1) : gui_rect_make(0, 0, 0, 0);
        case HOVER_STORAGE_OFF:
        case HOVER_STORAGE_READ_ONLY:
        case HOVER_STORAGE_WRITABLE: {
            if (section != PREF_SECTION_SYSTEM || gui_rect_is_empty(rects.storage_segment))
                return gui_rect_make(0, 0, 0, 0);
            int third_w = rects.storage_segment.w / 3;
            int x = rects.storage_segment.x;
            if (target == HOVER_STORAGE_READ_ONLY)
                x += third_w;
            else if (target == HOVER_STORAGE_WRITABLE)
                x += third_w * 2;
            int w =
                (target == HOVER_STORAGE_WRITABLE) ? (rects.storage_segment.x + rects.storage_segment.w - x) : third_w;
            return inflate_rect(gui_rect_make(x, rects.storage_segment.y, w, rects.storage_segment.h), 1);
        }
        case HOVER_NONE:
        default:
            return gui_rect_make(0, 0, 0, 0);
    }
}

static int compute_preferences_content_height(PreferencesState *state, int detail_w)
{
    int header_h = gui_card_header_h();
    int gap = gui_space_2();
    int section_h = header_h + gap * 2;

    if (state->section == PREF_SECTION_APPEARANCE) {
        section_h += gui_line_height() + 6 + gui_app_control_h(); // Theme
        section_h += gui_space_3();
        section_h += gui_line_height() + 6; // Wallpaper label
        bool stacked = detail_w < gui_scaled_metric(420);
        if (stacked)
            section_h += gui_app_control_h() * 2 + gui_space_1();
        else
            section_h += gui_app_control_h();
        section_h += gui_space_2() + gui_line_height(); // Status
    } else if (state->section == PREF_SECTION_DESKTOP) {
        int row_h = gui_scaled_metric(40);
        section_h += row_h * 2 + gui_scaled_metric(12);
    } else {
        section_h += gui_scaled_metric(40); // Terminal toggle
        section_h += gui_space_3() + gui_line_height() + 6 + gui_app_control_h(); // Storage
        section_h += gui_space_2() + gui_line_height(); // Status
    }
    return section_h;
}

static void draw_preferences(Surface *win, PreferencesState *state, PreferencesRects *rects, const Rect *present_rect)
{
    if (!win || !state || !rects)
        return;
    GuiAppLayout layout = gui_app_begin(win);
    int view_w = layout.outer_w + layout.outer_x * 2;
    int view_h = layout.outer_h + layout.outer_y + gui_app_outer_padding();

    int nav_w = layout.body_rect.w >= gui_scaled_metric(680) ? gui_scaled_metric(180) : gui_scaled_metric(150);
    int nav_item_h = gui_scaled_metric(52);
    bool stacked_nav = layout.body_rect.w < gui_scaled_metric(620);
    int nav_content_h = stacked_nav ? nav_item_h : (nav_item_h + gui_space_1()) * PREF_SECTION_COUNT;
    int detail_w = stacked_nav ? layout.body_rect.w : layout.body_rect.w - nav_w - gui_app_section_gap();
    int detail_content_h = compute_preferences_content_height(state, detail_w);

    int body_content_h = (stacked_nav) ? (nav_content_h + gui_app_section_gap() + detail_content_h)
                                       : (nav_content_h > detail_content_h ? nav_content_h : detail_content_h);
    int content_total = layout.body_rect.y + body_content_h + gui_app_outer_padding();
    gui_set_content_size(win, view_w, content_total);

    memset(rects, 0, sizeof(*rects));

    int nav_x = layout.body_rect.x;
    int nav_y = layout.body_rect.y;
    int scroll_y = (g_my_window) ? g_my_window->scroll_y : 0;
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

    // 1. Draw non-sticky backgrounds
    if (!stacked_nav) {
        gui_draw_panel_inset(win, detail_x, detail_y, detail_w, detail_h, g_gui_style.app_surface, g_gui_style.border,
                             g_gui_style.chrome_bg_alt);
    } else {
        gui_draw_panel_inset(win, detail_x, detail_y, detail_w, detail_h, g_gui_style.app_surface, g_gui_style.border,
                             g_gui_style.chrome_bg_alt);
    }

    // 2. Draw scrolling content
    int content_x = detail_x + gui_space_2();
    int content_y = detail_y + card_header_h + gui_space_2();
    int content_w = detail_w - gui_space_4();

    if (state->section == PREF_SECTION_APPEARANCE) {
        gui_draw_string(win, content_x, content_y, "Theme", g_gui_style.text_dim, g_gui_style.app_surface);
        rects->theme_segment =
            gui_rect_make(content_x, content_y + gui_line_height() + 6, gui_scaled_metric(180), gui_app_control_h());
        const char *theme_labels[2] = {"Dark", "Light"};
        int hovered_seg = -1;
        if (state->hovered == HOVER_THEME_DARK)
            hovered_seg = 0;
        else if (state->hovered == HOVER_THEME_LIGHT)
            hovered_seg = 1;
        gui_app_draw_segmented_choice(win, rects->theme_segment.x, rects->theme_segment.y, rects->theme_segment.w,
                                      rects->theme_segment.h, theme_labels, 2,
                                      state->theme_mode == GUI_THEME_LIGHT ? 1 : 0, hovered_seg);

        int wallpaper_y = rects->theme_segment.y + rects->theme_segment.h + gui_space_3();
        gui_draw_string(win, content_x, wallpaper_y, "Wallpaper", g_gui_style.text_dim, g_gui_style.app_surface);
        int controls_y = wallpaper_y + gui_line_height() + 6;
        int apply_w = gui_scaled_metric(72);
        int default_w = gui_scaled_metric(92);
        bool stacked_controls = content_w < gui_scaled_metric(420);
        if (stacked_controls) {
            rects->wallpaper_field = gui_rect_make(content_x, controls_y, content_w, gui_app_control_h());
            rects->wallpaper_apply =
                gui_rect_make(content_x, rects->wallpaper_field.y + rects->wallpaper_field.h + gui_space_1(),
                               (content_w - gui_space_1()) / 2, gui_app_control_h());
            rects->wallpaper_default_btn = gui_rect_make(
                rects->wallpaper_apply.x + rects->wallpaper_apply.w + gui_space_1(), rects->wallpaper_apply.y,
                content_x + content_w - (rects->wallpaper_apply.x + rects->wallpaper_apply.w + gui_space_1()),
                gui_app_control_h());
        } else {
            rects->wallpaper_field = gui_rect_make(
                content_x, controls_y, content_w - (apply_w + default_w + gui_space_1() * 2), gui_app_control_h());
            rects->wallpaper_apply = gui_rect_make(rects->wallpaper_field.x + rects->wallpaper_field.w + gui_space_1(),
                                                   rects->wallpaper_field.y, apply_w, gui_app_control_h());
            rects->wallpaper_default_btn =
                gui_rect_make(rects->wallpaper_apply.x + rects->wallpaper_apply.w + gui_space_1(),
                               rects->wallpaper_apply.y, default_w, gui_app_control_h());
        }
        gui_app_draw_text_field(win, rects->wallpaper_field.x, rects->wallpaper_field.y, rects->wallpaper_field.w,
                                rects->wallpaper_field.h, state->wallpaper_path, state->field_focused,
                                state->hovered == HOVER_WALLPAPER_FIELD);
        gui_app_draw_button(win, rects->wallpaper_apply.x, rects->wallpaper_apply.y, rects->wallpaper_apply.w,
                            rects->wallpaper_apply.h, "Apply", true, false, state->hovered == HOVER_WALLPAPER_APPLY);
        gui_app_draw_button(win, rects->wallpaper_default_btn.x, rects->wallpaper_default_btn.y,
                            rects->wallpaper_default_btn.w, rects->wallpaper_default_btn.h, "Default", false, false,
                            state->hovered == HOVER_WALLPAPER_DEFAULT);

        gui_draw_string(win, content_x, rects->wallpaper_default_btn.y + rects->wallpaper_default_btn.h + gui_space_2(),
                        state->status, g_gui_style.text_muted, g_gui_style.app_surface);
    } else if (state->section == PREF_SECTION_DESKTOP) {
        int row_h = gui_scaled_metric(40);
        rects->grid_toggle = gui_rect_make(content_x, content_y, content_w, row_h);
        rects->seconds_toggle = gui_rect_make(content_x, content_y + row_h + gui_scaled_metric(12), content_w, row_h);
        gui_app_draw_toggle_row(win, rects->grid_toggle.x, rects->grid_toggle.y, rects->grid_toggle.w,
                                rects->grid_toggle.h, "Show desktop grid", nullptr,
                                (state->system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0, false,
                                state->hovered == HOVER_GRID_TOGGLE);
        gui_app_draw_toggle_row(win, rects->seconds_toggle.x, rects->seconds_toggle.y, rects->seconds_toggle.w,
                                rects->seconds_toggle.h, "Show seconds in menu bar clock", nullptr,
                                (state->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0, false,
                                state->hovered == HOVER_SECONDS_TOGGLE);
    } else {
        rects->terminal_toggle = gui_rect_make(content_x, content_y, content_w, gui_scaled_metric(40));
        gui_app_draw_toggle_row(win, rects->terminal_toggle.x, rects->terminal_toggle.y, rects->terminal_toggle.w,
                                rects->terminal_toggle.h, "Open Terminal at startup", nullptr,
                                (state->system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) != 0, false,
                                state->hovered == HOVER_TERMINAL_TOGGLE);
        int storage_y = rects->terminal_toggle.y + rects->terminal_toggle.h + gui_space_3();
        gui_draw_string(win, content_x, storage_y, "Storage Mode", g_gui_style.text_dim, g_gui_style.app_surface);
        rects->storage_segment =
            gui_rect_make(content_x, storage_y + gui_line_height() + 6, gui_scaled_metric(260), gui_app_control_h());
        const char *storage_labels[3] = {"Off", "Read-Only", "Writable"};
        int hovered_seg = -1;
        if (state->hovered == HOVER_STORAGE_OFF)
            hovered_seg = 0;
        else if (state->hovered == HOVER_STORAGE_READ_ONLY)
            hovered_seg = 1;
        else if (state->hovered == HOVER_STORAGE_WRITABLE)
            hovered_seg = 2;
        gui_app_draw_segmented_choice(win, rects->storage_segment.x, rects->storage_segment.y, rects->storage_segment.w,
                                      rects->storage_segment.h, storage_labels, 3, state->storage_mode, hovered_seg);
        gui_draw_string(win, content_x, rects->storage_segment.y + rects->storage_segment.h + gui_space_2(),
                        state->status, g_gui_style.text_muted, g_gui_style.app_surface);
    }

    // 3. Draw sticky overlays (sidebar and headers)
    if (!stacked_nav) {
        gui_draw_panel_inset(win, nav_x, sticky_nav_y, nav_w, layout.body_rect.h, g_gui_style.app_surface,
                             g_gui_style.border, g_gui_style.chrome_bg_alt);
    } else {
        gui_draw_panel_inset(win, nav_x, sticky_nav_y, nav_w, nav_h, g_gui_style.app_surface, g_gui_style.border,
                             g_gui_style.chrome_bg_alt);
    }

    const char *nav_labels[PREF_SECTION_COUNT] = {"Appearance", "Desktop", "System"};
    const char *nav_details[PREF_SECTION_COUNT] = {"Theme and wallpaper", "Desktop shell", "Startup and storage"};
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
        rects->nav[i] = gui_rect_make(item_x, item_y, item_w, nav_item_h);
        gui_app_draw_nav_item(win, rects->nav[i].x, rects->nav[i].y, rects->nav[i].w, rects->nav[i].h, nav_labels[i],
                              nav_details[i], state->section == i, state->hovered == hovered_nav_target(i));
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

    gui_app_draw_header(win, &layout, "Settings", "Appearance, desktop, and startup defaults", nullptr);

    if (present_rect && !gui_rect_is_empty(*present_rect)) {
        gui_blit_to_screen_rect(win, present_rect->x, present_rect->y, present_rect->w, present_rect->h);
    } else {
        gui_blit_to_screen_rect(win, 0, 0, (int)win->width, (int)win->height);
    }
}

static HoverTarget update_hover_target(const PreferencesRects &rects, int section, int x, int y)
{
    for (int i = 0; i < PREF_SECTION_COUNT; i++) {
        if (point_in_rect(rects.nav[i], x, y))
            return hovered_nav_target(i);
    }

    if (section == PREF_SECTION_APPEARANCE) {
        if (point_in_rect(rects.theme_segment, x, y)) {
            int rel_x = x - rects.theme_segment.x;
            return (rel_x < rects.theme_segment.w / 2) ? HOVER_THEME_DARK : HOVER_THEME_LIGHT;
        }
        if (point_in_rect(rects.wallpaper_field, x, y))
            return HOVER_WALLPAPER_FIELD;
        if (point_in_rect(rects.wallpaper_apply, x, y))
            return HOVER_WALLPAPER_APPLY;
        if (point_in_rect(rects.wallpaper_default_btn, x, y))
            return HOVER_WALLPAPER_DEFAULT;
    } else if (section == PREF_SECTION_DESKTOP) {
        if (point_in_rect(rects.grid_toggle, x, y))
            return HOVER_GRID_TOGGLE;
        if (point_in_rect(rects.seconds_toggle, x, y))
            return HOVER_SECONDS_TOGGLE;
    } else if (section == PREF_SECTION_SYSTEM) {
        if (point_in_rect(rects.terminal_toggle, x, y))
            return HOVER_TERMINAL_TOGGLE;
        if (point_in_rect(rects.storage_segment, x, y)) {
            int rel_x = x - rects.storage_segment.x;
            int third_w = rects.storage_segment.w / 3;
            if (rel_x < third_w)
                return HOVER_STORAGE_OFF;
            if (rel_x < third_w * 2)
                return HOVER_STORAGE_READ_ONLY;
            return HOVER_STORAGE_WRITABLE;
        }
    }

    return HOVER_NONE;
}

extern "C" int main()
{
    Surface win = gui_register_window_ex("Settings", (uint32_t)gui_scaled_metric(760), (uint32_t)gui_scaled_metric(460),
                                         WIN_FLAG_RESIZABLE);
    if (!win.buffer)
        return 1;
    gui_window_set_min_size(gui_scaled_metric(560), gui_scaled_metric(420));

    gui_sync_theme_from_registry();
    gui_request_focus();

    Registry *registry = gui_registry();
    PreferencesState state = {};
    PreferencesRects rects = {};
    load_preferences_state(&state, registry);
    gui_apply_theme(state.theme_mode);

    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;
    bool needs_redraw = true;
    bool redraw_full = true;
    Rect redraw_rect = gui_rect_make(0, 0, 0, 0);

    auto request_redraw = [&](bool full, Rect rect) {
        needs_redraw = true;
        if (full || gui_rect_is_empty(rect)) {
            redraw_full = true;
            redraw_rect = gui_rect_make(0, 0, 0, 0);
            return;
        }
        if (redraw_full)
            return;
        redraw_rect = merge_rects(redraw_rect, rect);
    };

    while (true) {
        Event ev = {};
        while (poll_event(&ev) > 0) {
            if (ev.type == EVT_WINDOW_CLOSE)
                return 0;
            if (ev.type == EVT_WINDOW_RESIZE && gui_sync_window_size(&win) > 0) {
                request_redraw(true, gui_rect_make(0, 0, 0, 0));
                continue;
            }
            if (ev.type == EVT_MOUSE_MOVE) {
                HoverTarget previous_hovered = state.hovered;
                HoverTarget hovered = update_hover_target(rects, state.section, ev.mouse.x, ev.mouse.y);
                if (hovered != state.hovered) {
                    state.hovered = hovered;
                    request_redraw(false, merge_rects(hover_target_rect(rects, state.section, previous_hovered),
                                                      hover_target_rect(rects, state.section, hovered)));
                }
                continue;
            }
            if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 1) {
                HoverTarget hovered = update_hover_target(rects, state.section, ev.mouse.x, ev.mouse.y);
                state.hovered = hovered;
                state.field_focused = (hovered == HOVER_WALLPAPER_FIELD);

                if (hovered == HOVER_NAV_APPEARANCE)
                    state.section = PREF_SECTION_APPEARANCE;
                else if (hovered == HOVER_NAV_DESKTOP)
                    state.section = PREF_SECTION_DESKTOP;
                else if (hovered == HOVER_NAV_SYSTEM)
                    state.section = PREF_SECTION_SYSTEM;
                else if (hovered == HOVER_THEME_DARK || hovered == HOVER_THEME_LIGHT) {
                    bool wallpaper_tracks_theme = wallpaper_is_default_family_path(state.wallpaper_path);
                    state.theme_mode = (hovered == HOVER_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
                    if (wallpaper_tracks_theme) {
                        strncpy(state.wallpaper_path, wallpaper_default_path_for_theme(state.theme_mode),
                                sizeof(state.wallpaper_path) - 1);
                        state.wallpaper_path[sizeof(state.wallpaper_path) - 1] = '\0';
                    }
                    apply_system_settings(&state, registry, "Theme updated", "Theme applied for this session");
                    gui_sync_theme_from_registry();
                } else if (hovered == HOVER_WALLPAPER_APPLY) {
                    apply_wallpaper(&state, registry, state.wallpaper_path);
                } else if (hovered == HOVER_WALLPAPER_DEFAULT) {
                    apply_wallpaper(&state, registry, wallpaper_default_path_for_theme(state.theme_mode));
                } else if (hovered == HOVER_GRID_TOGGLE) {
                    state.system_flags ^= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
                    apply_system_settings(&state, registry, "Desktop setting updated",
                                          "Desktop setting applied for this session");
                } else if (hovered == HOVER_SECONDS_TOGGLE) {
                    state.system_flags ^= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
                    apply_system_settings(&state, registry, "Clock setting updated",
                                          "Clock setting applied for this session");
                } else if (hovered == HOVER_TERMINAL_TOGGLE) {
                    state.system_flags ^= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
                    apply_system_settings(&state, registry, "Startup setting updated",
                                          "Startup setting applied for this session");
                } else if (hovered == HOVER_STORAGE_OFF || hovered == HOVER_STORAGE_READ_ONLY ||
                           hovered == HOVER_STORAGE_WRITABLE) {
                    int new_mode = hovered == HOVER_STORAGE_OFF        ? STORAGE_MODE_OFF
                                   : hovered == HOVER_STORAGE_WRITABLE ? STORAGE_MODE_WRITABLE
                                                                       : STORAGE_MODE_READ_ONLY;
                    if (request_storage_mode_change(registry, new_mode)) {
                        snprintf(state.status, sizeof(state.status), "Storage Mode update requested");
                    } else {
                        snprintf(state.status, sizeof(state.status), "Failed to update Storage Mode");
                    }
                }
                request_redraw(true, gui_rect_make(0, 0, 0, 0));
                continue;
            }
            if (ev.type == EVT_KEY_DOWN && state.field_focused && ev.key.c != 0) {
                char c = ev.key.c;
                size_t len = strlen(state.wallpaper_path);
                if (c == '\n' || c == '\r') {
                    apply_wallpaper(&state, registry, state.wallpaper_path);
                    request_redraw(true, gui_rect_make(0, 0, 0, 0));
                } else if ((c == '\b' || c == 127) && len > 0) {
                    state.wallpaper_path[len - 1] = '\0';
                    request_redraw(false, inflate_rect(rects.wallpaper_field, 1));
                } else if (c >= 32 && c <= 126 && len + 1 < sizeof(state.wallpaper_path)) {
                    state.wallpaper_path[len] = c;
                    state.wallpaper_path[len + 1] = '\0';
                    request_redraw(false, inflate_rect(rects.wallpaper_field, 1));
                }
            }
        }

        registry = gui_registry();
        int current_storage_mode = registry && registry->storage_mode <= STORAGE_MODE_WRITABLE
                                       ? (int)registry->storage_mode
                                       : get_storage_mode();
        if (current_storage_mode >= STORAGE_MODE_OFF && current_storage_mode <= STORAGE_MODE_WRITABLE &&
            current_storage_mode != state.storage_mode) {
            state.storage_mode = current_storage_mode;
            request_redraw(true, gui_rect_make(0, 0, 0, 0));
        }
        if (registry && registry->settings_generation != last_settings_generation) {
            last_settings_generation = registry->settings_generation;
            gui_sync_theme_from_registry();
            state.theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
            state.system_flags = registry->system_flags;
            if (registry->wallpaper_active[0]) {
                strncpy(state.wallpaper_path, registry->wallpaper_active, sizeof(state.wallpaper_path) - 1);
                state.wallpaper_path[sizeof(state.wallpaper_path) - 1] = '\0';
            }
            request_redraw(true, gui_rect_make(0, 0, 0, 0));
        }

        if (needs_redraw) {
            draw_preferences(&win, &state, &rects, redraw_full ? nullptr : &redraw_rect);
            needs_redraw = false;
            redraw_full = false;
            redraw_rect = gui_rect_make(0, 0, 0, 0);
        } else {
            sleep_ms(10);
        }
    }

    return 0;
}
