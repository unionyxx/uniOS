#pragma once

#include "wm_window.h"

// Pixel and surface primitives.
uint32_t mix_rgb(uint32_t a, uint32_t b, uint8_t t);
uint32_t mix_rgb_keep_alpha(uint32_t base, uint32_t tint, uint8_t t);
int color_luma(uint32_t color);
uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t coverage);
void copy_surface_rect(Surface *dst, int dst_x, int dst_y, const Surface *src, int src_x, int src_y, int w, int h);
bool ensure_surface_capacity(Surface *surface, uint32_t width, uint32_t height);

// Blur.
void blur_surface_box(const Surface *src, Surface *dst, int radius);
void blur_surface_material(const Surface *src, Surface *dst, float sigma, int saturation_pct, int brightness_bias);

// Window decoration and client drawing.
void invalidate_window_decoration_cache(Window &w);
void draw_window_decoration_clipped(Surface *dst, Window &w, const DirtyRect &clip, bool focused, bool hovered_frame,
                                    int hovered_button);
void draw_window_client_clipped(Surface *dst, const Window &w, const DirtyRect &clip);

#ifdef __cplusplus
extern "C" {
#endif
uint8_t gui_rounded_rect_coverage_local(int32_t col, int32_t row, int32_t w, int32_t h, int32_t r,
                                        uint32_t rounded_edges);
#ifdef __cplusplus
}
#endif

// Wallpaper and desktop base.
void init_wallpaper();
void reload_wallpaper(Registry *registry, bool prefer_requested);
void paint_desktop_base(Surface *surface);

// Shell (menubar/dock) blur.
bool init_shell_blur_buffers(Registry *registry, uint32_t dock_w, uint32_t dock_h);
void capture_shell_backdrop_for_rect(const DirtyRect &rect, Registry *registry);
void recapture_shell_blur_sources(Registry *registry);
void flush_shell_blur_updates(Registry *registry);

// Composition helpers.
bool move_backbuffer_rect(const DirtyRect &old_rect, const DirtyRect &new_rect);
bool compose_rect_clipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                          const Registry *registry);
void compose_rect_unclipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                            const Registry *registry);
