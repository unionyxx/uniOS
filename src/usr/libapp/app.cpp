#include "app.h"

#include <stdlib.h>
#include <string.h>

#include "../libc/config_utils.h"
#include "../libc/log.h"
#include "../libc/unistd.h"

#define APP_DIRTY_MAX 32
#define APP_DEFAULT_IDLE_MS 16

struct App
{
    AppConfig config;
    void *user;
    Surface window;
    Surface canvas;
    size_t canvas_capacity; // bytes
    bool focused;
    bool exit_requested;
    int exit_code;
    bool last_pump_active;
#ifdef DEBUG
    bool first_frame_logged;
#endif
    bool dirty_full;
    int dirty_count;
    Rect dirty_rects[APP_DIRTY_MAX];
    uint32_t last_settings_generation;
    uint64_t next_frame_ticks;
};

static void app_mark_dirty_rect(App *app, Rect rect);

static uint32_t app_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

// Mirror the canvas Surface fields onto the window surface and grow the
// backing allocation when the window backing outgrew it. Existing pixels are
// preserved across growth (row-wise copy, pitch-aware).
static bool app_sync_canvas(App *app)
{
    Surface *w = &app->window;
    Surface *c = &app->canvas;
    if (!w->buffer || w->pitch == 0)
        return false;

    size_t needed = (size_t)(w->pitch / 4) * w->height * sizeof(uint32_t);
    if (needed > app->canvas_capacity) {
        uint32_t *grown = (uint32_t *)malloc(needed);
        if (!grown)
            return false;
        if (c->buffer && c->pitch > 0) {
            uint32_t rows = app_min_u32(c->height, w->height);
            uint32_t row_px = app_min_u32(c->pitch / 4, w->pitch / 4);
            uint32_t new_stride = w->pitch / 4;
            uint32_t old_stride = c->pitch / 4;
            for (uint32_t y = 0; y < rows; y++)
                memcpy(grown + (size_t)y * new_stride, c->buffer + (size_t)y * old_stride, row_px * sizeof(uint32_t));
        }
        free(c->buffer);
        c->buffer = grown;
        app->canvas_capacity = needed;
    }
    c->width = w->width;
    c->height = w->height;
    c->pitch = w->pitch;
    c->owns_buffer = false;
    return true;
}

static void app_copy_rect_to_window(App *app, int x, int y, int w, int h)
{
    Surface *win = &app->window;
    Surface *cv = &app->canvas;
    if (!win->buffer || !cv->buffer)
        return;
    if (x < 0 || y < 0 || w <= 0 || h <= 0)
        return;
    if (x + w > (int)cv->width || y + h > (int)cv->height)
        return;
    if (x + w > (int)win->width || y + h > (int)win->height)
        return;
    uint32_t ws = win->pitch / 4;
    uint32_t cs = cv->pitch / 4;
    for (int row = 0; row < h; row++)
        memcpy(&win->buffer[(size_t)(y + row) * ws + x], &cv->buffer[(size_t)(y + row) * cs + x],
               (size_t)w * sizeof(uint32_t));
}

App *app_create(const AppConfig *config, void *user)
{
    if (!config || !config->title || config->width <= 0 || config->height <= 0)
        return nullptr;

    App *app = (App *)malloc(sizeof(App));
    if (!app)
        return nullptr;
    memset(app, 0, sizeof(*app));
    app->config = *config;
    app->user = user;
    app->exit_code = 0;

    app->window =
        gui_register_window_ex(config->title, (uint32_t)config->width, (uint32_t)config->height, config->flags);
    if (!app->window.buffer) {
        LOG_ERROR("app", "window registration failed: %s", config->title);
        free(app);
        return nullptr;
    }
    if (config->min_width > 0 && config->min_height > 0)
        gui_window_set_min_size(config->min_width, config->min_height);

    gui_sync_theme_from_registry();
    gui_request_focus();

    Registry *registry = gui_registry();
    app->last_settings_generation = registry ? registry->settings_generation : 0;

    if (config->on_draw && !app_sync_canvas(app)) {
        LOG_ERROR("app", "canvas allocation failed: %s", config->title);
        free(app);
        return nullptr;
    }

    if (config->on_menus)
        config->on_menus(app);

    app->next_frame_ticks = get_ticks() + (config->frame_ticks ? config->frame_ticks : APP_DEFAULT_IDLE_MS);
    return app;
}

void app_destroy(App *app)
{
    if (!app)
        return;
    free(app->canvas.buffer);
    free(app);
}

void app_exit(App *app, int code)
{
    if (!app)
        return;
    app->exit_requested = true;
    app->exit_code = code;
}

void *app_user(App *app)
{
    return app ? app->user : nullptr;
}

Surface *app_window(App *app)
{
    return app ? &app->window : nullptr;
}

Surface *app_canvas(App *app)
{
    return app ? &app->canvas : nullptr;
}

int app_view_w(App *app)
{
    (void)app;
    return (g_my_window && g_my_window->w > 0) ? g_my_window->w : 0;
}

int app_view_h(App *app)
{
    (void)app;
    return (g_my_window && g_my_window->h > 0) ? g_my_window->h : 0;
}

int app_scroll_x(App *app)
{
    (void)app;
    return g_my_window ? g_my_window->scroll_x : 0;
}

int app_scroll_y(App *app)
{
    (void)app;
    return g_my_window ? g_my_window->scroll_y : 0;
}

bool app_focused(App *app)
{
    return app ? app->focused : false;
}

void app_request_draw(App *app)
{
    if (app)
        app->dirty_full = true;
}

void app_invalidate_all(App *app)
{
    if (app)
        app->dirty_full = true;
}

static void app_mark_dirty_rect(App *app, Rect rect)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    if (app->dirty_full)
        return;
    if (app->dirty_count >= APP_DIRTY_MAX) {
        app->dirty_full = true;
        app->dirty_count = 0;
        return;
    }
    app->dirty_rects[app->dirty_count++] = rect;
}

void app_invalidate(App *app, int x, int y, int w, int h)
{
    if (!app)
        return;
    app_mark_dirty_rect(app, gui_rect_make(x, y, w, h));
}

bool app_needs_draw(App *app)
{
    return app && (app->dirty_full || app->dirty_count > 0);
}

int app_set_content_size(App *app, int content_w, int content_h)
{
    if (!app)
        return -1;
    int rc = gui_set_content_size(&app->window, content_w, content_h);
    if (rc < 0)
        return rc;
    if (!app->config.on_draw)
        return rc;
    return app_sync_canvas(app) ? rc : -1;
}

void app_set_title(App *app, const char *title)
{
    (void)app;
    gui_set_window_title(title);
}

void app_publish_menus(App *app)
{
    if (app && app->config.on_menus)
        app->config.on_menus(app);
}

void app_set_frame_ticks(App *app, uint64_t frame_ticks)
{
    if (!app)
        return;
    app->config.frame_ticks = frame_ticks;
}

// Publish whatever is currently dirty; the caller decides when on_draw runs.
static void app_publish(App *app)
{
    bool full = app->dirty_full;
    int rect_count = app->dirty_count;
    Rect rects[APP_DIRTY_MAX];
    for (int i = 0; i < rect_count; i++)
        rects[i] = app->dirty_rects[i];
    app->dirty_full = false;
    app->dirty_count = 0;

    if (!app->canvas.buffer || !app->window.buffer)
        return;

    auto log_first_frame = [&]() {
#ifdef DEBUG
        if (!app->first_frame_logged) {
            app->first_frame_logged = true;
            LOG_INFO("app", "%s submitted first frame", app->config.title ? app->config.title : "app");
        }
#else
        (void)app;
#endif
    };

    if (full) {
        app_copy_rect_to_window(app, 0, 0, (int)app->canvas.width, (int)app->canvas.height);
        gui_blit_to_screen_rect(&app->window, 0, 0, (int)app->canvas.width, (int)app->canvas.height);
        log_first_frame();
        return;
    }
    for (int i = 0; i < rect_count; i++) {
        Rect r = rects[i];
        if (r.x < 0) {
            r.w += r.x;
            r.x = 0;
        }
        if (r.y < 0) {
            r.h += r.y;
            r.y = 0;
        }
        if (r.x + r.w > (int)app->canvas.width)
            r.w = (int)app->canvas.width - r.x;
        if (r.y + r.h > (int)app->canvas.height)
            r.h = (int)app->canvas.height - r.y;
        if (r.w <= 0 || r.h <= 0)
            continue;
        app_copy_rect_to_window(app, r.x, r.y, r.w, r.h);
        gui_blit_to_screen_rect(&app->window, r.x, r.y, r.w, r.h);
    }
    if (rect_count > 0)
        log_first_frame();
}

void app_commit(App *app)
{
    if (!app || !app_needs_draw(app))
        return;
    if (app->config.on_draw && app->canvas.buffer)
        app->config.on_draw(app, &app->canvas);
    app_publish(app);
}

// Drain all pending GUI events; returns false when the window was closed.
static bool app_drain_events(App *app)
{
    Event ev = {};
    while (poll_event(&ev) > 0) {
        app->last_pump_active = true;
        switch (ev.type) {
            case EVT_WINDOW_CLOSE:
                app_exit(app, 0);
                return false;

            case EVT_WINDOW_RESIZE:
                if (gui_sync_window_size(&app->window) > 0) {
                    if (app->config.on_draw)
                        app_sync_canvas(app);
                    app_invalidate_all(app);
                    if (app->config.on_event)
                        app->config.on_event(app, &ev);
                }
                break;

            case EVT_WINDOW_SCROLL:
                app_invalidate_all(app);
                if (app->config.on_event)
                    app->config.on_event(app, &ev);
                break;

            case EVT_FOCUS:
                app->focused = true;
                app_publish_menus(app);
                app_invalidate_all(app);
                if (app->config.on_event)
                    app->config.on_event(app, &ev);
                break;

            case EVT_UNFOCUS:
                app->focused = false;
                app_invalidate_all(app);
                if (app->config.on_event)
                    app->config.on_event(app, &ev);
                break;

            default:
                if (app->config.on_event)
                    app->config.on_event(app, &ev);
                break;
        }
        if (app->exit_requested)
            return false;
    }
    return true;
}

static bool app_sync_settings(App *app)
{
    Registry *registry = gui_registry();
    if (!registry || registry->settings_generation == app->last_settings_generation)
        return false;
    app->last_settings_generation = registry->settings_generation;
    if (gui_sync_theme_from_registry())
        app_invalidate_all(app);
    if (app->config.on_settings)
        app->config.on_settings(app);
    return true;
}

static bool app_drain_menu_commands(App *app)
{
    uint32_t cmd = 0;
    bool any = false;
    while (gui_menu_take_command(&cmd)) {
        any = true;
        if (app->config.on_menu)
            app->config.on_menu(app, cmd);
    }
    return any;
}

bool app_pump(App *app)
{
    if (!app)
        return false;
    app->last_pump_active = false;
    if (!app_drain_events(app))
        return false;
    if (app_sync_settings(app))
        app->last_pump_active = true;
    if (app_drain_menu_commands(app))
        app->last_pump_active = true;
    if (app->config.on_idle)
        app->config.on_idle(app);
    return !app->exit_requested;
}

int app_run(const AppConfig *config, void *user)
{
    App *app = app_create(config, user);
    if (!app)
        return 1;

    app_invalidate_all(app);
    app_commit(app);

    uint32_t idle_ms = config->idle_ms ? config->idle_ms : APP_DEFAULT_IDLE_MS;

    while (!app->exit_requested) {
        if (!app_pump(app))
            break;

        if (app->config.frame_ticks > 0) {
            // Continuous mode: draw every frame; the app decides what to
            // publish by invalidating rects during on_draw.
            if (app->config.on_draw && app->canvas.buffer)
                app->config.on_draw(app, &app->canvas);
            app_publish(app);
            sleep_until_ticks(app->next_frame_ticks);
            uint64_t now = get_ticks();
            app->next_frame_ticks += app->config.frame_ticks;
            if (now > app->next_frame_ticks)
                app->next_frame_ticks = now + app->config.frame_ticks;
        } else if (app_needs_draw(app)) {
            app_commit(app);
        } else if (!app->last_pump_active) {
            sleep_ms(idle_ms);
        }
    }

    int code = app->exit_code;
    app_destroy(app);
    return code;
}

// --- Standard Edit menu + keyboard shortcuts ---------------------------------

static bool app_menu_add(MenuModel *model, int menu, const char *label, uint32_t id, bool enabled, const char *accel)
{
    return gui_menu_model_add_item(model, menu, label, id, enabled ? 0 : MENU_FLAG_DISABLED, accel);
}

int app_menus_add_edit(MenuModel *model, uint32_t present, uint32_t enabled)
{
    if (!model || present == 0)
        return -1;

    int menu = gui_menu_model_add_menu(model, "Edit");
    if (menu < 0)
        return -1;

    if (present & APP_EDIT_UNDO)
        app_menu_add(model, menu, "Undo", APP_CMD_UNDO, enabled & APP_EDIT_UNDO, "Ctrl+Z");
    if (present & APP_EDIT_REDO)
        app_menu_add(model, menu, "Redo", APP_CMD_REDO, enabled & APP_EDIT_REDO, "Ctrl+Y");
    if ((present & (APP_EDIT_UNDO | APP_EDIT_REDO)) && (present & (APP_EDIT_CUT | APP_EDIT_COPY | APP_EDIT_PASTE)))
        gui_menu_model_add_separator(model, menu);

    if (present & APP_EDIT_CUT)
        app_menu_add(model, menu, "Cut", APP_CMD_CUT, enabled & APP_EDIT_CUT, "Ctrl+X");
    if (present & APP_EDIT_COPY)
        app_menu_add(model, menu, "Copy", APP_CMD_COPY, enabled & APP_EDIT_COPY, "Ctrl+C");
    if (present & APP_EDIT_PASTE)
        app_menu_add(model, menu, "Paste", APP_CMD_PASTE, enabled & APP_EDIT_PASTE, "Ctrl+V");
    if ((present & (APP_EDIT_CUT | APP_EDIT_COPY | APP_EDIT_PASTE)) &&
        (present & (APP_EDIT_DELETE | APP_EDIT_SELECT_ALL)))
        gui_menu_model_add_separator(model, menu);

    if (present & APP_EDIT_DELETE)
        app_menu_add(model, menu, "Delete", APP_CMD_DELETE, enabled & APP_EDIT_DELETE, nullptr);
    if (present & APP_EDIT_SELECT_ALL)
        app_menu_add(model, menu, "Select All", APP_CMD_SELECT_ALL, enabled & APP_EDIT_SELECT_ALL, "Ctrl+A");

    return menu;
}

int app_menus_add_help(MenuModel *model, uint32_t tips_cmd)
{
    if (!model)
        return -1;

    int menu = gui_menu_model_add_menu(model, "Help");
    if (menu < 0)
        return -1;

    if (tips_cmd != 0) {
        gui_menu_model_add_item(model, menu, "Tips", tips_cmd, 0, nullptr);
        gui_menu_model_add_separator(model, menu);
    }
    gui_menu_model_add_item(model, menu, "About uniOS", MENU_CMD_ABOUT_UNIOS, 0, nullptr);
    return menu;
}

uint32_t app_edit_shortcut(const Event *ev)
{
    if (!ev || ev->type != EVT_KEY_DOWN)
        return 0;

    unsigned char c = static_cast<unsigned char>(ev->key.c);
    // Ctrl+letter arrives as the control character 1..26.
    if (c < 1 || c > 26)
        return 0;

    switch (c) {
        case 26: // Ctrl+Z
            return APP_CMD_UNDO;
        case 25: // Ctrl+Y
            return APP_CMD_REDO;
        case 24: // Ctrl+X
            return APP_CMD_CUT;
        case 3: // Ctrl+C
            return APP_CMD_COPY;
        case 22: // Ctrl+V
            return APP_CMD_PASTE;
        case 1: // Ctrl+A
            return APP_CMD_SELECT_ALL;
        default:
            return 0;
    }
}

// --- Persistent app settings --------------------------------------------------

int app_setting_load_int(const char *key, int fallback)
{
    if (!key)
        return fallback;
    return cfg_load_int(APP_SETTINGS_CONFIG_PATH, key, fallback);
}

bool app_setting_save_int(const char *key, int value)
{
    if (!key)
        return false;
    return cfg_save_int(APP_SETTINGS_CONFIG_PATH, key, value);
}
