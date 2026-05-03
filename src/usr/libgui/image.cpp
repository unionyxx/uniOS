#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gui.h"

static constexpr uint32_t UOIC_MAGIC = 0x43494F55u; // "UOIC", little-endian
static constexpr uint16_t UOIC_VERSION = 1;
static constexpr uint16_t UOIC_CODEC_QOI = 1;
static constexpr uint16_t UOIC_CODEC_BGRA = 3;
static constexpr uint16_t UOIC_VARIANT_DEFAULT = 0;
static constexpr uint16_t UOIC_ALPHA_STRAIGHT = 1;
static constexpr uint16_t UOIC_ALPHA_PREMULTIPLIED = 2;

static constexpr uint32_t UOWP_MAGIC = 0x50574F55u; // "UOWP", little-endian
static constexpr uint16_t UOWP_VERSION = 1;
static constexpr uint16_t UOWP_CODEC_RAW = 4;
static constexpr uint16_t UOWP_COLOR_SPACE_SRGB = 1;
static constexpr uint16_t UOWP_TRANSFER_SDR = 1;

static constexpr uint32_t UOCU_MAGIC = 0x55434F55u; // "UOCU", little-endian
static constexpr uint16_t UOCU_VERSION = 1;
static constexpr uint16_t UOCU_CODEC_QOI = 1;
static constexpr uint16_t UOCU_CODEC_RAW = 3;

struct UoicHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint32_t metadata_offset;
    uint32_t metadata_size;
} __attribute__((packed));

struct UoicEntry
{
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
} __attribute__((packed));

struct UowpHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint32_t metadata_offset;
    uint32_t metadata_size;
} __attribute__((packed));

struct UowpEntry
{
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
} __attribute__((packed));

struct UocuHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint32_t metadata_offset;
    uint32_t metadata_size;
} __attribute__((packed));

struct UocuEntry
{
    uint16_t width;
    uint16_t height;
    uint16_t scale;
    uint16_t variant;
    uint16_t cursor_role;
    uint16_t codec;
    uint16_t hotspot_x;
    uint16_t hotspot_y;
    uint32_t frame_duration_ms;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t decoded_size;
    uint32_t checksum;
} __attribute__((packed));

static_assert(sizeof(UoicHeader) == 28, "UoicHeader layout must match the on-disk format");
static_assert(sizeof(UoicEntry) == 28, "UoicEntry layout must match the on-disk format");
static_assert(sizeof(UowpHeader) == 28, "UowpHeader layout must match the on-disk format");
static_assert(sizeof(UowpEntry) == 36, "UowpEntry layout must match the on-disk format");
static_assert(sizeof(UocuHeader) == 28, "UocuHeader layout must match the on-disk format");
static_assert(sizeof(UocuEntry) == 36, "UocuEntry layout must match the on-disk format");

struct QoiPixel
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

static bool read_file(const char *path, uint8_t **out_data, uint32_t *out_size)
{
    if (out_data)
        *out_data = nullptr;
    if (out_size)
        *out_size = 0;
    if (!path || !out_data || !out_size)
        return false;

    VNodeStat st = {};
    if (stat(path, &st) != 0 || st.is_dir || st.size == 0 || st.size > 0xFFFFFFFFu)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    uint8_t *data = static_cast<uint8_t *>(malloc((size_t)st.size));
    if (!data) {
        close(fd);
        return false;
    }

    uint64_t total = 0;
    while (total < st.size) {
        int n = read(fd, data + total, (size_t)(st.size - total));
        if (n <= 0)
            break;
        total += (uint64_t)n;
    }
    close(fd);
    if (total != st.size) {
        free(data);
        return false;
    }

    *out_data = data;
    *out_size = (uint32_t)st.size;
    return true;
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint32_t qoi_pixel_hash(const QoiPixel &px)
{
    return ((uint32_t)px.r * 3u + (uint32_t)px.g * 5u + (uint32_t)px.b * 7u + (uint32_t)px.a * 11u) & 63u;
}

static uint32_t pack_surface_pixel(QoiPixel px, uint16_t alpha_layout)
{
    if (px.a == 0) {
        return 0;
    }
    if (alpha_layout == UOIC_ALPHA_STRAIGHT && px.a < 255u) {
        px.r = (uint8_t)(((uint32_t)px.r * (uint32_t)px.a + 127u) / 255u);
        px.g = (uint8_t)(((uint32_t)px.g * (uint32_t)px.a + 127u) / 255u);
        px.b = (uint8_t)(((uint32_t)px.b * (uint32_t)px.a + 127u) / 255u);
    }
    return ((uint32_t)px.a << 24) | ((uint32_t)px.r << 16) | ((uint32_t)px.g << 8) | (uint32_t)px.b;
}

static bool uoic_entry_dimensions_valid(const UoicEntry &entry)
{
    if (entry.width == 0 || entry.height == 0)
        return false;
    uint64_t decoded_size = (uint64_t)entry.width * (uint64_t)entry.height * 4u;
    if (decoded_size == 0 || decoded_size > 0xFFFFFFFFu)
        return false;
    return entry.decoded_size == (uint32_t)decoded_size;
}

static bool uoic_data_range_valid(const UoicEntry &entry, uint32_t file_size)
{
    if (entry.data_size == 0 || entry.data_offset > file_size)
        return false;
    return (uint64_t)entry.data_offset + (uint64_t)entry.data_size <= (uint64_t)file_size;
}

static bool uoic_entry_supported(const UoicEntry &entry, uint32_t file_size)
{
    if (entry.variant != UOIC_VARIANT_DEFAULT)
        return false;
    if (entry.alpha != UOIC_ALPHA_STRAIGHT && entry.alpha != UOIC_ALPHA_PREMULTIPLIED)
        return false;
    if (entry.codec != UOIC_CODEC_QOI && entry.codec != UOIC_CODEC_BGRA)
        return false;
    return uoic_entry_dimensions_valid(entry) && uoic_data_range_valid(entry, file_size);
}

static const UoicEntry *uoic_select_entry(const UoicEntry *entries, uint32_t count, uint32_t file_size,
                                          uint32_t physical_px)
{
    const UoicEntry *smallest_covering = nullptr;
    const UoicEntry *largest = nullptr;

    for (uint32_t i = 0; i < count; i++) {
        const UoicEntry &entry = entries[i];
        if (!uoic_entry_supported(entry, file_size))
            continue;

        if (!largest || entry.width > largest->width)
            largest = &entry;

        if (entry.width >= physical_px && (!smallest_covering || entry.width < smallest_covering->width))
            smallest_covering = &entry;
    }

    return smallest_covering ? smallest_covering : largest;
}

static bool qoi_decode_to_surface(const uint8_t *encoded, uint32_t encoded_size, uint32_t expected_width,
                                  uint32_t expected_height, uint16_t alpha_layout, Surface *out)
{
    static const uint8_t qoi_padding[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    static constexpr uint32_t QOI_HEADER_SIZE = 14;
    static constexpr uint8_t QOI_OP_RGB = 0xFEu;
    static constexpr uint8_t QOI_OP_RGBA = 0xFFu;
    static constexpr uint8_t QOI_MASK_2 = 0xC0u;

    if (!encoded || !out || encoded_size < QOI_HEADER_SIZE + sizeof(qoi_padding))
        return false;
    if (memcmp(encoded, "qoif", 4) != 0)
        return false;
    if (memcmp(encoded + encoded_size - sizeof(qoi_padding), qoi_padding, sizeof(qoi_padding)) != 0)
        return false;

    uint32_t width = read_be32(encoded + 4);
    uint32_t height = read_be32(encoded + 8);
    uint8_t channels = encoded[12];
    if (width != expected_width || height != expected_height || (channels != 3u && channels != 4u))
        return false;

    Surface image = gui_create_surface(width, height);
    if (!image.buffer)
        return false;

    QoiPixel index[64] = {};
    QoiPixel px = {0, 0, 0, 255};
    uint32_t run = 0;
    uint32_t read_offset = QOI_HEADER_SIZE;
    const uint32_t payload_end = encoded_size - (uint32_t)sizeof(qoi_padding);
    const uint64_t pixel_count = (uint64_t)width * (uint64_t)height;

    for (uint64_t pixel_index = 0; pixel_index < pixel_count; pixel_index++) {
        if (run > 0) {
            run--;
        } else {
            if (read_offset >= payload_end) {
                gui_destroy_surface(&image);
                return false;
            }

            uint8_t b1 = encoded[read_offset++];
            if (b1 == QOI_OP_RGB) {
                if (read_offset + 3u > payload_end) {
                    gui_destroy_surface(&image);
                    return false;
                }
                px.r = encoded[read_offset++];
                px.g = encoded[read_offset++];
                px.b = encoded[read_offset++];
            } else if (b1 == QOI_OP_RGBA) {
                if (read_offset + 4u > payload_end) {
                    gui_destroy_surface(&image);
                    return false;
                }
                px.r = encoded[read_offset++];
                px.g = encoded[read_offset++];
                px.b = encoded[read_offset++];
                px.a = encoded[read_offset++];
            } else {
                uint8_t tag = b1 & QOI_MASK_2;
                if (tag == 0x00u) {
                    px = index[b1 & 0x3Fu];
                } else if (tag == 0x40u) {
                    px.r = (uint8_t)(px.r + ((b1 >> 4) & 0x03u) - 2u);
                    px.g = (uint8_t)(px.g + ((b1 >> 2) & 0x03u) - 2u);
                    px.b = (uint8_t)(px.b + (b1 & 0x03u) - 2u);
                } else if (tag == 0x80u) {
                    if (read_offset >= payload_end) {
                        gui_destroy_surface(&image);
                        return false;
                    }
                    uint8_t b2 = encoded[read_offset++];
                    int dg = (int)(b1 & 0x3Fu) - 32;
                    int dr_dg = (int)((b2 >> 4) & 0x0Fu) - 8;
                    int db_dg = (int)(b2 & 0x0Fu) - 8;
                    px.r = (uint8_t)(px.r + dg + dr_dg);
                    px.g = (uint8_t)(px.g + dg);
                    px.b = (uint8_t)(px.b + dg + db_dg);
                } else {
                    run = b1 & 0x3Fu;
                }
            }
        }

        index[qoi_pixel_hash(px)] = px;
        image.buffer[pixel_index] = pack_surface_pixel(px, alpha_layout);
    }

    *out = image;
    return true;
}

static bool bgra_decode_to_surface(const uint8_t *encoded, uint32_t encoded_size, const UoicEntry &entry, Surface *out)
{
    if (!encoded || !out || encoded_size != entry.decoded_size)
        return false;

    Surface image = gui_create_surface(entry.width, entry.height);
    if (!image.buffer)
        return false;

    uint32_t stride = image.pitch / 4u;
    for (uint32_t y = 0; y < image.height; y++) {
        uint32_t *dst = &image.buffer[(size_t)y * stride];
        const uint8_t *src = encoded + (size_t)y * (size_t)entry.width * 4u;
        for (uint32_t x = 0; x < image.width; x++) {
            QoiPixel px = {src[x * 4u + 2u], src[x * 4u + 1u], src[x * 4u], src[x * 4u + 3u]};
            dst[x] = pack_surface_pixel(px, entry.alpha);
        }
    }

    *out = image;
    return true;
}

static bool metadata_range_valid(uint32_t metadata_offset, uint32_t metadata_size, uint32_t file_size)
{
    if (metadata_size == 0)
        return metadata_offset == 0;
    if (metadata_offset > file_size)
        return false;
    return (uint64_t)metadata_offset + (uint64_t)metadata_size <= (uint64_t)file_size;
}

static bool uowp_entry_dimensions_valid(const UowpEntry &entry)
{
    if (entry.width == 0 || entry.height == 0)
        return false;
    uint64_t decoded_size = (uint64_t)entry.width * (uint64_t)entry.height * 4u;
    return decoded_size != 0 && decoded_size <= 0xFFFFFFFFu && entry.data_size == (uint32_t)decoded_size;
}

static bool uowp_data_range_valid(const UowpEntry &entry, uint32_t file_size)
{
    if (entry.data_size == 0 || entry.data_offset > file_size)
        return false;
    if ((uint64_t)entry.data_offset + (uint64_t)entry.data_size > (uint64_t)file_size)
        return false;
    if (entry.preview_size == 0)
        return entry.preview_offset == 0;
    if (entry.preview_offset > file_size)
        return false;
    return (uint64_t)entry.preview_offset + (uint64_t)entry.preview_size <= (uint64_t)file_size;
}

static bool uowp_entry_supported(const UowpEntry &entry, uint32_t file_size)
{
    if (entry.codec != UOWP_CODEC_RAW)
        return false;
    if (entry.color_space != UOWP_COLOR_SPACE_SRGB || entry.transfer != UOWP_TRANSFER_SDR)
        return false;
    return uowp_entry_dimensions_valid(entry) && uowp_data_range_valid(entry, file_size);
}

static bool uowp_entry_covers_target(const UowpEntry &entry, uint32_t target_width, uint32_t target_height)
{
    return target_width == 0 || target_height == 0 || (entry.width >= target_width && entry.height >= target_height);
}

static const UowpEntry *uowp_select_variant_entry(const UowpEntry *entries, uint32_t count, uint32_t file_size,
                                                  uint16_t variant, uint32_t target_width, uint32_t target_height)
{
    const UowpEntry *smallest_covering = nullptr;
    const UowpEntry *largest = nullptr;

    for (uint32_t i = 0; i < count; i++) {
        const UowpEntry &entry = entries[i];
        if (entry.variant != variant || !uowp_entry_supported(entry, file_size))
            continue;

        uint64_t area = (uint64_t)entry.width * (uint64_t)entry.height;
        if (!largest || area > (uint64_t)largest->width * (uint64_t)largest->height)
            largest = &entry;

        if (uowp_entry_covers_target(entry, target_width, target_height) &&
            (!smallest_covering || area < (uint64_t)smallest_covering->width * (uint64_t)smallest_covering->height)) {
            smallest_covering = &entry;
        }
    }

    return smallest_covering ? smallest_covering : largest;
}

static const UowpEntry *uowp_select_entry(const UowpEntry *entries, uint32_t count, uint32_t file_size,
                                          uint16_t preferred_variant, uint32_t target_width, uint32_t target_height)
{
    if (const UowpEntry *entry =
            uowp_select_variant_entry(entries, count, file_size, preferred_variant, target_width, target_height)) {
        return entry;
    }
    if (preferred_variant != GUI_UOWP_VARIANT_DEFAULT) {
        if (const UowpEntry *entry = uowp_select_variant_entry(entries, count, file_size, GUI_UOWP_VARIANT_DEFAULT,
                                                               target_width, target_height)) {
            return entry;
        }
    }

    const UowpEntry *smallest_covering = nullptr;
    const UowpEntry *largest = nullptr;
    for (uint32_t i = 0; i < count; i++) {
        const UowpEntry &entry = entries[i];
        if (!uowp_entry_supported(entry, file_size))
            continue;

        uint64_t area = (uint64_t)entry.width * (uint64_t)entry.height;
        if (!largest || area > (uint64_t)largest->width * (uint64_t)largest->height)
            largest = &entry;
        if (uowp_entry_covers_target(entry, target_width, target_height) &&
            (!smallest_covering || area < (uint64_t)smallest_covering->width * (uint64_t)smallest_covering->height)) {
            smallest_covering = &entry;
        }
    }
    return smallest_covering ? smallest_covering : largest;
}

static bool uowp_bgra_decode_to_surface(const uint8_t *encoded, const UowpEntry &entry, Surface *out)
{
    if (!encoded || !out)
        return false;

    Surface image = gui_create_surface(entry.width, entry.height);
    if (!image.buffer)
        return false;

    uint32_t stride = image.pitch / 4u;
    for (uint32_t y = 0; y < image.height; y++) {
        uint32_t *dst = &image.buffer[(size_t)y * stride];
        const uint8_t *src = encoded + (size_t)y * (size_t)entry.width * 4u;
        for (uint32_t x = 0; x < image.width; x++) {
            uint8_t b = src[x * 4u + 0u];
            uint8_t g = src[x * 4u + 1u];
            uint8_t r = src[x * 4u + 2u];
            uint8_t a = src[x * 4u + 3u];
            dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    *out = image;
    return true;
}

static bool uocu_entry_dimensions_valid(const UocuEntry &entry)
{
    if (entry.width == 0 || entry.height == 0)
        return false;
    uint64_t decoded_size = (uint64_t)entry.width * (uint64_t)entry.height * 4u;
    if (decoded_size == 0 || decoded_size > 0xFFFFFFFFu)
        return false;
    return entry.decoded_size == (uint32_t)decoded_size;
}

static bool uocu_data_range_valid(const UocuEntry &entry, uint32_t file_size)
{
    if (entry.data_size == 0 || entry.data_offset > file_size)
        return false;
    return (uint64_t)entry.data_offset + (uint64_t)entry.data_size <= (uint64_t)file_size;
}

static bool uocu_entry_supported(const UocuEntry &entry, uint16_t cursor_role, uint16_t variant, uint32_t file_size)
{
    if (entry.cursor_role != cursor_role || entry.variant != variant)
        return false;
    if (entry.hotspot_x >= entry.width || entry.hotspot_y >= entry.height)
        return false;
    if (entry.codec != UOCU_CODEC_QOI && entry.codec != UOCU_CODEC_RAW)
        return false;
    return uocu_entry_dimensions_valid(entry) && uocu_data_range_valid(entry, file_size);
}

static const UocuEntry *uocu_select_entry(const UocuEntry *entries, uint32_t count, uint32_t file_size,
                                          uint16_t cursor_role, uint16_t variant, uint32_t physical_px)
{
    const UocuEntry *best = nullptr;
    uint32_t best_delta = UINT32_MAX;

    for (uint32_t i = 0; i < count; i++) {
        const UocuEntry &entry = entries[i];
        if (!uocu_entry_supported(entry, cursor_role, variant, file_size))
            continue;

        uint32_t delta = entry.width > physical_px ? entry.width - physical_px : physical_px - entry.width;
        if (!best || delta < best_delta || (delta == best_delta && entry.width > best->width)) {
            best = &entry;
            best_delta = delta;
        }
    }

    return best;
}

static bool uocu_raw_decode_to_surface(const uint8_t *encoded, uint32_t encoded_size, const UocuEntry &entry,
                                       Surface *out)
{
    if (!encoded || !out || encoded_size != entry.decoded_size)
        return false;

    Surface image = gui_create_surface(entry.width, entry.height);
    if (!image.buffer)
        return false;

    uint32_t stride = image.pitch / 4u;
    for (uint32_t y = 0; y < image.height; y++) {
        uint32_t *dst = &image.buffer[(size_t)y * stride];
        const uint8_t *src = encoded + (size_t)y * (size_t)entry.width * 4u;
        for (uint32_t x = 0; x < image.width; x++) {
            uint8_t b = src[x * 4u + 0u];
            uint8_t g = src[x * 4u + 1u];
            uint8_t r = src[x * 4u + 2u];
            uint8_t a = src[x * 4u + 3u];
            dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    *out = image;
    return true;
}

extern "C" {

void gui_destroy_surface(Surface *s)
{
    if (!s)
        return;
    if (s->owns_buffer && s->buffer) {
        free(s->buffer);
    }
    s->buffer = nullptr;
    s->width = 0;
    s->height = 0;
    s->pitch = 0;
    s->owns_buffer = false;
    s->display_handle = 0;
}

bool gui_load_uoic(const char *path, uint32_t logical_px, uint32_t display_scale_pct, Surface *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    if (logical_px == 0)
        logical_px = 1;
    if (display_scale_pct == 0)
        display_scale_pct = 100;
    uint64_t physical_px64 = ((uint64_t)logical_px * (uint64_t)display_scale_pct + 50u) / 100u;
    uint32_t physical_px = physical_px64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)physical_px64;
    if (physical_px == 0)
        physical_px = 1;

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!read_file(path, &data, &size))
        return false;

    if (size < sizeof(UoicHeader)) {
        free(data);
        return false;
    }

    const UoicHeader *header = reinterpret_cast<const UoicHeader *>(data);
    if (header->magic != UOIC_MAGIC || header->version != UOIC_VERSION || header->flags != 0 ||
        header->entry_count == 0 || !metadata_range_valid(header->metadata_offset, header->metadata_size, size)) {
        free(data);
        return false;
    }

    uint64_t directory_bytes = (uint64_t)header->entry_count * sizeof(UoicEntry);
    if (directory_bytes == 0 || directory_bytes > 0xFFFFFFFFu || header->directory_size != (uint32_t)directory_bytes ||
        header->directory_offset < sizeof(UoicHeader) || header->directory_offset > size ||
        (uint64_t)header->directory_offset + directory_bytes > (uint64_t)size) {
        free(data);
        return false;
    }

    const UoicEntry *entries = reinterpret_cast<const UoicEntry *>(data + header->directory_offset);
    const UoicEntry *entry = uoic_select_entry(entries, header->entry_count, size, physical_px);
    if (!entry) {
        free(data);
        return false;
    }

    const uint8_t *encoded = data + entry->data_offset;
    bool ok = false;
    if (entry->codec == UOIC_CODEC_QOI)
        ok = qoi_decode_to_surface(encoded, entry->data_size, entry->width, entry->height, entry->alpha, out);
    else if (entry->codec == UOIC_CODEC_BGRA)
        ok = bgra_decode_to_surface(encoded, entry->data_size, *entry, out);
    else
        ok = false;

    free(data);
    if (!ok)
        memset(out, 0, sizeof(*out));
    return ok;
}

bool gui_load_uowp(const char *path, uint16_t preferred_variant, uint32_t target_width, uint32_t target_height,
                   Surface *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!read_file(path, &data, &size))
        return false;

    if (size < sizeof(UowpHeader)) {
        free(data);
        return false;
    }

    const UowpHeader *header = reinterpret_cast<const UowpHeader *>(data);
    if (header->magic != UOWP_MAGIC || header->version != UOWP_VERSION || header->flags != 0 ||
        header->entry_count == 0 || !metadata_range_valid(header->metadata_offset, header->metadata_size, size)) {
        free(data);
        return false;
    }

    uint64_t directory_bytes = (uint64_t)header->entry_count * sizeof(UowpEntry);
    if (directory_bytes == 0 || directory_bytes > 0xFFFFFFFFu || header->directory_size != (uint32_t)directory_bytes ||
        header->directory_offset < sizeof(UowpHeader) || header->directory_offset > size ||
        (uint64_t)header->directory_offset + directory_bytes > (uint64_t)size) {
        free(data);
        return false;
    }

    const UowpEntry *entries = reinterpret_cast<const UowpEntry *>(data + header->directory_offset);
    const UowpEntry *entry =
        uowp_select_entry(entries, header->entry_count, size, preferred_variant, target_width, target_height);
    if (!entry) {
        free(data);
        return false;
    }

    const uint8_t *encoded = data + entry->data_offset;
    bool ok = false;
    if (entry->codec == UOWP_CODEC_RAW)
        ok = uowp_bgra_decode_to_surface(encoded, *entry, out);

    free(data);
    if (!ok)
        memset(out, 0, sizeof(*out));
    return ok;
}

bool gui_load_uocu(const char *path, uint16_t cursor_role, uint32_t logical_px, uint32_t display_scale_pct,
                   uint16_t preferred_variant, Surface *out, uint16_t *hotspot_x, uint16_t *hotspot_y,
                   uint32_t *frame_duration_ms)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (hotspot_x)
        *hotspot_x = 0;
    if (hotspot_y)
        *hotspot_y = 0;
    if (frame_duration_ms)
        *frame_duration_ms = 0;

    if (logical_px == 0)
        logical_px = 1;
    if (display_scale_pct == 0)
        display_scale_pct = 100;
    uint64_t physical_px64 = ((uint64_t)logical_px * (uint64_t)display_scale_pct + 50u) / 100u;
    uint32_t physical_px = physical_px64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)physical_px64;
    if (physical_px == 0)
        physical_px = 1;

    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!read_file(path, &data, &size))
        return false;

    if (size < sizeof(UocuHeader)) {
        free(data);
        return false;
    }

    const UocuHeader *header = reinterpret_cast<const UocuHeader *>(data);
    if (header->magic != UOCU_MAGIC || header->version != UOCU_VERSION || header->flags != 0 ||
        header->entry_count == 0 || !metadata_range_valid(header->metadata_offset, header->metadata_size, size)) {
        free(data);
        return false;
    }

    uint64_t directory_bytes = (uint64_t)header->entry_count * sizeof(UocuEntry);
    if (directory_bytes == 0 || directory_bytes > 0xFFFFFFFFu || header->directory_size != (uint32_t)directory_bytes ||
        header->directory_offset < sizeof(UocuHeader) || header->directory_offset > size ||
        (uint64_t)header->directory_offset + directory_bytes > (uint64_t)size) {
        free(data);
        return false;
    }

    const UocuEntry *entries = reinterpret_cast<const UocuEntry *>(data + header->directory_offset);
    const UocuEntry *entry =
        uocu_select_entry(entries, header->entry_count, size, cursor_role, preferred_variant, physical_px);
    if (!entry && preferred_variant != GUI_UOCU_VARIANT_DEFAULT) {
        entry = uocu_select_entry(entries, header->entry_count, size, cursor_role, GUI_UOCU_VARIANT_DEFAULT,
                                  physical_px);
    }
    if (!entry) {
        free(data);
        return false;
    }

    const uint8_t *encoded = data + entry->data_offset;
    bool ok = false;
    if (entry->codec == UOCU_CODEC_QOI) {
        ok = qoi_decode_to_surface(encoded, entry->data_size, entry->width, entry->height, UOIC_ALPHA_PREMULTIPLIED,
                                   out);
    } else if (entry->codec == UOCU_CODEC_RAW) {
        ok = uocu_raw_decode_to_surface(encoded, entry->data_size, *entry, out);
    }

    if (ok) {
        if (hotspot_x)
            *hotspot_x = entry->hotspot_x;
        if (hotspot_y)
            *hotspot_y = entry->hotspot_y;
        if (frame_duration_ms)
            *frame_duration_ms = entry->frame_duration_ms;
    }

    free(data);
    if (!ok)
        memset(out, 0, sizeof(*out));
    return ok;
}

void gui_blit_scaled_cover(Surface *dest, const Surface *src)
{
    if (!dest || !src || !dest->buffer || !src->buffer || dest->width == 0 || dest->height == 0 || src->width == 0 ||
        src->height == 0)
        return;

    uint64_t scaled_w_by_h = (uint64_t)src->width * dest->height;
    uint64_t scaled_h_by_w = (uint64_t)src->height * dest->width;
    uint32_t sample_w = src->width;
    uint32_t sample_h = src->height;
    uint32_t src_x0 = 0;
    uint32_t src_y0 = 0;

    if (scaled_w_by_h > scaled_h_by_w) {
        sample_w = (uint32_t)(((uint64_t)src->height * dest->width) / dest->height);
        if (sample_w == 0)
            sample_w = 1;
        if (sample_w > src->width)
            sample_w = src->width;
        src_x0 = (src->width - sample_w) / 2u;
    } else if (scaled_h_by_w > scaled_w_by_h) {
        sample_h = (uint32_t)(((uint64_t)src->width * dest->height) / dest->width);
        if (sample_h == 0)
            sample_h = 1;
        if (sample_h > src->height)
            sample_h = src->height;
        src_y0 = (src->height - sample_h) / 2u;
    }

    uint32_t dest_stride = dest->pitch / 4;
    uint32_t src_stride = src->pitch / 4;
    for (uint32_t y = 0; y < dest->height; y++) {
        uint32_t sy = src_y0 + (uint32_t)(((uint64_t)y * sample_h) / dest->height);
        if (sy >= src->height)
            sy = src->height - 1u;
        for (uint32_t x = 0; x < dest->width; x++) {
            uint32_t sx = src_x0 + (uint32_t)(((uint64_t)x * sample_w) / dest->width);
            if (sx >= src->width)
                sx = src->width - 1u;
            dest->buffer[(size_t)y * dest_stride + x] = src->buffer[(size_t)sy * src_stride + sx];
        }
    }
}
}
