#include "media_codec_internal.h"

namespace {

struct gif_lzw
{
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
};

uint32_t gif_lzw_code(gif_lzw *l, int bits)
{
    while (l->bitcnt < bits) {
        if (l->pos >= l->size)
            return 0xFFFFFFFFu;
        l->bitbuf |= static_cast<uint32_t>(l->data[l->pos++]) << l->bitcnt;
        l->bitcnt += 8;
    }
    uint32_t v = l->bitbuf & ((1u << bits) - 1u);
    l->bitbuf >>= bits;
    l->bitcnt -= bits;
    return v;
}

constexpr int GIF_MAX_CODES = 4096;

bool gif_lzw_decode(const uint8_t *data, size_t size, int min_code_size, uint8_t *out, uint64_t out_cap,
                    uint64_t *out_len)
{
    if (min_code_size < 2 || min_code_size > 11)
        return false;

    int prefix[GIF_MAX_CODES];
    uint8_t suffix[GIF_MAX_CODES];
    uint8_t stack[GIF_MAX_CODES];

    gif_lzw l = {data, size, 0, 0, 0};
    const uint32_t clear = 1u << min_code_size;
    const uint32_t end = clear + 1;

    int bits = min_code_size + 1;
    uint32_t next = 0;
    int prev = -1;
    *out_len = 0;

    auto reset_table = [&]() {
        next = end + 1;
        bits = min_code_size + 1;
        prev = -1;
    };
    reset_table();

    for (;;) {
        uint32_t code = gif_lzw_code(&l, bits);
        if (code == 0xFFFFFFFFu)
            return false;
        if (code == clear) {
            reset_table();
            continue;
        }
        if (code == end)
            return *out_len == out_cap;

        int emit_code;
        bool table_added = false;
        if (code < next) {
            emit_code = static_cast<int>(code);
        } else if (code == next && prev >= 0) {
            // KwKwK case: the entry is prev + first byte of prev, and it is
            // the only table entry added for this code. Walk to the literal
            // root to find that first byte.
            int walk = prev;
            int depth = 0;
            while (walk >= static_cast<int>(clear)) {
                if (walk >= static_cast<int>(next) || ++depth > GIF_MAX_CODES)
                    return false;
                walk = prefix[walk];
            }
            if (next < GIF_MAX_CODES) {
                prefix[next] = prev;
                suffix[next] = static_cast<uint8_t>(walk);
                emit_code = static_cast<int>(next);
                next++;
                table_added = true;
            } else {
                // Full table: emit prev + first byte without storing it.
                emit_code = prev;
                int w2 = prev;
                uint32_t plen = 0;
                uint8_t pstack[GIF_MAX_CODES + 1];
                while (w2 >= static_cast<int>(clear)) {
                    if (w2 >= static_cast<int>(next) || plen >= GIF_MAX_CODES)
                        return false;
                    pstack[plen++] = suffix[w2];
                    w2 = prefix[w2];
                }
                if (*out_len + plen + 1 > out_cap)
                    return false;
                for (uint32_t i = plen; i > 0; i--)
                    out[(*out_len)++] = pstack[i - 1];
                out[(*out_len)++] = static_cast<uint8_t>(walk);
                prev = emit_code;
                continue;
            }
        } else {
            return false;
        }

        // Emit the string for emit_code (walk chain, reverse).
        uint32_t len = 0;
        int walk = emit_code;
        if (walk < static_cast<int>(clear)) {
            stack[len++] = static_cast<uint8_t>(walk);
        } else {
            while (walk >= static_cast<int>(clear)) {
                if (walk >= static_cast<int>(next) || len >= GIF_MAX_CODES)
                    return false;
                stack[len++] = suffix[walk];
                walk = prefix[walk];
            }
            stack[len++] = static_cast<uint8_t>(walk);
        }
        if (*out_len + len > out_cap)
            return false;
        for (uint32_t i = len; i > 0; i--)
            out[(*out_len)++] = stack[i - 1];

        // Normal case: add prev string + first byte of current string.
        if (prev >= 0 && !table_added) {
            int first_walk = emit_code;
            int depth = 0;
            while (first_walk >= static_cast<int>(clear)) {
                if (first_walk >= static_cast<int>(next) || ++depth > GIF_MAX_CODES)
                    return false;
                first_walk = prefix[first_walk];
            }
            if (next < GIF_MAX_CODES) {
                prefix[next] = prev;
                suffix[next] = static_cast<uint8_t>(first_walk);
                next++;
                if (next == (1u << bits) && bits < 12)
                    bits++;
            }
            // Table full: keep decoding without new entries (bits stay 12).
        } else if (table_added) {
            if (next == (1u << bits) && bits < 12)
                bits++;
        }
        prev = emit_code;
    }
}

bool gif_read_subblocks(media_reader *r, uint8_t **out_buf, size_t *out_len)
{
    size_t cap = 4096, len = 0;
    uint8_t *buf = static_cast<uint8_t *>(media_alloc(cap));
    if (!buf)
        return false;
    for (;;) {
        uint8_t sub_len;
        if (!media_read_u8(r, &sub_len)) {
            media_free(buf);
            return false;
        }
        if (sub_len == 0)
            break;
        if (len + sub_len > 64ull * 1024 * 1024) {
            media_free(buf);
            return false;
        }
        if (len + sub_len > cap) {
            size_t new_cap = cap * 2;
            while (new_cap < len + sub_len)
                new_cap *= 2;
            uint8_t *grown = static_cast<uint8_t *>(media_alloc(new_cap));
            if (!grown) {
                media_free(buf);
                return false;
            }
            for (size_t i = 0; i < len; i++)
                grown[i] = buf[i];
            media_free(buf);
            buf = grown;
            cap = new_cap;
        }
        const uint8_t *p;
        if (!media_read_bytes(r, &p, sub_len)) {
            media_free(buf);
            return false;
        }
        for (uint8_t i = 0; i < sub_len; i++)
            buf[len + i] = p[i];
        len += sub_len;
    }
    *out_buf = buf;
    *out_len = len;
    return true;
}

} // namespace

bool gif_decode(const uint8_t *data, size_t size, media_image *out)
{
    if (!out || size < 13)
        return false;
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F' || data[3] != '8' || (data[4] != '7' && data[4] != '9') ||
        data[5] != 'a')
        return false;

    media_reader r = {data, size, 6};
    uint16_t width, height;
    uint8_t packed;
    if (!media_read_le16(&r, &width) || !media_read_le16(&r, &height) || !media_read_u8(&r, &packed))
        return false;
    uint8_t background, aspect;
    if (!media_read_u8(&r, &background) || !media_read_u8(&r, &aspect))
        return false;
    if (width == 0 || height == 0 || width > MEDIA_MAX_DIMENSION || height > MEDIA_MAX_DIMENSION ||
        static_cast<uint64_t>(width) * height > MEDIA_MAX_PIXELS)
        return false;

    uint32_t gct[256];
    int gct_count = 0;
    if (packed & 0x80) {
        gct_count = 2 << (packed & 0x07);
        for (int i = 0; i < gct_count; i++) {
            uint8_t rgb[3];
            if (!media_read_u8(&r, &rgb[0]) || !media_read_u8(&r, &rgb[1]) || !media_read_u8(&r, &rgb[2]))
                return false;
            media_write_argb(&gct[i], rgb[0], rgb[1], rgb[2], 255);
        }
    }

    uint32_t *canvas = static_cast<uint32_t *>(media_alloc(static_cast<uint64_t>(width) * height * 4));
    if (!canvas)
        return false;
    uint32_t bg = (gct_count > 0 && background < gct_count) ? gct[background] : 0x00000000;
    for (uint64_t i = 0; i < static_cast<uint64_t>(width) * height; i++)
        canvas[i] = bg;

    int transparent_index = -1;
    bool found_image = false;

    for (;;) {
        uint8_t introducer;
        if (!media_read_u8(&r, &introducer))
            break;

        if (introducer == 0x3B) // trailer
            break;

        if (introducer == 0x21) { // extension
            uint8_t label;
            if (!media_read_u8(&r, &label))
                break;
            if (label == 0xF9) { // graphics control extension: one fixed 4-byte sub-block
                uint8_t len;
                if (!media_read_u8(&r, &len) || len != 4)
                    break;
                uint8_t gpacked, delay_lo, delay_hi, tindex, terminator;
                if (!media_read_u8(&r, &gpacked) || !media_read_u8(&r, &delay_lo) || !media_read_u8(&r, &delay_hi) ||
                    !media_read_u8(&r, &tindex) || !media_read_u8(&r, &terminator))
                    break;
                transparent_index = (gpacked & 0x01) ? tindex : -1;
                continue;
            }
            // Other extensions: skip their data sub-blocks.
            uint8_t *tmp;
            size_t tmp_len;
            if (!gif_read_subblocks(&r, &tmp, &tmp_len))
                break;
            media_free(tmp);
            continue;
        }

        if (introducer == 0x2C) { // image descriptor: decode the first frame
            uint16_t left, top, iw, ih;
            uint8_t ipacked;
            if (!media_read_le16(&r, &left) || !media_read_le16(&r, &top) || !media_read_le16(&r, &iw) ||
                !media_read_le16(&r, &ih) || !media_read_u8(&r, &ipacked))
                break;
            if (iw == 0 || ih == 0 || left + iw > width || top + ih > height)
                break;

            uint32_t lct[256];
            int lct_count = 0;
            const uint32_t *palette = gct;
            int palette_count = gct_count;
            if (ipacked & 0x80) {
                lct_count = 2 << (ipacked & 0x07);
                bool lct_ok = true;
                for (int i = 0; i < lct_count; i++) {
                    uint8_t rgb[3];
                    if (!media_read_u8(&r, &rgb[0]) || !media_read_u8(&r, &rgb[1]) || !media_read_u8(&r, &rgb[2])) {
                        lct_ok = false;
                        break;
                    }
                    media_write_argb(&lct[i], rgb[0], rgb[1], rgb[2], 255);
                }
                if (!lct_ok)
                    break;
                palette = lct;
                palette_count = lct_count;
            }

            uint8_t min_code_size;
            if (!media_read_u8(&r, &min_code_size))
                break;
            uint8_t *lzw_data;
            size_t lzw_len;
            if (!gif_read_subblocks(&r, &lzw_data, &lzw_len))
                break;

            uint64_t pixels_needed = static_cast<uint64_t>(iw) * ih;
            uint8_t *indices = static_cast<uint8_t *>(media_alloc(static_cast<size_t>(pixels_needed)));
            uint64_t got = 0;
            bool ok = indices && gif_lzw_decode(lzw_data, lzw_len, min_code_size, indices, pixels_needed, &got);
            media_free(lzw_data);
            if (!ok) {
                if (indices)
                    media_free(indices);
                break;
            }

            const bool interlaced = ipacked & 0x40;
            const int pass_start[4] = {0, 4, 2, 1};
            const int pass_step[4] = {8, 8, 4, 2};
            uint64_t src = 0;
            bool rendering = true;
            for (int pass = 0; pass < 4 && rendering; pass++) {
                if (!interlaced && pass > 0)
                    break;
                int start = interlaced ? pass_start[pass] : 0;
                int step = interlaced ? pass_step[pass] : 1;
                for (int y = start; y < ih && rendering; y += step) {
                    for (int x = 0; x < iw; x++) {
                        if (src >= pixels_needed) {
                            rendering = false;
                            break;
                        }
                        uint8_t idx = indices[src++];
                        if (idx >= palette_count)
                            continue;
                        // Transparent pixels keep their palette color with alpha cleared.
                        uint32_t col = palette[idx];
                        if (idx == transparent_index)
                            col &= 0x00FFFFFFu;
                        canvas[static_cast<uint64_t>(top + y) * width + (left + x)] = col;
                    }
                }
            }
            media_free(indices);
            found_image = true;
            break;
        }

        break; // unknown block
    }

    if (!found_image) {
        media_free(canvas);
        return false;
    }

    out->pixels = canvas;
    out->width = width;
    out->height = height;
    return true;
}
