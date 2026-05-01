# UOWP Wallpaper Format

UOWP is the uniOS runtime wallpaper package format. It stores one or more wallpaper variants in a single file.

## Identity

- Extension: `.uowp`
- Magic: `UOWP`
- Byte order: little-endian
- Current runtime use: desktop wallpaper loading
- Current generated default package: light and dark variants

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

## Runtime Use

The GUI image loader selects a wallpaper entry by theme variant and display size. The selected image is scaled and cropped to cover the desktop.

The default wallpaper path is:

```text
/usr/share/wallpapers/default.uowp
```

Wallpaper settings are read from:

```text
/data/WALLPAPR.CFG
```

with fallback to:

```text
/etc/wallpaper.conf
```

## Generation

The default package is generated from:

```text
assets/wallpapers/wp_light.svg
assets/wallpapers/wp_dark.svg
```

The wallpaper packaging tool writes `.uowp` files:

```sh
python3 tools/wallpaper_package.py \
    --light assets/wallpapers/wp_light.svg \
    --dark assets/wallpapers/wp_dark.svg \
    --output default.uowp
```
