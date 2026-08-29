# UOF Font Format

UOF is the uniOS runtime font format. It stores preprocessed bitmap font data (glyph atlas + metrics) for the userspace GUI library, so no desktop font parsing happens at runtime.

## Identity

- Extension: `.uof`
- Magic: `0x4E464F55` (`UOFN`, little-endian)
- Version: 1
- Runtime users: libgui text rendering (apps, menubar, dock, window manager, terminal)

## File Layout

```c
typedef struct {
    uint32_t magic;         // 0x4E464F55
    uint16_t version;
    uint16_t flags;
    uint16_t pixel_size;
    uint16_t atlas_width;
    uint16_t atlas_height;
    int16_t  ascent;
    int16_t  descent;
    int16_t  line_gap;
    uint16_t glyph_count;
    uint16_t kerning_count;
    uint16_t fallback_index;
    uint32_t glyph_offset;
    uint32_t kerning_offset;
    uint32_t atlas_offset;
} UofHeader; // packed
```

The glyph table follows the header, then kerning pairs, then the 8-bit alpha atlas. Each glyph records its codepoint, atlas position, size, bearings, and advance. Loaders validate all offsets and counts against the file size.

## Runtime Use

At startup the GUI picks a pixel size from the framebuffer dimensions (11-15, clamped to 11..18) and loads the nearest size of each family from `/usr/share/fonts/`:

| Prefix | Family | Use |
| --- | --- | --- |
| `inter-ui` | Inter UI | Default UI text |
| `inter-title` | Inter Title | Headings and emphasis |
| `geist-mono` | Geist Mono | Terminal and code |

Filenames follow `<prefix>-<size>.uof`. The atlas stores 8-bit linear pixel-coverage per glyph; the runtime blitter lifts fg/bg to linear light, blends by coverage, and encodes back to sRGB, so anti-aliased edges composite with correct perceptual weights. Only light text on a dark backdrop gets coverage shaping (a strong gamma pulls its bright fringes toward the background, which otherwise read as a bold halo); dark text on a light backdrop is blended unshaped, since any coverage gamma crushes thin strokes and makes weight uneven. When a glyph is drawn over a fully opaque background, the blend is write-only (no destination read). When a font fails to load, a built-in 8x8 bitmap font takes over.

Generated font files are staged under:

```text
/usr/share/fonts/
```

## Generation

Font sources live under `assets/fonts/`. The conversion tool writes `.uof` files:

```sh
python3 tools/uof_convert.py
```

Generated binaries are committed; keep binaries and sources in sync in the same change. UOF is not a general desktop font format — it exists to avoid parsing full desktop font formats inside the GUI runtime.
