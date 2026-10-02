# UOF Font Format

UOF is the uniOS runtime font format. It stores preprocessed bitmap font data (coverage atlas + metrics + kerning) for the userspace GUI library, so no desktop font parsing happens at runtime.

## Identity

- Extension: `.uof`
- Magic: `0x4E464F55` (`UOFN`, little-endian)
- Version: 2
- Runtime users: libgui text rendering (apps, menubar, dock, window manager, terminal)

## File Layout

```c
typedef struct {              // 54 bytes, packed, little-endian
    uint32_t magic;           // 0x4E464F55
    uint16_t version;         // 2
    uint16_t flags;           // bit0: horizontally oversampled atlas
    uint16_t pixel_size;
    uint16_t oversample_x;    // subcolumns per output pixel (4 or 1)
    uint16_t atlas_width;     // output pixels; row stride = width * oversample_x
    uint16_t atlas_height;
    int16_t  ascent;          // px
    int16_t  descent;         // px
    int16_t  line_gap;        // px
    uint32_t glyph_count;
    uint32_t kerning_count;   // exception pairs
    uint16_t matrix_c1;       // kern class matrix dims (row = left glyph class)
    uint16_t matrix_c2;
    uint32_t fallback_index;
    uint32_t glyph_offset;
    uint32_t kerning_offset;
    uint32_t matrix_offset;
    uint32_t atlas_offset;    // RLE stream runs to end of file
} UofHeader2;

typedef struct {              // 20 bytes, sorted by codepoint
    uint32_t codepoint;
    uint16_t atlas_x;         // subcolumn units
    uint16_t atlas_y;
    uint16_t width;           // output px (oversampled: includes 1 px pad per side)
    uint16_t height;
    int16_t  bearing_x26;     // 26.6 fixed point
    int16_t  bearing_y;       // px
    int16_t  advance_x26;     // 26.6 fixed point
    uint8_t  kern_left;       // class id when this glyph is the left pair member
    uint8_t  kern_right;      // class id when this glyph is the right pair member
} UofGlyph2;

typedef struct {              // 10 bytes, sorted by (left, right), unique
    uint32_t left_cp;
    uint32_t right_cp;
    int16_t  value;           // 26.6, additive with the class matrix
} UofKern2;
```

Sections follow in order: glyph table, kerning exceptions, the `(matrix_c1+1) x (matrix_c2+1)` row-major `int16` class matrix (26.6 values), then the atlas.

The atlas is 8-bit coverage, RLE-compressed as `(run_length-1, value)` byte pairs (max run 256) expanding to exactly `atlas_width * oversample_x * atlas_height` bytes. Loaders validate every offset, count and rect before trusting the file, and reject anything that is not version 2.

## Rasterization model

- Glyphs are rendered from the source outlines at 4x pixel size with hinting disabled and box-filtered down vertically; horizontally, UI fonts keep full 4x resolution (oversampling) so the runtime can position glyphs on true subpixel positions.
- Runtime subpixel positioning: the pen accumulates in 26.6 fixed point; each glyph's phase (fractional pen position, quantized to 1/16 px) selects a 5-tap FIR filter over the subcolumns of its output pixels. Mono fonts skip this (integer grid, single sample per pixel).
- Kerning comes from GPOS pair positioning, flattened into the class matrix plus additive exception pairs; the runtime looks up `matrix[left->kern_left][right->kern_right] + exception(left_cp, right_cp)` between adjacent glyphs.
- Advances and kerning are 26.6, so spacing matches the scalable font metrics instead of rounded integers.
- The 8-bit coverage is linear pixel area. The runtime blitter blends in linear light with a polarity-aware coverage gamma (light-on-dark text gets shaped coverage, dark-on-light is blended unshaped); see `src/usr/libgui/font.cpp`.

## Runtime Use

At startup the GUI picks a pixel size from the framebuffer dimensions (11-15, clamped to 11..18) and loads the nearest size of each family from `/usr/share/fonts/`:

| Prefix | Family | Use | Oversample |
| --- | --- | --- | --- |
| `inter-ui` | Inter UI (regular) | Default UI text | 4 |
| `inter-title` | Inter UI (semibold) | Headings and emphasis | 4 |
| `geist-mono` | Geist Mono | Terminal and code | 1 (integer grid) |

Filenames follow `<prefix>-<size>.uof`. Text strings are decoded as UTF-8 (invalid bytes render the fallback glyph), and codepoints beyond the charset also render the fallback glyph. Measurement, truncation and drawing share one walker (`src/usr/libgui/font_internal.h`), so laid-out width always matches drawn width.

## Generation

Font sources live under `assets/fonts_src/`. Regenerate all families and sizes (8-18):

```sh
pip install freetype-py fonttools   # cross-platform; wheels bundle FreeType
python3 tools/uof_convert.py --all
```

Single file: `python3 tools/uof_convert.py --font <src> --output <out>.uof --size <n> --charset ui|mono`. The `ui` charset covers printable ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic and typographic punctuation; the `mono` charset covers ASCII, common Latin-1 punctuation/symbols and the box-drawing block. Codepoints the font lacks are skipped silently.

The tool self-validates every emitted file (re-parses it, checks bounds and sort order). `tools/uof_preview.py` renders sample text from a `.uof` with the same math as the runtime for host-side visual checks:

```sh
python3 tools/uof_preview.py rootfs/usr/share/fonts/inter-ui-13.uof --scale 4
```

Generated font files are committed; keep binaries and sources in sync in the same change. UOF is not a general desktop font format — it exists to avoid parsing full desktop font formats inside the GUI runtime.
