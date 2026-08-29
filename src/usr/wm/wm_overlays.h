#pragma once

#include "wm_render.h"

extern IndexState g_index;
extern ControlCenterState g_control_center;
extern ContextMenuState g_context_menu;
extern StoragePromptState g_storage_prompt;
extern NotificationCenterState g_notifications;

// Actions shared by the overlays (launch, show-desktop, settings publish).
void launch_or_focus_app(Registry *registry, const char *title, const char *path);
void show_desktop_windows();
void publish_settings_changed(Registry *registry);

// Index launcher.
DirtyRect index_overlay_bounds();
void open_index();
void close_index();
void update_index_search();
bool activate_index_selection(Registry *registry);
bool handle_index_pointer_down(Registry *registry, int mouse_x, int mouse_y);
void update_index_hover(int mouse_x, int mouse_y);
void draw_index_overlay_clipped(const DirtyRect &clip, const Registry *registry);

// Control center.
DirtyRect control_center_bounds();
void toggle_control_center();
void close_control_center();
bool handle_control_center_pointer_down(Registry *registry, int mouse_x, int mouse_y);
void handle_control_center_pointer_up();
void update_control_center_hover(int mouse_x, int mouse_y);
bool update_control_center_drag(int mouse_x, int mouse_y);
bool handle_control_center_scroll(Registry *registry, int mouse_x, int mouse_y, int scroll_y);
void sync_control_center_state_from_registry(const Registry *registry);
void draw_control_center_overlay_clipped(const DirtyRect &clip);
int control_panel_card_h();
DirtyRect control_panel_item_rect(ControlPanelItem item);

// Context menu.
void open_context_menu(const Registry *registry, ContextMenuKind kind, int target_index, int anchor_x, int anchor_y);
void close_context_menu();
void update_context_menu_hover(const Registry *registry, int mouse_x, int mouse_y);
bool activate_context_menu_item(Registry *registry, int index);
DirtyRect context_menu_bounds();
int build_context_menu_items(const Registry *registry, GuiMenuItem *items, int max_items);
void draw_context_menu_overlay(const Registry *registry);
void draw_context_menu_overlay_clipped(const DirtyRect &clip, const Registry *registry);

// Storage mode prompt.
StoragePromptLayout storage_prompt_layout();
void sync_storage_prompt_state(bool force_visible);
void ensure_default_user_storage_layout();
void open_storage_prompt();
void dismiss_storage_prompt();
void update_storage_prompt_hover(int mouse_x, int mouse_y);
bool apply_storage_mode_request(Registry *registry, int mode);
bool activate_storage_prompt_button(Registry *registry, int mouse_x, int mouse_y);
void draw_storage_prompt_overlay_clipped(const DirtyRect &clip);
void draw_storage_prompt_overlay();

// Notifications and toasts.
void wm_push_notification(const char *title, const char *message);
void draw_toast_overlay_clipped(const DirtyRect &clip);
void draw_notification_center_clipped(const DirtyRect &clip, int start_y);
