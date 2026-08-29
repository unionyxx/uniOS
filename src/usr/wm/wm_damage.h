#pragma once

#include "wm_types.h"

extern DirtyRect g_dirty_rects[MAX_DIRTY_RECTS];
extern int g_dirty_count;
extern bool g_window_visibility_cache_dirty;
extern bool g_dirty_frame_ready;

void enqueue_damage_rect(int x, int y, int w, int h);
void collapse_dirty_rects_to_bounds();
void invalidate_dirty_frame();
void normalize_dirty_rects(bool interactive);
bool clip_dirty_rect_to_screen(DirtyRect &rect);
