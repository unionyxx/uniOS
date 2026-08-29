#pragma once

#include "wm_present.h"

// Top-level compositor pipeline: bootstrap, event dispatch, registry
// synchronization, and the per-frame build/present phases that main() chains
// together.
extern bool g_scene_is_presentbuffer;

Registry *wm_bootstrap();
void wm_handle_events(Registry *registry, Event &ev);
void wm_registry_sync_init(Registry *registry, GuiThemeMode initial_theme);
void wm_sync_registry(Registry *registry);
// Returns false when the frame was skipped (queue full during an interactive
// manipulation); the caller continues the loop.
bool wm_build_frame(Registry *registry, bool manip, bool inter, bool resizing, uint32_t limit);
void wm_scene_mark_presentbuffer();
void wm_scene_mark_cursor_baked();
