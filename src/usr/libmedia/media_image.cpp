#include "media_codec_internal.h"

#include <stdlib.h>

void *media_alloc(size_t n)
{
    return malloc(n);
}

void media_free(void *p)
{
    free(p);
}

bool media_image_decode(const void *data, size_t size, media_image *out)
{
    if (!data || size < 4 || !out)
        return false;
    out->pixels = nullptr;
    out->width = 0;
    out->height = 0;

    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    if (size >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
        return png_decode(bytes, size, out);
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
        return jpeg_decode(bytes, size, out);
    if (bytes[0] == 'B' && bytes[1] == 'M')
        return bmp_decode(bytes, size, out);
    if (size >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F')
        return gif_decode(bytes, size, out);
    if (bytes[0] == 'q' && bytes[1] == 'o' && bytes[2] == 'i' && bytes[3] == 'f')
        return qoi_decode(bytes, size, out);
    return false;
}

void media_image_free(media_image *img)
{
    if (!img)
        return;
    if (img->pixels)
        media_free(img->pixels);
    img->pixels = nullptr;
    img->width = 0;
    img->height = 0;
}

bool media_image_scale(const media_image *src, media_image *dst, int32_t dst_w, int32_t dst_h)
{
    if (!src || !dst || !src->pixels || src->width <= 0 || src->height <= 0 || dst_w <= 0 || dst_h <= 0 ||
        dst_w > MEDIA_MAX_DIMENSION || dst_h > MEDIA_MAX_DIMENSION ||
        static_cast<uint64_t>(dst_w) * dst_h > MEDIA_MAX_PIXELS)
        return false;

    dst->pixels = nullptr;
    dst->width = 0;
    dst->height = 0;

    if (dst_w == src->width && dst_h == src->height) {
        uint32_t *copy = static_cast<uint32_t *>(media_alloc(static_cast<uint64_t>(dst_w) * dst_h * 4));
        if (!copy)
            return false;
        for (uint64_t i = 0; i < static_cast<uint64_t>(dst_w) * dst_h; i++)
            copy[i] = src->pixels[i];
        dst->pixels = copy;
        dst->width = dst_w;
        dst->height = dst_h;
        return true;
    }

    uint32_t *out = static_cast<uint32_t *>(media_alloc(static_cast<uint64_t>(dst_w) * dst_h * 4));
    if (!out)
        return false;

    // Bilinear sampling in 16.16 fixed point.
    const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(src->width) << 16) / dst_w);
    const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(src->height) << 16) / dst_h);

    for (int32_t y = 0; y < dst_h; y++) {
        uint32_t fy = y * sy + (sy >> 1);
        int32_t y0 = static_cast<int32_t>(fy >> 16);
        int32_t y1 = y0 + 1 < src->height ? y0 + 1 : src->height - 1;
        uint32_t wy = (fy >> 8) & 0xFF;
        const uint32_t *row0 = src->pixels + static_cast<uint64_t>(y0) * src->width;
        const uint32_t *row1 = src->pixels + static_cast<uint64_t>(y1) * src->width;

        for (int32_t x = 0; x < dst_w; x++) {
            uint32_t fx = x * sx + (sx >> 1);
            int32_t x0 = static_cast<int32_t>(fx >> 16);
            if (x0 >= src->width)
                x0 = src->width - 1;
            int32_t x1 = x0 + 1 < src->width ? x0 + 1 : src->width - 1;
            uint32_t wx = (fx >> 8) & 0xFF;

            uint32_t p00 = row0[x0], p01 = row0[x1], p10 = row1[x0], p11 = row1[x1];
            uint32_t result = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                uint32_t c00 = (p00 >> shift) & 0xFF;
                uint32_t c01 = (p01 >> shift) & 0xFF;
                uint32_t c10 = (p10 >> shift) & 0xFF;
                uint32_t c11 = (p11 >> shift) & 0xFF;
                uint32_t top = c00 * (256 - wx) + c01 * wx;
                uint32_t bot = c10 * (256 - wx) + c11 * wx;
                uint32_t val = (top * (256 - wy) + bot * wy + (1u << 15)) >> 16;
                result |= val << shift;
            }
            out[static_cast<uint64_t>(y) * dst_w + x] = result;
        }
    }

    dst->pixels = out;
    dst->width = dst_w;
    dst->height = dst_h;
    return true;
}
