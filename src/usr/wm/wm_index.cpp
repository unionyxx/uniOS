#include "wm_overlays.h"
#include "wm_window.h"
#include "wm_input.h"
#include "wm_damage.h"
#include "wm_present.h"
#include "wm_settings.h"
#include "wm_metrics.h"

IndexState g_index = {};

static void copy_cstr(char *dst, size_t dst_size, const char *src)
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

static inline char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c | 0x20) : c;
}

static bool ascii_starts_with_ci(const char *text, const char *query)
{
    if (!text || !query)
        return false;
    while (*query) {
        if (!*text || ascii_lower(*text) != ascii_lower(*query))
            return false;
        text++;
        query++;
    }
    return true;
}

static bool ascii_contains_ci(const char *text, const char *query)
{
    if (!text || !query)
        return false;
    if (!*query)
        return true;
    for (const char *p = text; *p; p++) {
        const char *h = p;
        const char *n = query;
        while (*h && *n && ascii_lower(*h) == ascii_lower(*n)) {
            h++;
            n++;
        }
        if (!*n)
            return true;
    }
    return false;
}

struct IndexCatalogEntry
{
    const char *title;
    const char *detail;
    const char *path;
    bool is_app;
    IndexActionKind action;
};

static const IndexCatalogEntry k_index_catalog[] = {
    {"Terminal", "Command-line shell", "/bin/terminal.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Files", "Browse files and volumes", "/bin/files.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Settings", "System settings", "/bin/preferences.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Latitude", "Project editor", "/bin/latitude.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"About uniOS", "System information", "/bin/about.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Control Panel", "Network, appearance, volume", "control", false, INDEX_ACTION_OPEN_CONTROL_PANEL},
    {"Storage Mode", "Choose storage access", "storage", false, INDEX_ACTION_OPEN_STORAGE_PROMPT},
    {"Show Desktop", "Hide open windows", "desktop", false, INDEX_ACTION_SHOW_DESKTOP},
    {"Toggle Dark Mode", "Switch appearance", "appearance", false, INDEX_ACTION_TOGGLE_THEME},
    {"Toggle Desktop Grid", "Show or hide grid lines", "desktop grid", false, INDEX_ACTION_TOGGLE_DESKTOP_GRID},
    {"Toggle Clock Seconds", "Show or hide clock seconds", "clock seconds", false, INDEX_ACTION_TOGGLE_CLOCK_SECONDS},
    {"Toggle Motion", "Turn window motion on or off", "animations", false, INDEX_ACTION_TOGGLE_ANIMATIONS},
    {"Toggle Transparency", "Use transparent or solid surfaces", "transparency", false,
     INDEX_ACTION_TOGGLE_TRANSPARENCY},
};

static constexpr int INDEX_CATALOG_COUNT = (int)(sizeof(k_index_catalog) / sizeof(k_index_catalog[0]));

static int index_catalog_count()
{
    return INDEX_CATALOG_COUNT;
}

static int score_index_entry(const IndexCatalogEntry &entry, const char *query, int query_len, int ordinal)
{
    if (query_len <= 0)
        return 1000 - ordinal;

    int score = 0;
    if (ascii_starts_with_ci(entry.title, query))
        score = 1200;
    else if (ascii_contains_ci(entry.title, query))
        score = 950;
    else if (ascii_contains_ci(entry.detail, query))
        score = 650;
    else if (ascii_contains_ci(entry.path, query))
        score = 500;
    if (score == 0)
        return 0;
    if (entry.is_app)
        score += 24;
    return score - ordinal;
}

static void set_index_result(IndexResult &dst, const IndexCatalogEntry &src, int score)
{
    copy_cstr(dst.title, sizeof(dst.title), src.title);
    copy_cstr(dst.detail, sizeof(dst.detail), src.detail);
    copy_cstr(dst.path, sizeof(dst.path), src.path);
    dst.is_app = src.is_app;
    dst.action = src.action;
    dst.score = score;
}

DirtyRect index_overlay_bounds()
{
    int margin = gui_space_2();
    int max_w = (int)g_screen.width - margin * 2;
    int max_h = (int)g_screen.height - wm_menubar_h() - margin * 2;
    int min_w = gui_scaled_metric(280);
    int min_h = gui_scaled_metric(220);
    int bw = gui_scaled_metric(640);
    int bh = gui_scaled_metric(432);
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
    int x = ((int)g_screen.width - bw) / 2;
    int y = wm_menubar_h() + gui_scaled_metric(36);
    int max_y = (int)g_screen.height - bh - margin;
    if (y > max_y)
        y = max_y;
    if (x < margin)
        x = margin;
    if (y < wm_menubar_h() + margin)
        y = wm_menubar_h() + margin;
    return {x, y, bw, bh};
}

static DirtyRect index_damage_bounds()
{
    return rect_expand(index_overlay_bounds(), gui_scaled_metric(14));
}

static int index_result_item_h()
{
    int h = gui_scaled_metric(52);
    return h < gui_scaled_metric(40) ? gui_scaled_metric(40) : h;
}

static DirtyRect index_search_bounds()
{
    DirtyRect box = index_overlay_bounds();
    int pad = gui_space_2();
    int h = gui_scaled_metric(44);
    return {box.x + pad, box.y + pad, box.w - pad * 2, h};
}

static int index_results_start_y()
{
    DirtyRect search = index_search_bounds();
    return search.y + search.h + gui_space_1();
}

static int index_result_at(int mouse_x, int mouse_y)
{
    if (!g_index.active || !point_in_rect(index_overlay_bounds(), mouse_x, mouse_y))
        return -1;
    int y = index_results_start_y();
    int h = index_result_item_h();
    int pad = gui_space_2();
    DirtyRect box = index_overlay_bounds();
    int bottom = box.y + box.h - pad;
    for (int i = 0; i < g_index.result_count; i++) {
        DirtyRect row = {box.x + pad, y, box.w - pad * 2, h};
        if (row.y + row.h > bottom)
            break;
        if (point_in_rect(row, mouse_x, mouse_y))
            return i;
        y += h + gui_scaled_metric(2);
    }
    return -1;
}

void update_index_search()
{
    IndexResult sorted[INDEX_CATALOG_COUNT];
    int sorted_count = 0;

    for (int i = 0; i < index_catalog_count(); i++) {
        int score = score_index_entry(k_index_catalog[i], g_index.query, g_index.query_len, i);
        if (score <= 0)
            continue;
        IndexResult candidate = {};
        set_index_result(candidate, k_index_catalog[i], score);

        int insert_at = sorted_count;
        while (insert_at > 0 && sorted[insert_at - 1].score < candidate.score) {
            if (insert_at < INDEX_MAX_RESULTS)
                sorted[insert_at] = sorted[insert_at - 1];
            insert_at--;
        }
        if (insert_at < INDEX_MAX_RESULTS)
            sorted[insert_at] = candidate;
        if (sorted_count < INDEX_MAX_RESULTS)
            sorted_count++;
    }

    g_index.result_count = sorted_count;
    for (int i = 0; i < g_index.result_count; i++)
        g_index.results[i] = sorted[i];
    if (g_index.result_count <= 0) {
        g_index.selected_index = -1;
        g_index.hovered_index = -1;
    } else {
        if (g_index.selected_index < 0 || g_index.selected_index >= g_index.result_count)
            g_index.selected_index = 0;
        if (g_index.hovered_index >= g_index.result_count)
            g_index.hovered_index = -1;
    }

    DirtyRect damage = index_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

void open_index()
{
    if (g_index.active)
        return;
    close_control_center();
    close_context_menu();
    g_index = {};
    g_index.active = true;
    g_index.selected_index = 0;
    g_index.hovered_index = -1;
    g_index.open_ticks = get_ticks();
    update_index_search();
}

void close_index()
{
    if (!g_index.active)
        return;
    DirtyRect damage = index_damage_bounds();
    g_index.active = false;
    g_index.hovered_index = -1;
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

bool activate_index_selection(Registry *registry)
{
    if (!g_index.active || g_index.selected_index < 0 || g_index.selected_index >= g_index.result_count)
        return false;

    IndexResult chosen = g_index.results[g_index.selected_index];
    close_index();

    switch (chosen.action) {
        case INDEX_ACTION_LAUNCH_APP:
            launch_or_focus_app(registry, chosen.title, chosen.path);
            return true;
        case INDEX_ACTION_OPEN_CONTROL_PANEL:
            toggle_control_center();
            return true;
        case INDEX_ACTION_OPEN_STORAGE_PROMPT:
            open_storage_prompt();
            return true;
        case INDEX_ACTION_SHOW_DESKTOP:
            show_desktop_windows();
            return true;
        case INDEX_ACTION_TOGGLE_THEME:
            if (registry) {
                registry->theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_DARK : GUI_THEME_LIGHT;
                publish_settings_changed(registry);
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
            return true;
        case INDEX_ACTION_TOGGLE_DESKTOP_GRID:
            if (registry) {
                registry->system_flags ^= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
                g_system_flags = registry->system_flags;
                publish_settings_changed(registry);
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, wm_menubar_h());
            if (registry && registry->window_count > 1)
                enqueue_damage_rect(registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                                    registry->windows[1].h);
            return true;
        case INDEX_ACTION_TOGGLE_CLOCK_SECONDS:
            if (registry) {
                registry->system_flags ^= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
                g_system_flags = registry->system_flags;
                publish_settings_changed(registry);
                persist_wm_settings();
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, wm_menubar_h());
            return true;
        case INDEX_ACTION_TOGGLE_ANIMATIONS:
            g_control_center.animations_enabled = !g_control_center.animations_enabled;
            if (registry) {
                registry->animations_enabled = g_control_center.animations_enabled;
                publish_settings_changed(registry);
            } else {
                persist_wm_settings();
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
            return true;
        case INDEX_ACTION_TOGGLE_TRANSPARENCY:
            g_control_center.transparency_level = (g_control_center.transparency_level > 200) ? 180 : 255;
            if (registry) {
                registry->transparency_level = g_control_center.transparency_level;
                publish_settings_changed(registry);
            } else {
                persist_wm_settings();
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
            return true;
        default:
            return false;
    }
}

bool handle_index_pointer_down(Registry *registry, int mouse_x, int mouse_y)
{
    if (!g_index.active)
        return false;
    if (!point_in_rect(index_overlay_bounds(), mouse_x, mouse_y))
        return false;
    int hit = index_result_at(mouse_x, mouse_y);
    if (hit >= 0) {
        g_index.selected_index = hit;
        DirtyRect damage = index_damage_bounds();
        enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
        activate_index_selection(registry);
    }
    return true;
}

void update_index_hover(int mouse_x, int mouse_y)
{
    if (!g_index.active)
        return;
    int hit = index_result_at(mouse_x, mouse_y);
    if (hit == g_index.hovered_index)
        return;
    g_index.hovered_index = hit;
    if (hit >= 0)
        g_index.selected_index = hit;
    DirtyRect damage = index_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

static int wm_index_result_item_h()
{
    return gui_app_row_tall_h();
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

    int radius = gui_radius_xl();

    gui_draw_panel_shadow(&g_backbuffer, box.x, box.y, box.w, box.h, radius);

    gui_draw_chrome_frame(&g_backbuffer, box.x, box.y, box.w, box.h, radius, g_gui_style.app_surface, true);

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
    int row_gap = gui_app_row_gap();
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

