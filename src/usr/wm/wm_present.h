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

// Swapchain / present pipeline.
void apply_display_event(const DisplayEvent &event);
uint32_t present_frame(const Surface *source, const DirtyRect *rects, int rect_count, uint32_t frame_sequence,
                       DisplayBufferHandle cursor_handle, int cursor_x, int cursor_y);
void sync_presentbuffer_alias_from_active_slot();
bool select_presentbuffer_slot_for_frame();
void wm_drain_display_events();
void refresh_display_queue_from_status();
void mark_other_presentbuffer_slots_stale(const DirtyRect *rects, int rect_count, uint32_t fresh_slot);
void wm_present_init_sequences(uint32_t first_submitted);
uint32_t wm_present_last_sequence();
uint32_t wm_present_next_sequence();
void wm_present_note_submitted(uint32_t sequence);
// Returns true when the loop should continue without releasing an in-flight
// present buffer (swapchain present-wait path).
bool wm_present_end_frame(Registry *registry, bool manip, bool inter, uint32_t limit, uint64_t frame_tsc_start);
