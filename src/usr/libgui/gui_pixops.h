#pragma once

#include <stdint.h>

// SSE2 row primitives for XRGB8888 surfaces (4 bytes/pixel, premultiplied
// alpha convention). Rows may start at arbitrary pixel offsets; loads and
// stores are unaligned-safe. Cached stores only — these targets are read
// back immediately by the next pass, so non-temporal stores are forbidden.

void pix_copy_row(uint32_t *dst, const uint32_t *src, uint32_t count);
void pix_fill_row(uint32_t *dst, uint32_t count, uint32_t color);

// Premultiplied src-over: out = src + dst * (255 - src_a) / 255.
// Bit-exact with gui_blend_premultiplied for valid premultiplied input
// (src channels never exceed src alpha). Division by 255 uses the
// (x + 128 + ((x + 128) >> 8)) >> 8 approximation.
void pix_blend_row_premultiplied(uint32_t *dst, const uint32_t *src, uint32_t count);

// Same blend, output alpha forced to 0xFF (opaque destination).
void pix_blend_row_premultiplied_opaque_dst(uint32_t *dst, const uint32_t *src, uint32_t count);

// Scalar references used by the self-test and as overlap fallbacks.
void pix_blend_row_premultiplied_ref(uint32_t *dst, const uint32_t *src, uint32_t count);
void pix_blend_row_premultiplied_opaque_dst_ref(uint32_t *dst, const uint32_t *src, uint32_t count);

// Randomized diff of the SIMD row ops against the scalar references.
// Returns true when every case matches.
bool gui_pixops_self_test(void);
