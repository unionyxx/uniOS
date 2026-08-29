#pragma once

#include "wm_types.h"

extern uint32_t g_system_flags;

RuntimeGuiSettings load_runtime_settings();
bool persist_runtime_settings(const Registry *registry);
void persist_wm_settings();
void flush_pending_settings_persist(const Registry *registry);
void load_wm_settings();
void wm_apply_input_settings(const RuntimeGuiSettings &s);
