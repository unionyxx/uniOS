#pragma once
#include <stddef.h>
#include <stdint.h>
#include <uapi/gui.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    bool owns_buffer;
    DisplayBufferHandle display_handle;
    // Padded allocation footprint (see gui_create_surface / resize capacity
    // users): width/height carry the caller's current logical size, capacity
    // carries how big the buffer actually is.
    uint32_t capacity_w;
    uint32_t capacity_h;
} Surface;

typedef struct
{
    uint32_t codepoint;
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t width;
    uint16_t height;
    int16_t bearing_x;
    int16_t bearing_y;
    int16_t advance_x;
    int16_t reserved;
} GuiGlyph;

typedef struct
{
    uint32_t magic;
    uint32_t glyph_count;
    uint32_t fallback_index;
    uint16_t pixel_size;
    uint16_t atlas_width;
    uint16_t atlas_height;
    int16_t ascent;
    int16_t descent;
    int16_t line_gap;
    int16_t line_height;
    int16_t max_advance;
    int16_t max_ink_width;
    int16_t ascii_index[128];
    int16_t ascii_advance[128];
    uint8_t alpha_lut[256];
    GuiGlyph *glyphs;
    uint8_t *atlas;
} GuiFont;

typedef struct
{
    uint32_t app_bg;
    uint32_t app_surface;
    uint32_t app_surface_alt;
    uint32_t chrome_bg;
    uint32_t chrome_bg_alt;
    uint32_t chrome_edge;
    uint32_t border;
    uint32_t border_focus;
    uint32_t border_hover;
    uint32_t accent;
    uint32_t accent_soft;
    uint32_t text;
    uint32_t text_dim;
    uint32_t text_muted;
    uint32_t success;
    uint32_t warning;
    uint32_t danger;
    uint32_t overlay_scrim;
} GuiStylePalette;

typedef struct
{
    uint32_t desktop_bg;
    uint32_t frame_outline;
    uint32_t button_close;
    uint32_t button_minimize;
    uint32_t button_maximize;
    uint32_t button_outline;
} GuiChromePalette;

enum
{
    GUI_SPACE_0_5 = 4,
    GUI_SPACE_1 = 8,
    GUI_SPACE_1_5 = 12,
    GUI_SPACE_2 = 16,
    GUI_SPACE_3 = 24,
    GUI_SPACE_4 = 32,
    GUI_CARD_HEADER_H = 30,
    GUI_BADGE_H = 18,
    GUI_BADGE_PAD_X = 8,
    GUI_BADGE_RADIUS = 9,
    GUI_APP_OUTER_PADDING = 16,
    GUI_APP_SECTION_GAP = 14,
    GUI_APP_HEADER_H = 58,
    GUI_APP_ROW_H = 34,
    GUI_APP_CONTROL_H = 26,
    GUI_DIALOG_MIN_W = 360,
    GUI_DIALOG_MAX_W = 480,
    GUI_DIALOG_BUTTON_W = 88
};

typedef struct
{
    Rect header_rect;
    Rect body_rect;
    int outer_x;
    int outer_y;
    int outer_w;
    int outer_h;
} GuiAppLayout;

// Modal dialog geometry. `panel` is the centered dialog box, `field` the
// optional text input, and `cancel`/`confirm` the footer buttons (`cancel`
// only valid when the dialog was laid out with a cancel label).
typedef struct
{
    Rect panel;
    Rect field;
    Rect cancel;
    Rect confirm;
} GuiDialogLayout;

typedef struct
{
    const char *label;
    bool enabled;
    bool separator;
} GuiMenuItem;

extern GuiStylePalette g_gui_style;
extern GuiChromePalette g_gui_chrome;

typedef enum
{
    GUI_CURSOR_ARROW = 0,
    GUI_CURSOR_MOVE,
    GUI_CURSOR_RESIZE_H,
    GUI_CURSOR_RESIZE_V,
    GUI_CURSOR_RESIZE_D1,
    GUI_CURSOR_RESIZE_D2,
} GuiCursorKind;

// Monochrome icon set authored as SVG sources under glyphs/ and rasterized to
// white .uoic silhouettes staged at /usr/share/glyphs/. gui_draw_glyph loads
// and caches a silhouette per kind/size and tints it to `fg` at draw time, so
// one asset serves every color context. `size` is the square edge.
typedef enum
{
    GUI_GLYPH_NONE = 0,
    GUI_GLYPH_FOLDER,
    GUI_GLYPH_FOLDER_UP,
    GUI_GLYPH_FILE,
    GUI_GLYPH_FILE_TEXT,
    GUI_GLYPH_FILE_CODE,
    GUI_GLYPH_FILE_CONFIG,
    GUI_GLYPH_FILE_IMAGE,
    GUI_GLYPH_FILE_ARCHIVE,
    GUI_GLYPH_FILE_BINARY,
    GUI_GLYPH_DRIVE,
    GUI_GLYPH_HOME,
    GUI_GLYPH_DESKTOP,
    GUI_GLYPH_DOCUMENTS,
    GUI_GLYPH_DOWNLOADS,
    GUI_GLYPH_PICTURES,
    GUI_GLYPH_ARROW_UP,
    GUI_GLYPH_NETWORK,
    GUI_GLYPH_SETTINGS,
    GUI_GLYPH_APPEARANCE,
    GUI_GLYPH_DEVICES,
    GUI_GLYPH_INFO,
    GUI_GLYPH_WARNING,
    GUI_GLYPH_DISPLAY,
    GUI_GLYPH_CLOCK,
    GUI_GLYPH_GRID,
    GUI_GLYPH_ANIMATION,
    GUI_GLYPH_TRANSPARENCY,
    GUI_GLYPH_VOLUME,
    GUI_GLYPH_STORAGE,
    GUI_GLYPH_CALENDAR,
    GUI_GLYPH_TERMINAL,
    GUI_GLYPH_APP,
    GUI_GLYPH_COMMAND,
    GUI_GLYPH_SEARCH,
    GUI_GLYPH_COUNT,
} GuiGlyphKind;

void gui_draw_glyph(Surface *s, int32_t x, int32_t y, int32_t size, GuiGlyphKind glyph, uint32_t fg);
// Square edge for the standard 16px sidebar/list glyph, UI-scaled.
int gui_glyph_std_size(void);

#define GUI_UOWP_VARIANT_DEFAULT 0u
#define GUI_UOWP_VARIANT_LIGHT 1u
#define GUI_UOWP_VARIANT_DARK 2u
#define GUI_UOWP_VARIANT_LOCK_BLUR 3u
#define GUI_UOWP_VARIANT_DYNAMIC 4u

#define GUI_UOCU_VARIANT_DEFAULT 0u
#define GUI_UOCU_VARIANT_DARK 1u
#define GUI_UOCU_VARIANT_LIGHT 2u
#define GUI_UOCU_VARIANT_HIGH_CONTRAST 3u
#define GUI_UOCU_VARIANT_LARGE 4u

#define GUI_UOCU_ROLE_ARROW 0u
#define GUI_UOCU_ROLE_IBEAM 1u
#define GUI_UOCU_ROLE_HAND 2u
#define GUI_UOCU_ROLE_CROSSHAIR 3u
#define GUI_UOCU_ROLE_WAIT 4u
#define GUI_UOCU_ROLE_PROGRESS 5u
#define GUI_UOCU_ROLE_MOVE 6u
#define GUI_UOCU_ROLE_RESIZE_NS 7u
#define GUI_UOCU_ROLE_RESIZE_EW 8u
#define GUI_UOCU_ROLE_RESIZE_NESW 9u
#define GUI_UOCU_ROLE_RESIZE_NWSE 10u
#define GUI_UOCU_ROLE_NOT_ALLOWED 11u

Surface gui_init_framebuffer(void);

Surface gui_register_window(const char *title, uint32_t width, uint32_t height);
Surface gui_register_window_ex(const char *title, uint32_t width, uint32_t height, uint32_t flags);
int gui_set_window_owner_pid(uint32_t pid);
int gui_set_window_title(const char *title);
bool gui_window_title_matches(const char *window_title, const char *app_title);
void gui_notify(const char *title, const char *message);

void gui_menu_model_reset(MenuModel *model);
int gui_menu_model_add_menu(MenuModel *model, const char *name);
bool gui_menu_model_add_item(MenuModel *model, int menu_index, const char *label, uint32_t id, uint32_t flags,
                             const char *accel);
bool gui_menu_model_add_separator(MenuModel *model, int menu_index);
bool gui_menu_publish(const MenuModel *model);
bool gui_menu_take_command(uint32_t *out_id);

bool gui_clipboard_copy(const char *text, size_t len);
bool gui_clipboard_paste(char *out, size_t out_size, size_t *out_len);

// Open-request handoff (see Registry.open_generation): the launcher submits a
// path before fork/exec; the launched app takes it once at startup.
bool gui_open_request_submit(const char *path);
bool gui_open_request_take(char *out, size_t out_size);

int gui_request_focus(void);
int gui_sync_window_size(Surface *s);
int gui_commit_window_damage(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h);
int gui_wait_frame(uint64_t *frame_ticks, uint32_t *completed_sequence);
int gui_poll_frame(uint64_t *frame_ticks, uint32_t *completed_sequence);
int gui_window_set_min_size(int width, int height);
int gui_window_get_min_size(int *width, int *height);
int gui_set_content_size(Surface *s, int content_w, int content_h);
// Declare the interactive (non-drag) rects of the unified headerbar band, in
// client canvas coordinates. The WM forwards clicks/hovers landing in them to
// this window instead of treating them as a titlebar drag. count 0 makes the
// whole band draggable. Rects beyond WINDOW_HEADER_INPUT_MAX are dropped.
void gui_window_set_header_input(const Rect *rects, int count);
extern WindowEntry *g_my_window;

Surface gui_create_surface(uint32_t width, uint32_t height);
void gui_destroy_surface(Surface *s);

void gui_draw_pixel(Surface *s, int32_t x, int32_t y, uint32_t color);
void gui_draw_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gui_fill_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
// Alpha-blending fill (color's alpha channel is respected), unlike gui_fill_rect
// which overwrites. Used for dimming scrims over content.
void gui_fill_rect_blend(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gui_fill_rounded_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color);
void gui_draw_rounded_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color);
void gui_fill_circle(Surface *s, int32_t x, int32_t y, int32_t r, uint32_t color);
void gui_draw_circle_stroke(Surface *s, int32_t x, int32_t y, int32_t r, int32_t thickness, uint32_t color);

bool gui_fonts_init(void);
const GuiFont *gui_font_default(void);
const GuiFont *gui_font_title(void);
const GuiFont *gui_font_mono(void);
const GuiFont *gui_font_mono_size(int pixel_size);
int gui_font_line_height(const GuiFont *font);
int gui_font_ascent(const GuiFont *font);
int gui_font_max_advance(const GuiFont *font);
int gui_font_mono_cell_width(const GuiFont *font);
int gui_measure_text(const GuiFont *font, const char *str);
void gui_draw_text(Surface *s, const GuiFont *font, int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg);
void gui_draw_mono_cell(Surface *s, const GuiFont *font, int32_t x, int32_t y, int32_t cell_w, int32_t cell_h, char c,
                        uint32_t fg, uint32_t bg);
void gui_draw_char(Surface *s, int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg);
void gui_draw_string(Surface *s, int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg);
int gui_measure_text_n(const GuiFont *font, const char *str, size_t len);
size_t gui_truncate_text(const GuiFont *font, const char *str, int max_width, char *out, size_t out_size);
void gui_draw_text_clipped(Surface *s, const GuiFont *font, int32_t x, int32_t y, int32_t max_width, const char *str,
                           uint32_t fg, uint32_t bg);
void gui_draw_text_rect_clipped(Surface *s, const GuiFont *font, int32_t x, int32_t y, int32_t max_width,
                                int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h, const char *str,
                                uint32_t fg, uint32_t bg);

void gui_blit(Surface *dest, Surface *src, int32_t dest_x, int32_t dest_y);
void gui_blit_alpha(Surface *dest, Surface *src, int32_t dest_x, int32_t dest_y);
void gui_blit_rect(Surface *dest, Surface *src, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w, int32_t h);
void gui_blit_rect_fill(Surface *dest, Surface *src, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w,
                        int32_t h, uint32_t fill);
uint8_t gui_rounded_rect_coverage_local(int32_t col, int32_t row, int32_t w, int32_t h, int32_t r,
                                        uint32_t rounded_edges);

void gui_blit_to_screen_rect(Surface *src, int32_t x, int32_t y, int32_t w, int32_t h);

bool gui_load_uoic(const char *path, uint32_t logical_px, uint32_t display_scale_pct, Surface *out);
bool gui_load_uowp(const char *path, uint16_t preferred_variant, uint32_t target_width, uint32_t target_height,
                   Surface *out);
bool gui_load_uocu(const char *path, uint16_t cursor_role, uint32_t logical_px, uint32_t display_scale_pct,
                   uint16_t preferred_variant, Surface *out, uint16_t *hotspot_x, uint16_t *hotspot_y,
                   uint32_t *frame_duration_ms);
void gui_blit_scaled_cover(Surface *dest, const Surface *src);

bool gui_intersect_rect(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2, int *ox, int *oy, int *ow,
                        int *oh);
bool gui_load_file(const char *path, uint8_t **out_data, uint32_t *out_size);
Registry *gui_registry(void);
void gui_apply_theme(GuiThemeMode mode);
bool gui_sync_theme_from_registry(void);
int gui_ui_scale_pct(void);
int gui_scaled_metric(int base_px);
int gui_space_0_5(void);
int gui_space_1(void);
int gui_space_1_5(void);
int gui_space_2(void);
int gui_space_3(void);
int gui_space_4(void);
int gui_card_header_h(void);
int gui_badge_h(void);
int gui_badge_pad_x(void);
int gui_app_outer_padding(void);
int gui_app_section_gap(void);
int gui_app_header_h(void);
int gui_app_row_h(void);
// Vertical gap between consecutive list/nav rows.
int gui_app_row_gap(void);
// Two-line row (title + detail) for toggles and settings rows.
int gui_app_row_tall_h(void);
// Two-line navigation item (title + detail).
int gui_app_nav_h(void);
int gui_app_control_h(void);
// The unified headerbar height — the top region of the client area that apps
// reserve for traffic lights + toolbar. The WM overlays traffic lights there.
int gui_headerbar_h(void);
// Width of the WM traffic-light cluster (close/minimize/maximize) at the left
// of the headerbar, so apps keep their own controls clear of it.
int gui_traffic_lights_w(void);
int gui_menubar_h(void);
int gui_system_menubar_canvas_h(void);
int gui_scrollbar_w(void);
int gui_scrollbar_min_thumb(void);
int gui_dialog_button_w(void);

GuiAppLayout gui_app_begin(Surface *s);
void gui_app_draw_nav_item(Surface *s, int x, int y, int w, int h, GuiGlyphKind icon, const char *label, bool active,
                           bool hovered);
void gui_app_draw_list_row(Surface *s, int x, int y, int w, int h, GuiGlyphKind icon, const char *title,
                           const char *detail, bool active, bool hovered, bool muted);
// Flat list row carrying a small text badge pill (for symbol/outline markers
// where a glyph is not a fit).
void gui_app_draw_badged_row(Surface *s, int x, int y, int w, int h, const char *badge, const char *title,
                             const char *detail, bool active, bool hovered, bool muted);
void gui_app_draw_toggle_row(Surface *s, int x, int y, int w, int h, GuiGlyphKind icon, const char *label,
                             const char *detail, bool on, bool active, bool hovered);
// Slider row: label left, percent value right, track along the bottom.
// Track geometry for hit-testing comes from gui_app_slider_track_rect; map a
// pointer x position to a value with gui_app_slider_value_from_x.
int gui_app_slider_h(void);
Rect gui_app_slider_track_rect(int x, int y, int w, int h);
uint32_t gui_app_slider_value_from_x(int mouse_x, const Rect *track, uint32_t max_value);
void gui_app_draw_slider(Surface *s, int x, int y, int w, int h, const char *label, uint32_t value, uint32_t max_value,
                         bool hovered);
void gui_app_draw_segmented_choice(Surface *s, int x, int y, int w, int h, const char *const *labels, int count,
                                   int selected, int hovered_index);
void gui_app_draw_text_field(Surface *s, int x, int y, int w, int h, const char *value, bool focused, bool hovered);
void gui_app_draw_button(Surface *s, int x, int y, int w, int h, const char *label, bool primary, bool focused,
                         bool hovered);
// Button with an explicit pressed state (darker fill, no drop shadow, label
// nudged down) for click feedback.
void gui_app_draw_button_ex(Surface *s, int x, int y, int w, int h, const char *label, bool primary, bool focused,
                            bool hovered, bool pressed);

// Boxed list group (libadwaita-style grouped card): one continuous rounded
// container that rows share, instead of each row carrying its own framed box.
void gui_draw_boxed_container(Surface *s, int x, int y, int w, int h);
// Hairline divider between boxed rows. The stroke is indented from the left so
// it starts past the row's icon/label gutter and never touches the card edge.
void gui_draw_boxed_divider(Surface *s, int x, int y, int w, int indent);
// Uppercase, reduced-opacity caption used to segment sidebar sections without
// horizontal rules.
void gui_app_draw_section_caption(Surface *s, int x, int y, int w, const char *caption);
// Height of a single-line sidebar navigation pill.
int gui_app_sidebar_row_h(void);
int gui_popup_menu_item_h(void);
int gui_popup_menu_height(const GuiMenuItem *items, int count);
int gui_popup_menu_width(const GuiMenuItem *items, int count, int min_width);
int gui_popup_menu_hit_test(const GuiMenuItem *items, int count, int x, int y, int w, int mx, int my);
void gui_draw_popup_menu(Surface *s, int x, int y, int w, const GuiMenuItem *items, int count, int hovered_index);
void gui_draw_popup_menu_ext(Surface *s, int x, int y, int w, const GuiMenuItem *items, int count, int hovered_index,
                             const char *const *accel_labels, const bool *checked_flags);

// Canonical drop shadow for floating panels (popups, dialogs, shell
// overlays): three stacked rounded fills below the panel. Draw it before the
// panel itself.
void gui_draw_panel_shadow(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r);

// Modal dialog: scrim + shadow + panel + card header + body lines (or a text
// field) + footer buttons. gui_dialog_layout computes every rect (panel width
// is derived from the longest line); gui_draw_dialog renders the whole stack.
// view_scroll_y supports scrolled app views (pass 0 otherwise).
GuiDialogLayout gui_dialog_layout(int view_w, int view_h, int view_scroll_y, const char *const *lines, int line_count,
                                  bool has_field);
void gui_draw_dialog(Surface *s, int view_w, int view_h, int view_scroll_y, const GuiDialogLayout *layout,
                     const char *title, const char *const *lines, int line_count, const char *field_value,
                     const char *confirm_label, bool confirm_hovered, bool confirm_pressed, const char *cancel_label,
                     bool cancel_hovered, bool cancel_pressed);

// Vertical scrollbar: track + rounded thumb. thumb_offset is the thumb top
// relative to the track top; pass hovered to brighten the thumb.
void gui_draw_scrollbar(Surface *s, int x, int y, int w, int h, int thumb_offset, int thumb_length, bool hovered);

void gui_draw_cursor(Surface *s, int32_t x, int32_t y);
void gui_draw_cursor_kind(Surface *s, int32_t x, int32_t y, GuiCursorKind kind);
void gui_get_cursor_hotspot(GuiCursorKind kind, int32_t *hot_x, int32_t *hot_y);
void gui_get_cursor_bounds(GuiCursorKind kind, int32_t x, int32_t y, int32_t *bx, int32_t *by, int32_t *bw,
                           int32_t *bh);

static inline int gui_text_width(const char *str)
{
    return gui_measure_text(gui_font_default(), str);
}

static inline int gui_line_height(void)
{
    return gui_font_line_height(gui_font_default());
}

static inline int gui_text_baseline_offset(void)
{
    return gui_font_ascent(gui_font_default());
}

static inline int gui_align_text_y(const GuiFont *font, int box_y, int box_h)
{
    int line_h = gui_font_line_height(font);
    if (box_h <= line_h)
        return box_y;
    return box_y + (box_h - line_h) / 2;
}

static inline int gui_align_text_x_center(const GuiFont *font, int box_x, int box_w, const char *text)
{
    int text_w = gui_measure_text(font, text ? text : "");
    if (text_w >= box_w)
        return box_x;
    return box_x + (box_w - text_w) / 2;
}

static inline int gui_align_text_x_right(const GuiFont *font, int box_x, int box_w, const char *text)
{
    int text_w = gui_measure_text(font, text ? text : "");
    if (text_w >= box_w)
        return box_x;
    return box_x + box_w - text_w;
}

static inline int gui_corner_radius(int w, int h, int preferred)
{
    if (w <= 0 || h <= 0)
        return 0;
    int max_radius = (w < h ? w : h) / 2;
    if (preferred > max_radius)
        preferred = max_radius;
    if (preferred < 0)
        preferred = 0;
    return preferred;
}

static inline int gui_radius_xs(void)
{
    return gui_scaled_metric(4);
}

static inline int gui_radius_sm(void)
{
    return gui_scaled_metric(6);
}

static inline int gui_radius_md(void)
{
    return gui_scaled_metric(10);
}

static inline int gui_radius_lg(void)
{
    return gui_scaled_metric(12);
}

static inline int gui_radius_xl(void)
{
    return gui_scaled_metric(12);
}

static inline int gui_panel_radius(int w, int h)
{
    return gui_corner_radius(w, h, gui_radius_md());
}

static inline void gui_fill_surface(Surface *s, uint32_t color)
{
    gui_fill_rect(s, 0, 0, (int32_t)s->width, (int32_t)s->height, color);
}

static inline void gui_draw_separator_h(Surface *s, int x, int y, int w, uint32_t color)
{
    gui_fill_rect_blend(s, x, y, w, 1, color);
}

static inline void gui_draw_separator_v(Surface *s, int x, int y, int h, uint32_t color)
{
    gui_fill_rect_blend(s, x, y, 1, h, color);
}

// Theme-aware translucent washes. Dark theme washes are white at low alpha
// (specular lifts on #141416..#22222A); light theme washes are black at low
// alpha. All row hovers, hairlines and inset wells route through these so both
// themes read identically.
uint32_t gui_hairline_color(void);         // divider hairlines (~8%)
uint32_t gui_hover_wash_color(void);       // row/button hover lift (~6%)
uint32_t gui_inset_wash_color(void);       // wells: off switch tracks, slider tracks
uint32_t gui_edge_wash_color(void);        // input/segment container edges (~7%)
uint32_t gui_subtle_card_wash_color(void); // flat tiles/notification cards (~6%)
uint32_t gui_active_wash_color(void);      // active sidebar selection pill (~12%)
bool gui_theme_is_light(void);             // true when the active theme is GUI_THEME_LIGHT

// The edge every floating surface shares with windows (macOS-style): a dark
// translucent stroke on the outside of the silhouette (it reads as part of the
// drop shadow) and a light translucent rim just inside it so the layer stays
// defined when windows overlap. Dark theme raises both opacities a little for a
// crisper, more defined edge. Popup menus, dialogs, shell overlays and the dock
// all use these so every edge is pixel-identical to a window frame.
uint32_t gui_window_outer_stroke_color(void);
uint32_t gui_window_inner_rim_color(void);
// Opaque body fill + the outer/inner edge strokes.
void gui_draw_window_frame(Surface *s, int x, int y, int w, int h, int radius, uint32_t body);
// Body + outer stroke only, without the light inner rim. For surfaces that draw
// content flush to the top edge and then draw the rim on top of it.
void gui_draw_window_frame_no_rim(Surface *s, int x, int y, int w, int h, int radius, uint32_t body);
// The light inner rim only.
void gui_draw_window_rim(Surface *s, int x, int y, int w, int h, int radius);
// Edge strokes only, for surfaces that paint their own body (glass panels).
void gui_draw_window_ring(Surface *s, int x, int y, int w, int h, int radius);

static inline void gui_fill_top_rounded_panel(Surface *s, int x, int y, int w, int h, int r, uint32_t color)
{
    if (h <= 0 || w <= 0)
        return;
    gui_fill_rounded_rect(s, x, y, w, h, r, color);
    if (r > 0 && h > r)
        gui_fill_rect(s, x, y + r, w, h - r, color);
}

static inline void gui_draw_card_header_ext(Surface *s, int x, int y, int w, int r, const char *title,
                                            const char *detail)
{
    int header_h = gui_card_header_h();
    gui_fill_top_rounded_panel(s, x, y, w, header_h, gui_corner_radius(w, header_h, r), g_gui_style.app_surface_alt);
    gui_draw_separator_h(s, x + gui_space_1_5(), y + header_h - 1, w - gui_space_3(), gui_hairline_color());
    int title_y = gui_align_text_y(gui_font_title(), y, header_h);
    if (title)
        gui_draw_text_clipped(s, gui_font_title(), x + gui_space_2(), title_y, w - gui_space_3(), title,
                              g_gui_style.text, g_gui_style.app_surface_alt);
    if (detail && *detail) {
        int detail_w = gui_measure_text(gui_font_default(), detail);
        int detail_x = x + w - gui_space_2() - detail_w;
        if (detail_x < x + gui_space_4())
            detail_x = x + gui_space_4();
        int detail_y = gui_align_text_y(gui_font_default(), y, header_h);
        gui_draw_text_clipped(s, gui_font_default(), detail_x, detail_y, w - (detail_x - x) - gui_space_2(), detail,
                              g_gui_style.text_muted, g_gui_style.app_surface_alt);
    }
}

static inline void gui_draw_card_header(Surface *s, int x, int y, int w, const char *title, const char *detail)
{
    gui_draw_card_header_ext(s, x, y, w, gui_panel_radius(w, gui_card_header_h()), title, detail);
}

static inline void gui_draw_badge(Surface *s, int x, int y, const char *label, uint32_t bg, uint32_t fg)
{
    int text_w = gui_measure_text(gui_font_default(), label);
    int badge_pad_x = gui_badge_pad_x();
    int h = gui_badge_h();
    int w = text_w + badge_pad_x * 2;
    int r = h / 2; // Pill shape
    gui_fill_rounded_rect(s, x, y, w, h, r, bg);
    int text_y = gui_align_text_y(gui_font_default(), y, h);
    gui_draw_text_clipped(s, gui_font_default(), x + badge_pad_x, text_y, text_w, label, fg, bg);
}

static inline void gui_draw_focus_frame(Surface *s, int x, int y, int w, int h, bool focused, bool hovered)
{
    uint32_t border = hovered ? g_gui_style.border_hover : (focused ? g_gui_style.border_focus : g_gui_style.border);
    int r = gui_panel_radius(w, h);
    gui_draw_rounded_rect(s, x, y, w, h, r, border);
    if (focused && w > 4 && h > 4)
        gui_draw_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, gui_corner_radius(w - 2, h - 2, r - 1),
                              g_gui_style.accent_soft);
}

static inline int gui_draw_wrapped_value(Surface *s, int x, int y, int w, const char *value, uint32_t fg, uint32_t bg)
{
    if (!value || !*value || w <= 0)
        return 0;

    const GuiFont *font = gui_font_default();
    char line[160];
    int line_count = 0;
    const char *ptr = value;

    while (*ptr) {
        while (*ptr == ' ')
            ptr++;
        if (!*ptr)
            break;

        const char *start = ptr;
        const char *best = nullptr;
        const char *scan = ptr;
        while (*scan) {
            if (*scan == ' ')
                best = scan;
            int measured = gui_measure_text_n(font, start, (size_t)(scan - start + 1));
            if (measured > w)
                break;
            scan++;
        }

        const char *end = scan;
        if (*scan && best && best > start)
            end = best;
        if (end == start)
            end = start + 1;

        int copy_len = (int)(end - start);
        if (copy_len > (int)sizeof(line) - 1)
            copy_len = (int)sizeof(line) - 1;
        for (int i = 0; i < copy_len; i++)
            line[i] = start[i];
        line[copy_len] = '\0';

        gui_draw_text_clipped(s, font, x, y + line_count * gui_line_height(), w, line, fg, bg);
        line_count++;

        ptr = (*end == ' ') ? end + 1 : end;
    }

    return line_count * gui_line_height();
}

#ifdef __cplusplus
}
#endif
