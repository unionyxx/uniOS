#pragma once

#include "wm_window.h"

extern WmInputState g_input;

// Hit testing.
int hit_test_resize(const Window &w, int px, int py);
bool point_in_titlebar(const Window &w, int px, int py);
bool point_in_client(const Window &w, int px, int py);
bool point_in_outer(const Window &w, int px, int py);
bool point_in_button(const Window &w, int px, int py, int button_index);
int system_window_hit(int px, int py);
bool pointer_blocked_by_shell_overlay(int px, int py);

// Client event delivery.
void post_mouse_event_to_window(const Window &w, EventType type, int px, int py, uint8_t button, int8_t scroll_y = 0);
void post_key_event_to_window(const Window &w, EventType type, char c, uint8_t scancode);
void post_plain_event_to_window(const Window &w, EventType type);
void post_scroll_event_to_window(const Window &w);

// Pointer tracking.
void apply_mouse_move(Registry *registry, int new_mouse_x, int new_mouse_y);
void update_hover_feedback();
void update_cursor_kind();
void mark_cursor_transition_damage(int old_x, int old_y, GuiCursorKind old_kind, int new_x, int new_y,
                                   GuiCursorKind new_kind);
bool wm_cursor_backend_allowed();

// True when a pointer at (px,py) should be treated as targeting the window's
// client area for input (transparent windows hit-test visible pixels).
bool point_targets_window_client_for_input(const Window &w, int px, int py);

// Hardware/software cursor plane selection for the frame being built.
bool prepare_cursor_overlay_damage(bool interactive, DirtyRect *cursor_rect_out, bool track_damage);
void wm_cursor_begin_frame();
void wm_cursor_erase_previous_software(bool hw_cursor_allowed, bool interactive);
bool wm_cursor_select_plane(bool hw_cursor_allowed, const DirtyRect &cursor_rect, bool interactive);
void wm_cursor_finish_frame(bool draw_cursor, bool software, const DirtyRect &cursor_rect);
void wm_cursor_frame_plane(DisplayBufferHandle *handle, int *x, int *y);
