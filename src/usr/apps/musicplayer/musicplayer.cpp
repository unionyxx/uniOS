#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>

#include "../../libapp/app.h"
#include "../../libc/log.h"
#include "../../libc/unistd.h"

namespace {

enum PlayerStatus
{
    PLAYER_EMPTY,
    PLAYER_ERROR,
    PLAYER_LOADED,
};

struct PlayerState
{
    char open_path[256];
    PlayerStatus status;
    bool initialized;
};

void draw_centered_text(Surface *s, const GuiFont *font, int y, const char *text, uint32_t fg)
{
    int text_w = gui_measure_text(font, text);
    int x = (static_cast<int>(s->width) - text_w) / 2;
    if (x < 0)
        x = 0;
    gui_draw_text_clipped(s, font, x, y, static_cast<int>(s->width), text, fg, g_gui_style.app_bg);
}

void draw_player(Surface *s)
{
    gui_fill_surface(s, g_gui_style.app_bg);

    // The empty state is centered in the area below the unified headerbar band.
    int header_h = gui_headerbar_h();
    int view_h = static_cast<int>(s->height) - header_h;

    int y = header_h + (view_h - gui_font_line_height(gui_font_title()) - gui_line_height() * 2) / 2;
    if (y < header_h)
        y = header_h;
    draw_centered_text(s, gui_font_title(), y, "No track open", g_gui_style.text);
    draw_centered_text(s, gui_font_default(), y + gui_font_line_height(gui_font_title()) + gui_space_1(),
                       "Open audio from Files, or play the demo from /Music.", g_gui_style.text_muted);
}

void musicplayer_draw(App *app, Surface *canvas)
{
    PlayerState *st = (PlayerState *)app_user(app);
    if (!st->initialized) {
        st->initialized = true;
        if (st->open_path[0])
            LOG_INFO("musicplayer", "open request: %s", st->open_path);
        else
            LOG_INFO("musicplayer", "ready");
    }
    draw_player(canvas);
}

void musicplayer_menus(App *app)
{
    (void)app;
    MenuModel model;
    gui_menu_model_reset(&model);
    app_menus_add_help(&model, 0);
    gui_menu_publish(&model);
}

} // namespace

extern "C" int main()
{
    static PlayerState st = {};
    if (gui_open_request_take(st.open_path, sizeof(st.open_path)))
        st.open_path[sizeof(st.open_path) - 1] = '\0';
    else
        st.open_path[0] = '\0';

    AppConfig config = {};
    config.title = "Music";
    config.width = gui_scaled_metric(520);
    config.height = gui_scaled_metric(420);
    config.min_width = gui_scaled_metric(320);
    config.min_height = gui_scaled_metric(240);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 33;
    config.on_draw = musicplayer_draw;
    config.on_menus = musicplayer_menus;

    return app_run(&config, &st);
}
