#include "media_codec_internal.h"

bool bmp_decode(const uint8_t *data, size_t size, media_image *out)
{
    if (!out || size < 54 || data[0] != 'B' || data[1] != 'M')
        return false;

    media_reader r = {data, size, 2};
    uint32_t file_size, pixel_offset, header_size;
    uint16_t reserved1, reserved2;
    if (!media_read_le32(&r, &file_size) || !media_read_le16(&r, &reserved1) || !media_read_le16(&r, &reserved2) ||
        !media_read_le32(&r, &pixel_offset) || !media_read_le32(&r, &header_size))
        return false;
    if (file_size > size || pixel_offset >= size || header_size < 40)
        return false;

    uint32_t width_u, height_u, compression, image_size, colors_used;
    uint16_t planes, bpp;
    int32_t width, height;
    if (!media_read_le32(&r, &width_u) || !media_read_le32(&r, &height_u) || !media_read_le16(&r, &planes) ||
        !media_read_le16(&r, &bpp) || !media_read_le32(&r, &compression) || !media_read_le32(&r, &image_size) ||
        !media_read_le32(&r, &colors_used))
        return false;

    width = static_cast<int32_t>(width_u);
    height = static_cast<int32_t>(height_u);
    bool top_down = false;
    if (height < 0) {
        height = -height;
        top_down = true;
    }
    if (planes != 1 || width <= 0 || height <= 0 || width > MEDIA_MAX_DIMENSION || height > MEDIA_MAX_DIMENSION ||
        static_cast<uint64_t>(width) * height > MEDIA_MAX_PIXELS)
        return false;
    if (compression != 0 || (bpp != 24 && bpp != 32))
        return false;

    const uint32_t bytes_per_pixel = bpp / 8;
    const uint64_t row_stride = ((static_cast<uint64_t>(width) * bytes_per_pixel + 3) / 4) * 4;
    const uint64_t required = pixel_offset + row_stride * static_cast<uint64_t>(height);
    if (required > size)
        return false;

    uint32_t *pixels = static_cast<uint32_t *>(media_alloc(static_cast<uint64_t>(width) * height * 4));
    if (!pixels)
        return false;

    // BI_RGB 32bpp: the high byte is officially reserved and often zero even
    // for opaque images. Only honor alpha if some pixel carries a nonzero one.
    bool alpha_meaningful = false;
    if (bytes_per_pixel == 4) {
        for (int32_t y = 0; y < height && !alpha_meaningful; y++) {
            const uint8_t *row = data + pixel_offset + static_cast<uint64_t>(y) * row_stride;
            for (int32_t x = 0; x < width; x++) {
                if (row[static_cast<uint64_t>(x) * 4 + 3] != 0) {
                    alpha_meaningful = true;
                    break;
                }
            }
        }
    }

    for (int32_t y = 0; y < height; y++) {
        int32_t src_row = top_down ? y : (height - 1 - y);
        const uint8_t *row = data + pixel_offset + static_cast<uint64_t>(src_row) * row_stride;
        uint32_t *dst = pixels + static_cast<uint64_t>(y) * width;
        for (int32_t x = 0; x < width; x++) {
            const uint8_t *p = row + static_cast<uint64_t>(x) * bytes_per_pixel;
            uint8_t alpha = (bytes_per_pixel == 4 && alpha_meaningful) ? p[3] : 255;
            media_write_argb(dst + x, p[2], p[1], p[0], alpha);
        }
    }

    out->pixels = pixels;
    out->width = width;
    out->height = height;
    return true;
}
