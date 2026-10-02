#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../libc/log.h"
#include "font_internal.h"
#include "gui.h"

// UOF v2 loader and text rasterizer. The format is produced by
// tools/uof_convert.py: 8-bit coverage atlases baked from 4x4 supersampled
// FreeType renders, 26.6 fixed-point advances, GPOS kerning as a class
// matrix plus additive pair exceptions, and horizontally oversampled
// subcolumns for draw-time subpixel positioning.
//
// Layout mirrors the on-disk records exactly (packed, little-endian), so the
// glyph table is memcpy'd straight into place.

static constexpr uint32_t UOF_MAGIC = 0x4E464F55u; // "UOFN", little-endian
static constexpr uint16_t UOF_VERSION = 2;

struct UofHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint16_t pixel_size;
    uint16_t oversample_x;
    uint16_t atlas_width;
    uint16_t atlas_height;
    int16_t ascent;
    int16_t descent;
    int16_t line_gap;
    uint32_t glyph_count;
    uint32_t kerning_count;
    uint16_t matrix_c1;
    uint16_t matrix_c2;
    uint32_t fallback_index;
    uint32_t glyph_offset;
    uint32_t kerning_offset;
    uint32_t matrix_offset;
    uint32_t atlas_offset;
} __attribute__((packed));

struct UofGlyphRecord
{
    uint32_t codepoint;
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t width;
    uint16_t height;
    int16_t bearing_x26;
    int16_t bearing_y;
    int16_t advance_x26;
    uint8_t kern_left;
    uint8_t kern_right;
} __attribute__((packed));

struct UofKernRecord
{
    uint32_t left;
    uint32_t right;
    int16_t value;
} __attribute__((packed));

static_assert(sizeof(UofHeader) == 54, "UOF v2 header size");
static_assert(sizeof(UofGlyphRecord) == sizeof(GuiGlyph), "glyph record mirrors GuiGlyph");
static_assert(sizeof(UofKernRecord) == sizeof(GuiKern), "kern record mirrors GuiKern");

static GuiFont g_ui_font = {};
static GuiFont g_title_font = {};
static GuiFont g_mono_font = {};
static bool g_fonts_initialized = false;
static bool g_fonts_ready = false;

// Lazily loaded mono fonts for terminal zoom, indexed by k_font_sizes slot.
static GuiFont g_mono_zoom_fonts[11] = {};
static bool g_mono_zoom_loaded[11] = {};

static constexpr int k_font_sizes[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
static constexpr int k_font_size_count = (int)(sizeof(k_font_sizes) / sizeof(k_font_sizes[0]));
static constexpr size_t k_gui_text_scan_limit = 1024;
static constexpr uint32_t k_max_atlas_bytes = 512u * 1024u * 1024u;

static bool gui_font_load_from_file(GuiFont *font, const char *path);
static const GuiGlyph *gui_font_fallback_glyph(const GuiFont *font);
static size_t gui_bounded_line_length(const char *str, size_t limit);

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

static bool load_font_files_near(GuiFont *font, const char *prefix, int preferred_size, bool allow_below_11)
{
    if (!font || !prefix)
        return false;

    int target = allow_below_11 ? preferred_size : clamp_font_target(preferred_size);
    int preferred_index = nearest_font_index(target);
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

static bool load_nearest_font(GuiFont *font, const char *prefix, int preferred_size)
{
    return load_font_files_near(font, prefix, preferred_size, false);
}

// Like load_nearest_font but without the 11px floor, so terminal zoom-out can
// reach the smaller bundled sizes.
static bool load_font_at_size(GuiFont *font, const char *prefix, int pixel_size)
{
    return load_font_files_near(font, prefix, pixel_size, true);
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

// RLE atlas decode: (run_length-1, value) byte pairs expanding to exactly
// atlas_stride * atlas_height coverage bytes.
static bool uof_decode_atlas_rle(const uint8_t *stream, size_t stream_bytes, uint8_t *atlas, uint32_t atlas_bytes)
{
    if (stream_bytes % 2 != 0)
        return false;
    uint32_t out = 0;
    for (size_t i = 0; i + 1 < stream_bytes; i += 2) {
        uint32_t run = (uint32_t)stream[i] + 1u;
        if (run > atlas_bytes - out)
            return false;
        memset(atlas + out, stream[i + 1], run);
        out += run;
    }
    return out == atlas_bytes;
}

static bool gui_font_load_from_file(GuiFont *font, const char *path)
{
    if (!font || !path)
        return false;

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!gui_load_file(path, &data, &size))
        return false;

    bool ok = false;
    GuiGlyph *glyphs = nullptr;
    GuiKern *kern_pairs = nullptr;
    int16_t *matrix = nullptr;
    uint8_t *atlas = nullptr;

    do {
        if (size < sizeof(UofHeader))
            break;
        const UofHeader *header = reinterpret_cast<const UofHeader *>(data);
        if (header->magic != UOF_MAGIC || header->version != UOF_VERSION)
            break;
        if (header->oversample_x != 1u && header->oversample_x != 4u)
            break;
        if (header->glyph_count == 0 || header->fallback_index >= header->glyph_count)
            break;
        // Glyph indices are cached in int16_t (ascii_index); reject tables that
        // would wrap a stored index.
        if (header->glyph_count > 32767u)
            break;
        if (header->matrix_c1 > 255u || header->matrix_c2 > 255u)
            break;

        uint64_t glyph_bytes64 = (uint64_t)header->glyph_count * (uint64_t)sizeof(UofGlyphRecord);
        uint64_t kern_bytes64 = (uint64_t)header->kerning_count * (uint64_t)sizeof(UofKernRecord);
        uint64_t matrix_count64 = ((uint64_t)header->matrix_c1 + 1u) * ((uint64_t)header->matrix_c2 + 1u);
        uint64_t matrix_bytes64 = matrix_count64 * 2u;
        uint64_t glyph_end = (uint64_t)header->glyph_offset + glyph_bytes64;
        uint64_t kern_end = (uint64_t)header->kerning_offset + kern_bytes64;
        uint64_t matrix_end = (uint64_t)header->matrix_offset + matrix_bytes64;

        if (header->glyph_offset != sizeof(UofHeader) || header->glyph_offset >= size)
            break;
        if (header->kerning_offset != glyph_end || header->kerning_offset > size || kern_end > size)
            break;
        if (header->matrix_offset != kern_end || header->matrix_offset > size || matrix_end > size)
            break;
        if (header->atlas_offset != matrix_end || header->atlas_offset > size)
            break;
        if (glyph_bytes64 > 0xFFFFFFFFu || kern_bytes64 > 0xFFFFFFFFu || matrix_bytes64 > 0xFFFFFFFFu)
            break;

        uint32_t stride = (uint32_t)header->atlas_width * (uint32_t)header->oversample_x;
        uint64_t atlas_bytes64 = (uint64_t)stride * (uint64_t)header->atlas_height;
        if (atlas_bytes64 == 0 || atlas_bytes64 > k_max_atlas_bytes)
            break;
        uint32_t atlas_bytes = (uint32_t)atlas_bytes64;

        glyphs = static_cast<GuiGlyph *>(malloc(glyph_bytes64 == 0 ? 1 : (size_t)glyph_bytes64));
        kern_pairs = header->kerning_count ? static_cast<GuiKern *>(malloc((size_t)kern_bytes64)) : nullptr;
        matrix = static_cast<int16_t *>(malloc((size_t)matrix_bytes64));
        atlas = static_cast<uint8_t *>(malloc(atlas_bytes));
        if (!glyphs || !matrix || !atlas || (header->kerning_count && !kern_pairs))
            break;

        memcpy(glyphs, data + header->glyph_offset, (size_t)glyph_bytes64);
        if (kern_pairs)
            memcpy(kern_pairs, data + header->kerning_offset, (size_t)kern_bytes64);
        memcpy(matrix, data + header->matrix_offset, (size_t)matrix_bytes64);
        if (!uof_decode_atlas_rle(data + header->atlas_offset, size - header->atlas_offset, atlas, atlas_bytes))
            break;

        memset(font, 0, sizeof(*font));
        font->magic = header->magic;
        font->glyph_count = header->glyph_count;
        font->fallback_index = header->fallback_index;
        font->pixel_size = header->pixel_size;
        font->oversample_x = header->oversample_x;
        font->atlas_width = header->atlas_width;
        font->atlas_height = header->atlas_height;
        font->ascent = header->ascent;
        font->descent = header->descent;
        font->line_gap = header->line_gap;
        font->kern_pairs = kern_pairs;
        font->kern_count = header->kerning_count;
        font->kern_matrix = matrix;
        font->matrix_c1 = header->matrix_c1;
        font->matrix_c2 = header->matrix_c2;
        font->glyphs = glyphs;
        font->atlas = atlas;
        kern_pairs = nullptr;
        matrix = nullptr;
        glyphs = nullptr;
        atlas = nullptr;

        font->max_advance = 8;
        font->max_ink_width = 8;
        for (int i = 0; i < 128; i++)
            font->ascii_index[i] = -1;

        for (uint32_t i = 0; i < font->glyph_count; i++) {
            GuiGlyph &glyph = font->glyphs[i];
            // A glyph whose atlas rect escapes the atlas would read out of
            // bounds at draw time (corrupt .uof); blank its ink so it renders
            // nothing. Oversampled slots also need one subcolumn of headroom
            // left of atlas_x for the phase filter's negative taps.
            uint32_t rect_end_x = (uint32_t)glyph.atlas_x + (uint32_t)glyph.width * font->oversample_x;
            if (rect_end_x > stride || (uint32_t)glyph.atlas_y + glyph.height > font->atlas_height ||
                (font->oversample_x > 1u && glyph.atlas_x < font->oversample_x)) {
                glyph.width = 0;
                glyph.height = 0;
            }
            int16_t adv_px = (int16_t)((glyph.advance_x26 + 32) >> 6);
            if (adv_px > font->max_advance)
                font->max_advance = adv_px;
            if ((int)glyph.width > font->max_ink_width)
                font->max_ink_width = (int)glyph.width;
            if (glyph.codepoint < 128u)
                font->ascii_index[glyph.codepoint] = (int16_t)i;
        }
        if (font->max_advance <= 0)
            font->max_advance = 8;
        if (font->max_ink_width <= 0)
            font->max_ink_width = font->max_advance;

        font->line_height = (int16_t)(font->ascent + font->descent + font->line_gap);
        if (font->line_height <= 0)
            font->line_height = font->pixel_size > 0 ? (int16_t)font->pixel_size : 16;

        int32_t fallback_advance26 = font->max_advance * 64;
        if (const GuiGlyph *fallback = gui_font_fallback_glyph(font)) {
            if (fallback->advance_x26 > 0)
                fallback_advance26 = fallback->advance_x26;
        }

        for (int i = 0; i < 128; i++)
            font->ascii_advance26[i] = (int16_t)fallback_advance26;
        for (uint32_t i = 0; i < font->glyph_count; i++) {
            const GuiGlyph &glyph = font->glyphs[i];
            if (glyph.codepoint < 128u)
                font->ascii_advance26[glyph.codepoint] =
                    glyph.advance_x26 > 0 ? glyph.advance_x26 : (int16_t)fallback_advance26;
        }
        ok = true;
    } while (false);

    free(glyphs);
    free(kern_pairs);
    free(matrix);
    free(atlas);
    if (!ok)
        memset(font, 0, sizeof(*font));
    free(data);
    return ok;
}

static const GuiGlyph *gui_font_find_glyph(const GuiFont *font, uint32_t codepoint)
{
    if (!font || !font->glyphs || font->glyph_count == 0)
        return nullptr;
    if (codepoint < 128u) {
        int16_t idx = font->ascii_index[codepoint];
        if (idx >= 0 && (uint32_t)idx < font->glyph_count)
            return &font->glyphs[idx];
        // Non-ASCII-mapped control chars fall through to the table search,
        // then the fallback glyph.
    }
    // Glyphs are stored sorted by codepoint: binary search.
    uint32_t lo = 0;
    uint32_t hi = font->glyph_count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        uint32_t cp = font->glyphs[mid].codepoint;
        if (cp == codepoint)
            return &font->glyphs[mid];
        if (cp < codepoint)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (font->fallback_index < font->glyph_count)
        return &font->glyphs[font->fallback_index];
    return &font->glyphs[0];
}

static const GuiGlyph *gui_font_fallback_glyph(const GuiFont *font)
{
    if (!font || !font->glyphs || font->glyph_count == 0)
        return nullptr;
    if (font->fallback_index < font->glyph_count)
        return &font->glyphs[font->fallback_index];
    return &font->glyphs[0];
}

// Kern between two adjacent glyphs: class matrix hit plus an additive
// exception pair (kept sorted for binary search). Both contributions are
// 26.6; missing entries contribute nothing.
static int32_t gui_font_kern26(const GuiFont *font, const GuiGlyph *left, const GuiGlyph *right, uint32_t left_cp,
                               uint32_t right_cp)
{
    if (!font || !left || !right)
        return 0;
    int32_t total = 0;
    if (left->kern_left && right->kern_right)
        total = font->kern_matrix[(uint32_t)left->kern_left * (uint32_t)(font->matrix_c2 + 1) + right->kern_right];
    if (font->kern_count) {
        uint32_t lo = 0;
        uint32_t hi = font->kern_count;
        while (lo < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1);
            const GuiKern &pair = font->kern_pairs[mid];
            if (pair.left == left_cp && pair.right == right_cp)
                return total + pair.value;
            if (pair.left < left_cp || (pair.left == left_cp && pair.right < right_cp))
                lo = mid + 1;
            else
                hi = mid;
        }
    }
    return total;
}

// Decode one UTF-8 codepoint. Invalid sequences (including overlong encodings
// and truncated input) decode to U+FFFD and consume a single byte, matching
// the fallback-glyph tolerance of the byte-oriented v1 renderer.
static uint32_t utf8_decode(const char *str, const char *end, size_t *consumed)
{
    *consumed = 1;
    uint8_t first = (uint8_t)str[0];
    if (first < 0x80u)
        return first;
    size_t len = 0;
    uint32_t cp = 0;
    if ((first & 0xE0u) == 0xC0u) {
        len = 2;
        cp = first & 0x1Fu;
    } else if ((first & 0xF0u) == 0xE0u) {
        len = 3;
        cp = first & 0x0Fu;
    } else if ((first & 0xF8u) == 0xF0u) {
        len = 4;
        cp = first & 0x07u;
    } else {
        return 0xFFFDu;
    }
    if (str + len > end)
        return 0xFFFDu;
    for (size_t i = 1; i < len; i++) {
        uint8_t cont = (uint8_t)str[i];
        if ((cont & 0xC0u) != 0x80u)
            return 0xFFFDu;
        cp = (cp << 6) | (cont & 0x3Fu);
    }
    // Reject overlong encodings, surrogates and out-of-range codepoints.
    static const uint32_t k_min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < k_min_cp[len] || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        return 0xFFFDu;
    *consumed = len;
    return cp;
}

size_t gui_text_walk_init(GuiTextWalk *w, const GuiFont *font, const char *str, size_t len)
{
    if (!w)
        return 0;
    w->font = font;
    w->it = str;
    w->prev_glyph = nullptr;
    w->prev_cp = 0;
    w->pen26 = 0;
    size_t run = gui_bounded_line_length(str, len);
    w->end = str ? str + run : nullptr;
    return run;
}

const GuiGlyph *gui_text_walk_next(GuiTextWalk *w, uint32_t *codepoint, int32_t *pos26, size_t *consumed)
{
    if (!w || !w->it || w->it >= w->end)
        return nullptr;

    size_t len = 1;
    uint32_t cp = utf8_decode(w->it, w->end, &len);
    const GuiGlyph *glyph = w->font ? gui_font_find_glyph(w->font, cp) : nullptr;
    if (glyph && w->prev_glyph)
        w->pen26 += gui_font_kern26(w->font, w->prev_glyph, glyph, w->prev_cp, cp);

    if (codepoint)
        *codepoint = cp;
    if (pos26)
        *pos26 = w->pen26;
    if (consumed)
        *consumed = len;

    if (glyph)
        w->pen26 += glyph->advance_x26;
    else
        w->pen26 += 8 * 64; // null font: built-in 8x8 cell per byte
    w->it += len;
    w->prev_glyph = glyph;
    w->prev_cp = cp;
    return glyph;
}

int32_t gui_text_advance26(const GuiFont *font, const char *str, size_t len)
{
    GuiTextWalk walk;
    gui_text_walk_init(&walk, font, str, len);
    uint32_t cp = 0;
    int32_t pos26 = 0;
    size_t consumed = 0;
    while (gui_text_walk_next(&walk, &cp, &pos26, &consumed) != nullptr) {
    }
    return walk.pen26;
}

static inline uint8_t effective_color_alpha(uint32_t color)
{
    uint8_t alpha = (uint8_t)(color >> 24);
    if (alpha != 0)
        return alpha;
    return color == 0 ? 0u : 255u;
}

// sRGB <-> linear-light lookup tables (8-bit). Blending is performed in linear
// light so anti-aliased glyph edges composite with correct perceptual weights,
// eliminating the gamma-blend asymmetry (text rendering too thin on dark
// backgrounds, too heavy on light ones). Derived from the IEC 61966-2-1 sRGB
// transfer function: for v in [0,255], u = v/255, lin = u<=0.04045 ? u/12.92 :
// ((u+0.055)/1.055)^2.4; and the inverse for linear->sRGB. 8-bit resolution is
// exact at the 0/255 endpoints and loses only sub-percent detail in the deep
// shadows, imperceptible for text edges.
static const uint8_t s_srgb_to_linear[256] = {
    0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,
    2,   2,   2,   2,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
    6,   7,   7,   7,   8,   8,   8,   8,   9,   9,   9,   10,  10,  10,  11,  11,  12,  12,  12,  13,  13,  13,
    14,  14,  15,  15,  16,  16,  17,  17,  17,  18,  18,  19,  19,  20,  20,  21,  22,  22,  23,  23,  24,  24,
    25,  25,  26,  27,  27,  28,  29,  29,  30,  30,  31,  32,  32,  33,  34,  35,  35,  36,  37,  37,  38,  39,
    40,  41,  41,  42,  43,  44,  45,  45,  46,  47,  48,  49,  50,  51,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  76,  77,  78,  79,  80,  81,
    82,  84,  85,  86,  87,  88,  90,  91,  92,  93,  95,  96,  97,  99,  100, 101, 103, 104, 105, 107, 108, 109,
    111, 112, 114, 115, 116, 118, 119, 121, 122, 124, 125, 127, 128, 130, 131, 133, 134, 136, 138, 139, 141, 142,
    144, 146, 147, 149, 151, 152, 154, 156, 157, 159, 161, 163, 164, 166, 168, 170, 171, 173, 175, 177, 179, 181,
    183, 184, 186, 188, 190, 192, 194, 196, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216, 218, 220, 222, 224,
    226, 229, 231, 233, 235, 237, 239, 242, 244, 246, 248, 250, 253, 255};
static const uint8_t s_linear_to_srgb[256] = {
    0,   13,  22,  28,  34,  38,  42,  46,  50,  53,  56,  59,  61,  64,  66,  69,  71,  73,  75,  77,  79,  81,
    83,  85,  86,  88,  90,  92,  93,  95,  96,  98,  99,  101, 102, 104, 105, 106, 108, 109, 110, 112, 113, 114,
    115, 117, 118, 119, 120, 121, 122, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
    139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 148, 149, 150, 151, 152, 153, 154, 155, 155, 156, 157, 158,
    159, 159, 160, 161, 162, 163, 163, 164, 165, 166, 167, 167, 168, 169, 170, 170, 171, 172, 173, 173, 174, 175,
    175, 176, 177, 178, 178, 179, 180, 180, 181, 182, 182, 183, 184, 185, 185, 186, 187, 187, 188, 189, 189, 190,
    190, 191, 192, 192, 193, 194, 194, 195, 196, 196, 197, 197, 198, 199, 199, 200, 200, 201, 202, 202, 203, 203,
    204, 205, 205, 206, 206, 207, 208, 208, 209, 209, 210, 210, 211, 212, 212, 213, 213, 214, 214, 215, 215, 216,
    216, 217, 218, 218, 219, 219, 220, 220, 221, 221, 222, 222, 223, 223, 224, 224, 225, 226, 226, 227, 227, 228,
    228, 229, 229, 230, 230, 231, 231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 238,
    239, 239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 246, 247, 247, 248, 248, 249,
    249, 250, 250, 251, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255};

// Polarity-aware coverage gamma. Edges are linear-blended for perceptual
// correctness, but a light glyph on a dark backdrop leaves bright fringes that
// read as a bold/blurry halo, while a dark glyph on a light backdrop only
// wants slight extra sharpening. So the linear coverage ramp is shaped per
// polarity before blending: a strong gamma (2.0) pulls light-on-dark fringes
// toward black, a mild gamma (1.3) gives dark-on-light a stricter edge.
// Derived as round(pow(c/255, g) * 255); exact at the 0/255 endpoints.
static const uint8_t s_gamma_light_on_dark[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,
    2,   2,   2,   2,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,   7,   7,   7,
    8,   8,   8,   9,   9,   9,   10,  10,  11,  11,  11,  12,  12,  13,  13,  14,  14,  15,  15,  16,  16,  17,
    17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  23,  23,  24,  24,  25,  26,  26,  27,  28,  28,  29,  30,
    30,  31,  32,  32,  33,  34,  35,  35,  36,  37,  38,  38,  39,  40,  41,  42,  42,  43,  44,  45,  46,  47,
    47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,
    68,  69,  70,  71,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  84,  85,  86,  87,  88,  89,  91,  92,
    93,  94,  95,  97,  98,  99,  100, 102, 103, 104, 105, 107, 108, 109, 111, 112, 113, 115, 116, 117, 119, 120,
    121, 123, 124, 126, 127, 128, 130, 131, 133, 134, 136, 137, 139, 140, 142, 143, 145, 146, 148, 149, 151, 152,
    154, 155, 157, 158, 160, 162, 163, 165, 166, 168, 170, 171, 173, 175, 176, 178, 180, 181, 183, 185, 186, 188,
    190, 192, 193, 195, 197, 199, 200, 202, 204, 206, 207, 209, 211, 213, 215, 217, 218, 220, 222, 224, 226, 228,
    230, 232, 233, 235, 237, 239, 241, 243, 245, 247, 249, 251, 253, 255};

static inline uint32_t color_luma(uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFFu;
    uint32_t g = (c >> 8) & 0xFFu;
    uint32_t b = c & 0xFFu;
    return (r * 54u + g * 183u + b * 19u + 128u) >> 8;
}

// Apply coverage shaping only to light text on a dark backdrop: the bright AA
// fringes of a light glyph read as a bold/blurry halo, so gamma 2.0 pulls them
// toward the background. Dark text on a light backdrop is left unshaped (pure
// linear blend) -- any coverage gamma crushes thin strokes disproportionately
// and makes weight uneven, and dark-on-light already composites correctly in
// linear light. A transparent backdrop has unknown polarity -> no shaping.
static inline const uint8_t *select_gamma_lut(uint32_t fg, uint32_t bg, bool opaque_bg)
{
    if (opaque_bg && color_luma(fg) > color_luma(bg))
        return s_gamma_light_on_dark;
    return nullptr;
}

// Subpixel phase filter weights for the 4x oversampled atlas. The pen's
// fractional part is quantized to 1/16 px (16 phases); phase p shifts the
// sampling window by tf = p/4 subcolumns with a linear crossfade r = (p%4)/4:
// taps at subcols [4*col - tf - 1 .. 4*col - tf + 3] weighted (r, 1, 1, 1,
// 1-r) in 1/64 units, so coverage = sum(weights * subcols) >> 8. The baking
// pad plus the packer's 2 px slot gap keep every tap addressable.
static const uint8_t s_fir_weights[16][5] = {
    {0, 64, 64, 64, 64}, {16, 64, 64, 64, 48}, {32, 64, 64, 64, 32}, {48, 64, 64, 64, 16},
    {0, 64, 64, 64, 64}, {16, 64, 64, 64, 48}, {32, 64, 64, 64, 32}, {48, 64, 64, 64, 16},
    {0, 64, 64, 64, 64}, {16, 64, 64, 64, 48}, {32, 64, 64, 64, 32}, {48, 64, 64, 64, 16},
    {0, 64, 64, 64, 64}, {16, 64, 64, 64, 48}, {32, 64, 64, 64, 32}, {48, 64, 64, 64, 16},
};

static void draw_single_glyph(Surface *s, const GuiFont *font, int32_t origin26, int32_t top_y, const GuiGlyph *glyph,
                              uint32_t fg, uint32_t bg, int32_t clip_x = 0, int32_t clip_y = 0, int32_t clip_w = -1,
                              int32_t clip_h = -1)
{
    if (!s || !s->buffer || !font || !glyph || !font->atlas)
        return;

    uint32_t fg_alpha = effective_color_alpha(fg);
    if (fg_alpha == 0)
        return;

    // Pen plus bearing gives the glyph origin in 26.6. Oversampled fonts
    // floor to the pixel grid and keep the fraction as the FIR phase;
    // integer-grid fonts (mono) round to the nearest pixel.
    int32_t pos26 = origin26 + glyph->bearing_x26;
    int32_t dest_x;
    uint32_t phase = 0;
    if (font->oversample_x > 1u) {
        dest_x = pos26 >> 6;
        phase = (uint32_t)(pos26 & 63u) >> 2; // 0..15, 1/16 px steps
    } else {
        dest_x = (pos26 + 32) >> 6;
    }
    int32_t dest_y = top_y + font->ascent - glyph->bearing_y;
    int32_t start_col = 0;
    int32_t start_row = 0;
    int32_t end_col = glyph->width;
    int32_t end_row = glyph->height;

    // Evaluate standard rect clips relative to glyph origin
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

    // Bounds limit relative to surface
    if (dest_x < 0)
        start_col = dest_x < -start_col ? -dest_x : start_col;
    if (dest_y < 0)
        start_row = dest_y < -start_row ? -dest_y : start_row;
    if (dest_x + end_col > (int32_t)s->width)
        end_col = (int32_t)s->width - dest_x;
    if (dest_y + end_row > (int32_t)s->height)
        end_row = (int32_t)s->height - dest_y;

    if (start_col >= end_col || start_row >= end_row)
        return;

    uint32_t stride = s->pitch / 4;
    uint32_t atlas_stride = (uint32_t)font->atlas_width * font->oversample_x;

    // Linear-light alpha blending. fg/bg are sRGB-encoded; lift channels to
    // linear via LUT, blend by the (linear) coverage fraction, encode back.
    // When bg is fully opaque the destination is assumed to already equal bg
    // (callers pre-fill), so the blend is write-only -- no dst read, no cache
    // pollution. This is the fast path for terminal cells and filled labels.
    bool opaque_bg = (effective_color_alpha(bg) == 255u);
    uint8_t fr = s_srgb_to_linear[(fg >> 16) & 0xFFu];
    uint8_t fg_g = s_srgb_to_linear[(fg >> 8) & 0xFFu];
    uint8_t fb = s_srgb_to_linear[fg & 0xFFu];
    uint8_t br = 0, bg_g = 0, bb = 0;
    if (opaque_bg) {
        br = s_srgb_to_linear[(bg >> 16) & 0xFFu];
        bg_g = s_srgb_to_linear[(bg >> 8) & 0xFFu];
        bb = s_srgb_to_linear[bg & 0xFFu];
    }
    uint32_t fg_opaque = 0xFF000000u | (fg & 0x00FFFFFFu);
    const uint8_t *gamma = select_gamma_lut(fg, bg, opaque_bg);
    const uint8_t *weights = s_fir_weights[phase];
    int32_t tap0 = -((int32_t)(phase >> 2) + 1); // first tap subcolumn offset within the glyph slot
    bool oversampled = (font->oversample_x > 1u);

    for (int32_t row = start_row; row < end_row; row++) {
        uint32_t *dst = &s->buffer[(uint32_t)(dest_y + row) * stride + (uint32_t)(dest_x + start_col)];
        const uint8_t *atlas =
            &font->atlas[(uint32_t)(glyph->atlas_y + row) * atlas_stride + (uint32_t)(glyph->atlas_x + start_col)];
        for (int32_t col = start_col; col < end_col; col++) {
            uint8_t raw_coverage;
            if (oversampled) {
                // 5-tap FIR over the subcolumns of this output pixel,
                // shifted by the phase; the taps stay inside the glyph slot
                // (baked 1 px pad + packer's 2 px gap).
                const uint8_t *taps = atlas + (col - start_col) * 4 + tap0;
                uint32_t acc = (uint32_t)weights[0] * taps[0] + (uint32_t)weights[1] * taps[1] +
                               (uint32_t)weights[2] * taps[2] + (uint32_t)weights[3] * taps[3] +
                               (uint32_t)weights[4] * taps[4];
                raw_coverage = (uint8_t)(acc >> 8);
            } else {
                raw_coverage = atlas[col - start_col];
            }
            uint8_t coverage = gamma ? gamma[raw_coverage] : raw_coverage;
            if (coverage == 0) {
                dst++;
                continue;
            }

            // Composite linear alpha: fg_alpha * coverage (both linear).
            uint32_t alpha = (fg_alpha * coverage + 255u) >> 8;

            if (alpha >= 254u) {
                *dst++ = fg_opaque;
                continue;
            }

            uint32_t inv = 255u - alpha;
            uint32_t or_l, og_l, ob_l;
            if (opaque_bg) {
                or_l = (fr * alpha + br * inv + 127u) / 255u;
                og_l = (fg_g * alpha + bg_g * inv + 127u) / 255u;
                ob_l = (fb * alpha + bb * inv + 127u) / 255u;
            } else {
                uint32_t d = *dst;
                uint32_t dr = s_srgb_to_linear[(d >> 16) & 0xFFu];
                uint32_t dg = s_srgb_to_linear[(d >> 8) & 0xFFu];
                uint32_t db = s_srgb_to_linear[d & 0xFFu];
                or_l = (fr * alpha + dr * inv + 127u) / 255u;
                og_l = (fg_g * alpha + dg * inv + 127u) / 255u;
                ob_l = (fb * alpha + db * inv + 127u) / 255u;
            }
            *dst++ = 0xFF000000u | ((uint32_t)s_linear_to_srgb[or_l] << 16) | ((uint32_t)s_linear_to_srgb[og_l] << 8) |
                     (uint32_t)s_linear_to_srgb[ob_l];
        }
    }
}

static void draw_text_run_clipped(Surface *s, const GuiFont *font, int32_t x, int32_t y, const char *str, uint32_t fg,
                                  uint32_t bg, int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h)
{
    if (!s || !s->buffer || !font || !str)
        return;

    GuiTextWalk walk;
    gui_text_walk_init(&walk, font, str, k_gui_text_scan_limit);
    uint32_t cp = 0;
    int32_t pos26 = 0;
    size_t consumed = 0;
    const GuiGlyph *glyph;
    while ((glyph = gui_text_walk_next(&walk, &cp, &pos26, &consumed)) != nullptr) {
        draw_single_glyph(s, font, x * 64 + pos26, y, glyph, fg, bg, clip_x, clip_y, clip_w, clip_h);
        if (clip_w >= 0 && (walk.pen26 >> 6) + x >= clip_x + clip_w)
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

// Mono font at a requested pixel size (terminal zoom). Loaded on first use and
// cached per size index, so repeated zoom steps do not re-read the .uof.
const GuiFont *gui_font_mono_size(int pixel_size)
{
    gui_fonts_init();
    if (!g_fonts_ready)
        return nullptr;

    int idx = nearest_font_index(pixel_size);
    if (idx < 0)
        idx = 0;
    if (idx >= k_font_size_count)
        idx = k_font_size_count - 1;

    if (!g_mono_zoom_loaded[idx]) {
        if (!load_font_at_size(&g_mono_zoom_fonts[idx], "geist-mono", k_font_sizes[idx]))
            return &g_mono_font;
        g_mono_zoom_loaded[idx] = true;
    }
    return &g_mono_zoom_fonts[idx];
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
        return (int)(len * 8u);

    return (gui_text_advance26(font, str, len) + 32) >> 6;
}

void gui_draw_text(Surface *s, const GuiFont *font, int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg)
{
    if (!s || !s->buffer || !str || !font)
        return;

    const char *line_start = str;
    int32_t pen_y = y;
    int line_height = gui_font_line_height(font);
    bool paint_bg = effective_color_alpha(bg) != 0;
    size_t remaining = k_gui_text_scan_limit;

    while (remaining > 0 && *line_start) {
        size_t line_len = gui_bounded_line_length(line_start, remaining);

        if (line_len > 0) {
            if (paint_bg) {
                int line_width = (gui_text_advance26(font, line_start, line_len) + 32) >> 6;
                if (line_width > 0)
                    gui_fill_rect(s, x, pen_y, line_width, line_height, bg);
            }

            GuiTextWalk walk;
            gui_text_walk_init(&walk, font, line_start, line_len);
            uint32_t cp = 0;
            int32_t pos26 = 0;
            size_t consumed = 0;
            const GuiGlyph *glyph;
            while ((glyph = gui_text_walk_next(&walk, &cp, &pos26, &consumed)) != nullptr)
                draw_single_glyph(s, font, x * 64 + pos26, pen_y, glyph, fg, bg);
        }

        if (line_len >= remaining || line_start[line_len] == '\0')
            break;
        line_start += line_len + 1;
        remaining -= line_len + 1u;
        pen_y += line_height;
    }
}

void gui_draw_text_rect_clipped(Surface *s, const GuiFont *font, int32_t x, int32_t y, int32_t max_width,
                                int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h, const char *str,
                                uint32_t fg, uint32_t bg)
{
    if (!s || !s->buffer || !str || max_width <= 0 || clip_w <= 0 || clip_h <= 0 || !font)
        return;

    char safe_text[512] = {};
    size_t safe_len = gui_bounded_line_length(str, sizeof(safe_text) - 1u);
    for (size_t i = 0; i < safe_len; i++)
        safe_text[i] = str[i];
    safe_text[safe_len] = '\0';

    char clipped[512];
    const char *text = safe_text;
    if (gui_measure_text(font, safe_text) > max_width) {
        size_t len = gui_truncate_text(font, safe_text, max_width, clipped, sizeof(clipped));
        if (len == 0)
            return;
        text = clipped;
    }

    draw_text_run_clipped(s, font, x, y, text, fg, bg, clip_x, clip_y, clip_w, clip_h);
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
    draw_single_glyph(s, font, x * 64, top_y, glyph, fg, bg, x, y, cell_w, cell_h);
}
}
