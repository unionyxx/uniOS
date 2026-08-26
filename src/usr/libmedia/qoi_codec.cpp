#include "media_codec_internal.h"

bool qoi_decode(const uint8_t *data, size_t size, media_image *out)
{
    if (!out || size < 14)
        return false;
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f')
        return false;

    media_reader r = {data, size, 4};
    uint32_t width, height;
    uint8_t channels, colorspace;
    if (!media_read_be32(&r, &width) || !media_read_be32(&r, &height) || !media_read_u8(&r, &channels) ||
        !media_read_u8(&r, &colorspace))
        return false;
    if (width == 0 || height == 0 || width > MEDIA_MAX_DIMENSION || height > MEDIA_MAX_DIMENSION ||
        static_cast<uint64_t>(width) * height > MEDIA_MAX_PIXELS || channels < 3 || channels > 4)
        return false;

    uint64_t total = static_cast<uint64_t>(width) * height;
    uint32_t *pixels = static_cast<uint32_t *>(media_alloc(static_cast<size_t>(total) * 4));
    if (!pixels)
        return false;

    uint32_t index[64] = {};
    uint8_t px[4] = {0, 0, 0, 255};
    uint64_t written = 0;

    auto store_pixel = [&](uint32_t argb) {
        pixels[written++] = argb;
        uint32_t slot = (px[0] * 3u + px[1] * 5u + px[2] * 7u + px[3] * 11u) % 64u;
        index[slot] = argb;
    };

    bool ok = true;
    while (written < total && ok) {
        uint8_t tag;
        if (!media_read_u8(&r, &tag)) {
            ok = false;
            break;
        }
        if (tag == 0xFE) { // QOI_OP_RGB
            if (!media_read_u8(&r, &px[0]) || !media_read_u8(&r, &px[1]) || !media_read_u8(&r, &px[2])) {
                ok = false;
                break;
            }
        } else if (tag == 0xFF) { // QOI_OP_RGBA
            if (!media_read_u8(&r, &px[0]) || !media_read_u8(&r, &px[1]) || !media_read_u8(&r, &px[2]) ||
                !media_read_u8(&r, &px[3])) {
                ok = false;
                break;
            }
        } else if ((tag & 0xC0) == 0x00) { // QOI_OP_INDEX
            uint32_t argb = index[tag & 0x3F];
            px[0] = static_cast<uint8_t>(argb >> 16);
            px[1] = static_cast<uint8_t>(argb >> 8);
            px[2] = static_cast<uint8_t>(argb);
            px[3] = static_cast<uint8_t>(argb >> 24);
        } else if ((tag & 0xC0) == 0x40) { // QOI_OP_DIFF
            px[0] = static_cast<uint8_t>(px[0] + ((tag >> 4) & 0x03) - 2);
            px[1] = static_cast<uint8_t>(px[1] + ((tag >> 2) & 0x03) - 2);
            px[2] = static_cast<uint8_t>(px[2] + (tag & 0x03) - 2);
        } else if ((tag & 0xC0) == 0x80) { // QOI_OP_LUMA
            uint8_t extra;
            if (!media_read_u8(&r, &extra)) {
                ok = false;
                break;
            }
            int dg = (tag & 0x3F) - 32;
            int dr = dg + ((extra >> 4) & 0x0F) - 8;
            int db = dg + (extra & 0x0F) - 8;
            px[0] = static_cast<uint8_t>(px[0] + dr);
            px[1] = static_cast<uint8_t>(px[1] + dg);
            px[2] = static_cast<uint8_t>(px[2] + db);
        } else { // QOI_OP_RUN
            uint32_t run = (tag & 0x3F) + 1u;
            uint32_t argb = (static_cast<uint32_t>(px[3]) << 24) | (static_cast<uint32_t>(px[0]) << 16) |
                            (static_cast<uint32_t>(px[1]) << 8) | px[2];
            if (written + run > total) {
                ok = false;
                break;
            }
            for (uint32_t i = 0; i < run; i++)
                pixels[written++] = argb;
            uint32_t slot = (px[0] * 3u + px[1] * 5u + px[2] * 7u + px[3] * 11u) % 64u;
            index[slot] = argb;
            continue;
        }
        uint32_t argb = (static_cast<uint32_t>(px[3]) << 24) | (static_cast<uint32_t>(px[0]) << 16) |
                        (static_cast<uint32_t>(px[1]) << 8) | px[2];
        store_pixel(argb);
    }

    if (!ok || written != total) {
        media_free(pixels);
        return false;
    }

    out->pixels = pixels;
    out->width = static_cast<int32_t>(width);
    out->height = static_cast<int32_t>(height);
    return true;
}
