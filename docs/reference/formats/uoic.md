# UOIC Icon Format

UOIC is the uniOS runtime icon package format. It stores pre-rendered icon images so userspace does not need to render SVG files at runtime.

## Identity

- Extension: `.uoic`
- Magic: `0x43494F55` (`UOIC`, little-endian)
- Byte order: little-endian
- Runtime pixel format: BGRA8888
- Origin: top-left
- Alpha: straight or premultiplied, per entry metadata

## Header

```c
#define UOIC_MAGIC 0x43494F55u // "UOIC", little-endian

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint32_t metadata_offset;
    uint32_t metadata_size;
} UoicHeader;
```

## Directory Entry

```c
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t scale;
    uint16_t variant;
    uint16_t codec;
    uint16_t alpha;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t decoded_size;
    uint32_t checksum;
} UoicEntry;
```

- `codec`: QOI = 1, BGRA = 3.
- `alpha`: straight = 1, premultiplied = 2.
- Entries carry a decoded-size and checksum for validation.

## Runtime Use

The GUI image loader (`gui_load_uoic`) selects the smallest entry that covers the requested physical size (logical size x display scale), falling back to the largest entry. Decoding supports QOI and raw BGRA.

Generated app icons are staged under:

```text
/usr/share/appicons/
```

## Generation

Source SVG files live under `appicons/`. The icon generation tool renders fixed sizes and writes `.uoic` packages:

```sh
python3 tools/appicon_rasterize.py \
    --source-root appicons \
    --output-root rootfs/usr/share/appicons
```

Generated binaries are committed; keep binaries and sources in sync in the same change.
