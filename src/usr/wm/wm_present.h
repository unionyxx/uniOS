#pragma once

#include "wm_types.h"

extern Surface g_screen;
extern Surface g_backbuffer;
extern Surface g_presentbuffer;
extern Surface g_wallpaper;
extern Surface g_menubar_blur;
extern Surface g_dock_blur;
extern Surface g_menubar_blur_source;
extern Surface g_dock_blur_source;
extern DisplayCaps g_display_caps;
extern bool g_display_copy_path;

extern DisplayBufferHandle g_presentbuffer_handle;
extern PresentBufferSlot g_presentbuffer_slots[MAX_PRESENT_BUFFER_SLOTS];
extern uint32_t g_presentbuffer_slot_count;
extern uint32_t g_presentbuffer_active_slot;
extern DisplayQueueState g_display_queue;
extern WmFrameStats g_frame_stats;
extern WmBenchState g_bench;

void mark_presentbuffer_slots_stale(const DirtyRect &dirty);
void wm_stats_note_dirty_set(const DirtyRect *rects, int rect_count);
void wm_stats_note_stale_repair(int rect_count);
void draw_stats_overlay_clipped(const DirtyRect &clip);
void wm_stats_overlay_bounds(DirtyRect *out_box, DirtyRect *out_damage);
void wm_bench_tick(Registry *registry);
uint64_t wm_tsc_now(void);
uint64_t wm_tsc_to_us(uint64_t cycles);
