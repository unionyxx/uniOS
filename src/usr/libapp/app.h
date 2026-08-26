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

#ifdef __cplusplus
}
#endif
