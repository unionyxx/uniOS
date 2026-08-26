#pragma once

#include <stddef.h>
#include <stdint.h>

#include "media_image.h"

// Per-format decoders. Each is strict about bounds: malformed input from user
// files must fail cleanly, never overread.
bool png_decode(const uint8_t *data, size_t size, media_image *out);
bool jpeg_decode(const uint8_t *data, size_t size, media_image *out);
bool bmp_decode(const uint8_t *data, size_t size, media_image *out);
bool gif_decode(const uint8_t *data, size_t size, media_image *out);
bool qoi_decode(const uint8_t *data, size_t size, media_image *out);

// Decode limits shared by all codecs.
constexpr int32_t MEDIA_MAX_DIMENSION = 16384;
constexpr uint64_t MEDIA_MAX_PIXELS = 16u * 1024u * 1024u; // 64 MiB of ARGB

void *media_alloc(size_t n);
void media_free(void *p);

struct media_reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
};

static inline bool media_read_bytes(media_reader *r, const uint8_t **out, size_t n)
{
    if (n > r->size || r->pos > r->size - n)
        return false;
    *out = r->data + r->pos;
    r->pos += n;
    return true;
}

static inline bool media_read_u8(media_reader *r, uint8_t *out)
{
    const uint8_t *p;
    if (!media_read_bytes(r, &p, 1))
        return false;
    *out = p[0];
    return true;
}

static inline bool media_read_be16(media_reader *r, uint16_t *out)
{
    const uint8_t *p;
    if (!media_read_bytes(r, &p, 2))
        return false;
    *out = static_cast<uint16_t>((p[0] << 8) | p[1]);
    return true;
}

static inline bool media_read_be32(media_reader *r, uint32_t *out)
{
    const uint8_t *p;
    if (!media_read_bytes(r, &p, 4))
        return false;
    *out = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
    return true;
}

static inline bool media_read_le16(media_reader *r, uint16_t *out)
{
    const uint8_t *p;
    if (!media_read_bytes(r, &p, 2))
        return false;
    *out = static_cast<uint16_t>(p[0] | (p[1] << 8));
    return true;
}

static inline bool media_read_le32(media_reader *r, uint32_t *out)
{
    const uint8_t *p;
    if (!media_read_bytes(r, &p, 4))
        return false;
    *out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
    return true;
}

static inline void media_write_argb(uint32_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    *dst = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}
