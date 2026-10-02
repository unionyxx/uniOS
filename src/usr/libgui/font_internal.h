#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gui.h"

// Internal text-walking ABI shared by libgui measurement, truncation and
// drawing. One walker decodes UTF-8 (invalid bytes fall back to the font's
// fallback glyph, never crashing), applies GPOS kerning, and accumulates the
// pen in 26.6 fixed point so glyph origins land on true subpixel positions.

typedef struct
{
    const GuiFont *font;
    const char *it;  // next byte to decode
    const char *end; // one past the last byte of the run
    int32_t pen26;   // accumulated advance after the last glyph, 26.6
    const GuiGlyph *prev_glyph;
    uint32_t prev_cp;
} GuiTextWalk;

// Begin a run over str, stopping at the first of: len bytes consumed, NUL,
// or '\n'. Returns the run length in bytes. font may be null (the walker
// then reports the built-in 8x8 fallback metrics, 8 px per byte).
size_t gui_text_walk_init(GuiTextWalk *w, const GuiFont *font, const char *str, size_t len);

// Decode the next codepoint, add the kern between the previous and current
// glyph, record the glyph origin (pen after kerning) and the byte count
// consumed, then advance the pen by the glyph advance. Returns the glyph
// (never null for a loaded font; the fallback glyph covers misses) or null
// when the run is exhausted (or the font is null).
const GuiGlyph *gui_text_walk_next(GuiTextWalk *w, uint32_t *codepoint, int32_t *pos26, size_t *consumed);

// Total run advance (26.6) measured without drawing; bounded like the walker.
int32_t gui_text_advance26(const GuiFont *font, const char *str, size_t len);
