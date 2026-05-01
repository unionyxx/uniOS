#include <drivers/video/framebuffer.h>
#include <kernel/boot_splash.h>

namespace {

static bool s_active = false;
static bool s_full_redraw = true;
static uint32_t s_progress_percent = 0;

static constexpr uint32_t k_background = 0xFF000000;
static constexpr uint32_t k_bar_border = 0xFF4D4D4D;
static constexpr uint32_t k_bar_fill = 0xFFF2F2F2;
static constexpr uint32_t k_bar_track = 0xFF161616;
static constexpr int k_bar_height = 10;
static constexpr int k_bar_inner_padding = 2;

static int clamp_bar_width(int screen_width)
{
    int width = screen_width / 4;
    if (width < 176)
        width = 176;
    if (width > 320)
        width = 320;
    if (width > screen_width - 64)
        width = screen_width - 64;
    return width > 0 ? width : 1;
}

static void clear_bar_region(int bar_x, int bar_y, int bar_w)
{
    constexpr int k_clear_pad = 2;
    gfx_fill_rect(bar_x - k_clear_pad, bar_y - k_clear_pad, bar_w + k_clear_pad * 2, k_bar_height + k_clear_pad * 2,
                  k_background);
}

static void draw_frame(void)
{
    if (!s_active)
        return;

    const int screen_width = static_cast<int>(gfx_get_width());
    const int screen_height = static_cast<int>(gfx_get_height());
    if (screen_width <= 0 || screen_height <= 0)
        return;

    const int bar_w = clamp_bar_width(screen_width);
    const int bar_radius = k_bar_height / 2;
    const int bar_x = (screen_width - bar_w) / 2;
    const int bar_y = (screen_height - k_bar_height) / 2;
    const int inner_w = bar_w - k_bar_inner_padding * 2;
    const int inner_h = k_bar_height - k_bar_inner_padding * 2;
    const int inner_radius = inner_h / 2;
    int fill_w = (inner_w * static_cast<int>(s_progress_percent)) / 100;
    if (s_progress_percent > 0 && fill_w == 0)
        fill_w = 1;
    if (fill_w > inner_w)
        fill_w = inner_w;

    if (s_full_redraw) {
        gfx_clear(k_background);
        s_full_redraw = false;
    } else {
        clear_bar_region(bar_x, bar_y, bar_w);
    }

    gfx_fill_rounded_rect(bar_x, bar_y, bar_w, k_bar_height, bar_radius, k_bar_track);
    if (fill_w > 0) {
        gfx_fill_rounded_rect(bar_x + k_bar_inner_padding, bar_y + k_bar_inner_padding, fill_w, inner_h, inner_radius,
                              k_bar_fill);
    }
    gfx_draw_rounded_rect(bar_x, bar_y, bar_w, k_bar_height, bar_radius, k_bar_border);

    if (gfx_is_double_buffered()) {
        gfx_swap_buffers();
    }
}

} // namespace

void boot_splash_init(void)
{
#ifdef DEBUG
    s_active = false;
    s_progress_percent = 0;
    return;
#else
    s_active = gfx_get_width() > 0 && gfx_get_height() > 0;
    s_full_redraw = true;
    s_progress_percent = 0;
    draw_frame();
#endif
}

void boot_splash_set_progress(uint32_t percent)
{
    if (!s_active)
        return;
    if (percent > 100)
        percent = 100;
    if (percent < s_progress_percent)
        percent = s_progress_percent;
    s_progress_percent = percent;
    draw_frame();
}

bool boot_splash_is_active(void)
{
    return s_active;
}
