#include "media_codec_internal.h"

namespace {

constexpr size_t PNG_MAX_COMPRESSED = 64ull * 1024 * 1024;

struct png_bit_reader
{
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
    bool error;
};

void png_bits_init(png_bit_reader *b, const uint8_t *data, size_t size)
{
    b->data = data;
    b->size = size;
    b->pos = 0;
    b->bitbuf = 0;
    b->bitcnt = 0;
    b->error = false;
}

uint32_t png_bits_take(png_bit_reader *b, int n)
{
    while (b->bitcnt < n) {
        if (b->pos >= b->size) {
            b->error = true;
            return 0;
        }
        b->bitbuf |= static_cast<uint32_t>(b->data[b->pos++]) << b->bitcnt;
        b->bitcnt += 8;
    }
    uint32_t val = b->bitbuf & ((1u << n) - 1u);
    b->bitbuf >>= n;
    b->bitcnt -= n;
    return val;
}

struct png_huff
{
    int counts[16];
    int symbols[288];
    int symbol_count;
};

bool png_huff_build(png_huff *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i < 16; i++)
        h->counts[i] = 0;
    for (int i = 0; i < n; i++) {
        if (lengths[i] > 15)
            return false;
        h->counts[lengths[i]]++;
    }
    h->counts[0] = 0;
    // Over-subscription check: code space must never go negative.
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0)
            return false;
    }
    h->symbol_count = 0;
    // Canonical ordering: ascending length, then symbol order.
    for (int len = 1; len <= 15; len++) {
        for (int sym = 0; sym < n; sym++) {
            if (lengths[sym] == len)
                h->symbols[h->symbol_count++] = sym;
        }
    }
    return true;
}

int png_huff_decode(png_bit_reader *b, const png_huff *h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= static_cast<int>(png_bits_take(b, 1));
        if (b->error)
            return -1;
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
        code <<= 1;
    }
    return -1;
}

const uint16_t png_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t png_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t png_dist_base[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t png_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t png_clen_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool png_inflate_block(png_bit_reader *b, const png_huff *lit, const png_huff *dist, uint8_t *out, size_t out_cap,
                       size_t *out_len)
{
    for (;;) {
        int sym = png_huff_decode(b, lit);
        if (sym < 0)
            return false;
        if (sym < 256) {
            if (*out_len >= out_cap)
                return false;
            out[(*out_len)++] = static_cast<uint8_t>(sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29)
                return false;
            uint32_t length = png_len_base[sym] + png_bits_take(b, png_len_extra[sym]);
            if (b->error)
                return false;
            int dsym = png_huff_decode(b, dist);
            if (dsym < 0 || dsym >= 30)
                return false;
            uint32_t distance = png_dist_base[dsym] + png_bits_take(b, png_dist_extra[dsym]);
            if (b->error)
                return false;
            if (distance > *out_len || length > out_cap - *out_len)
                return false;
            size_t src = *out_len - distance;
            for (uint32_t i = 0; i < length; i++)
                out[*out_len + i] = out[src + i];
            *out_len += length;
        }
    }
}

bool png_inflate(const uint8_t *data, size_t size, uint8_t *out, size_t out_cap, size_t *out_len)
{
    png_bit_reader b;
    png_bits_init(&b, data, size);
    *out_len = 0;

    // zlib header: CMF/FLG, optional dictionary flag (rejected).
    uint32_t cmf = png_bits_take(&b, 8);
    uint32_t flg = png_bits_take(&b, 8);
    if (b.error)
        return false;
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0)
        return false;
    if (flg & 0x20)
        return false;

    for (;;) {
        uint32_t bfinal = png_bits_take(&b, 1);
        uint32_t btype = png_bits_take(&b, 2);
        if (b.error)
            return false;
        if (btype == 0) {
            // Stored block: realign to byte boundary.
            b.bitbuf = 0;
            b.bitcnt = 0;
            if (b.pos + 4 > b.size)
                return false;
            uint32_t len = b.data[b.pos] | (b.data[b.pos + 1] << 8);
            uint32_t nlen = b.data[b.pos + 2] | (b.data[b.pos + 3] << 8);
            b.pos += 4;
            if (len != (~nlen & 0xFFFF) || len > b.size - b.pos || len > out_cap - *out_len)
                return false;
            for (uint32_t i = 0; i < len; i++)
                out[*out_len + i] = b.data[b.pos + i];
            *out_len += len;
            b.pos += len;
        } else if (btype == 1 || btype == 2) {
            png_huff lit, dist;
            if (btype == 1) {
                uint8_t lens[288];
                for (int i = 0; i <= 143; i++)
                    lens[i] = 8;
                for (int i = 144; i <= 255; i++)
                    lens[i] = 9;
                for (int i = 256; i <= 279; i++)
                    lens[i] = 7;
                for (int i = 280; i <= 287; i++)
                    lens[i] = 8;
                if (!png_huff_build(&lit, lens, 288))
                    return false;
                uint8_t dlens[30];
                for (int i = 0; i < 30; i++)
                    dlens[i] = 5;
                if (!png_huff_build(&dist, dlens, 30))
                    return false;
            } else {
                uint32_t hlit = png_bits_take(&b, 5) + 257;
                uint32_t hdist = png_bits_take(&b, 5) + 1;
                uint32_t hclen = png_bits_take(&b, 4) + 4;
                if (b.error || hlit > 286 || hdist > 30)
                    return false;
                uint8_t clen_lens[19] = {};
                for (uint32_t i = 0; i < hclen; i++)
                    clen_lens[png_clen_order[i]] = static_cast<uint8_t>(png_bits_take(&b, 3));
                if (b.error)
                    return false;
                png_huff clen;
                if (!png_huff_build(&clen, clen_lens, 19))
                    return false;
                uint8_t lens[320] = {};
                uint32_t total = hlit + hdist;
                uint32_t i = 0;
                while (i < total) {
                    int sym = png_huff_decode(&b, &clen);
                    if (sym < 0)
                        return false;
                    if (sym < 16) {
                        lens[i++] = static_cast<uint8_t>(sym);
                    } else if (sym == 16) {
                        if (i == 0)
                            return false;
                        uint32_t rep = png_bits_take(&b, 2) + 3;
                        uint8_t prev = lens[i - 1];
                        if (b.error || i + rep > total)
                            return false;
                        while (rep--)
                            lens[i++] = prev;
                    } else if (sym == 17) {
                        uint32_t rep = png_bits_take(&b, 3) + 3;
                        if (b.error || i + rep > total)
                            return false;
                        i += rep;
                    } else {
                        uint32_t rep = png_bits_take(&b, 7) + 11;
                        if (b.error || i + rep > total)
                            return false;
                        i += rep;
                    }
                }
                if (!png_huff_build(&lit, lens, static_cast<int>(hlit)) ||
                    !png_huff_build(&dist, lens + hlit, static_cast<int>(hdist)))
                    return false;
            }
            if (!png_inflate_block(&b, &lit, &dist, out, out_cap, out_len))
                return false;
        } else {
            return false;
        }
        if (bfinal)
            break;
    }
    return !b.error && *out_len == out_cap;
}

int png_sample(const uint8_t *row, int32_t x, int bit_depth, int samples_per_pixel, int channel)
{
    // Extract one sample (already unfiltered row, no filter byte).
    int bit_index = (x * samples_per_pixel + channel) * bit_depth;
    if (bit_depth == 8)
        return row[bit_index / 8];
    if (bit_depth == 16)
        return row[bit_index / 8]; // caller takes the high byte via /8*2
    // Sub-byte depths: 1, 2, 4 (gray or palette only, one sample).
    int byte = row[bit_index / 8];
    int shift = 8 - bit_depth - (bit_index % 8);
    int mask = (1 << bit_depth) - 1;
    return (byte >> shift) & mask;
}

} // namespace

bool png_decode(const uint8_t *data, size_t size, media_image *out)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 + 25 || !out)
        return false;
    for (int i = 0; i < 8; i++) {
        if (data[i] != sig[i])
            return false;
    }

    media_reader r = {data, size, 8};

    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0, interlace = 0;
    bool have_ihdr = false;

    uint8_t palette[256][3] = {};
    int palette_count = 0;
    uint8_t trns[256] = {};
    int trns_count = -1; // -1 = none
    uint16_t trns_gray = 0;
    uint8_t trns_rgb[6] = {};
    bool have_trns = false;

    uint8_t *compressed = nullptr;
    size_t compressed_len = 0;
    bool result = false;

    for (;;) {
        uint32_t chunk_len, chunk_type;
        if (!media_read_be32(&r, &chunk_len) || !media_read_be32(&r, &chunk_type))
            break;
        if (chunk_len > r.size - r.pos - 4)
            break;
        size_t chunk_start = r.pos;

        if (chunk_type == 0x49484452) { // IHDR
            if (have_ihdr || chunk_len != 13)
                break;
            uint8_t compression, filter;
            if (!media_read_be32(&r, &width) || !media_read_be32(&r, &height) || !media_read_u8(&r, &bit_depth) ||
                !media_read_u8(&r, &color_type) || !media_read_u8(&r, &compression) || !media_read_u8(&r, &filter) ||
                !media_read_u8(&r, &interlace))
                break;
            if (compression != 0 || filter != 0 || interlace != 0)
                break;
            if (width == 0 || height == 0 || width > MEDIA_MAX_DIMENSION || height > MEDIA_MAX_DIMENSION ||
                static_cast<uint64_t>(width) * height > MEDIA_MAX_PIXELS)
                break;
            // Valid depth/type combinations.
            bool ok = false;
            switch (color_type) {
                case 0:
                    ok = bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8 || bit_depth == 16;
                    break;
                case 2:
                case 4:
                case 6:
                    ok = bit_depth == 8 || bit_depth == 16;
                    break;
                case 3:
                    ok = bit_depth == 1 || bit_depth == 2 || bit_depth == 4 || bit_depth == 8;
                    break;
                default:
                    break;
            }
            if (!ok)
                break;
            have_ihdr = true;
        } else if (chunk_type == 0x504C5445) { // PLTE
            if (!have_ihdr || chunk_len == 0 || chunk_len % 3 != 0 || chunk_len / 3 > 256)
                break;
            const uint8_t *p;
            if (!media_read_bytes(&r, &p, chunk_len))
                break;
            palette_count = static_cast<int>(chunk_len / 3);
            for (int i = 0; i < palette_count; i++) {
                palette[i][0] = p[static_cast<size_t>(i) * 3];
                palette[i][1] = p[static_cast<size_t>(i) * 3 + 1];
                palette[i][2] = p[static_cast<size_t>(i) * 3 + 2];
            }
        } else if (chunk_type == 0x74524E53) { // tRNS
            if (!have_ihdr)
                break;
            const uint8_t *p;
            if (!media_read_bytes(&r, &p, chunk_len))
                break;
            have_trns = true;
            if (color_type == 3) {
                if (chunk_len > 256)
                    break;
                trns_count = static_cast<int>(chunk_len);
                for (uint32_t i = 0; i < chunk_len; i++)
                    trns[i] = p[i];
            } else if (color_type == 0 && chunk_len == 2) {
                trns_gray = static_cast<uint16_t>((p[0] << 8) | p[1]);
            } else if (color_type == 2 && chunk_len == 6) {
                for (int i = 0; i < 6; i++)
                    trns_rgb[i] = p[i];
            }
        } else if (chunk_type == 0x49444154) { // IDAT
            if (!have_ihdr)
                break;
            if (compressed_len + chunk_len > PNG_MAX_COMPRESSED)
                break;
            if (!compressed) {
                compressed = static_cast<uint8_t *>(media_alloc(chunk_len > 0 ? chunk_len : 1));
            } else {
                uint8_t *grown = static_cast<uint8_t *>(media_alloc(compressed_len + chunk_len));
                if (!grown) {
                    media_free(compressed);
                    compressed = nullptr;
                    break;
                }
                for (size_t i = 0; i < compressed_len; i++)
                    grown[i] = compressed[i];
                media_free(compressed);
                compressed = grown;
            }
            if (!compressed)
                break;
            const uint8_t *p;
            if (!media_read_bytes(&r, &p, chunk_len))
                break;
            for (size_t i = 0; i < chunk_len; i++)
                compressed[compressed_len + i] = p[i];
            compressed_len += chunk_len;
        } else if (chunk_type == 0x49454E44) { // IEND
            if (!have_ihdr || !compressed)
                break;

            int channels = 1;
            if (color_type == 2)
                channels = 3;
            else if (color_type == 4)
                channels = 2;
            else if (color_type == 6)
                channels = 4;

            uint64_t stride = (static_cast<uint64_t>(width) * channels * bit_depth + 7) / 8;
            uint64_t raw_size = static_cast<uint64_t>(height) * (stride + 1);
            if (raw_size > 128ull * 1024 * 1024)
                break;

            uint8_t *raw = static_cast<uint8_t *>(media_alloc(static_cast<size_t>(raw_size)));
            if (!raw)
                break;
            size_t got = 0;
            if (!png_inflate(compressed, compressed_len, raw, static_cast<size_t>(raw_size), &got)) {
                media_free(raw);
                break;
            }

            uint32_t *pixels = static_cast<uint32_t *>(media_alloc(static_cast<size_t>(width) * height * 4));
            if (!pixels) {
                media_free(raw);
                break;
            }

            // Unfilter and expand scanlines.
            const int filter_bpp = bit_depth < 8 ? 1 : channels * (bit_depth / 8);
            bool fail = false;
            for (uint32_t y = 0; y < height && !fail; y++) {
                uint8_t *row = raw + static_cast<uint64_t>(y) * (stride + 1) + 1;
                uint8_t filter = raw[static_cast<uint64_t>(y) * (stride + 1)];
                const uint8_t *prev = y > 0 ? raw + static_cast<uint64_t>(y - 1) * (stride + 1) + 1 : nullptr;

                switch (filter) {
                    case 0:
                        break;
                    case 1: // Sub
                        for (uint64_t i = filter_bpp; i < stride; i++)
                            row[i] = static_cast<uint8_t>(row[i] + row[i - filter_bpp]);
                        break;
                    case 2: // Up
                        if (prev) {
                            for (uint64_t i = 0; i < stride; i++)
                                row[i] = static_cast<uint8_t>(row[i] + prev[i]);
                        }
                        break;
                    case 3: { // Average
                        for (uint64_t i = 0; i < stride; i++) {
                            uint8_t a = i >= static_cast<uint64_t>(filter_bpp) ? row[i - filter_bpp] : 0;
                            uint8_t b = prev ? prev[i] : 0;
                            row[i] = static_cast<uint8_t>(row[i] + ((a + b) >> 1));
                        }
                        break;
                    }
                    case 4: { // Paeth
                        for (uint64_t i = 0; i < stride; i++) {
                            uint8_t a = i >= static_cast<uint64_t>(filter_bpp) ? row[i - filter_bpp] : 0;
                            uint8_t b = prev ? prev[i] : 0;
                            uint8_t c = (prev && i >= static_cast<uint64_t>(filter_bpp)) ? prev[i - filter_bpp] : 0;
                            int p = a + b - c;
                            int pa = p > a ? p - a : a - p;
                            int pb = p > b ? p - b : b - p;
                            int pc = p > c ? p - c : c - p;
                            uint8_t pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                            row[i] = static_cast<uint8_t>(row[i] + pred);
                        }
                        break;
                    }
                    default:
                        fail = true;
                        break;
                }
                if (fail)
                    break;

                uint32_t *dst = pixels + static_cast<uint64_t>(y) * width;
                for (size_t x = 0; x < width; x++) {
                    uint8_t cr = 0, cg = 0, cb = 0, ca = 255;
                    if (color_type == 0) { // gray
                        int g = png_sample(row, static_cast<int32_t>(x), bit_depth, 1, 0);
                        // Sub-byte tRNS compares against the raw sample, before scaling.
                        if (have_trns && bit_depth < 8 && static_cast<uint16_t>(g) == trns_gray)
                            ca = 0;
                        if (bit_depth < 8)
                            g = g * 255 / ((1 << bit_depth) - 1);
                        else if (bit_depth == 16)
                            g = row[x * 2];
                        cr = cg = cb = static_cast<uint8_t>(g);
                        if (have_trns && bit_depth == 8 && static_cast<uint16_t>(g) == trns_gray)
                            ca = 0;
                        if (have_trns && bit_depth == 16 && row[x * 2] == (trns_gray >> 8) &&
                            row[x * 2 + 1] == (trns_gray & 0xFF))
                            ca = 0;
                    } else if (color_type == 2) { // RGB
                        if (bit_depth == 16) {
                            cr = row[x * 6];
                            cg = row[x * 6 + 2];
                            cb = row[x * 6 + 4];
                            if (have_trns && row[x * 6] == trns_rgb[0] && row[x * 6 + 1] == trns_rgb[1] &&
                                row[x * 6 + 2] == trns_rgb[2] && row[x * 6 + 3] == trns_rgb[3] &&
                                row[x * 6 + 4] == trns_rgb[4] && row[x * 6 + 5] == trns_rgb[5])
                                ca = 0;
                        } else {
                            cr = row[x * 3];
                            cg = row[x * 3 + 1];
                            cb = row[x * 3 + 2];
                            // 8-bit tRNS stores big-endian 16-bit samples; the
                            // 8-bit value lives in the low byte of each pair.
                            if (have_trns && row[x * 3] == trns_rgb[1] && row[x * 3 + 1] == trns_rgb[3] &&
                                row[x * 3 + 2] == trns_rgb[5])
                                ca = 0;
                        }
                    } else if (color_type == 3) { // palette
                        int idx = png_sample(row, static_cast<int32_t>(x), bit_depth, 1, 0);
                        if (idx >= palette_count) {
                            fail = true;
                            break;
                        }
                        cr = palette[idx][0];
                        cg = palette[idx][1];
                        cb = palette[idx][2];
                        ca = (trns_count >= 0 && idx < trns_count) ? trns[idx] : 255;
                    } else if (color_type == 4) { // gray + alpha
                        if (bit_depth == 16) {
                            cr = cg = cb = row[x * 4];
                            ca = row[x * 4 + 2];
                        } else {
                            cr = cg = cb = row[x * 2];
                            ca = row[x * 2 + 1];
                        }
                    } else { // RGBA
                        if (bit_depth == 16) {
                            cr = row[x * 8];
                            cg = row[x * 8 + 2];
                            cb = row[x * 8 + 4];
                            ca = row[x * 8 + 6];
                        } else {
                            cr = row[x * 4];
                            cg = row[x * 4 + 1];
                            cb = row[x * 4 + 2];
                            ca = row[x * 4 + 3];
                        }
                    }
                    media_write_argb(dst + x, cr, cg, cb, ca);
                }
            }

            media_free(raw);
            if (fail) {
                media_free(pixels);
                break;
            }
            out->pixels = pixels;
            out->width = static_cast<int32_t>(width);
            out->height = static_cast<int32_t>(height);
            result = true;
            break;
        } else {
            // Ancillary/unknown chunk: skip payload (+ CRC).
            r.pos = chunk_start + chunk_len;
        }
        r.pos += 4; // CRC
        if (r.pos > r.size)
            break;
    }

    if (compressed)
        media_free(compressed);
    return result;
}
