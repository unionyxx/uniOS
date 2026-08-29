#include "wm_settings.h"

#include "wm_metrics.h"
#include "wm_overlays.h"
#include "wm_present.h"

uint32_t g_system_flags = SYSTEM_FLAG_SHOW_DESKTOP_GRID;

static bool g_persist_settings_pending = false;

static bool cfg_value_enabled(const char *value, bool fallback)
{
    if (!value || !*value)
        return fallback;
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0 || strcmp(value, "no") == 0)
        return false;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0 || strcmp(value, "yes") == 0)
        return true;
    return fallback;
}

static const char *flag_text(uint32_t flags, uint32_t flag)
{
    return (flags & flag) ? "1" : "0";
}

static uint32_t cfg_uint_clamped(const char *value, uint32_t fallback, uint32_t min, uint32_t max)
{
    if (!value || !*value)
        return fallback;
    int parsed = atoi(value);
    if (parsed < (int)min)
        return min;
    if (parsed > (int)max)
        return max;
    return (uint32_t)parsed;
}

RuntimeGuiSettings load_runtime_settings()
{
    RuntimeGuiSettings s = {GUI_THEME_DARK, SYSTEM_FLAG_SHOW_DESKTOP_GRID, true, true, true, 180, 75, 256, 500, 33};
    char cfg[512], val[64];
    const char *cands[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(cands, 2, cfg, sizeof(cfg))) {
        if (cfg_line_value(cfg, "theme", val, sizeof(val))) {
            if (strcmp(val, "light") == 0)
                s.theme_mode = GUI_THEME_LIGHT;
            else if (strcmp(val, "dark") == 0)
                s.theme_mode = GUI_THEME_DARK;
        }
        if (cfg_line_value(cfg, "show_desktop_grid", val, sizeof(val))) {
            if (cfg_value_enabled(val, (s.system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0))
                s.system_flags |= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
            else
                s.system_flags &= ~SYSTEM_FLAG_SHOW_DESKTOP_GRID;
        }
        if (cfg_line_value(cfg, "clock_show_seconds", val, sizeof(val))) {
            if (cfg_value_enabled(val, (s.system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0))
                s.system_flags |= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
            else
                s.system_flags &= ~SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
        }
        if (cfg_line_value(cfg, "launch_terminal_on_boot", val, sizeof(val))) {
            if (cfg_value_enabled(val, (s.system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) != 0))
                s.system_flags |= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
            else
                s.system_flags &= ~SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
        }
        if (cfg_line_value(cfg, "ethernet_enabled", val, sizeof(val)))
            s.ethernet_enabled = cfg_value_enabled(val, s.ethernet_enabled);
        if (cfg_line_value(cfg, "ethernet_use_dhcp", val, sizeof(val)))
            s.ethernet_use_dhcp = cfg_value_enabled(val, s.ethernet_use_dhcp);
        if (cfg_line_value(cfg, "animations_enabled", val, sizeof(val)))
            s.animations_enabled = cfg_value_enabled(val, s.animations_enabled);
        if (cfg_line_value(cfg, "transparency_level", val, sizeof(val)))
            s.transparency_level = cfg_uint_clamped(val, s.transparency_level, 0, 255);
        if (cfg_line_value(cfg, "volume_level", val, sizeof(val)))
            s.volume_level = cfg_uint_clamped(val, s.volume_level, 0, 100);
        if (cfg_line_value(cfg, "input_pointer_speed", val, sizeof(val)))
            s.input_pointer_speed = cfg_uint_clamped(val, s.input_pointer_speed, 16, 1024);
        if (cfg_line_value(cfg, "input_repeat_delay", val, sizeof(val)))
            s.input_repeat_delay = cfg_uint_clamped(val, s.input_repeat_delay, 50, 2000);
        if (cfg_line_value(cfg, "input_repeat_rate", val, sizeof(val)))
            s.input_repeat_rate = cfg_uint_clamped(val, s.input_repeat_rate, 10, 500);
    }
    return s;
}

// Restore the persisted input-device scalars to the kernel. Called once at WM
// boot so pointer speed and key-repeat survive reboots; the Settings app
// re-applies live when the user drags the sliders.
void wm_apply_input_settings(const RuntimeGuiSettings &s)
{
    input_set_pointer_speed(s.input_pointer_speed);
    input_set_repeat_rate(s.input_repeat_delay, s.input_repeat_rate);
}

bool persist_runtime_settings(const Registry *registry)
{
    if (!registry)
        return false;

    uint32_t flags = registry->system_flags;
    GuiThemeMode mode = registry->theme_mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK;

    // Input-device scalars live in the kernel (no Registry mirror), so preserve
    // whatever the Settings app last persisted instead of zeroing them when the
    // WM rewrites SYSTEM.CFG.
    uint32_t pointer_speed = 256, repeat_delay = 500, repeat_rate = 33;
    char existing[512], val[64];
    const char *cands[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(cands, 2, existing, sizeof(existing))) {
        if (cfg_line_value(existing, "input_pointer_speed", val, sizeof(val)))
            pointer_speed = cfg_uint_clamped(val, pointer_speed, 16, 1024);
        if (cfg_line_value(existing, "input_repeat_delay", val, sizeof(val)))
            repeat_delay = cfg_uint_clamped(val, repeat_delay, 50, 2000);
        if (cfg_line_value(existing, "input_repeat_rate", val, sizeof(val)))
            repeat_rate = cfg_uint_clamped(val, repeat_rate, 10, 500);
    }

    char contents[384];
    int n = snprintf(contents, sizeof(contents),
                     "theme=%s\n"
                     "show_desktop_grid=%s\n"
                     "clock_show_seconds=%s\n"
                     "launch_terminal_on_boot=%s\n"
                     "ethernet_enabled=%d\n"
                     "ethernet_use_dhcp=%d\n"
                     "animations_enabled=%d\n"
                     "transparency_level=%u\n"
                     "volume_level=%u\n"
                     "input_pointer_speed=%u\n"
                     "input_repeat_delay=%u\n"
                     "input_repeat_rate=%u\n",
                     mode == GUI_THEME_LIGHT ? "light" : "dark", flag_text(flags, SYSTEM_FLAG_SHOW_DESKTOP_GRID),
                     flag_text(flags, SYSTEM_FLAG_CLOCK_SHOW_SECONDS),
                     flag_text(flags, SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT), registry->ethernet_enabled ? 1 : 0,
                     registry->ethernet_use_dhcp ? 1 : 0, registry->animations_enabled ? 1 : 0,
                     registry->transparency_level, registry->volume_level <= 100 ? registry->volume_level : 100,
                     pointer_speed, repeat_delay, repeat_rate);
    if (n <= 0 || (size_t)n >= sizeof(contents))
        return false;
    return cfg_write_text_file_atomic(SYSTEM_CONFIG_PATH, contents);
}

void persist_wm_settings()
{
    g_persist_settings_pending = true;
}

void flush_pending_settings_persist(const Registry *registry)
{
    if (!g_persist_settings_pending || !registry)
        return;
    g_persist_settings_pending = false;
    persist_runtime_settings(registry);
}

void load_wm_settings()
{
    char config[1024];
    const char *candidates[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(candidates, 2, config, sizeof(config))) {
        char value[64];
        if (cfg_line_value(config, "theme", value, sizeof(value))) {
            g_control_center.dark_mode = (strcmp(value, "light") != 0);
        }
        if (cfg_line_value(config, "show_desktop_grid", value, sizeof(value))) {
            g_control_center.desktop_grid = (value[0] != '0');
        }
        if (cfg_line_value(config, "clock_show_seconds", value, sizeof(value))) {
            g_control_center.clock_seconds = (value[0] != '0');
        }
        if (cfg_line_value(config, "ethernet_enabled", value, sizeof(value))) {
            g_control_center.network_enabled = cfg_value_enabled(value, g_control_center.network_enabled);
        }
        if (cfg_line_value(config, "animations_enabled", value, sizeof(value))) {
            g_control_center.animations_enabled = cfg_value_enabled(value, g_control_center.animations_enabled);
        }
        if (cfg_line_value(config, "transparency_level", value, sizeof(value))) {
            g_control_center.transparency_level = cfg_uint_clamped(value, g_control_center.transparency_level, 0, 255);
        }
        if (cfg_line_value(config, "volume_level", value, sizeof(value))) {
            g_control_center.volume = cfg_uint_clamped(value, g_control_center.volume, 0, 100);
        }
    }
}
