#include <drivers/video/display.h>
#include <kernel/ktest.h>
#include <libk/kstring.h>
#include <uapi/display.h>

static void init_edid(uint8_t *edid, uint32_t block_count)
{
    if (!edid || block_count == 0)
        return;
    kstring::zero_memory(edid, (size_t)block_count * 128u);

    static const uint8_t k_header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    for (uint32_t i = 0; i < sizeof(k_header); i++)
        edid[i] = k_header[i];
    edid[18] = 0x01;
    edid[19] = 0x04;
    edid[20] = 0xA5;
    edid[24] = 0x0A;
    edid[35] = 0x01;
    edid[36] = 0x01;
    edid[37] = 0x01;
    for (uint32_t offset = 38; offset < 54; offset += 2) {
        edid[offset] = 0x01;
        edid[offset + 1] = 0x01;
    }
    edid[126] = (uint8_t)(block_count - 1u);
}

static void finalize_edid_block(uint8_t *block)
{
    if (!block)
        return;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 127; i++)
        sum += block[i];
    block[127] = (uint8_t)((256u - (sum & 0xFFu)) & 0xFFu);
}

static void write_detailed_timing(uint8_t *dtd, uint32_t width, uint32_t height, uint32_t refresh_hz, bool interlaced)
{
    if (!dtd)
        return;
    kstring::zero_memory(dtd, 18);

    uint32_t h_blank = width / 8u;
    if (h_blank < 80u)
        h_blank = 80u;
    uint32_t v_blank = height / 20u;
    if (v_blank < 20u)
        v_blank = 20u;

    uint32_t h_total = width + h_blank;
    uint32_t v_total = height + v_blank;
    uint64_t pixel_clock_10khz = ((uint64_t)h_total * (uint64_t)v_total * (uint64_t)refresh_hz + 5000ULL) / 10000ULL;
    uint32_t h_sync_offset = 48u;
    uint32_t h_sync_width = 32u;
    uint32_t v_sync_offset = 3u;
    uint32_t v_sync_width = 5u;

    dtd[0] = (uint8_t)(pixel_clock_10khz & 0xFFu);
    dtd[1] = (uint8_t)((pixel_clock_10khz >> 8) & 0xFFu);
    dtd[2] = (uint8_t)(width & 0xFFu);
    dtd[3] = (uint8_t)(h_blank & 0xFFu);
    dtd[4] = (uint8_t)(((width >> 8) & 0x0Fu) << 4);
    dtd[4] |= (uint8_t)((h_blank >> 8) & 0x0Fu);
    dtd[5] = (uint8_t)(height & 0xFFu);
    dtd[6] = (uint8_t)(v_blank & 0xFFu);
    dtd[7] = (uint8_t)(((height >> 8) & 0x0Fu) << 4);
    dtd[7] |= (uint8_t)((v_blank >> 8) & 0x0Fu);
    dtd[8] = (uint8_t)(h_sync_offset & 0xFFu);
    dtd[9] = (uint8_t)(h_sync_width & 0xFFu);
    dtd[10] = (uint8_t)(((v_sync_offset & 0x0Fu) << 4) | (v_sync_width & 0x0Fu));
    dtd[11] = (uint8_t)(((h_sync_offset >> 8) & 0x03u) << 6);
    dtd[11] |= (uint8_t)(((h_sync_width >> 8) & 0x03u) << 4);
    dtd[11] |= (uint8_t)(((v_sync_offset >> 4) & 0x03u) << 2);
    dtd[11] |= (uint8_t)((v_sync_width >> 4) & 0x03u);
    dtd[17] = interlaced ? 0x80u : 0x00u;
}

static void write_detailed_timing_raw(uint8_t *dtd, uint32_t width, uint32_t height, uint32_t h_blank, uint32_t v_blank,
                                      uint32_t pixel_clock_10khz, bool interlaced)
{
    if (!dtd)
        return;
    kstring::zero_memory(dtd, 18);

    uint32_t h_sync_offset = h_blank > 32u ? 16u : 8u;
    uint32_t h_sync_width =
        h_blank > (h_sync_offset + 32u) ? 32u : (h_blank > (h_sync_offset + 1u) ? (h_blank - h_sync_offset - 1u) : 1u);
    uint32_t v_sync_offset = v_blank > 8u ? 3u : 1u;
    uint32_t v_sync_width =
        v_blank > (v_sync_offset + 4u) ? 4u : (v_blank > v_sync_offset ? (v_blank - v_sync_offset) : 1u);

    dtd[0] = (uint8_t)(pixel_clock_10khz & 0xFFu);
    dtd[1] = (uint8_t)((pixel_clock_10khz >> 8) & 0xFFu);
    dtd[2] = (uint8_t)(width & 0xFFu);
    dtd[3] = (uint8_t)(h_blank & 0xFFu);
    dtd[4] = (uint8_t)(((width >> 8) & 0x0Fu) << 4);
    dtd[4] |= (uint8_t)((h_blank >> 8) & 0x0Fu);
    dtd[5] = (uint8_t)(height & 0xFFu);
    dtd[6] = (uint8_t)(v_blank & 0xFFu);
    dtd[7] = (uint8_t)(((height >> 8) & 0x0Fu) << 4);
    dtd[7] |= (uint8_t)((v_blank >> 8) & 0x0Fu);
    dtd[8] = (uint8_t)(h_sync_offset & 0xFFu);
    dtd[9] = (uint8_t)(h_sync_width & 0xFFu);
    dtd[10] = (uint8_t)(((v_sync_offset & 0x0Fu) << 4) | (v_sync_width & 0x0Fu));
    dtd[11] = (uint8_t)(((h_sync_offset >> 8) & 0x03u) << 6);
    dtd[11] |= (uint8_t)(((h_sync_width >> 8) & 0x03u) << 4);
    dtd[11] |= (uint8_t)(((v_sync_offset >> 4) & 0x03u) << 2);
    dtd[11] |= (uint8_t)((v_sync_width >> 4) & 0x03u);
    dtd[17] = interlaced ? 0x80u : 0x00u;
}

static void write_range_limits(uint8_t *descriptor, uint8_t min_hz, uint8_t max_hz)
{
    if (!descriptor)
        return;
    kstring::zero_memory(descriptor, 18);
    descriptor[3] = 0xFD;
    descriptor[5] = min_hz;
    descriptor[6] = max_hz;
    descriptor[7] = 0xFF;
    descriptor[8] = 0xFF;
}

static void write_cta_video_block(uint8_t *ext, uint8_t vic)
{
    if (!ext)
        return;
    kstring::zero_memory(ext, 128);
    ext[0] = 0x02;
    ext[1] = 0x03;
    ext[2] = 0x06;
    ext[4] = (uint8_t)((0x02u << 5) | 0x01u);
    ext[5] = vic;
}

static void write_displayid_detailed_block(uint8_t *ext, uint32_t width, uint32_t height, uint32_t refresh_hz,
                                           bool type_7)
{
    if (!ext)
        return;
    kstring::zero_memory(ext, 128);
    ext[0] = 0x70;
    ext[1] = 0x20;
    ext[2] = 23;
    ext[3] = 0x00;
    ext[4] = 0x00;
    ext[5] = type_7 ? 0x22u : 0x03u;
    ext[6] = 0x01u;
    ext[7] = 20u;

    uint8_t *timing = &ext[8];
    uint32_t h_blank = width / 8u;
    if (h_blank < 80u)
        h_blank = 80u;
    uint32_t v_blank = height / 20u;
    if (v_blank < 20u)
        v_blank = 20u;
    uint32_t h_total = width + h_blank;
    uint32_t v_total = height + v_blank;
    uint64_t pixel_clock_hz = (uint64_t)h_total * (uint64_t)v_total * (uint64_t)refresh_hz;
    uint32_t pixel_clock_units =
        type_7 ? (uint32_t)((pixel_clock_hz + 500ULL) / 1000ULL) : (uint32_t)((pixel_clock_hz + 5000ULL) / 10000ULL);

    timing[0] = (uint8_t)(pixel_clock_units & 0xFFu);
    timing[1] = (uint8_t)((pixel_clock_units >> 8) & 0xFFu);
    timing[2] = (uint8_t)((pixel_clock_units >> 16) & 0xFFu);
    timing[4] = (uint8_t)((width - 1u) & 0xFFu);
    timing[5] = (uint8_t)(((width - 1u) >> 8) & 0xFFu);
    timing[6] = (uint8_t)((h_blank - 1u) & 0xFFu);
    timing[7] = (uint8_t)(((h_blank - 1u) >> 8) & 0xFFu);
    timing[8] = 0x2Fu;
    timing[10] = 0x1Fu;
    timing[12] = (uint8_t)((height - 1u) & 0xFFu);
    timing[13] = (uint8_t)(((height - 1u) >> 8) & 0xFFu);
    timing[14] = (uint8_t)((v_blank - 1u) & 0xFFu);
    timing[15] = (uint8_t)(((v_blank - 1u) >> 8) & 0xFFu);
    timing[16] = 0x02u;
    timing[18] = 0x05u;
}

static void write_displayid_formula_block(uint8_t *ext, uint32_t width, uint32_t height, uint32_t refresh_hz)
{
    if (!ext)
        return;
    kstring::zero_memory(ext, 128);
    ext[0] = 0x70;
    ext[1] = 0x20;
    ext[2] = 9;
    ext[3] = 0x00;
    ext[4] = 0x00;
    ext[5] = 0x24u;
    ext[6] = 0x01u;
    ext[7] = 6u;

    uint8_t *timing = &ext[8];
    timing[0] = 0x00u;
    timing[1] = (uint8_t)((width - 1u) & 0xFFu);
    timing[2] = (uint8_t)(((width - 1u) >> 8) & 0xFFu);
    timing[3] = (uint8_t)((height - 1u) & 0xFFu);
    timing[4] = (uint8_t)(((height - 1u) >> 8) & 0xFFu);
    timing[5] = (uint8_t)(refresh_hz - 1u);
}

static bool clip_rect_for_test(Rect &rect, const DisplayCaps &caps)
{
    int64_t x = rect.x;
    int64_t y = rect.y;
    int64_t w = rect.w;
    int64_t h = rect.h;

    if (w <= 0 || h <= 0)
        return false;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    int64_t max_width = (int64_t)caps.width;
    int64_t max_height = (int64_t)caps.height;
    if (x >= max_width || y >= max_height)
        return false;
    if (x + w > max_width)
        w = max_width - x;
    if (y + h > max_height)
        h = max_height - y;
    if (w <= 0 || h <= 0)
        return false;

    rect.x = (int32_t)x;
    rect.y = (int32_t)y;
    rect.w = (int32_t)w;
    rect.h = (int32_t)h;
    return true;
}

static void drain_display_events_for_test()
{
    DisplayEvent event = {};
    while (display_event_wait(&event, false)) {
    }
}

static DisplayAtomicState g_test_atomic_state = {};
static DisplayMode g_test_atomic_mode = {};
static bool g_test_atomic_commit_called = false;
static bool g_test_atomic_commit_should_succeed = true;

static bool test_backend_get_caps(DisplayDevice *device, DisplayCaps *out_caps)
{
    if (!device || !out_caps)
        return false;
    *out_caps = device->primary_head.caps;
    return true;
}

static bool test_backend_get_status(DisplayDevice *device, DisplayStatus *out_status)
{
    if (!device || !out_status)
        return false;
    *out_status = device->primary_head.status;
    return true;
}

static uint32_t test_backend_present(DisplayDevice *device, const DisplayPresentRequest &request)
{
    (void)request;
    return device ? device->primary_head.status.completed_sequence : 0u;
}

static uint32_t test_backend_present_buffer(DisplayDevice *device, const DisplayBuffer &buffer,
                                            const Rect *damage_rects, uint32_t damage_count, uint32_t frame_sequence,
                                            uint32_t flags)
{
    (void)buffer;
    (void)damage_rects;
    (void)damage_count;
    (void)frame_sequence;
    (void)flags;
    return device ? device->primary_head.status.completed_sequence : 0u;
}

static uint32_t test_backend_wait(DisplayDevice *device)
{
    return device ? device->primary_head.status.completed_sequence : 0u;
}

static bool test_backend_set_mode(DisplayDevice *device, const DisplayMode &mode)
{
    (void)device;
    (void)mode;
    return false;
}

static bool test_backend_atomic_commit(DisplayDevice *device, const DisplayAtomicState &state, const DisplayMode &mode)
{
    (void)device;
    g_test_atomic_state = state;
    g_test_atomic_mode = mode;
    g_test_atomic_commit_called = true;
    return g_test_atomic_commit_should_succeed;
}

static const DisplayBackendOps g_test_atomic_backend_ops = {
    "test-atomic",
    test_backend_get_caps,
    test_backend_get_status,
    test_backend_present,
    test_backend_present_buffer,
    test_backend_wait,
    test_backend_set_mode,
    test_backend_atomic_commit,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

KTEST(display_edid_prefers_highest_exact_refresh_without_active_hint)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_detailed_timing(&edid[72], 1920, 1080, 144, false);
    write_detailed_timing(&edid[90], 1920, 1080, 180, false);
    finalize_edid_block(edid);

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 1920, 1080);
    KTEST_EXPECT_EQ(detected, 180u);
}

KTEST(display_edid_exact_hint_selects_closest_active_refresh)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_detailed_timing(&edid[72], 1920, 1080, 144, false);
    write_detailed_timing(&edid[90], 1920, 1080, 180, false);
    finalize_edid_block(edid);

    uint32_t detected_millihz = 0;
    KTEST_EXPECT(display_detect_exact_refresh_millihz_from_edid_hint(edid, sizeof(edid), 1920, 1080, 178000u,
                                                                     &detected_millihz));
    KTEST_EXPECT_EQ(detected_millihz, 180000u);
}

KTEST(display_edid_uses_cta_vic_when_exact_mode_is_only_in_extension)
{
    uint8_t edid[256];
    init_edid(edid, 2);
    write_detailed_timing(&edid[54], 1280, 720, 60, false);
    write_cta_video_block(&edid[128], 63);
    finalize_edid_block(&edid[0]);
    finalize_edid_block(&edid[128]);

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 1920, 1080);
    KTEST_EXPECT_EQ(detected, 120u);
}

KTEST(display_edid_invalid_checksum_falls_back_to_60hz)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 1920, 1080, 180, false);
    finalize_edid_block(edid);
    edid[127] ^= 0x01u;

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 1920, 1080);
    KTEST_EXPECT_EQ(detected, 60u);
}

KTEST(display_edid_uses_range_limits_when_exact_mode_is_missing)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_range_limits(&edid[72], 48, 180);
    finalize_edid_block(edid);

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 2560, 1440);
    KTEST_EXPECT_EQ(detected, 180u);
}

KTEST(display_edid_keeps_exact_base_timing_when_range_limits_only_describe_capability)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_range_limits(&edid[72], 48, 180);
    finalize_edid_block(edid);

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 1920, 1080);
    KTEST_EXPECT_EQ(detected, 60u);
}

KTEST(display_edid_keeps_exact_base_timing_even_with_extensions_present)
{
    uint8_t edid[256];
    init_edid(edid, 2);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_range_limits(&edid[72], 48, 180);
    write_cta_video_block(&edid[128], 4);
    finalize_edid_block(&edid[0]);
    finalize_edid_block(&edid[128]);

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 1920, 1080);
    KTEST_EXPECT_EQ(detected, 60u);
}

KTEST(display_edid_keeps_exact_native_timing_without_overriding_from_range_limits)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 2560, 1440, 60, false);
    write_range_limits(&edid[72], 48, 180);
    finalize_edid_block(edid);

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 2560, 1440);
    KTEST_EXPECT_EQ(detected, 60u);
}

KTEST(display_edid_uses_displayid_detailed_extension_for_exact_160hz)
{
    uint8_t edid[256];
    init_edid(edid, 2);
    write_detailed_timing(&edid[54], 2560, 1440, 60, false);
    write_displayid_detailed_block(&edid[128], 2560, 1440, 160, true);
    finalize_edid_block(&edid[0]);
    finalize_edid_block(&edid[128]);

    uint32_t detected_millihz = 0;
    KTEST_EXPECT(display_detect_exact_refresh_millihz_from_edid_hint(edid, sizeof(edid), 2560, 1440, 158000u,
                                                                     &detected_millihz));
    KTEST_EXPECT_EQ(detected_millihz, 160000u);
}
KTEST(display_edid_uses_displayid_formula_extension_for_exact_180hz)
{
    uint8_t edid[256];
    init_edid(edid, 2);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_displayid_formula_block(&edid[128], 1920, 1080, 180);
    finalize_edid_block(&edid[0]);
    finalize_edid_block(&edid[128]);

    uint32_t detected_millihz = 0;
    KTEST_EXPECT(display_detect_exact_refresh_millihz_from_edid_hint(edid, sizeof(edid), 1920, 1080, 178000u,
                                                                     &detected_millihz));
    KTEST_EXPECT_EQ(detected_millihz, 180000u);
}

KTEST(display_edid_prefers_displayid_detailed_over_base_detailed_without_hint)
{
    uint8_t edid[256];
    init_edid(edid, 2);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_displayid_detailed_block(&edid[128], 1920, 1080, 180, true);
    finalize_edid_block(&edid[0]);
    finalize_edid_block(&edid[128]);

    uint32_t detected = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 1920, 1080);
    KTEST_EXPECT_EQ(detected, 180u);
}

KTEST(display_edid_mode_list_marks_preferred_and_current_exact_modes)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 1920, 1080, 60, false);
    write_detailed_timing(&edid[72], 1920, 1080, 144, false);
    write_detailed_timing(&edid[90], 1920, 1080, 180, false);
    finalize_edid_block(edid);

    DisplayMode current = {};
    current.width = 1920u;
    current.height = 1080u;
    current.refresh_hz = 180u;
    current.nominal_refresh_millihz = 180000u;

    DisplayMode modes[8] = {};
    int count = display_detect_modes_from_edid(edid, sizeof(edid), modes, 8u, &current, 179982u);
    KTEST_EXPECT_EQ(count, 3);
    KTEST_EXPECT((modes[0].flags & DISPLAY_MODE_FLAG_PREFERRED) == 0);
    KTEST_EXPECT_EQ(modes[0].refresh_hz, 60u);
    KTEST_EXPECT_EQ(modes[1].refresh_hz, 144u);
    KTEST_EXPECT((modes[2].flags & DISPLAY_MODE_FLAG_PREFERRED) != 0);
    KTEST_EXPECT((modes[2].flags & DISPLAY_MODE_FLAG_CURRENT) != 0);
    KTEST_EXPECT((modes[2].flags & DISPLAY_MODE_FLAG_EXACT_TIMING) != 0);
    KTEST_EXPECT_EQ(modes[2].refresh_hz, 180u);
    KTEST_EXPECT_EQ(modes[2].measured_refresh_millihz, 179982u);
}

KTEST(display_edid_mode_list_prefers_highest_resolution_then_highest_refresh)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing(&edid[54], 1920, 1080, 180, false);
    write_detailed_timing(&edid[72], 2560, 1440, 60, false);
    write_detailed_timing(&edid[90], 2560, 1440, 144, false);
    finalize_edid_block(edid);

    DisplayMode modes[8] = {};
    int count = display_detect_modes_from_edid(edid, sizeof(edid), modes, 8u, nullptr, 0u);
    KTEST_EXPECT_EQ(count, 3);
    KTEST_EXPECT((modes[0].flags & DISPLAY_MODE_FLAG_PREFERRED) == 0);
    KTEST_EXPECT((modes[1].flags & DISPLAY_MODE_FLAG_PREFERRED) == 0);
    KTEST_EXPECT((modes[2].flags & DISPLAY_MODE_FLAG_PREFERRED) != 0);
    KTEST_EXPECT_EQ(modes[2].width, 2560u);
    KTEST_EXPECT_EQ(modes[2].height, 1440u);
    KTEST_EXPECT_EQ(modes[2].refresh_hz, 144u);
}

KTEST(display_edid_mode_list_preserves_exact_timing_metadata)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing_raw(&edid[54], 640, 480, 160, 45, 2518, false);
    finalize_edid_block(edid);

    DisplayMode current = {};
    current.width = 640u;
    current.height = 480u;
    current.refresh_hz = 60u;
    current.nominal_refresh_millihz = 59952u;

    DisplayMode modes[4] = {};
    int count = display_detect_modes_from_edid(edid, sizeof(edid), modes, 4u, &current, 59952u);
    KTEST_EXPECT_EQ(count, 1);
    KTEST_EXPECT_EQ(modes[0].pixel_clock_khz, 25180u);
    KTEST_EXPECT_EQ(modes[0].htotal, 800u);
    KTEST_EXPECT_EQ(modes[0].vtotal, 525u);
    KTEST_EXPECT_EQ(modes[0].hblank_start, 640u);
    KTEST_EXPECT_EQ(modes[0].hblank_end, 800u);
    KTEST_EXPECT_EQ(modes[0].hsync_start, 656u);
    KTEST_EXPECT_EQ(modes[0].hsync_end, 688u);
    KTEST_EXPECT_EQ(modes[0].vblank_start, 480u);
    KTEST_EXPECT_EQ(modes[0].vblank_end, 525u);
    KTEST_EXPECT_EQ(modes[0].vsync_start, 483u);
    KTEST_EXPECT_EQ(modes[0].vsync_end, 487u);
    KTEST_EXPECT((modes[0].flags & DISPLAY_MODE_FLAG_EXACT_TIMING) != 0);
    KTEST_EXPECT_EQ(modes[0].nominal_refresh_millihz, 59952u);
}

KTEST(display_edid_preserves_sub_hz_precision_internally)
{
    uint8_t edid[128];
    init_edid(edid, 1);
    write_detailed_timing_raw(&edid[54], 640, 480, 160, 45, 2518, false);
    finalize_edid_block(edid);

    uint32_t detected_millihz = display_detect_refresh_millihz_from_edid(edid, sizeof(edid), 640, 480);
    uint32_t detected_hz = display_detect_refresh_hz_from_edid(edid, sizeof(edid), 640, 480);
    KTEST_EXPECT_EQ(detected_millihz, 59952u);
    KTEST_EXPECT_EQ(detected_hz, 60u);
}

KTEST(display_caps_can_store_sub_hz_refresh_precision)
{
    DisplayCaps caps = {};
    caps.refresh_hz = 60u;
    caps.refresh_millihz = 59952u;

    KTEST_EXPECT_EQ(caps.refresh_hz, 60u);
    KTEST_EXPECT_EQ(caps.refresh_millihz, 59952u);
}

KTEST(display_caps_can_track_nominal_and_measured_refresh_independently)
{
    DisplayCaps caps = {};
    caps.refresh_hz = 180u;
    caps.refresh_millihz = 180000u;
    caps.nominal_refresh_millihz = 180000u;
    caps.measured_refresh_millihz = 179982u;

    KTEST_EXPECT_EQ(caps.nominal_refresh_millihz, 180000u);
    KTEST_EXPECT_EQ(caps.measured_refresh_millihz, 179982u);
    KTEST_EXPECT_EQ(caps.refresh_millihz, 180000u);
}

KTEST(display_complete_present_emits_flip_complete_event)
{
    drain_display_events_for_test();

    DisplayHead *head = display_primary_head();
    head->status = {};
    head->submitted_sequence = 7u;
    display_complete_present(head, 7u, DISPLAY_PRESENT_RESULT_OK, 4096u);

    DisplayEvent event = {};
    KTEST_EXPECT(display_event_wait(&event, false));
    KTEST_EXPECT_EQ(event.type, DISPLAY_EVENT_FLIP_COMPLETE);
    KTEST_EXPECT_EQ(event.sequence, 7u);
    KTEST_EXPECT_EQ(head->status.completed_sequence, 7u);
}

KTEST(display_submit_and_complete_sequences_are_tracked_independently)
{
    DisplayHead head = {};
    head.caps.width = 1920u;
    head.caps.height = 1080u;

    Rect damage = gui_rect_make(0, 0, 1920, 1080);
    display_note_present_submitted(&head, 12u, 2u, 1u, &damage, 1u);

    KTEST_EXPECT_EQ(head.submitted_sequence, 12u);
    KTEST_EXPECT_EQ(head.status.queued_sequence, 12u);
    KTEST_EXPECT_EQ(head.status.completed_sequence, 0u);

    display_complete_present(&head, 12u, DISPLAY_PRESENT_RESULT_OK, 1920u * 1080u);
    KTEST_EXPECT_EQ(head.submitted_sequence, 12u);
    KTEST_EXPECT_EQ(head.status.completed_sequence, 12u);
}

KTEST(display_set_mode_applies_atomic_connector_state)
{
    DisplayDevice *device = display_device_instance();
    DisplayDevice saved = *device;

    device->ops = &g_test_atomic_backend_ops;
    device->connector_count = 1u;
    device->crtc_count = 1u;
    device->plane_count = 1u;
    device->primary_head.caps = {};
    device->primary_head.caps.width = 1920u;
    device->primary_head.caps.height = 1080u;
    device->primary_head.caps.bpp = 32u;
    device->primary_head.caps.pitch = 1920u * 4u;
    device->primary_head.current_mode = {};
    device->primary_head.current_mode.mode_id = 1u;
    device->primary_head.current_mode.width = 1920u;
    device->primary_head.current_mode.height = 1080u;
    device->primary_head.current_mode.refresh_hz = 60u;
    device->primary_head.current_mode.nominal_refresh_millihz = 60000u;

    device->connectors[0] = {};
    device->connectors[0].info.connector_id = 1u;
    device->connectors[0].info.status = DISPLAY_CONNECTOR_STATUS_CONNECTED;
    device->connectors[0].info.active_mode_id = 1u;
    device->connectors[0].mode_count = 2u;
    device->connectors[0].modes[0] = device->primary_head.current_mode;
    device->connectors[0].modes[0].flags = DISPLAY_MODE_FLAG_CURRENT | DISPLAY_MODE_FLAG_PREFERRED;
    device->connectors[0].modes[1] = {};
    device->connectors[0].modes[1].mode_id = 2u;
    device->connectors[0].modes[1].width = 1920u;
    device->connectors[0].modes[1].height = 1080u;
    device->connectors[0].modes[1].refresh_hz = 180u;
    device->connectors[0].modes[1].nominal_refresh_millihz = 180000u;

    device->crtcs[0] = {};
    device->crtcs[0].crtc_id = 1u;
    device->crtcs[0].current_mode = device->connectors[0].modes[0];
    device->planes[0] = {};
    device->planes[0].info.plane_id = 1u;

    g_test_atomic_state = {};
    g_test_atomic_mode = {};
    g_test_atomic_commit_called = false;
    g_test_atomic_commit_should_succeed = true;

    DisplayMode request = device->connectors[0].modes[1];
    KTEST_EXPECT(display_set_mode(request));
    KTEST_EXPECT(g_test_atomic_commit_called);
    KTEST_EXPECT_EQ(g_test_atomic_state.connector_id, 1u);
    KTEST_EXPECT_EQ(g_test_atomic_state.mode_id, 2u);
    KTEST_EXPECT(g_test_atomic_state.modeset);
    KTEST_EXPECT_EQ(g_test_atomic_mode.nominal_refresh_millihz, 180000u);
    KTEST_EXPECT_EQ(device->connectors[0].info.active_mode_id, 2u);
    KTEST_EXPECT_EQ(device->primary_head.current_mode.mode_id, 2u);
    KTEST_EXPECT_EQ(device->primary_head.preferred_mode.mode_id, 1u);
    KTEST_EXPECT_EQ(device->crtcs[0].current_mode.mode_id, 2u);
    KTEST_EXPECT((device->connectors[0].modes[1].flags & DISPLAY_MODE_FLAG_CURRENT) != 0);
    KTEST_EXPECT((device->connectors[0].modes[0].flags & DISPLAY_MODE_FLAG_CURRENT) == 0);

    *device = saved;
}

KTEST(display_atomic_commit_applies_requested_connector_mode)
{
    DisplayDevice *device = display_device_instance();
    DisplayDevice saved = *device;

    device->ops = &g_test_atomic_backend_ops;
    device->connector_count = 1u;
    device->crtc_count = 1u;
    device->plane_count = 1u;
    device->primary_head.caps = {};
    device->primary_head.caps.width = 2560u;
    device->primary_head.caps.height = 1440u;
    device->primary_head.caps.bpp = 32u;
    device->primary_head.caps.pitch = 2560u * 4u;

    device->connectors[0] = {};
    device->connectors[0].info.connector_id = 7u;
    device->connectors[0].info.status = DISPLAY_CONNECTOR_STATUS_CONNECTED;
    device->connectors[0].mode_count = 2u;
    device->connectors[0].modes[0] = {};
    device->connectors[0].modes[0].mode_id = 1u;
    device->connectors[0].modes[0].width = 2560u;
    device->connectors[0].modes[0].height = 1440u;
    device->connectors[0].modes[0].refresh_hz = 60u;
    device->connectors[0].modes[0].nominal_refresh_millihz = 60000u;
    device->connectors[0].modes[0].flags = DISPLAY_MODE_FLAG_CURRENT | DISPLAY_MODE_FLAG_PREFERRED;
    device->connectors[0].modes[1] = {};
    device->connectors[0].modes[1].mode_id = 9u;
    device->connectors[0].modes[1].width = 2560u;
    device->connectors[0].modes[1].height = 1440u;
    device->connectors[0].modes[1].refresh_hz = 180u;
    device->connectors[0].modes[1].nominal_refresh_millihz = 180000u;

    g_test_atomic_state = {};
    g_test_atomic_mode = {};
    g_test_atomic_commit_called = false;
    g_test_atomic_commit_should_succeed = true;

    DisplayAtomicRequest request = {};
    request.connector_id = 7u;
    request.mode_id = 9u;
    request.flags = DISPLAY_ATOMIC_MODESET;
    request.damage_bounds = gui_rect_make(0, 0, 2560, 1440);

    KTEST_EXPECT(display_atomic_commit(request));
    KTEST_EXPECT(g_test_atomic_commit_called);
    KTEST_EXPECT_EQ(g_test_atomic_state.connector_id, 7u);
    KTEST_EXPECT_EQ(g_test_atomic_state.mode_id, 9u);
    KTEST_EXPECT(g_test_atomic_state.modeset);
    KTEST_EXPECT_EQ(device->connectors[0].info.active_mode_id, 9u);
    KTEST_EXPECT_EQ(device->primary_head.current_mode.mode_id, 9u);
    KTEST_EXPECT_EQ(device->primary_head.preferred_mode.mode_id, 1u);
    KTEST_EXPECT_EQ(device->primary_head.caps.nominal_refresh_millihz, 180000u);

    *device = saved;
}

KTEST(display_complete_present_promotes_pending_buffer_to_front)
{
    drain_display_events_for_test();

    DisplayHead *head = display_primary_head();
    head->status = {};
    head->buffer_count = 2u;
    head->front_buffer_index = 0u;
    head->pending_buffer_index = 1u;
    head->pending_flip_sequence = 9u;
    head->pending_flip = true;
    head->buffer_in_flight_sequence[1] = 9u;

    display_complete_present(head, 9u, DISPLAY_PRESENT_RESULT_OK, 8192u);

    KTEST_EXPECT_EQ(head->front_buffer_index, 1u);
    KTEST_EXPECT(!head->pending_flip);
    KTEST_EXPECT_EQ(head->pending_flip_sequence, 0u);
    KTEST_EXPECT_EQ(head->buffer_in_flight_sequence[1], 0u);
}

KTEST(display_fallback_wait_emits_vblank_event)
{
    drain_display_events_for_test();

    DisplayHead *head = display_primary_head();
    head->caps.refresh_hz = 60u;
    head->caps.flags = 0u;
    head->status.completed_sequence = 11u;
    head->status.vblank_count = 0u;
    head->next_deadline_tick = 0u;
    head->deadline_remainder = 0u;

    display_fallback_wait_until_next_frame(head);

    DisplayEvent event = {};
    KTEST_EXPECT(display_event_wait(&event, false));
    KTEST_EXPECT_EQ(event.type, DISPLAY_EVENT_VBLANK);
    KTEST_EXPECT_EQ(event.sequence, 11u);
}

KTEST(display_clip_rect_handles_negative_origin_without_overflow)
{
    DisplayCaps caps = {};
    caps.width = 800;
    caps.height = 600;

    Rect rect = {-20, -15, 100, 60};
    KTEST_EXPECT(clip_rect_for_test(rect, caps));
    KTEST_EXPECT_EQ(rect.x, 0);
    KTEST_EXPECT_EQ(rect.y, 0);
    KTEST_EXPECT_EQ(rect.w, 80);
    KTEST_EXPECT_EQ(rect.h, 45);
}

KTEST(display_clip_rect_rejects_overflowed_offscreen_rect)
{
    DisplayCaps caps = {};
    caps.width = 800;
    caps.height = 600;

    Rect rect = {2147483600, 10, 200, 20};
    KTEST_EXPECT(!clip_rect_for_test(rect, caps));
}

KTEST(display_clip_rect_clamps_large_visible_rect)
{
    DisplayCaps caps = {};
    caps.width = 800;
    caps.height = 600;

    Rect rect = {790, 590, 100, 100};
    KTEST_EXPECT(clip_rect_for_test(rect, caps));
    KTEST_EXPECT_EQ(rect.x, 790);
    KTEST_EXPECT_EQ(rect.y, 590);
    KTEST_EXPECT_EQ(rect.w, 10);
    KTEST_EXPECT_EQ(rect.h, 10);
}
