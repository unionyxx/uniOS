#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/event.h>

#include "../libgui/gui.h"

#ifdef __cplusplus
extern "C" {
#endif

// libapp: the application runtime. Owns the window, the double-buffered
// canvas, the event loop, resize/theme/menu/focus plumbing and damage
// publishing, so an app only supplies drawing and interaction logic.

typedef struct App App;

// Draw one full frame into the canvas (the private backbuffer). The runtime
// publishes only the invalidated regions to the compositor.
typedef void (*AppDrawFn)(App *app, Surface *canvas);

// Input and lifecycle events. EVT_WINDOW_CLOSE is handled by the runtime
// (clean exit); EVT_WINDOW_RESIZE arrives after the window and canvas were
// re-synced; EVT_WINDOW_SCROLL arrives after the canvas was invalidated.
typedef void (*AppEventFn)(App *app, const Event *ev);

// A menubar command was chosen (WindowEntry.menu_command_id).
typedef void (*AppMenuFn)(App *app, uint32_t command);

// Rebuild and publish the menubar model. Invoked at startup and whenever the
// window gains keyboard focus; call app_publish_menus to refresh it at other
// times (e.g. when selection or clipboard state changes).
typedef void (*AppMenusFn)(App *app);

// Fired after the shared registry settings generation changed and the theme
// was re-synced. Apps that mirror registry state (volume, flags, wallpaper)
// reload it here. May be NULL.
typedef void (*AppSettingsFn)(App *app);

// Called once per loop iteration after event/theme/menu plumbing, before
// pacing. For periodic checks (clock second ticks, storage-mode polling).
typedef void (*AppIdleFn)(App *app);

typedef struct AppConfig
{
    const char *title;
    int width, height;         // initial client size in pixels (caller scales)
    int min_width, min_height; // 0 leaves the limit unset
    uint32_t flags;            // WIN_FLAG_* (e.g. WIN_FLAG_RESIZABLE)

    // Frame pacing. 0 runs event-driven: the loop blocks lightly (idle_ms)
    // and only redraws when something was invalidated. >0 runs continuous:
    // on_draw executes every frame, paced to this many ticks (16 ~ 60 fps).
    uint64_t frame_ticks;
    uint32_t idle_ms; // event-driven idle sleep; 0 defaults to 16 ms

    AppDrawFn on_draw;         // required
    AppEventFn on_event;       // optional
    AppMenuFn on_menu;         // optional
    AppMenusFn on_menus;       // optional
    AppSettingsFn on_settings; // optional
    AppIdleFn on_idle;         // optional
} AppConfig;

// Run the app until the window is closed or app_exit is requested; returns
// the exit code. Never returns on registration failure (exits with 1).
int app_run(const AppConfig *config, void *user);

// Manual-loop mode for apps with extra event sources (pipes, timers):
// app_create registers the window and canvas, app_pump drains pending GUI
// events/theme/menu plumbing (returns false once close was requested),
// app_commit redraws and publishes damage when dirty, app_destroy cleans up.
App *app_create(const AppConfig *config, void *user);
bool app_pump(App *app);
void app_commit(App *app);
void app_destroy(App *app);

void app_exit(App *app, int code);

// Accessors.
void *app_user(App *app);
Surface *app_window(App *app); // shared window surface (compositor-visible)
Surface *app_canvas(App *app); // private backbuffer; draw target for on_draw
int app_view_w(App *app);      // visible client size (window, not content)
int app_view_h(App *app);
int app_scroll_x(App *app); // WM-driven scroll offset for content windows
int app_scroll_y(App *app);
bool app_focused(App *app);

// Redraw and damage. app_invalidate_all marks the whole canvas dirty;
// app_invalidate adds a rect (bounded list, collapses to full when it
// overflows). Continuous apps do not need either: every frame redraws.
void app_request_draw(App *app);
void app_invalidate(App *app, int x, int y, int w, int h);
void app_invalidate_all(App *app);
bool app_needs_draw(App *app);

// Scrollable-content windows: grow the window backing (and canvas) so the
// whole content is retained and the WM scrolls it. Call at the start of a
// frame, before drawing into the grown area.
int app_set_content_size(App *app, int content_w, int content_h);

void app_set_title(App *app, const char *title);
void app_publish_menus(App *app);
void app_set_frame_ticks(App *app, uint64_t frame_ticks);

// --- Standard Edit menu + keyboard shortcuts ---------------------------------
// Shared command IDs (below MENU_CMD_RESERVED_BASE) so every app's Edit menu
// and accelerators behave identically. Publish with app_menus_add_edit, then
// map Ctrl+<key> events with app_edit_shortcut and dispatch the result.

enum
{
    APP_CMD_UNDO = 0xE001,
    APP_CMD_REDO = 0xE002,
    APP_CMD_CUT = 0xE003,
    APP_CMD_COPY = 0xE004,
    APP_CMD_PASTE = 0xE005,
    APP_CMD_DELETE = 0xE006,
    APP_CMD_SELECT_ALL = 0xE007,
};

// Which standard entries an app implements at all (menu shows only these).
enum
{
    APP_EDIT_UNDO = 1 << 0,
    APP_EDIT_REDO = 1 << 1,
    APP_EDIT_CUT = 1 << 2,
    APP_EDIT_COPY = 1 << 3,
    APP_EDIT_PASTE = 1 << 4,
    APP_EDIT_DELETE = 1 << 5,
    APP_EDIT_SELECT_ALL = 1 << 6,
};

// Adds an "Edit" menu containing the entries set in `present`; entries not in
// `enabled` render disabled. Accelerator labels are filled in automatically.
// Returns the menu index, or -1 when the model is full.
int app_menus_add_edit(MenuModel *model, uint32_t present, uint32_t enabled);

// Adds the standard "Help" menu: an app tips entry (tips_cmd, omitted when 0)
// followed by the reserved About uniOS entry. Returns the menu index or -1.
int app_menus_add_help(MenuModel *model, uint32_t tips_cmd);

// Maps an EVT_KEY_DOWN Ctrl+letter to its APP_CMD_* (Ctrl+C -> APP_CMD_COPY,
// Ctrl+V -> APP_CMD_PASTE, ...). Returns 0 for anything else. Note: terminal
// must keep Ctrl+C for SIGINT and should not forward it here.
uint32_t app_edit_shortcut(const Event *ev);

// --- Persistent app settings --------------------------------------------------
// Per-app view toggles and preferences, stored in /data/APPS.CFG so app
// writes never race system settings. Keys are plain strings ("terminal_zoom",
// "files_sidebar", ...); values are integers.
int app_setting_load_int(const char *key, int fallback);
bool app_setting_save_int(const char *key, int value);

#ifdef __cplusplus
}
#endif
