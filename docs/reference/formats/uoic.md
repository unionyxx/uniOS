# UOIC Icon Format

UOIC is the uniOS runtime icon package format. It stores pre-rendered icon images so userspace does not need to render SVG files at runtime.

## Identity

- Extension: `.uoic`
- Magic: `UOIC`
- Byte order: little-endian
- Runtime pixel format: BGRA8888
- Origin: top-left
- Alpha: straight or premultiplied, depending on the entry metadata

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

## Runtime Use

The GUI image loader reads UOIC files from the runtime filesystem and selects an entry by requested size, scale, and variant.

Current generated app icons are stored under:

```text
/usr/share/appicons/
```

## Generation

Source SVG files live under `appicons/`.

The icon generation tool renders fixed sizes and writes `.uoic` packages:

```sh
python3 tools/appicon_rasterize.py \
    --source-root appicons \
    --output-root rootfs/usr/share/appicons
```

The Meson build stages generated icon packages into the runtime filesystem.
