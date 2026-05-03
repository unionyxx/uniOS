#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../libc/log.h"
#include "gui.h"

static constexpr uint32_t UOF_MAGIC = 0x4E464F55u; // "UOFN", little-endian
static constexpr uint16_t UOF_VERSION = 1;

struct UofHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t pixel_size;
    uint16_t atlas_width;
    uint16_t atlas_height;
    int16_t ascent;
    int16_t descent;
    int16_t line_gap;
    uint32_t glyph_count;
    uint32_t kerning_count;
    uint32_t fallback_index;
    uint32_t glyph_offset;
    uint32_t kerning_offset;
    uint32_t atlas_offset;
} __attribute__((packed));

static GuiFont g_ui_font = {};
static GuiFont g_title_font = {};
static GuiFont g_mono_font = {};
static bool g_fonts_initialized = false;
static bool g_fonts_ready = false;

static constexpr int k_font_sizes[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
static constexpr int k_font_size_count = (int)(sizeof(k_font_sizes) / sizeof(k_font_sizes[0]));
static constexpr size_t k_glyph_cache_cap = 256;
static constexpr size_t k_gui_text_scan_limit = 1024;
static bool gui_font_load_from_file(GuiFont *font, const char *path);
static inline const GuiGlyph *gui_font_fallback_glyph(const GuiFont *font);
static size_t gui_bounded_line_length(const char *str, size_t limit);

enum class FontRenderProfile : uint8_t
{
    Ui = 0,
    Title,
    Mono
};

static int clamp_font_target(int target)
{
    if (target < 11)
        return 11;
    if (target > 18)
        return 18;
    return target;
}

static int choose_font_pixel_size()
{
    uint32_t info[4] = {};
    if (fb_info(info) != 0)
        return 12;
    uint32_t width = info[0];
    uint32_t height = info[1];
    uint32_t min_dim = width < height ? width : height;
    uint32_t max_dim = width > height ? width : height;

    // Scale font size based on display resolution (min_dim).
    int target = 12;
    if (min_dim < 800)
        target = 11;
    else if (min_dim < 1050)
        target = 12;
    else if (min_dim < 1600)
        target = 13;
    else if (min_dim < 2100)
        target = 14;
    else
        target = 15;

    if (max_dim >= 3200 && min_dim >= 1800 && target < 15)
        target++;
    return clamp_font_target(target);
}

static int choose_title_font_pixel_size(int ui_size)
{
    if (ui_size <= 12)
        return ui_size;
    if (ui_size >= 16)
        return ui_size + 1;
    return ui_size;
}

static int nearest_font_index(int pixel_size)
{
    int best = 0;
    int best_delta = 999;
    for (int i = 0; i < k_font_size_count; i++) {
        int delta = k_font_sizes[i] - pixel_size;
        if (delta < 0)
            delta = -delta;
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return best;
}

static bool load_nearest_font(GuiFont *font, const char *prefix, int preferred_size)
{
    if (!font || !prefix)
        return false;

    int preferred_index = nearest_font_index(clamp_font_target(preferred_size));
    char path[64];
    for (int radius = 0; radius < k_font_size_count; radius++) {
        int left = preferred_index - radius;
        int right = preferred_index + radius;
        if (left >= 0) {
            snprintf(path, sizeof(path), "/usr/share/fonts/%s-%d.uof", prefix, k_font_sizes[left]);
            if (gui_font_load_from_file(font, path))
                return true;
        }
        if (right < k_font_size_count && right != left) {
            snprintf(path, sizeof(path), "/usr/share/fonts/%s-%d.uof", prefix, k_font_sizes[right]);
            if (gui_font_load_from_file(font, path))
                return true;
        }
    }
    return false;
}

static int fallback_text_width(const char *str)
{
    return (int)(gui_bounded_line_length(str, k_gui_text_scan_limit) * 8u);
}

static size_t gui_bounded_line_length(const char *str, size_t limit)
{
    if (!str)
        return 0;

    size_t len = 0;
    while (len < limit && str[len] && str[len] != '\n')
        len++;
    return len;
}

static void build_font_alpha_lut(GuiFont *font, FontRenderProfile profile)
{
    if (!font)
        return;
 
    int boost_factor = 120;
    switch (profile) {
        case FontRenderProfile::Ui:    boost_factor = 144; break;
        case FontRenderProfile::Title: boost_factor = 96; break;
        case FontRenderProfile::Mono:  boost_factor = 160; break;
    }
 
    for (int i = 0; i < 256; i++) {
        uint32_t x = (uint32_t)i;
        uint32_t boost = (x * (255u - x) * (uint32_t)boost_factor) / 65025u;
        uint32_t out = x + boost;
        font->alpha_lut[i] = (uint8_t)(out > 255 ? 255 : out);
    }
}

static bool load_file(const char *path, uint8_t **out_data, uint32_t *out_size)
{
    if (out_data)
        *out_data = nullptr;
    if (out_size)
        *out_size = 0;
    if (!path || !out_data || !out_size)
        return false;

    VNodeStat st = {};
    if (stat(path, &st) != 0 || st.is_dir || st.size == 0 || st.size > 0xFFFFFFFFu)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    uint8_t *data = static_cast<uint8_t *>(malloc((size_t)st.size));
    if (!data) {
        close(fd);
        return false;
    }

    uint64_t total = 0;
    while (total < st.size) {
        int n = read(fd, data + total, (size_t)(st.size - total));
        if (n <= 0)
            break;
        total += (uint64_t)n;
    }
    close(fd);
    if (total != st.size) {
        free(data);
        return false;
    }

    *out_data = data;
    *out_size = (uint32_t)st.size;
    return true;
}

static bool gui_font_load_from_file(GuiFont *font, const char *path)
{
    if (!font || !path)
        return false;

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!load_file(path, &data, &size))
        return false;
    if (size < sizeof(UofHeader)) {
        free(data);
        return false;
    }

    const UofHeader *header = reinterpret_cast<const UofHeader *>(data);
    if (header->magic != UOF_MAGIC || header->version != UOF_VERSION) {
        free(data);
        return false;
    }
    if (header->glyph_count == 0 || header->fallback_index >= header->glyph_count) {
        free(data);
        return false;
    }
    if (header->glyph_offset > size || header->atlas_offset > size || header->kerning_offset > size) {
        free(data);
        return false;
    }

    uint64_t glyph_bytes64 = (uint64_t)header->glyph_count * (uint64_t)sizeof(GuiGlyph);
    if (glyph_bytes64 > 0xFFFFFFFFu || (uint64_t)header->glyph_offset + glyph_bytes64 > size) {
        free(data);
        return false;
    }
    uint64_t atlas_bytes64 = (uint64_t)header->atlas_width * (uint64_t)header->atlas_height;
    if (atlas_bytes64 > 0xFFFFFFFFu || (uint64_t)header->atlas_offset + atlas_bytes64 > size) {
        free(data);
        return false;
    }
    uint32_t glyph_bytes = (uint32_t)glyph_bytes64;
    uint32_t atlas_bytes = (uint32_t)atlas_bytes64;

    memset(font, 0, sizeof(*font));
    font->magic = header->magic;
    font->glyph_count = header->glyph_count;
    font->fallback_index = header->fallback_index;
    font->pixel_size = (uint16_t)header->pixel_size;
    font->atlas_width = header->atlas_width;
    font->atlas_height = header->atlas_height;
    font->ascent = header->ascent;
    font->descent = header->descent;
    font->line_gap = header->line_gap;
    font->glyphs = static_cast<GuiGlyph *>(malloc(glyph_bytes));
    font->atlas = static_cast<uint8_t *>(malloc(atlas_bytes));
    if (!font->glyphs || !font->atlas) {
        free(font->glyphs);
        free(font->atlas);
        memset(font, 0, sizeof(*font));
        free(data);
        return false;
    }

    memcpy(font->glyphs, data + header->glyph_offset, glyph_bytes);
    memcpy(font->atlas, data + header->atlas_offset, atlas_bytes);
    free(data);

    font->max_advance = 8;
    font->max_ink_width = 8;
    for (int i = 0; i < 128; i++)
        font->ascii_index[i] = -1;
    for (uint32_t i = 0; i < font->glyph_count; i++) {
        const GuiGlyph &glyph = font->glyphs[i];
        if (glyph.advance_x > font->max_advance)
            font->max_advance = glyph.advance_x;
        if ((int16_t)glyph.width > font->max_ink_width)
            font->max_ink_width = (int16_t)glyph.width;
        if (glyph.codepoint < 128u)
            font->ascii_index[glyph.codepoint] = (int16_t)i;
    }
    if (font->max_advance <= 0)
        font->max_advance = 8;
    if (font->max_ink_width <= 0)
        font->max_ink_width = font->max_advance;
    font->line_height = font->ascent + font->descent + font->line_gap;
    if (font->line_height <= 0)
        font->line_height = font->pixel_size > 0 ? (int16_t)font->pixel_size : 16;

    int fallback_advance = font->max_advance;
    if (const GuiGlyph *fallback = gui_font_fallback_glyph(font)) {
        if (fallback->advance_x > 0)
            fallback_advance = fallback->advance_x;
    }
    for (int i = 0; i < 128; i++)
        font->ascii_advance[i] = (int16_t)fallback_advance;
    for (uint32_t i = 0; i < font->glyph_count; i++) {
        const GuiGlyph &glyph = font->glyphs[i];
        if (glyph.codepoint < 128u)
            font->ascii_advance[glyph.codepoint] = glyph.advance_x > 0 ? glyph.advance_x : (int16_t)fallback_advance;
    }
    return true;
}

static const GuiGlyph *gui_font_find_glyph(const GuiFont *font, uint32_t codepoint)
{
    if (!font || !font->glyphs || font->glyph_count == 0)
        return nullptr;
    if (codepoint < 128u) {
        int16_t idx = font->ascii_index[codepoint];
        if (idx >= 0 && (uint32_t)idx < font->glyph_count)
            return &font->glyphs[idx];
    }
    if (font->fallback_index < font->glyph_count)
        return &font->glyphs[font->fallback_index];
    return &font->glyphs[0];
}

static inline const GuiGlyph *gui_font_fallback_glyph(const GuiFont *font)
{
    if (!font || !font->glyphs || font->glyph_count == 0)
        return nullptr;
    if (font->fallback_index < font->glyph_count)
        return &font->glyphs[font->fallback_index];
    return &font->glyphs[0];
}

static inline const GuiGlyph *resolve_glyph_and_advance(const GuiFont *font, uint8_t ch, int *advance)
{
    if (advance)
        *advance = font ? gui_font_max_advance(font) : 8;
    if (!font)
        return nullptr;

    if (ch < 128u) {
        if (advance && font->ascii_advance[ch] > 0)
            *advance = font->ascii_advance[ch];
        int16_t idx = font->ascii_index[ch];
        if (idx >= 0 && (uint32_t)idx < font->glyph_count)
            return &font->glyphs[idx];
        return gui_font_fallback_glyph(font);
    }

    const GuiGlyph *glyph = gui_font_find_glyph(font, ch);
    if (advance && glyph)
        *advance = glyph->advance_x;
    return glyph;
}

static inline uint8_t effective_color_alpha(uint32_t color)
{
    uint8_t alpha = (uint8_t)(color >> 24);
    if (alpha != 0)
        return alpha;
    return color == 0 ? 0u : 255u;
}

static inline uint8_t scale_alpha_u8(uint8_t alpha, uint8_t coverage)
{
    return (uint8_t)(((uint32_t)alpha * (uint32_t)coverage + 127u) / 255u);
}



static void draw_single_glyph(Surface *s, const GuiFont *font, int32_t origin_x, int32_t top_y, const GuiGlyph *glyph,
                              uint32_t fg, uint32_t bg, const uint8_t *alpha_lut, int32_t clip_x = 0,
                              int32_t clip_y = 0, int32_t clip_w = -1, int32_t clip_h = -1)
{
    if (!s || !s->buffer || !font || !glyph || !font->atlas)
        return;
    (void)bg;

    uint8_t fg_alpha = effective_color_alpha(fg);
    if (fg_alpha == 0)
        return;

    int32_t dest_x = origin_x + glyph->bearing_x;
    int32_t dest_y = top_y + font->ascent - glyph->bearing_y;
    int32_t start_col = 0;
    int32_t start_row = 0;
    int32_t end_col = glyph->width;
    int32_t end_row = glyph->height;
    if (clip_w >= 0 && clip_h >= 0) {
        if (dest_x < clip_x)
            start_col = clip_x - dest_x;
        if (dest_y < clip_y)
            start_row = clip_y - dest_y;
        if (dest_x + end_col > clip_x + clip_w)
            end_col = clip_x + clip_w - dest_x;
        if (dest_y + end_row > clip_y + clip_h)
            end_row = clip_y + clip_h - dest_y;
    }
    if (dest_x < 0)
        start_col = -dest_x;
    if (dest_y < 0)
        start_row = -dest_y;
    if (dest_x + end_col > (int32_t)s->width)
        end_col = (int32_t)s->width - dest_x;
    if (dest_y + end_row > (int32_t)s->height)
        end_row = (int32_t)s->height - dest_y;
    if (start_col >= end_col || start_row >= end_row)
        return;

    uint32_t stride = s->pitch / 4;
    uint32_t fg_rgb = 0xFF000000u | (fg & 0x00FFFFFFu);
    uint32_t fr = (fg_rgb >> 16) & 0xFFu, fg_g = (fg_rgb >> 8) & 0xFFu, fb = fg_rgb & 0xFFu;
 
    for (int32_t row = start_row; row < end_row; row++) {
        uint32_t *dst = &s->buffer[(uint32_t)(dest_y + row) * stride + (uint32_t)(dest_x + start_col)];
        const uint8_t *atlas =
            &font->atlas[(uint32_t)(glyph->atlas_y + row) * font->atlas_width + (uint32_t)(glyph->atlas_x + start_col)];
        for (int32_t col = start_col; col < end_col; col++) {
            uint8_t raw_coverage = *atlas++;
            uint8_t coverage = alpha_lut ? alpha_lut[raw_coverage] : raw_coverage;
            if (coverage == 0) {
                dst++;
                continue;
            }
            uint8_t alpha = scale_alpha_u8(fg_alpha, coverage);
            if (alpha == 255) {
                *dst = fg_rgb;
            } else {
                uint32_t d = *dst;
                uint32_t inv = 255u - alpha;
                uint32_t dr = (d >> 16) & 0xFFu, dg = (d >> 8) & 0xFFu, db = d & 0xFFu;
                uint32_t r = (fr * alpha + dr * inv + 127u) / 255u;
                uint32_t g = (fg_g * alpha + dg * inv + 127u) / 255u;
                uint32_t b = (fb * alpha + db * inv + 127u) / 255u;
                *dst = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            dst++;
        }
    }
}

static void draw_text_run_clipped(Surface *s, const GuiFont *font, int32_t x, int32_t y, const char *str, uint32_t fg,
                                  uint32_t bg, const uint8_t *alpha_lut, int32_t clip_x, int32_t clip_y, int32_t clip_w,
                                  int32_t clip_h)
{
    if (!s || !s->buffer || !font || !str)
        return;

    int32_t pen_x = x;
    for (const char *it = str; *it && *it != '\n'; ++it) {
        int advance = 0;
        const GuiGlyph *glyph = resolve_glyph_and_advance(font, (uint8_t)*it, &advance);
        if (glyph) {
            draw_single_glyph(s, font, pen_x, y, glyph, fg, bg, alpha_lut, clip_x, clip_y, clip_w, clip_h);
            pen_x += advance;
        } else {
            pen_x += advance;
        }
        if (clip_w >= 0 && pen_x >= clip_x + clip_w)
            break;
    }
}

extern "C" {

bool gui_fonts_init(void)
{
    if (g_fonts_initialized)
        return g_fonts_ready;
    g_fonts_initialized = true;

    int ui_size = choose_font_pixel_size();
    bool ui_ok = load_nearest_font(&g_ui_font, "inter-ui", ui_size);
    bool title_ok = load_nearest_font(&g_title_font, "inter-title", choose_title_font_pixel_size(ui_size));
    bool mono_ok = load_nearest_font(&g_mono_font, "geist-mono", ui_size);

    if (!title_ok && ui_ok)
        g_title_font = g_ui_font;
    if (!mono_ok && ui_ok)
        g_mono_font = g_ui_font;

    if (ui_ok)
        build_font_alpha_lut(&g_ui_font, FontRenderProfile::Ui);
    if (title_ok)
        build_font_alpha_lut(&g_title_font, FontRenderProfile::Title);
    if (mono_ok)
        build_font_alpha_lut(&g_mono_font, FontRenderProfile::Mono);

    g_fonts_ready = ui_ok;
    if (!g_fonts_ready) {
        LOG_WARN("gui.font", "font load fallback: using built-in bitmap renderer");
    }
    return g_fonts_ready;
}

const GuiFont *gui_font_default(void)
{
    gui_fonts_init();
    return g_fonts_ready ? &g_ui_font : nullptr;
}

const GuiFont *gui_font_title(void)
{
    gui_fonts_init();
    return g_fonts_ready ? &g_title_font : nullptr;
}

const GuiFont *gui_font_mono(void)
{
    gui_fonts_init();
    return g_fonts_ready ? &g_mono_font : nullptr;
}

int gui_font_line_height(const GuiFont *font)
{
    if (!font)
        return 16;
    return font->line_height > 0 ? font->line_height : (font->pixel_size > 0 ? font->pixel_size : 16);
}

int gui_font_ascent(const GuiFont *font)
{
    if (!font || font->ascent <= 0)
        return 12;
    return font->ascent;
}

int gui_font_max_advance(const GuiFont *font)
{
    if (!font || font->max_advance <= 0)
        return 8;
    return font->max_advance;
}

int gui_font_mono_cell_width(const GuiFont *font)
{
    if (!font || font->max_advance <= 0)
        return 8;
    return font->max_advance;
}

int gui_measure_text(const GuiFont *font, const char *str)
{
    if (!str)
        return 0;

    size_t len = gui_bounded_line_length(str, k_gui_text_scan_limit);
    if (!font)
        return fallback_text_width(str);

    int width = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)str[i];
        if (ch < 128u) {
            width += font->ascii_advance[ch] > 0 ? font->ascii_advance[ch] : gui_font_max_advance(font);
        } else {
            const GuiGlyph *glyph = gui_font_find_glyph(font, ch);
            width += glyph ? glyph->advance_x : gui_font_max_advance(font);
        }
    }
    return width;
}

void gui_draw_text(Surface *s, const GuiFont *font, int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg)
{
    if (!s || !s->buffer || !str)
        return;
    if (!font)
        return;

    const char *line_start = str;
    int32_t pen_y = y;
    int line_height = gui_font_line_height(font);
    bool paint_bg = effective_color_alpha(bg) != 0;
    size_t remaining = k_gui_text_scan_limit;
    while (remaining > 0) {
        const char *line_end = line_start;
        size_t line_len = 0;
        while (line_len < remaining && *line_end && *line_end != '\n') {
            line_end++;
            line_len++;
        }

        const GuiGlyph *glyph_cache[k_glyph_cache_cap];
        int advance_cache[k_glyph_cache_cap];
        size_t glyph_count = 0;
        int line_width = 0;
        for (const char *it = line_start; it < line_end; ++it) {
            int advance = 0;
            const GuiGlyph *glyph = resolve_glyph_and_advance(font, (uint8_t)*it, &advance);
            if (glyph_count < k_glyph_cache_cap) {
                glyph_cache[glyph_count++] = glyph;
                advance_cache[glyph_count - 1] = advance;
            }
            line_width += advance;
        }
        if (line_width > 0) {
            if (paint_bg)
                gui_fill_rect(s, x, pen_y, line_width, line_height, bg);
            int32_t pen_x = x;
            if (glyph_count == (size_t)(line_end - line_start)) {
                for (size_t i = 0; i < glyph_count; i++) {
                    const GuiGlyph *glyph = glyph_cache[i];
                    if (glyph) {
                        draw_single_glyph(s, font, pen_x, pen_y, glyph, fg, bg, font->alpha_lut);
                        pen_x += advance_cache[i];
                    } else {
                        pen_x += advance_cache[i];
                    }
                }
            } else {
                for (const char *it = line_start; it < line_end; ++it) {
                    int advance = 0;
                    const GuiGlyph *glyph = resolve_glyph_and_advance(font, (uint8_t)*it, &advance);
                    if (glyph) {
                        draw_single_glyph(s, font, pen_x, pen_y, glyph, fg, bg, font->alpha_lut);
                        pen_x += advance;
                    } else {
                        pen_x += advance;
                    }
                }
            }
        }

        if (line_len >= remaining || *line_end == '\0')
            break;
        line_start = line_end + 1;
        remaining -= line_len + 1u;
        pen_y += line_height;
    }
}

void gui_draw_text_rect_clipped(Surface *s, const GuiFont *font, int32_t x, int32_t y, int32_t max_width,
                                int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h, const char *str,
                                uint32_t fg, uint32_t bg)
{
    if (!s || !s->buffer || !str || max_width <= 0 || clip_w <= 0 || clip_h <= 0)
        return;
    if (!font)
        return;

    char safe_text[256];
    size_t safe_len = gui_bounded_line_length(str, sizeof(safe_text) - 1u);
    for (size_t i = 0; i < safe_len; i++)
        safe_text[i] = str[i];
    safe_text[safe_len] = '\0';

    char clipped[256];
    const char *text = safe_text;
    if (gui_measure_text(font, safe_text) > max_width) {
        size_t len = gui_truncate_text(font, safe_text, max_width, clipped, sizeof(clipped));
        if (len == 0)
            return;
        text = clipped;
    }

    draw_text_run_clipped(s, font, x, y, text, fg, bg, font->alpha_lut, clip_x, clip_y, clip_w, clip_h);
}

void gui_draw_mono_cell(Surface *s, const GuiFont *font, int32_t x, int32_t y, int32_t cell_w, int32_t cell_h, char c,
                        uint32_t fg, uint32_t bg)
{
    if (!s || !s->buffer || cell_w <= 0 || cell_h <= 0)
        return;
    if (!font) {
        gui_draw_char(s, x, y, c, fg, bg);
        return;
    }

    const GuiGlyph *glyph = gui_font_find_glyph(font, (uint8_t)c);
    if (!glyph)
        return;

    int32_t line_h = gui_font_line_height(font);
    int32_t top_y = y + (cell_h - line_h) / 2;
    int32_t origin_x = x;
    draw_single_glyph(s, font, origin_x, top_y, glyph, fg, bg, font->alpha_lut, x, y, cell_w, cell_h);
}
}
