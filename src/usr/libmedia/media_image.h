#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decoded image: straight-alpha ARGB8888 (A<<24 | R<<16 | G<<8 | B).
struct media_image {
    uint32_t *pixels;
    int32_t width;
    int32_t height;
};

// Decode PNG, JPEG (baseline), BMP (24/32-bit BI_RGB), GIF (first frame) or
// QOI from memory. Returns false on unknown format, malformed input, or images
// above the decode limits; out is untouched on failure.
bool media_image_decode(const void *data, size_t size, struct media_image *out);

void media_image_free(struct media_image *img);

// Bilinear downscale/upscale into a freshly allocated image.
bool media_image_scale(const struct media_image *src, struct media_image *dst, int32_t dst_w, int32_t dst_h);

#ifdef __cplusplus
}
#endif
