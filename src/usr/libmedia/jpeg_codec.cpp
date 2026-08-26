#include "media_codec_internal.h"

namespace {

constexpr int JPEG_MAX_COMPONENTS = 3;

struct jpeg_huff
{
    uint8_t counts[17];
    uint8_t symbols[256];
    int symbol_count;
};

bool jpeg_huff_build(jpeg_huff *h, const uint8_t *values, int total)
{
    if (total > 256)
        return false;
    int left = 1;
    for (int len = 1; len <= 16; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0)
            return false;
    }
    // DHT lists values in ascending code-length order, which is exactly the
    // canonical ordering the incremental decoder expects.
    for (int i = 0; i < total; i++)
        h->symbols[i] = values[i];
    h->symbol_count = total;
    return true;
}

struct jpeg_bits
{
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
    bool error;
    bool marker;
};

void jpeg_bits_init(jpeg_bits *b, const uint8_t *data, size_t size)
{
    b->data = data;
    b->size = size;
    b->pos = 0;
    b->bitbuf = 0;
    b->bitcnt = 0;
    b->error = false;
    b->marker = false;
}

void jpeg_bits_fill(jpeg_bits *b)
{
    while (b->bitcnt <= 24) {
        if (b->pos >= b->size) {
            // Pad with 1-bits per the JPEG spec at end of stream.
            b->bitbuf |= 0xFFu << (24 - b->bitcnt);
            b->bitcnt += 8;
            continue;
        }
        uint8_t byte = b->data[b->pos++];
        if (byte == 0xFF) {
            if (b->pos < b->size && b->data[b->pos] == 0x00) {
                b->pos++; // stuffing: 0xFF 0x00 encodes 0xFF
            } else {
                // Real marker: rewind before the FF and pad with 1-bits.
                b->pos--;
                b->marker = true;
                b->bitbuf |= 0xFFu << (24 - b->bitcnt);
                b->bitcnt += 8;
                continue;
            }
        }
        b->bitbuf |= static_cast<uint32_t>(byte) << (24 - b->bitcnt);
        b->bitcnt += 8;
    }
}

uint32_t jpeg_bits_peek(jpeg_bits *b, int n)
{
    if (b->bitcnt < n)
        jpeg_bits_fill(b);
    return b->bitbuf >> (32 - n);
}

void jpeg_bits_skip(jpeg_bits *b, int n)
{
    b->bitbuf <<= n;
    b->bitcnt -= n;
}

uint32_t jpeg_bits_get(jpeg_bits *b, int n)
{
    uint32_t v = jpeg_bits_peek(b, n);
    jpeg_bits_skip(b, n);
    return v;
}

void jpeg_bits_align(jpeg_bits *b)
{
    int discard = b->bitcnt & 7;
    jpeg_bits_skip(b, discard);
}

int jpeg_huff_decode(jpeg_bits *b, const jpeg_huff *h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | static_cast<int>(jpeg_bits_get(b, 1));
        int count = h->counts[len];
        int offset = code - first;
        if (offset >= 0 && offset < count) {
            int sym = index + offset;
            if (sym >= 0 && sym < h->symbol_count)
                return h->symbols[sym];
            return -1;
        }
        index += count;
        first = (first + count) << 1;
    }
    return -1;
}

int jpeg_receive_extend(jpeg_bits *b, int t)
{
    if (t == 0)
        return 0;
    uint32_t v = jpeg_bits_get(b, t);
    if (v < (1u << (t - 1)))
        return static_cast<int>(v) - (1 << t) + 1;
    return static_cast<int>(v);
}

const uint8_t jpeg_zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// 12-bit fixed-point cosines: cos((2x+1)u*pi/16) * 4096, plus c(0)=1/sqrt(2).
const int16_t jpeg_cos[8][8] = {
    {4096, 4017, 3784, 3406, 2896, 2276, 1567, 799},     {4096, 3406, 1567, -799, -2896, -4017, -3784, -2276},
    {4096, 2276, -1567, -4017, -2896, 799, 3784, 3406},  {4096, 799, -3784, -2276, 2896, 3406, -1567, -4017},
    {4096, -799, -3784, 2276, 2896, -3406, -1567, 4017}, {4096, -2276, -1567, 4017, -2896, -799, 3784, -3406},
    {4096, -3406, 1567, 799, -2896, 4017, -3784, 2276},  {4096, -4017, 3784, -3406, 2896, -2276, 1567, -799},
};
constexpr int JPEG_C0 = 2896; // 1/sqrt(2) * 4096

void jpeg_idct_block(const int32_t *coef, uint8_t *out8)
{
    // Separable naive IDCT in 12-bit fixed point. coef is dequantized.
    // Stage 1 leaves one 12-bit fraction scale in tmp; stage 2 divides out
    // three 12-bit factors plus the 1/4 IDCT scale (36 + 2 bits).
    int64_t tmp[8][8];
    for (int v = 0; v < 8; v++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++) {
                int64_t c = coef[v * 8 + u];
                if (c == 0)
                    continue;
                int64_t scale = u == 0 ? JPEG_C0 : 4096;
                sum += c * scale * jpeg_cos[x][u];
            }
            tmp[v][x] = sum >> 12;
        }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int v = 0; v < 8; v++) {
                int64_t c = tmp[v][x];
                if (c == 0)
                    continue;
                int64_t scale = v == 0 ? JPEG_C0 : 4096;
                sum += c * scale * jpeg_cos[y][v];
            }
            int32_t val = static_cast<int32_t>(((sum + (1LL << 37)) >> 38) + 128);
            out8[y * 8 + x] = val < 0 ? 0 : (val > 255 ? 255 : static_cast<uint8_t>(val));
        }
    }
}

struct jpeg_component
{
    int id;
    int h, v;
    int tq;
    int dc_table, ac_table;
    int dc_pred;
    uint8_t *plane;
    int32_t plane_w, plane_h;
};

struct jpeg_state
{
    uint32_t width, height;
    int comp_count;
    jpeg_component comp[JPEG_MAX_COMPONENTS];
    int h_max, v_max;
    uint8_t quant[4][64];
    bool have_quant[4];
    jpeg_huff huff[2][4]; // [dc/ac][slot]
    bool have_huff[2][4];
    uint16_t restart_interval;
};

bool jpeg_decode_block(jpeg_bits *b, jpeg_state *s, jpeg_component *c, uint8_t *block_out)
{
    const jpeg_huff *dc = &s->huff[0][c->dc_table];
    const jpeg_huff *ac = &s->huff[1][c->ac_table];
    if (!s->have_huff[0][c->dc_table] || !s->have_huff[1][c->ac_table] || !s->have_quant[c->tq])
        return false;

    int32_t coef[64] = {};
    int t = jpeg_huff_decode(b, dc);
    if (t < 0 || t > 15)
        return false;
    int dc_val = c->dc_pred + jpeg_receive_extend(b, t);
    c->dc_pred = dc_val;
    coef[0] = dc_val * static_cast<int32_t>(s->quant[c->tq][0]);

    int k = 1;
    while (k < 64) {
        int rs = jpeg_huff_decode(b, ac);
        if (rs < 0)
            return false;
        int run = rs >> 4;
        int size = rs & 0x0F;
        if (size == 0) {
            if (run == 15) { // ZRL
                k += 16;
                continue;
            }
            break; // EOB
        }
        k += run;
        if (k >= 64)
            return false;
        int level = jpeg_receive_extend(b, size);
        coef[jpeg_zigzag[k]] = level * static_cast<int32_t>(s->quant[c->tq][jpeg_zigzag[k]]);
        k++;
    }

    jpeg_idct_block(coef, block_out);
    return true;
}

void jpeg_place_block(uint8_t *block, jpeg_component *c, int32_t px, int32_t py)
{
    for (int j = 0; j < 8; j++) {
        int32_t yy = py + j;
        if (yy >= c->plane_h)
            continue;
        for (int i = 0; i < 8; i++) {
            int32_t xx = px + i;
            if (xx >= c->plane_w)
                continue;
            c->plane[static_cast<uint64_t>(yy) * c->plane_w + xx] = block[j * 8 + i];
        }
    }
}

int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bilinear upsample of a subsampled chroma plane at full-resolution pixel
// (px, py). Chroma sample (i, j) is centered over its subsampled block, giving
// the half-pixel siting used by JFIF. Returns the interpolated 0..255 sample.
int32_t jpeg_sample_chroma(const jpeg_component *c, int32_t px, int32_t py, int32_t img_w, int32_t img_h, int32_t h_max,
                           int32_t v_max)
{
    const int32_t sx = h_max / c->h; // luma pixels per chroma sample
    const int32_t sy = v_max / c->v;
    const int32_t cw = (img_w + sx - 1) / sx;
    const int32_t ch = (img_h + sy - 1) / sy;

    // f = (px + 0.5) / sx - 0.5 in 16.16 fixed point. Clamp the coordinate
    // itself (not just the index) so border pixels sample column/row 0 with
    // zero fractional weight instead of blending toward sample 1.
    int64_t fx = ((static_cast<int64_t>(2 * px + 1) << 16) / (2 * static_cast<int64_t>(sx))) - (1 << 15);
    int64_t fy = ((static_cast<int64_t>(2 * py + 1) << 16) / (2 * static_cast<int64_t>(sy))) - (1 << 15);
    if (fx < 0)
        fx = 0;
    if (fy < 0)
        fy = 0;

    int32_t x0 = clampi(static_cast<int32_t>(fx >> 16), 0, cw - 1);
    int32_t y0 = clampi(static_cast<int32_t>(fy >> 16), 0, ch - 1);
    int32_t x1 = clampi(x0 + 1, 0, cw - 1);
    int32_t y1 = clampi(y0 + 1, 0, ch - 1);
    const int32_t ax = static_cast<int32_t>(fx & 0xFFFF);
    const int32_t ay = static_cast<int32_t>(fy & 0xFFFF);

    const uint8_t *p = c->plane;
    const int64_t p00 = p[static_cast<uint64_t>(y0) * c->plane_w + x0];
    const int64_t p01 = p[static_cast<uint64_t>(y0) * c->plane_w + x1];
    const int64_t p10 = p[static_cast<uint64_t>(y1) * c->plane_w + x0];
    const int64_t p11 = p[static_cast<uint64_t>(y1) * c->plane_w + x1];

    const int64_t top = p00 * (65536 - ax) + p01 * ax;
    const int64_t bot = p10 * (65536 - ax) + p11 * ax;
    return static_cast<int32_t>((top * (65536 - ay) + bot * ay) >> 32);
}

bool jpeg_decode_scan(jpeg_state *s, const uint8_t *scan, size_t scan_len)
{
    jpeg_bits b;
    jpeg_bits_init(&b, scan, scan_len);

    const int mcu_w = s->h_max * 8;
    const int mcu_h = s->v_max * 8;
    const uint32_t mcu_cols = (s->width + mcu_w - 1) / mcu_w;
    const uint32_t mcu_rows = (s->height + mcu_h - 1) / mcu_h;

    uint8_t block[64];
    uint32_t mcu_since_restart = 0;

    for (uint32_t my = 0; my < mcu_rows; my++) {
        for (uint32_t mx = 0; mx < mcu_cols; mx++) {
            for (int ci = 0; ci < s->comp_count; ci++) {
                jpeg_component *c = &s->comp[ci];
                for (int bv = 0; bv < c->v; bv++) {
                    for (int bh = 0; bh < c->h; bh++) {
                        if (!jpeg_decode_block(&b, s, c, block))
                            return false;
                        jpeg_place_block(block, c, (int32_t)(mx * c->h + bh) * 8, (int32_t)(my * c->v + bv) * 8);
                    }
                }
            }

            mcu_since_restart++;
            if (s->restart_interval > 0 && mcu_since_restart == s->restart_interval &&
                !(my == mcu_rows - 1 && mx == mcu_cols - 1)) {
                jpeg_bits_align(&b);
                // Expect RSTn marker here.
                if (b.pos + 2 > b.size || scan[b.pos] != 0xFF || (scan[b.pos + 1] & 0xF8) != 0xD0)
                    return false;
                b.pos += 2;
                b.marker = false;
                b.bitbuf = 0;
                b.bitcnt = 0;
                for (int ci = 0; ci < s->comp_count; ci++)
                    s->comp[ci].dc_pred = 0;
                mcu_since_restart = 0;
            }
        }
    }
    return true;
}

} // namespace

bool jpeg_decode(const uint8_t *data, size_t size, media_image *out)
{
    if (!out || size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    jpeg_state s = {};
    s.restart_interval = 0;

    size_t pos = 2;
    const uint8_t *scan = nullptr;
    size_t scan_len = 0;
    bool have_sof = false;

    for (;;) {
        // Find next marker, skipping any fill bytes.
        while (pos < size && data[pos] != 0xFF)
            return false; // entropy data must only appear after SOS
        while (pos < size && data[pos] == 0xFF)
            pos++;
        if (pos >= size)
            return false;
        uint8_t marker = data[pos++];
        if (marker == 0x00 || marker == 0xFF)
            continue;

        // Standalone markers without payload.
        if (marker == 0xD8)
            continue;
        if (marker == 0xD9)
            break; // EOI
        if (marker >= 0xD0 && marker <= 0xD7)
            continue;

        if (pos + 2 > size)
            return false;
        uint32_t seg_len = (static_cast<uint32_t>(data[pos]) << 8) | data[pos + 1];
        if (seg_len < 2 || pos + seg_len > size)
            return false;
        const uint8_t *seg = data + pos + 2;
        uint32_t payload = seg_len - 2;

        if (marker == 0xDB) { // DQT
            uint32_t i = 0;
            while (i < payload) {
                uint8_t pq_tq = seg[i++];
                int pq = pq_tq >> 4;
                int tq = pq_tq & 0x0F;
                if (pq != 0 || tq > 3 || i + 64 > payload)
                    return false;
                for (int k = 0; k < 64; k++)
                    s.quant[tq][jpeg_zigzag[k]] = seg[i + k];
                s.have_quant[tq] = true;
                i += 64;
            }
        } else if (marker == 0xC4) { // DHT
            uint32_t i = 0;
            while (i < payload) {
                uint8_t tc_th = seg[i++];
                int tc = tc_th >> 4;
                int th = tc_th & 0x0F;
                if (tc > 1 || th > 3 || i + 16 > payload)
                    return false;
                jpeg_huff *h = &s.huff[tc][th];
                for (int k = 0; k < 16; k++)
                    h->counts[k + 1] = seg[i + k];
                i += 16;
                int total = 0;
                for (int k = 1; k <= 16; k++)
                    total += h->counts[k];
                if (total > 256 || i + total > payload)
                    return false;
                if (!jpeg_huff_build(h, seg + i, total))
                    return false;
                s.have_huff[tc][th] = true;
                i += total;
            }
        } else if (marker == 0xDD) { // DRI
            if (payload < 2)
                return false;
            s.restart_interval = static_cast<uint16_t>((seg[0] << 8) | seg[1]);
        } else if (marker == 0xC0 || marker == 0xC1) { // SOF0/SOF1 baseline + extended sequential
            if (have_sof || payload < 6 || seg[0] != 8)
                return false;
            s.height = (static_cast<uint32_t>(seg[1]) << 8) | seg[2];
            s.width = (static_cast<uint32_t>(seg[3]) << 8) | seg[4];
            s.comp_count = seg[5];
            if (s.width == 0 || s.height == 0 || s.width > MEDIA_MAX_DIMENSION || s.height > MEDIA_MAX_DIMENSION ||
                static_cast<uint64_t>(s.width) * s.height > MEDIA_MAX_PIXELS)
                return false;
            if (s.comp_count != 1 && s.comp_count != 3)
                return false;
            if (payload < 6u + 3u * s.comp_count)
                return false;
            s.h_max = s.v_max = 0;
            for (int i = 0; i < s.comp_count; i++) {
                jpeg_component *c = &s.comp[i];
                c->id = seg[6 + i * 3];
                c->h = seg[7 + i * 3] >> 4;
                c->v = seg[7 + i * 3] & 0x0F;
                c->tq = seg[8 + i * 3];
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
                    return false;
                if (c->h > s.h_max)
                    s.h_max = c->h;
                if (c->v > s.v_max)
                    s.v_max = c->v;
            }
            have_sof = true;
        } else if (marker == 0xDA) { // SOS
            if (!have_sof || payload < 1)
                return false;
            int ns = seg[0];
            if (ns != s.comp_count || payload < static_cast<uint32_t>(1 + 2 * ns + 3))
                return false;
            for (int i = 0; i < ns; i++) {
                int cs = seg[1 + i * 2];
                int tables = seg[2 + i * 2];
                int ci = -1;
                for (int j = 0; j < s.comp_count; j++) {
                    if (s.comp[j].id == cs)
                        ci = j;
                }
                if (ci < 0)
                    return false;
                s.comp[ci].dc_table = tables >> 4;
                s.comp[ci].ac_table = tables & 0x0F;
                if (s.comp[ci].dc_table > 3 || s.comp[ci].ac_table > 3)
                    return false;
            }
            // Ss=0, Se=63, Ah/Al=0 required for baseline.
            if (seg[1 + 2 * ns] != 0 || seg[2 + 2 * ns] != 63 || seg[3 + 2 * ns] != 0)
                return false;

            scan = data + pos + seg_len;
            if (scan >= data + size)
                return false;
            scan_len = size - static_cast<size_t>(scan - data);

            // Allocate component planes.
            const uint32_t mcu_w = s.h_max * 8;
            const uint32_t mcu_h = s.v_max * 8;
            const uint32_t mcu_cols = (s.width + mcu_w - 1) / mcu_w;
            const uint32_t mcu_rows = (s.height + mcu_h - 1) / mcu_h;
            for (int i = 0; i < s.comp_count; i++) {
                jpeg_component *c = &s.comp[i];
                c->plane_w = static_cast<int32_t>(mcu_cols * c->h * 8);
                c->plane_h = static_cast<int32_t>(mcu_rows * c->v * 8);
                uint64_t bytes = static_cast<uint64_t>(c->plane_w) * c->plane_h;
                c->plane = static_cast<uint8_t *>(media_alloc(static_cast<size_t>(bytes)));
                if (!c->plane) {
                    for (int j = 0; j < i; j++)
                        media_free(s.comp[j].plane);
                    return false;
                }
            }

            if (!jpeg_decode_scan(&s, scan, scan_len)) {
                for (int i = 0; i < s.comp_count; i++)
                    media_free(s.comp[i].plane);
                return false;
            }
            goto convert;
        }
        // APPn/COM/anything else: skip.
        pos += seg_len;
    }
    return false;

convert: {
    uint32_t *pixels =
        static_cast<uint32_t *>(media_alloc(static_cast<uint64_t>(s.width) * s.height * sizeof(uint32_t)));
    if (!pixels) {
        for (int i = 0; i < s.comp_count; i++)
            media_free(s.comp[i].plane);
        return false;
    }

    const jpeg_component *y = &s.comp[0];
    for (uint32_t py = 0; py < s.height; py++) {
        uint32_t *dst = pixels + static_cast<uint64_t>(py) * s.width;
        for (uint32_t px = 0; px < s.width; px++) {
            int32_t yv = y->plane[static_cast<uint64_t>(py) * y->plane_w + px];
            uint8_t r, g, b;
            if (s.comp_count == 1) {
                r = g = b = static_cast<uint8_t>(yv);
            } else {
                const jpeg_component *cb = &s.comp[1];
                const jpeg_component *cr = &s.comp[2];
                int32_t cbv = jpeg_sample_chroma(cb, static_cast<int32_t>(px), static_cast<int32_t>(py),
                                                 static_cast<int32_t>(s.width), static_cast<int32_t>(s.height), s.h_max,
                                                 s.v_max) -
                              128;
                int32_t crv = jpeg_sample_chroma(cr, static_cast<int32_t>(px), static_cast<int32_t>(py),
                                                 static_cast<int32_t>(s.width), static_cast<int32_t>(s.height), s.h_max,
                                                 s.v_max) -
                              128;
                // BT.601 full range, 10-bit fixed point.
                int32_t rv = yv + ((1436 * crv) >> 10);
                int32_t gv = yv - ((352 * cbv + 731 * crv) >> 10);
                int32_t bv = yv + ((1815 * cbv) >> 10);
                r = rv < 0 ? 0 : (rv > 255 ? 255 : static_cast<uint8_t>(rv));
                g = gv < 0 ? 0 : (gv > 255 ? 255 : static_cast<uint8_t>(gv));
                b = bv < 0 ? 0 : (bv > 255 ? 255 : static_cast<uint8_t>(bv));
            }
            media_write_argb(dst + px, r, g, b, 255);
        }
    }

    for (int i = 0; i < s.comp_count; i++)
        media_free(s.comp[i].plane);

    out->pixels = pixels;
    out->width = static_cast<int32_t>(s.width);
    out->height = static_cast<int32_t>(s.height);
    return true;
}
}
