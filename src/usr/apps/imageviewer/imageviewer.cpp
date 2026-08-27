#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>

#include "../../libapp/app.h"
#include "../../libc/log.h"
#include "../../libc/unistd.h"
#include "../../libmedia/media_image.h"

namespace {

enum ViewerStatus
{
    VIEWER_EMPTY,
    VIEWER_ERROR,
    VIEWER_LOADED,
};

struct ViewerState
{
    media_image image;
    media_image scaled;
    int scaled_w, scaled_h;
    char path[256];
    char open_path[256];
    ViewerStatus status;
    bool initialized;
};

const char *base_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            name = p + 1;
    }
    return name;
}

void viewer_update_title(const ViewerState *st)
{
    char title[64];
    if (st->status == VIEWER_LOADED) {
        snprintf(title, sizeof(title), "%s - %dx%d - Image Viewer", base_name(st->path), (int)st->image.width,
                 (int)st->image.height);
    } else {
        snprintf(title, sizeof(title), "Image Viewer");
    }
    gui_set_window_title(title);
}

void viewer_reset(ViewerState *st)
{
    media_image_free(&st->image);
    media_image_free(&st->scaled);
    st->scaled_w = 0;
    st->scaled_h = 0;
    st->path[0] = '\0';
    st->status = VIEWER_EMPTY;
}

bool viewer_load(ViewerState *st, const char *path)
{
    viewer_reset(st);
    strncpy(st->path, path, sizeof(st->path) - 1);
    st->path[sizeof(st->path) - 1] = '\0';

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!gui_load_file(path, &data, &size)) {
        LOG_INFO("imageviewer", "load failed: %s", path);
        st->status = VIEWER_ERROR;
        viewer_update_title(st);
        return false;
    }
    bool ok = media_image_decode(data, size, &st->image);
    free(data);
    if (!ok) {
        LOG_INFO("imageviewer", "decode failed: %s", path);
        st->status = VIEWER_ERROR;
        viewer_update_title(st);
        return false;
    }
    st->status = VIEWER_LOADED;
    viewer_update_title(st);
    return true;
}

// Fit the image into the view, preserving aspect ratio; upscale is allowed so
// small images remain visible.
void compute_fit(int img_w, int img_h, int view_w, int view_h, int *out_w, int *out_h)
{
    *out_w = 0;
    *out_h = 0;
    if (img_w <= 0 || img_h <= 0 || view_w <= 0 || view_h <= 0)
        return;
    int64_t sx = (static_cast<int64_t>(view_w) << 16) / img_w;
    int64_t sy = (static_cast<int64_t>(view_h) << 16) / img_h;
    int64_t scale = sx < sy ? sx : sy;
    int dw = static_cast<int>((static_cast<int64_t>(img_w) * scale + 32768) >> 16);
    int dh = static_cast<int>((static_cast<int64_t>(img_h) * scale + 32768) >> 16);
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;
    if (dw > view_w)
        dw = view_w;
    if (dh > view_h)
        dh = view_h;
    *out_w = dw;
    *out_h = dh;
}

// Refresh the cached display-size copy when the fit size changed.
void viewer_ensure_scaled(ViewerState *st, int dw, int dh)
{
    if (st->scaled.pixels && st->scaled_w == dw && st->scaled_h == dh)
        return;
    media_image_free(&st->scaled);
    st->scaled_w = dw;
    st->scaled_h = dh;
    if (!media_image_scale(&st->image, &st->scaled, dw, dh)) {
        LOG_INFO("imageviewer", "scale to %dx%d failed, blitting native", dw, dh);
        st->scaled_w = 0;
        st->scaled_h = 0;
    }
}

void draw_checkerboard(Surface *s, int x, int y, int w, int h)
{
    int square = gui_scaled_metric(8);
    if (square < 2)
        square = 2;
    for (int row = 0; row < h; row += square) {
        int sh = square < h - row ? square : h - row;
        for (int col = 0; col < w; col += square) {
            int sw = square < w - col ? square : w - col;
            uint32_t color =
                (((row / square) + (col / square)) & 1) ? g_gui_style.app_surface_alt : g_gui_style.app_surface;
            gui_fill_rect(s, x + col, y + row, sw, sh, color);
        }
    }
}

void blend_image_onto(Surface *s, int x0, int y0, const media_image *img)
{
    if (!img->pixels)
        return;
    uint32_t stride = s->pitch / 4;
    for (int y = 0; y < img->height; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= static_cast<int>(s->height))
            continue;
        const uint32_t *src = img->pixels + static_cast<uint64_t>(y) * img->width;
        uint32_t *dst = s->buffer + static_cast<uint64_t>(dy) * stride;
        for (int x = 0; x < img->width; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= static_cast<int>(s->width))
                continue;
            uint32_t p = src[x];
            uint32_t a = p >> 24;
            if (a == 255) {
                dst[dx] = p;
                continue;
            }
            if (a == 0)
                continue;
            uint32_t d = dst[dx];
            uint32_t ia = 255 - a;
            uint32_t r = (((p >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia + 127) / 255;
            uint32_t g = (((p >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia + 127) / 255;
            uint32_t b = ((p & 0xFF) * a + (d & 0xFF) * ia + 127) / 255;
            dst[dx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void draw_centered_text(Surface *s, const GuiFont *font, int y, const char *text, uint32_t fg)
{
    int text_w = gui_measure_text(font, text);
    int x = (static_cast<int>(s->width) - text_w) / 2;
    if (x < 0)
        x = 0;
    gui_draw_text_clipped(s, font, x, y, static_cast<int>(s->width), text, fg, g_gui_style.app_bg);
}

void draw_viewer(Surface *s, ViewerState *st)
{
    gui_fill_surface(s, g_gui_style.app_bg);

    if (st->status == VIEWER_EMPTY) {
        int y = (static_cast<int>(s->height) - gui_font_line_height(gui_font_title()) - gui_line_height() * 2) / 2;
        if (y < 0)
            y = 0;
        draw_centered_text(s, gui_font_title(), y, "No image open", g_gui_style.text);
        draw_centered_text(s, gui_font_default(), y + gui_font_line_height(gui_font_title()) + gui_space_1(),
                           "Open an image from Files.", g_gui_style.text_muted);
        return;
    }

    if (st->status == VIEWER_ERROR) {
        int y = (static_cast<int>(s->height) - gui_font_line_height(gui_font_title()) - gui_line_height() * 2) / 2;
        if (y < 0)
            y = 0;
        draw_centered_text(s, gui_font_title(), y, "Could not load image", g_gui_style.text);
        draw_centered_text(s, gui_font_default(), y + gui_font_line_height(gui_font_title()) + gui_space_1(),
                           base_name(st->path), g_gui_style.text_muted);
        return;
    }

    int view_w = static_cast<int>(s->width);
    int view_h = static_cast<int>(s->height);
    int dw = 0, dh = 0;
    compute_fit(st->image.width, st->image.height, view_w, view_h, &dw, &dh);
    if (dw <= 0 || dh <= 0)
        return;

    viewer_ensure_scaled(st, dw, dh);
    const media_image *show = st->scaled.pixels ? &st->scaled : &st->image;
    int x0 = (view_w - show->width) / 2;
    int y0 = (view_h - show->height) / 2;

    draw_checkerboard(s, x0, y0, show->width, show->height);
    blend_image_onto(s, x0, y0, show);
}

void imageviewer_draw(App *app, Surface *canvas)
{
    ViewerState *st = (ViewerState *)app_user(app);
    if (!st->initialized) {
        st->initialized = true;
        if (st->open_path[0])
            viewer_load(st, st->open_path);
        else
            viewer_update_title(st);
    }
    draw_viewer(canvas, st);
}

void imageviewer_menus(App *app)
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
    static ViewerState st = {};
    if (gui_open_request_take(st.open_path, sizeof(st.open_path)))
        st.open_path[sizeof(st.open_path) - 1] = '\0';
    else
        st.open_path[0] = '\0';

    AppConfig config = {};
    config.title = "Image Viewer";
    config.width = gui_scaled_metric(860);
    config.height = gui_scaled_metric(560);
    config.min_width = gui_scaled_metric(320);
    config.min_height = gui_scaled_metric(240);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 33;
    config.on_draw = imageviewer_draw;
    config.on_menus = imageviewer_menus;

    return app_run(&config, &st);
}
