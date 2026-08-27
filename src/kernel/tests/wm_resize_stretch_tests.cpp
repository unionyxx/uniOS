#include <kernel/ktest.h>
#include <wm/resize_stretch.h>

KTEST(wm_stretch_identity_maps_each_pixel_to_itself)
{
    for (int len = 1; len <= 64; len++) {
        for (int c = 0; c < len; c++)
            KTEST_EXPECT_EQ(wm::stretch_map_coord(c, len, len), c);
    }
}

KTEST(wm_stretch_double_upscale_pairs_source_pixels)
{
    KTEST_EXPECT_EQ(wm::stretch_map_coord(0, 2, 4), 0);
    KTEST_EXPECT_EQ(wm::stretch_map_coord(1, 2, 4), 0);
    KTEST_EXPECT_EQ(wm::stretch_map_coord(2, 2, 4), 1);
    KTEST_EXPECT_EQ(wm::stretch_map_coord(3, 2, 4), 1);
}

KTEST(wm_stretch_double_downscale_samples_even_columns)
{
    KTEST_EXPECT_EQ(wm::stretch_map_coord(0, 4, 2), 0);
    KTEST_EXPECT_EQ(wm::stretch_map_coord(1, 4, 2), 2);
}

KTEST(wm_stretch_upscale_endpoints_cover_first_and_last_source)
{
    for (int src = 1; src <= 40; src++) {
        for (int dst = src; dst <= src * 3; dst++) {
            KTEST_EXPECT_EQ(wm::stretch_map_coord(0, src, dst), 0);
            KTEST_EXPECT_EQ(wm::stretch_map_coord(dst - 1, src, dst), src - 1);
        }
    }
}

KTEST(wm_stretch_mapping_is_bounded_and_monotonic)
{
    static const int cases[][2] = {{1, 7}, {3, 17}, {7, 3}, {13, 400}, {400, 13}, {127, 1024}, {1024, 127}};
    for (const auto &c : cases) {
        const int src = c[0], dst = c[1];
        int prev = 0;
        for (int x = 0; x < dst; x++) {
            int m = wm::stretch_map_coord(x, src, dst);
            KTEST_EXPECT(m >= 0 && m < src);
            KTEST_EXPECT(m >= prev);
            prev = m;
        }
    }
}

KTEST(wm_stretch_degenerate_lengths_map_to_zero)
{
    KTEST_EXPECT_EQ(wm::stretch_map_coord(5, 0, 10), 0);
    KTEST_EXPECT_EQ(wm::stretch_map_coord(5, 10, 0), 0);
    KTEST_EXPECT_EQ(wm::stretch_map_coord(-1, 10, 10), 0);
}
