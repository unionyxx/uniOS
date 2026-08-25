# UOWP Wallpaper Format

UOWP is the uniOS runtime wallpaper package format. It stores one or more wallpaper variants in a single file.

## Identity

- Extension: `.uowp`
- Magic: `0x50574F55` (`UOWP`, little-endian)
- Byte order: little-endian
- Current runtime use: desktop wallpaper loading
- Default package: light and dark variants built at meson time

## Header

```c
#define UOWP_MAGIC 0x50574F55u // "UOWP", little-endian

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint32_t metadata_offset;
    uint32_t metadata_size;
} UowpHeader;
```

## Directory Entry

```c
typedef struct {
    uint32_t width;
    uint32_t height;
    uint16_t codec;
    uint16_t variant;
    uint16_t color_space;
    uint16_t transfer;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t preview_offset;
    uint32_t preview_size;
    uint32_t checksum;
} UowpEntry;
```

- `codec`: RAW = 4.
- `variant`: default 0, light 1, dark 2, lock blur 3, dynamic 4.
- `color_space`: sRGB = 1; `transfer`: SDR = 1.
- Entries may carry an embedded preview image.

## Runtime Use

The GUI loader (`gui_load_uowp`) selects an entry by preferred theme variant and target size, then `gui_blit_scaled_cover` scales and crops it to cover the desktop. The window manager resolves the active path from `/data/WALLPAPR.CFG` (fallback `/etc/wallpaper.conf`), the registry request, or the default:

```text
/usr/share/wallpapers/default.uowp
```

See [Runtime configuration](../config.md) and [Window manager](../wm.md).

## Generation

The default package is generated from `assets/wallpapers/wp_light.svg` and `wp_dark.svg` (gitignored inputs):

```sh
python3 tools/wallpaper_package.py \
    --light assets/wallpapers/wp_light.svg \
    --dark assets/wallpapers/wp_dark.svg \
    --output default.uowp
```

Unlike icons and cursors, the default `.uowp` is built at meson time and is not committed.
