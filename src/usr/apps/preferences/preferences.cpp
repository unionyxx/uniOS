#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/display.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/gui.h>
#include <uapi/input.h>
#include <uapi/sysinfo.h>

#include "../../libapp/app.h"
#include "../../libapp/widgets.h"
#include "../../libc/config_utils.h"
#include "../../libc/log.h"
#include "../../libc/socket.h"
#include "../../libc/unistd.h"
#include "../../libc/wallpaper_defaults.h"

static constexpr const char *SYSTEM_CONFIG_PATH = "/data/SYSTEM.CFG";
static constexpr const char *SYSTEM_BOOTSTRAP_CONFIG_PATH = "/etc/system.conf";
static constexpr const char *WALLPAPER_CONFIG_PATH = "/data/WALLPAPR.CFG";
static constexpr const char *WALLPAPER_BOOTSTRAP_CONFIG_PATH = "/etc/wallpaper.conf";

// Menubar command IDs (dispatched through WindowEntry.menu_command_id).
enum
{
    PREF_MENU_HELP = 0x80,
};

enum PrefSection
{
    PREF_SECTION_APPEARANCE = 0,
    PREF_SECTION_DESKTOP = 1,
    PREF_SECTION_NETWORK = 2,
    PREF_SECTION_SYSTEM = 3,
    PREF_SECTION_DEVICES = 4,
    PREF_SECTION_ABOUT = 5,
    PREF_SECTION_COUNT = 6,
};

// Pointer-speed slider runs 50..200 (percent of 1.0x); the kernel multiplier is
// Q8 (256 == 1.0x), so value 100 maps to 256. Key-repeat slider runs 0..100
// (higher == faster); rate_ms is derived from it while the delay is persisted.
static constexpr uint32_t POINTER_SPEED_MIN = 50;
static constexpr uint32_t POINTER_SPEED_MAX = 200;
static constexpr uint32_t POINTER_SPEED_DEFAULT = 100;
static constexpr uint32_t KEY_REPEAT_DEFAULT = 95;

static uint32_t pointer_speed_to_mult(uint32_t slider)
{
    return slider * 256u / 100u;
}
static uint32_t pointer_mult_to_slider(uint32_t mult)
{
    return mult * 100u / 256u;
}
static uint32_t key_repeat_slider_to_rate(uint32_t slider)
{
    if (slider > 100)
        slider = 100;
    return 500u - slider * 490u / 100u;
}
static uint32_t key_repeat_rate_to_slider(uint32_t rate_ms)
{
    // Clamp so (500 - rate_ms) cannot underflow for out-of-range input.
    if (rate_ms < 10u)
        rate_ms = 10u;
    if (rate_ms > 500u)
        rate_ms = 500u;
    return (500u - rate_ms) * 100u / 490u;
}

// Snapshot of the system profile shown in the About section (the standalone
// About app was folded into this window).
struct AboutSnapshot
{
    SystemProfile profile;
    MemInfo mem;
    DisplayCaps caps;
    SysTime now;
    uint64_t uptime_seconds;
    int proc_count;
    char vendor[16];
};

struct PreferencesState
{
    GuiThemeMode theme_mode;
    uint32_t system_flags;
    int storage_mode;
    bool animations_enabled;
    uint32_t transparency_level;
    uint32_t volume_level;
    char wallpaper_path[256];
    char status[128];
    int section;
    // Live network snapshot (Network tab, refreshed on section entry).
    NetStatus net;
    bool net_valid;
    // Input device settings (Devices tab).
    uint32_t input_pointer_speed; // Q8 kernel multiplier (256 == 1.0x)
    uint32_t input_repeat_delay;  // ms
    uint32_t input_repeat_rate;   // ms
    InputDeviceInfo devices[INPUT_MAX_DEVICES];
    int device_count;
    uint64_t last_enum_tick;
    // About section snapshot (refreshed at 1 Hz while the section is shown).
    AboutSnapshot about;
    bool about_ready;
    uint64_t about_refresh_tick;
};

struct PreferencesApp
{
    PreferencesState state;
    Rect nav[PREF_SECTION_COUNT];
    Rect wallpaper_rect;
    int nav_hover;
    WidgetSegment theme;
    WidgetSegment storage;
    WidgetField wallpaper;
    WidgetButton apply;
    WidgetButton def;
    WidgetButton renew;
    WidgetToggle animations;
    WidgetToggle transparency;
    WidgetToggle grid;
    WidgetToggle seconds;
    WidgetToggle terminal;
    WidgetSlider volume;
    WidgetSlider pointer_speed;
    WidgetSlider key_repeat;
    WidgetToggle device_toggles[INPUT_MAX_DEVICES];
    Rect device_row_rects[INPUT_MAX_DEVICES];
    WidgetHelp help;
};

static void safe_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src)
        src = "";
    size_t i = 0;
    for (; i + 1 < dst_size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

// --- About section -----------------------------------------------------------
// The standalone About app was folded into this window as a section. The
// snapshot helpers below mirror its formatting so the data reads identically.

static void about_format_size_kb(uint64_t kb, char *out, size_t out_size)
{
    if (kb >= 1048576ull) {
        uint64_t gib10 = (kb * 10ull) >> 20;
        snprintf(out, out_size, "%llu.%llu GiB", (unsigned long long)(gib10 / 10ull),
                 (unsigned long long)(gib10 % 10ull));
        return;
    }
    uint64_t mib10 = (kb * 10ull) >> 10;
    snprintf(out, out_size, "%llu.%llu MiB", (unsigned long long)(mib10 / 10ull), (unsigned long long)(mib10 % 10ull));
}

static void about_format_uptime(uint64_t uptime, char *out, size_t out_size)
{
    uint64_t days = uptime / 86400ull;
    uint64_t hours = (uptime / 3600ull) % 24ull;
    uint64_t mins = (uptime / 60ull) % 60ull;
    uint64_t secs = uptime % 60ull;
    if (days > 0)
        snprintf(out, out_size, "%llud %02lluh %02llum %02llus", (unsigned long long)days, (unsigned long long)hours,
                 (unsigned long long)mins, (unsigned long long)secs);
    else
        snprintf(out, out_size, "%02lluh %02llum %02llus", (unsigned long long)hours, (unsigned long long)mins,
                 (unsigned long long)secs);
}

static void about_format_display_flags(uint32_t flags, char *out, size_t out_size)
{
    out[0] = '\0';
    char *p = out;
    size_t rem = out_size;
    auto append = [&](const char *str) {
        if (p != out && rem > 2) {
            *p++ = ',';
            *p++ = ' ';
            rem -= 2;
        }
        size_t len = strlen(str);
        if (len >= rem)
            len = rem > 0 ? rem - 1 : 0;
        memcpy(p, str, len);
        p += len;
        rem -= len;
        *p = '\0';
    };
    if (flags & DISPLAY_FLAG_HAS_VBLANK)
        append("VBlank");
    if (flags & DISPLAY_FLAG_HAS_PAGE_FLIP)
        append("Page Flip");
    if (flags & DISPLAY_FLAG_HAS_CURSOR_PLANE)
        append("Cursor Plane");
    if (flags & DISPLAY_FLAG_HAS_OVERLAY)
        append("Overlay");
    if (flags & DISPLAY_FLAG_HAS_COMPOSITOR)
        append("Compositor");
    if (flags & DISPLAY_FLAG_USES_COPY_PATH)
        append("Copy Path");
    if (flags & DISPLAY_FLAG_STRICT_SYNC_ONLY)
        append("Strict Sync");
    if (p == out)
        snprintf(out, out_size, "Basic");
}

static void about_format_refresh(uint32_t refresh_millihz, char *out, size_t out_size)
{
    if (refresh_millihz == 0) {
        snprintf(out, out_size, "Unavailable");
        return;
    }
    snprintf(out, out_size, "%u.%03u Hz", refresh_millihz / 1000u, refresh_millihz % 1000u);
}

static void about_cpu_vendor(char *out)
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *reinterpret_cast<uint32_t *>(&out[0]) = ebx;
    *reinterpret_cast<uint32_t *>(&out[4]) = edx;
    *reinterpret_cast<uint32_t *>(&out[8]) = ecx;
    out[12] = '\0';
}

static void about_collect_snapshot(AboutSnapshot *snapshot)
{
    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof(*snapshot));

    static char s_vendor[16] = {0};
    static SystemProfile s_profile = {};
    static bool s_static_loaded = false;
    if (!s_static_loaded) {
        about_cpu_vendor(s_vendor);
        get_sysinfo(&s_profile);
        s_static_loaded = true;
    }

    memcpy(snapshot->vendor, s_vendor, sizeof(snapshot->vendor));
    snapshot->profile = s_profile;
    get_meminfo(&snapshot->mem);
    display_get_caps(&snapshot->caps);
    get_time(&snapshot->now);

    ProcessInfo procs[64];
    snapshot->proc_count = get_procs(procs, 64);
    if (snapshot->proc_count < 0)
        snapshot->proc_count = 0;
    snapshot->uptime_seconds = get_uptime();
}

// --- Section metrics ---------------------------------------------------------
// The detail pane is laid out as libadwaita-style boxed groups: a continuous
// rounded card per group, rows separated by inset hairlines. These helpers
// compute row/group heights so content sizing and drawing stay in agreement.

static inline int pref_boxed_row_h(void)
{
    return gui_app_row_tall_h();
}
static inline int pref_boxed_slider_h(void)
{
    return gui_app_slider_h();
}
// Compact label/value row used by the About section.
static inline int pref_kv_row_h(void)
{
    int h = gui_line_height() + gui_space_1();
    return h < gui_scaled_metric(26) ? gui_scaled_metric(26) : h;
}
static inline int pref_group_gap(void)
{
    return gui_space_2();
}
static inline int pref_group_height(const int *row_heights, int count)
{
    int h = 0;
    for (int i = 0; i < count; i++)
        h += row_heights[i];
    return h;
}

// Drawing state for one boxed group: the container and its hairlines are
// painted up front, then rows are consumed top-down with pref_group_next.
struct PrefGroup
{
    int x, y, w;
    int row_y;
};

static void pref_group_begin(Surface *win, PrefGroup *g, int x, int y, int w, const int *row_heights, int count)
{
    int total = pref_group_height(row_heights, count);
    gui_draw_boxed_container(win, x, y, w, total);
    int divider_y = y;
    for (int i = 0; i + 1 < count; i++) {
        divider_y += row_heights[i];
        gui_draw_boxed_divider(win, x, divider_y, w, gui_space_2());
    }
    g->x = x;
    g->y = y;
    g->w = w;
    g->row_y = y;
}

static Rect pref_group_next(PrefGroup *g, int row_h)
{
    Rect r = gui_rect_make(g->x, g->row_y, g->w, row_h);
    g->row_y += row_h;
    return r;
}

// Label/value metric row for boxed groups (label left, value right-aligned).
static void pref_kv_row(Surface *win, const Rect *r, const char *label, const char *value)
{
    if (!win || !r || r->w <= 0 || r->h <= 0)
        return;
    int text_y = r->y + (r->h - gui_line_height()) / 2;
    gui_draw_text_clipped(win, gui_font_default(), r->x + gui_space_2(), text_y, r->w / 2, label ? label : "",
                          g_gui_style.text_dim, 0);
    int value_w = gui_measure_text(gui_font_default(), value ? value : "");
    int value_x = r->x + r->w - gui_space_2() - value_w;
    if (value_x < r->x + r->w / 2)
        value_x = r->x + r->w / 2;
    gui_draw_text_clipped(win, gui_font_default(), value_x, text_y, r->x + r->w - gui_space_2() - value_x,
                          value ? value : "", g_gui_style.text, 0);
}

// Refresh the live network snapshot from the kernel. Called when the Network
// tab becomes active (mirrors the Devices-tab lazy refresh).
static void refresh_network_status(PreferencesState *state)
{
    if (!state)
        return;
    state->net_valid = net_status(&state->net) == 0;
}

// Refresh the input-device snapshot from the kernel. Called lazily when the
// Devices tab is shown and debounced from idle to track hotplug.
static void refresh_input_devices(PreferencesState *state)
{
    if (!state)
        return;
    int count = input_enum_devices(state->devices, INPUT_MAX_DEVICES);
    state->device_count = count < 0 ? 0 : (count > INPUT_MAX_DEVICES ? INPUT_MAX_DEVICES : count);
}

static bool input_device_find(const PreferencesState *state, uint32_t id, int *out_index)
{
    if (!state || !out_index)
        return false;
    for (int i = 0; i < state->device_count; i++) {
        if (state->devices[i].id == id) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

// Apply the loaded pointer-speed/repeat scalars to the kernel (idempotent).
static void apply_input_scalars(const PreferencesState *state)
{
    if (!state)
        return;
    input_set_pointer_speed(state->input_pointer_speed);
    input_set_repeat_rate(state->input_repeat_delay, state->input_repeat_rate);
}

static void load_preferences_state(PreferencesState *state, Registry *registry)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->theme_mode = GUI_THEME_DARK;
    state->system_flags = SYSTEM_FLAG_SHOW_DESKTOP_GRID;
    state->storage_mode = STORAGE_MODE_READ_ONLY;
    state->animations_enabled = true;
    state->transparency_level = 180;
    state->volume_level = 75;
    state->input_pointer_speed = 256;
    state->input_repeat_delay = 500;
    state->input_repeat_rate = 33;
    state->device_count = 0;
    state->last_enum_tick = 0;
    state->section = PREF_SECTION_APPEARANCE;
    safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                   wallpaper_default_path_for_theme(state->theme_mode));

    char config[512];
    const char *system_candidates[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(system_candidates, sizeof(system_candidates) / sizeof(system_candidates[0]),
                                      config, sizeof(config))) {
        char value[64];
        if (cfg_line_value(config, "theme", value, sizeof(value))) {
            state->theme_mode = (strcmp(value, "light") == 0) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
        }
        if (cfg_line_value(config, "show_desktop_grid", value, sizeof(value))) {
            if (cfg_value_enabled(value, (state->system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0))
                state->system_flags |= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
            else
                state->system_flags &= ~SYSTEM_FLAG_SHOW_DESKTOP_GRID;
        }
        if (cfg_line_value(config, "clock_show_seconds", value, sizeof(value))) {
            if (cfg_value_enabled(value, (state->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0))
                state->system_flags |= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
            else
                state->system_flags &= ~SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
        }
        if (cfg_line_value(config, "launch_terminal_on_boot", value, sizeof(value))) {
            if (cfg_value_enabled(value, (state->system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) != 0))
                state->system_flags |= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
            else
                state->system_flags &= ~SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
        }
        if (cfg_line_value(config, "animations_enabled", value, sizeof(value)))
            state->animations_enabled = cfg_value_enabled(value, state->animations_enabled);
        if (cfg_line_value(config, "transparency_level", value, sizeof(value))) {
            int level = atoi(value);
            if (level < 0)
                level = 0;
            if (level > 255)
                level = 255;
            state->transparency_level = (uint32_t)level;
        }
        if (cfg_line_value(config, "volume_level", value, sizeof(value))) {
            int volume = atoi(value);
            if (volume < 0)
                volume = 0;
            if (volume > 100)
                volume = 100;
            state->volume_level = (uint32_t)volume;
        }
        if (cfg_line_value(config, "input_pointer_speed", value, sizeof(value))) {
            int mult = atoi(value);
            // Q8 multiplier, matching the kernel's accepted range (16..1024).
            // The slider writes 128..512; wider acceptance keeps speeds set at
            // other UIs from being silently reset on boot.
            if (mult >= 16 && mult <= 1024)
                state->input_pointer_speed = (uint32_t)mult;
        }
        if (cfg_line_value(config, "input_repeat_delay", value, sizeof(value))) {
            int delay = atoi(value);
            if (delay > 0 && delay <= 2000)
                state->input_repeat_delay = (uint32_t)delay;
        }
        if (cfg_line_value(config, "input_repeat_rate", value, sizeof(value))) {
            int rate = atoi(value);
            // key_repeat_rate_to_slider expects the rate within [10, 500] ms.
            if (rate >= 10 && rate <= 500)
                state->input_repeat_rate = (uint32_t)rate;
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                       wallpaper_default_path_for_theme(state->theme_mode));
    }

    const char *wallpaper_candidates[] = {WALLPAPER_CONFIG_PATH, WALLPAPER_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(wallpaper_candidates,
                                      sizeof(wallpaper_candidates) / sizeof(wallpaper_candidates[0]), config,
                                      sizeof(config))) {
        char value[256];
        if (cfg_line_value(config, "", value, sizeof(value))) {
            safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), value);
        } else {
            for (size_t i = 0; config[i]; i++) {
                if (config[i] == '\n' || config[i] == '\r') {
                    config[i] = '\0';
                    break;
                }
            }
            if (config[0]) {
                safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), config);
            }
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                       wallpaper_default_path_for_theme(state->theme_mode));
    }

    if (registry) {
        state->theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
        state->system_flags = registry->system_flags;
        state->animations_enabled = registry->animations_enabled;
        state->transparency_level = registry->transparency_level;
        state->volume_level = registry->volume_level <= 100 ? registry->volume_level : 100;
        if (registry->storage_mode <= STORAGE_MODE_WRITABLE)
            state->storage_mode = (int)registry->storage_mode;
        if (registry->wallpaper_active[0]) {
            safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), registry->wallpaper_active);
        } else if (registry->wallpaper_requested[0]) {
            safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), registry->wallpaper_requested);
        }
    }
    if (wallpaper_is_default_family_path(state->wallpaper_path)) {
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                       wallpaper_default_path_for_theme(state->theme_mode));
    }
    int storage_mode = get_storage_mode();
    if (storage_mode >= STORAGE_MODE_OFF && storage_mode <= STORAGE_MODE_WRITABLE)
        state->storage_mode = storage_mode;
    snprintf(state->status, sizeof(state->status), "Ready");
}

static bool storage_is_persist_writable(const PreferencesState &state)
{
    return state.storage_mode == STORAGE_MODE_WRITABLE;
}

static void set_session_only_status(PreferencesState *state, const char *session_only_status)
{
    if (!state)
        return;
    if (state->storage_mode == STORAGE_MODE_OFF) {
        snprintf(state->status, sizeof(state->status), "%s (storage is off)", session_only_status);
    } else if (state->storage_mode == STORAGE_MODE_READ_ONLY) {
        snprintf(state->status, sizeof(state->status), "%s (storage is read-only)", session_only_status);
    } else {
        snprintf(state->status, sizeof(state->status), "%s", session_only_status);
    }
}

static bool request_storage_mode_change(Registry *registry, int new_mode)
{
    if (!registry)
        return false;
    if (new_mode < STORAGE_MODE_OFF || new_mode > STORAGE_MODE_WRITABLE)
        return false;
    registry->storage_request_mode = (uint32_t)new_mode;
    asm volatile("sfence" ::: "memory");
    registry->storage_request_generation = registry->storage_request_generation + 1u;
    asm volatile("sfence" ::: "memory");
    return true;
}

static bool persist_system_settings(const PreferencesState &state)
{
    char config[512];
    snprintf(config, sizeof(config),
             "theme=%s\n"
             "show_desktop_grid=%d\n"
             "clock_show_seconds=%d\n"
             "launch_terminal_on_boot=%d\n"
             "animations_enabled=%d\n"
             "transparency_level=%u\n"
             "volume_level=%u\n"
             "input_pointer_speed=%u\n"
             "input_repeat_delay=%u\n"
             "input_repeat_rate=%u\n",
             state.theme_mode == GUI_THEME_LIGHT ? "light" : "dark",
             (state.system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) ? 1 : 0,
             (state.system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) ? 1 : 0,
             (state.system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) ? 1 : 0, state.animations_enabled ? 1 : 0,
             state.transparency_level, state.volume_level <= 100 ? state.volume_level : 100, state.input_pointer_speed,
             state.input_repeat_delay, state.input_repeat_rate);
    // Atomic write-temp-then-rename so a crashed write cannot corrupt the boot
    // config (the bootstrap /etc/system.conf covers the rename window).
    return cfg_write_text_file_atomic(SYSTEM_CONFIG_PATH, config);
}

static void publish_system_settings(const PreferencesState &state, Registry *registry)
{
    if (!registry)
        return;
    registry->theme_mode = (uint32_t)state.theme_mode;
    registry->system_flags = state.system_flags;
    registry->animations_enabled = state.animations_enabled;
    registry->transparency_level = state.transparency_level;
    registry->volume_level = state.volume_level <= 100 ? state.volume_level : 100;
    asm volatile("sfence" ::: "memory");
    registry->settings_generation = registry->settings_generation + 1u;
    asm volatile("sfence" ::: "memory");
}

// Publish to the WM Registry and persist to SYSTEM.CFG. apply_system_settings
// and apply_network_settings were byte-identical save for status strings, so a
// single helper covers both; callers supply the persisted/session labels.
static void apply_settings(PreferencesState *state, Registry *registry, const char *persisted_status,
                           const char *session_only_status)
{
    if (!state)
        return;
    publish_system_settings(*state, registry);
    if (!storage_is_persist_writable(*state)) {
        set_session_only_status(state, session_only_status);
        return;
    }
    if (persist_system_settings(*state)) {
        snprintf(state->status, sizeof(state->status), "%s", persisted_status);
    } else {
        LOG_ERROR("preferences", "failed to persist %s to %s", persisted_status, SYSTEM_CONFIG_PATH);
        snprintf(state->status, sizeof(state->status), "%s", session_only_status);
    }
}

static bool apply_wallpaper(PreferencesState *state, Registry *registry, const char *path)
{
    if (!state || !path || !*path)
        return false;
    char resolved_path[256];
    const char *requested_path = wallpaper_resolve_path_for_theme(path, state->theme_mode);
    safe_copy_text(resolved_path, sizeof(resolved_path), requested_path);
    Surface image = {};
    if (!gui_load_uowp(resolved_path, wallpaper_uowp_variant_for_theme(state->theme_mode), 0, 0, &image)) {
        snprintf(state->status, sizeof(state->status), "Wallpaper is not a readable UOWP");
        return false;
    }
    gui_destroy_surface(&image);

    safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), resolved_path);
    if (registry) {
        safe_copy_text(registry->wallpaper_requested, sizeof(registry->wallpaper_requested), resolved_path);
        registry->wallpaper_generation = registry->wallpaper_generation + 1u;
        registry->wallpaper_reload_requested = true;
        asm volatile("sfence" ::: "memory");
    }
    if (!storage_is_persist_writable(*state)) {
        set_session_only_status(state, "Wallpaper applied for this session");
        return true;
    }
    char config[320];
    snprintf(config, sizeof(config), "%s\n", resolved_path);
    if (cfg_write_text_file_atomic(WALLPAPER_CONFIG_PATH, config)) {
        snprintf(state->status, sizeof(state->status), "Wallpaper updated");
    } else {
        LOG_ERROR("preferences", "failed to persist wallpaper to %s", WALLPAPER_CONFIG_PATH);
        snprintf(state->status, sizeof(state->status), "Wallpaper applied for this session");
    }
    return true;
}

static inline int pref_control_row_h(void)
{
    return gui_app_control_h() + gui_space_1() * 2;
}

static int pref_heading_h(void)
{
    return gui_font_line_height(gui_font_title()) + gui_space_2();
}

static int compute_preferences_content_height(PreferencesState *state, int detail_w)
{
    int gap = pref_group_gap();
    // The section heading lives in the headerbar band, not the scrolling body.
    int section_h = 0;
    int control_row = pref_control_row_h();

    if (state->section == PREF_SECTION_APPEARANCE) {
        // detail_w is already the content width (padding removed); match the
        // draw path's stacked condition exactly so heights agree.
        bool stacked_controls = detail_w < gui_scaled_metric(420);
        section_h += control_row;                                              // theme
        section_h += gap + (stacked_controls ? 2 * control_row : control_row); // wallpaper path + actions
        section_h += gap + 2 * pref_boxed_row_h();                             // motion + transparency
        section_h += gap + gui_line_height();                                  // status
    } else if (state->section == PREF_SECTION_DESKTOP) {
        section_h += 2 * pref_boxed_row_h() + pref_boxed_slider_h();
    } else if (state->section == PREF_SECTION_NETWORK) {
        section_h += 2 * pref_boxed_row_h();
        section_h += gap + gui_line_height() * 2;
    } else if (state->section == PREF_SECTION_SYSTEM) {
        section_h += pref_boxed_row_h();
        section_h += gap + control_row; // storage mode
        section_h += gap + gui_line_height();
    } else if (state->section == PREF_SECTION_DEVICES) {
        section_h += 2 * pref_boxed_slider_h();
        section_h += gap;
        int rows = state->device_count > 0 ? state->device_count : 1;
        section_h += rows * pref_boxed_row_h();
        section_h += gap + gui_line_height();
    } else { // PREF_SECTION_ABOUT
        int kv = pref_kv_row_h();
        int overview_h = gui_space_2() + gui_font_line_height(gui_font_title()) + gui_space_0_5() + gui_line_height() +
                         gui_space_0_5() + gui_line_height() + gui_space_2();
        section_h += overview_h;
        section_h += gap + 3 * kv; // runtime
        section_h += gap + 5 * kv; // display
        section_h += gap + 5 * kv; // memory
        section_h += gap + 4 * kv; // platform
        (void)detail_w;
    }
    return section_h;
}

static void preferences_menus(App *app)
{
    (void)app;
    MenuModel model;
    gui_menu_model_reset(&model);

    app_menus_add_help(&model, PREF_MENU_HELP);

    gui_menu_publish(&model);
}

static void draw_about_section(Surface *win, PreferencesState *state, int content_x, int content_w, int y)
{
    const AboutSnapshot &a = state->about;
    const SystemProfile &profile = a.profile;
    const MemInfo &mem = a.mem;
    const DisplayCaps &caps = a.caps;
    const SysTime &now = a.now;

    int gap = pref_group_gap();
    int kv = pref_kv_row_h();
    int space_2 = gui_space_2();

    // Overview card: product identity, build badge and commit line.
    int title_line_h = gui_font_line_height(gui_font_title());
    int line_h = gui_line_height();
    int overview_h = space_2 + title_line_h + gui_space_0_5() + line_h + gui_space_0_5() + line_h + space_2;
    gui_draw_boxed_container(win, content_x, y, content_w, overview_h);
    int oy = y + space_2;
    const char *build_label = profile.kernel_build_debug ? "Debug Build" : "Release Build";
    int badge_w = gui_measure_text(gui_font_default(), build_label) + gui_badge_pad_x() * 2;
    int badge_x = content_x + content_w - space_2 - badge_w;
    if (badge_x < content_x + gui_scaled_metric(140))
        badge_x = content_x + gui_scaled_metric(140);
    gui_draw_text_clipped(win, gui_font_title(), content_x + space_2, oy,
                          badge_x - (content_x + space_2) - gui_space_1(), "uniOS", g_gui_style.text, 0);
    int badge_y = oy + (title_line_h - gui_badge_h()) / 2;
    gui_draw_badge(win, badge_x, badge_y, build_label, g_gui_style.accent_soft, g_gui_style.text);
    oy += title_line_h + gui_space_0_5();

    char summary[128];
    snprintf(summary, sizeof(summary), "Commit %s  |  x86_64",
             profile.kernel_commit[0] ? profile.kernel_commit : "unknown");
    gui_draw_text_clipped(win, gui_font_default(), content_x + space_2, oy, content_w - space_2 * 2, summary,
                          g_gui_style.text_dim, 0);
    oy += line_h + gui_space_0_5();
    gui_draw_text_clipped(win, gui_font_default(), content_x + space_2, oy, content_w - space_2 * 2,
                          "Core system profile and runtime state", g_gui_style.text_muted, 0);
    y += overview_h + gap;

    char time_buf[64], uptime_buf[64], proc_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day, now.hour,
             now.minute, now.second);
    about_format_uptime(a.uptime_seconds, uptime_buf, sizeof(uptime_buf));
    snprintf(proc_buf, sizeof(proc_buf), "%d", a.proc_count);
    {
        int heights[3] = {kv, kv, kv};
        PrefGroup g;
        pref_group_begin(win, &g, content_x, y, content_w, heights, 3);
        Rect r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Local Time", time_buf);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Uptime", uptime_buf);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Processes", proc_buf);
    }
    y += 3 * kv + gap;

    char resolution[32], depth[32], nominal[32], measured[32], flags[96];
    snprintf(resolution, sizeof(resolution), "%ux%u", caps.width, caps.height);
    snprintf(depth, sizeof(depth), "%u-bit", caps.bpp);
    uint32_t nominal_millihz = caps.nominal_refresh_millihz
                                   ? caps.nominal_refresh_millihz
                                   : (caps.refresh_millihz ? caps.refresh_millihz : caps.refresh_hz * 1000u);
    about_format_refresh(nominal_millihz, nominal, sizeof(nominal));
    about_format_refresh(caps.measured_refresh_millihz, measured, sizeof(measured));
    about_format_display_flags(caps.flags, flags, sizeof(flags));
    {
        int heights[5] = {kv, kv, kv, kv, kv};
        PrefGroup g;
        pref_group_begin(win, &g, content_x, y, content_w, heights, 5);
        Rect r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Resolution", resolution);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Target Refresh", nominal);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Actual Refresh", measured);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Color Depth", depth);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Flags", flags);
    }
    y += 5 * kv + gap;

    char total[32], used[32], free_kb[32], heap_total[32], heap_used[32];
    about_format_size_kb(mem.total_kb, total, sizeof(total));
    about_format_size_kb(mem.used_kb, used, sizeof(used));
    about_format_size_kb(mem.free_kb, free_kb, sizeof(free_kb));
    about_format_size_kb(mem.heap_total_kb, heap_total, sizeof(heap_total));
    about_format_size_kb(mem.heap_used_kb, heap_used, sizeof(heap_used));
    {
        int heights[5] = {kv, kv, kv, kv, kv};
        PrefGroup g;
        pref_group_begin(win, &g, content_x, y, content_w, heights, 5);
        Rect r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Total", total);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Used", used);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Free", free_kb);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Heap Total", heap_total);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Heap Used", heap_used);
    }
    y += 5 * kv + gap;

    char cores_buf[16], timer_hz[32], bootloader[96];
    snprintf(cores_buf, sizeof(cores_buf), "%u", profile.cpu_count ? profile.cpu_count : 1u);
    snprintf(timer_hz, sizeof(timer_hz), "%u Hz", profile.timer_hz);
    if (profile.bootloader_version[0])
        snprintf(bootloader, sizeof(bootloader), "%s %s",
                 profile.bootloader_name[0] ? profile.bootloader_name : "Unknown", profile.bootloader_version);
    else
        snprintf(bootloader, sizeof(bootloader), "%s",
                 profile.bootloader_name[0] ? profile.bootloader_name : "Unknown");
    {
        int heights[4] = {kv, kv, kv, kv};
        PrefGroup g;
        pref_group_begin(win, &g, content_x, y, content_w, heights, 4);
        Rect r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "CPU Vendor", a.vendor);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "CPU Cores", cores_buf);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Timer", timer_hz);
        r = pref_group_next(&g, kv);
        pref_kv_row(win, &r, "Bootloader", bootloader);
    }
}

static void draw_preferences(App *app, Surface *win)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    PreferencesState *state = &st->state;
    if (!win)
        return;
    GuiAppLayout layout = gui_app_begin(win);
    int view_w = layout.outer_w + layout.outer_x * 2;
    int view_h = layout.outer_h + layout.outer_y + gui_app_outer_padding();
    int pad = gui_app_outer_padding();

    int nav_item_h = gui_app_nav_h();
    int nav_pitch = nav_item_h + gui_app_row_gap();
    bool stacked_nav = view_w < gui_scaled_metric(620);
    int nav_w = stacked_nav ? view_w : gui_scaled_metric(200);
    int nav_content_h = stacked_nav ? nav_item_h : gui_headerbar_h() + pad + PREF_SECTION_COUNT * nav_pitch + pad;

    int detail_x = stacked_nav ? 0 : nav_w;
    int detail_w = stacked_nav ? view_w : view_w - nav_w;
    int content_x = detail_x + pad;
    int content_w = detail_w - pad * 2;
    int detail_content_h = compute_preferences_content_height(state, content_w);

    int body_content_h = (stacked_nav) ? (nav_content_h + gui_app_section_gap() + detail_content_h) : detail_content_h;
    int content_total = gui_headerbar_h() + pad + body_content_h + pad;
    app_set_content_size(app, view_w, content_total);

    memset(st->nav, 0, sizeof(st->nav));

    int scroll_y = app_scroll_y(app);

    // Edge-to-edge split view: the nav rail is flush with the window frame and
    // spans the full visible height; the detail pane fills the rest. No nested
    // floating panels — structure comes from tone + a hairline divider.
    int nav_x = 0;
    int detail_y;
    if (!stacked_nav) {
        gui_fill_rect(win, 0, scroll_y, nav_w, view_h, g_gui_style.app_surface);
        gui_draw_separator_v(win, nav_w - 1, scroll_y, view_h, gui_hairline_color());
        // Detail content starts below the headerbar deadzone.
        detail_y = gui_headerbar_h() + pad;
    } else {
        // Stacked layout: the nav row scrolls with the content below the band.
        detail_y = gui_headerbar_h() + pad + nav_content_h + gui_app_section_gap();
    }

    // Nav items start below the headerbar to clear the traffic-light deadzone.
    int sticky_nav_y = (stacked_nav ? 0 : scroll_y) + gui_headerbar_h() + pad;
    int y = detail_y;
    int gap = pref_group_gap();
    int control_row = pref_control_row_h();
    int tall = pref_boxed_row_h();

    if (state->section == PREF_SECTION_APPEARANCE) {
        const char *theme_labels[2] = {"Dark", "Light"};
        {
            int heights[1] = {control_row};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 1);
            Rect row = pref_group_next(&g, control_row);
            int text_y = gui_align_text_y(gui_font_default(), row.y, row.h);
            gui_draw_text_clipped(win, gui_font_default(), row.x + gui_space_2(), text_y, row.w / 2, "Theme",
                                  g_gui_style.text, 0);
            int seg_w = gui_scaled_metric(180);
            st->theme.rect = gui_rect_make(row.x + row.w - seg_w - gui_space_2(),
                                           row.y + (row.h - gui_app_control_h()) / 2, seg_w, gui_app_control_h());
            widget_segment_draw(win, &st->theme, theme_labels, 2, state->theme_mode == GUI_THEME_LIGHT ? 1 : 0);
        }
        y += control_row + gap;

        bool stacked_controls = content_w < gui_scaled_metric(420);
        int apply_w = gui_scaled_metric(72);
        int default_w = gui_scaled_metric(92);
        if (stacked_controls) {
            int heights[2] = {control_row, control_row};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 2);
            Rect row = pref_group_next(&g, control_row);
            st->wallpaper_rect = gui_rect_make(row.x + gui_space_2(), row.y + (row.h - gui_app_control_h()) / 2,
                                               row.w - gui_space_4(), gui_app_control_h());
            widget_field_draw(win, &st->wallpaper, st->wallpaper_rect.x, st->wallpaper_rect.y, st->wallpaper_rect.w,
                              st->wallpaper_rect.h);
            row = pref_group_next(&g, control_row);
            int cy = row.y + (row.h - gui_app_control_h()) / 2;
            int pair_w = apply_w + default_w + gui_space_1();
            int left = row.x + (row.w - pair_w) / 2;
            st->apply.rect = gui_rect_make(left, cy, apply_w, gui_app_control_h());
            st->def.rect = gui_rect_make(left + apply_w + gui_space_1(), cy, default_w, gui_app_control_h());
        } else {
            int heights[1] = {control_row};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 1);
            Rect row = pref_group_next(&g, control_row);
            int cy = row.y + (row.h - gui_app_control_h()) / 2;
            int right_edge = row.x + row.w - gui_space_2();
            st->def.rect = gui_rect_make(right_edge - default_w, cy, default_w, gui_app_control_h());
            st->apply.rect = gui_rect_make(st->def.rect.x - gui_space_1() - apply_w, cy, apply_w, gui_app_control_h());
            st->wallpaper_rect =
                gui_rect_make(row.x + gui_space_2(), cy, st->apply.rect.x - gui_space_1() - (row.x + gui_space_2()),
                              gui_app_control_h());
            widget_field_draw(win, &st->wallpaper, st->wallpaper_rect.x, st->wallpaper_rect.y, st->wallpaper_rect.w,
                              st->wallpaper_rect.h);
        }
        widget_button_draw(win, &st->apply, "Apply", true, false);
        widget_button_draw(win, &st->def, "Default", false, false);
        y += (stacked_controls ? 2 * control_row : control_row) + gap;

        {
            int heights[2] = {tall, tall};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 2);
            st->animations.rect = pref_group_next(&g, tall);
            widget_toggle_draw(win, &st->animations, "Motion", "Animate window and system transitions",
                               state->animations_enabled);
            st->transparency.rect = pref_group_next(&g, tall);
            widget_toggle_draw(win, &st->transparency, "Transparency", "Use transparent menu bar and Dock surfaces",
                               state->transparency_level < 255);
        }
        y += 2 * tall + gap;

        gui_draw_string(win, content_x, y, state->status, g_gui_style.text_muted, 0);
    } else if (state->section == PREF_SECTION_DESKTOP) {
        int heights[3] = {tall, tall, pref_boxed_slider_h()};
        PrefGroup g;
        pref_group_begin(win, &g, content_x, y, content_w, heights, 3);
        st->grid.rect = pref_group_next(&g, tall);
        widget_toggle_draw(win, &st->grid, "Show desktop grid", nullptr,
                           (state->system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0);
        st->seconds.rect = pref_group_next(&g, tall);
        widget_toggle_draw(win, &st->seconds, "Show seconds in menu bar clock", nullptr,
                           (state->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0);
        st->volume.rect = pref_group_next(&g, pref_boxed_slider_h());
        widget_slider_draw(win, &st->volume, "Volume", 100);
    } else if (state->section == PREF_SECTION_NETWORK) {
        if (!state->net_valid)
            refresh_network_status(state);
        const NetStatus *net = &state->net;
        const char *nic = "none";
        if (net->nic == NET_NIC_E1000)
            nic = "e1000";
        else if (net->nic == NET_NIC_RTL8139)
            nic = "rtl8139";
        const char *rows[6][2] = {{"Interface", nic},      {"Link", net->link_up ? "up" : "down"},
                                  {"IP address", nullptr}, // filled below (formatted dotted quads)
                                  {"Netmask", nullptr},    {"Gateway", nullptr},
                                  {"DNS", nullptr}};
        char ip_text[4][20];
        const uint32_t addrs[4] = {net->ip, net->netmask, net->gateway, net->dns};
        for (int i = 0; i < 4; i++) {
            snprintf(ip_text[i], sizeof(ip_text[i]), "%u.%u.%u.%u", addrs[i] & 0xFF, (addrs[i] >> 8) & 0xFF,
                     (addrs[i] >> 16) & 0xFF, (addrs[i] >> 24) & 0xFF);
            rows[i + 2][1] = ip_text[i];
        }
        int heights[7];
        for (int i = 0; i < 6; i++)
            heights[i] = control_row;
        heights[6] = control_row;
        PrefGroup g;
        pref_group_begin(win, &g, content_x, y, content_w, heights, 7);
        for (int i = 0; i < 6; i++) {
            Rect row = pref_group_next(&g, control_row);
            int text_y = gui_align_text_y(gui_font_default(), row.y, row.h);
            gui_draw_text_clipped(win, gui_font_default(), row.x + gui_space_2(), text_y, row.w / 2, rows[i][0],
                                  g_gui_style.text, 0);
            const char *value = rows[i][1];
            if (!state->net_valid)
                value = "unavailable";
            else if (i >= 2 && addrs[i - 2] == 0)
                value = "not configured";
            gui_draw_text_clipped(win, gui_font_default(), row.x + row.w / 2, text_y, row.w / 2 - gui_space_4(), value,
                                  g_gui_style.text_muted, 0);
        }
        {
            Rect row = pref_group_next(&g, control_row);
            int cy = row.y + (row.h - gui_app_control_h()) / 2;
            int renew_w = gui_scaled_metric(150);
            st->renew.rect = gui_rect_make(row.x + row.w - renew_w - gui_space_2(), cy, renew_w, gui_app_control_h());
            widget_button_draw(win, &st->renew, "Renew DHCP Lease", true, false);
        }
        y += 7 * control_row + gap;
        gui_draw_string(win, content_x, y, "DHCP is the only address source; the shell `dhcp` command renews too.",
                        g_gui_style.text_muted, 0);
        gui_draw_string(win, content_x, y + gui_line_height(), state->status, g_gui_style.text_muted, 0);
    } else if (state->section == PREF_SECTION_SYSTEM) {
        {
            int heights[1] = {tall};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 1);
            st->terminal.rect = pref_group_next(&g, tall);
            widget_toggle_draw(win, &st->terminal, "Open Terminal at startup", nullptr,
                               (state->system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) != 0);
        }
        y += tall + gap;

        const char *storage_labels[3] = {"Off", "Read-Only", "Writable"};
        {
            int heights[1] = {control_row};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 1);
            Rect row = pref_group_next(&g, control_row);
            int text_y = gui_align_text_y(gui_font_default(), row.y, row.h);
            gui_draw_text_clipped(win, gui_font_default(), row.x + gui_space_2(), text_y, row.w / 2, "Storage Mode",
                                  g_gui_style.text, 0);
            int seg_w = gui_scaled_metric(260);
            if (seg_w > row.w - gui_scaled_metric(140))
                seg_w = row.w - gui_scaled_metric(140);
            st->storage.rect = gui_rect_make(row.x + row.w - seg_w - gui_space_2(),
                                             row.y + (row.h - gui_app_control_h()) / 2, seg_w, gui_app_control_h());
            widget_segment_draw(win, &st->storage, storage_labels, 3, state->storage_mode);
        }
        y += control_row + gap;
        gui_draw_string(win, content_x, y, state->status, g_gui_style.text_muted, 0);
    } else if (state->section == PREF_SECTION_DEVICES) {
        {
            int heights[2] = {pref_boxed_slider_h(), pref_boxed_slider_h()};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 2);
            st->pointer_speed.rect = pref_group_next(&g, pref_boxed_slider_h());
            widget_slider_draw(win, &st->pointer_speed, "Pointer speed", POINTER_SPEED_MAX);
            st->key_repeat.rect = pref_group_next(&g, pref_boxed_slider_h());
            widget_slider_draw(win, &st->key_repeat, "Key repeat rate", 100);
        }
        y += 2 * pref_boxed_slider_h() + gap;

        if (state->device_count <= 0) {
            int heights[1] = {tall};
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, 1);
            Rect row = pref_group_next(&g, tall);
            int text_y = gui_align_text_y(gui_font_default(), row.y, row.h);
            gui_draw_text_clipped(win, gui_font_default(), row.x + gui_space_2(), text_y, row.w - gui_space_4(),
                                  "No input devices detected.", g_gui_style.text_muted, 0);
        } else {
            int heights[INPUT_MAX_DEVICES];
            for (int i = 0; i < state->device_count; i++)
                heights[i] = tall;
            PrefGroup g;
            pref_group_begin(win, &g, content_x, y, content_w, heights, state->device_count);
            for (int i = 0; i < state->device_count; i++) {
                st->device_row_rects[i] = pref_group_next(&g, tall);
                char detail[48];
                if (state->devices[i].vendor_id != 0 || state->devices[i].product_id != 0)
                    snprintf(detail, sizeof(detail), "%04X:%04X", state->devices[i].vendor_id,
                             state->devices[i].product_id);
                else
                    safe_copy_text(detail, sizeof(detail), "Built-in");
                Rect row = st->device_row_rects[i];
                widget_toggle_draw(win, &st->device_toggles[i], state->devices[i].name, detail,
                                   state->devices[i].enabled);
            }
        }
        y += (state->device_count > 0 ? state->device_count : 1) * tall + gap;
        gui_draw_string(win, content_x, y, state->status, g_gui_style.text_muted, 0);
    } else { // PREF_SECTION_ABOUT
        if (!state->about_ready) {
            about_collect_snapshot(&state->about);
            state->about_ready = true;
        }
        draw_about_section(win, state, content_x, content_w, y);
    }

    // Sticky nav rail.
    const char *nav_labels[PREF_SECTION_COUNT] = {"Appearance", "Desktop", "Network", "System", "Devices", "About"};
    const GuiGlyphKind nav_glyphs[PREF_SECTION_COUNT] = {GUI_GLYPH_APPEARANCE, GUI_GLYPH_DESKTOP, GUI_GLYPH_NETWORK,
                                                         GUI_GLYPH_SETTINGS,   GUI_GLYPH_DEVICES, GUI_GLYPH_INFO};
    int pill_inset = gui_scaled_metric(8);
    for (int i = 0; i < PREF_SECTION_COUNT; i++) {
        int item_x, item_y, item_w;
        if (stacked_nav) {
            int slot_gap = gui_space_0_5();
            int slot_w = (view_w - pill_inset * 2 - slot_gap * (PREF_SECTION_COUNT - 1)) / PREF_SECTION_COUNT;
            item_x = pill_inset + i * (slot_w + slot_gap);
            item_y = sticky_nav_y;
            item_w = (i == PREF_SECTION_COUNT - 1) ? (view_w - pill_inset - item_x) : slot_w;
        } else {
            item_x = nav_x + pill_inset;
            item_y = sticky_nav_y + i * nav_pitch;
            item_w = nav_w - pill_inset * 2;
        }
        st->nav[i] = gui_rect_make(item_x, item_y, item_w, nav_item_h);
        gui_app_draw_nav_item(win, st->nav[i].x, st->nav[i].y, st->nav[i].w, st->nav[i].h, nav_glyphs[i], nav_labels[i],
                              state->section == i, st->nav_hover == i);
    }

    // Sticky headerbar over the detail pane: an opaque band carrying the active
    // section title like a macOS toolbar title, so the band is viewport content
    // instead of dead space. Drawn last so scrolled content (and the stacked nav
    // row) slides beneath it. The title is not interactive, so the band around it
    // remains draggable.
    {
        int band_x = stacked_nav ? 0 : nav_w;
        int band_w = view_w - band_x;
        gui_fill_rect(win, band_x, scroll_y, band_w, gui_headerbar_h(), g_gui_style.app_bg);
        const char *section_titles[PREF_SECTION_COUNT] = {"Appearance", "Desktop", "Network",
                                                          "System",     "Devices", "About"};
        int title_x = content_x > gui_traffic_lights_w() ? content_x : gui_traffic_lights_w() + gui_space_1();
        int title_y = scroll_y + gui_align_text_y(gui_font_title(), 0, gui_headerbar_h());
        gui_draw_text_clipped(win, gui_font_title(), title_x, title_y, view_w - title_x - pad,
                              section_titles[state->section], g_gui_style.text, 0);
    }

    if (st->help.open) {
        static const char *tips[] = {
            "Pick a section on the left to change its settings",
            "Theme, volume and toggles apply immediately",
            "Devices lists connected mice and keyboards; toggles freeze a source",
            "Pointer speed and key repeat apply to all input devices",
            "Storage mode controls whether changes persist to /data",
            "Settings marked session-only reset on the next boot",
        };
        widget_help_draw(win, view_w, view_h, scroll_y, "Settings Help", tips, 6);
    }
}

static void preferences_draw(App *app, Surface *canvas)
{
    draw_preferences(app, canvas);
}

static void preferences_sync_from_registry(PreferencesApp *st, Registry *registry)
{
    if (!registry)
        return;
    PreferencesState *state = &st->state;
    state->theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
    state->system_flags = registry->system_flags;
    state->animations_enabled = registry->animations_enabled;
    state->transparency_level = registry->transparency_level;
    state->volume_level = registry->volume_level <= 100 ? registry->volume_level : 100;
    st->volume.value = state->volume_level;
    st->pointer_speed.value = pointer_mult_to_slider(state->input_pointer_speed);
    st->key_repeat.value = key_repeat_rate_to_slider(state->input_repeat_rate);
    if (registry->wallpaper_active[0])
        safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path), registry->wallpaper_active);
    widget_field_set(&st->wallpaper, state->wallpaper_path);
}

static void preferences_settings(App *app)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    preferences_sync_from_registry(st, gui_registry());
    app_invalidate_all(app);
}

static void preferences_menu(App *app, uint32_t cmd)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    if (cmd == PREF_MENU_HELP) {
        st->help.open = true;
        app_invalidate_all(app);
    } else if (cmd == MENU_CMD_PREFERENCES_ABOUT) {
        st->state.section = PREF_SECTION_ABOUT;
        app_invalidate_all(app);
    }
}

static void preferences_clear_hover(PreferencesApp *st)
{
    st->nav_hover = -1;
    widget_toggle_reset(&st->animations);
    widget_toggle_reset(&st->transparency);
    widget_toggle_reset(&st->grid);
    widget_toggle_reset(&st->seconds);
    widget_toggle_reset(&st->terminal);
    for (int i = 0; i < INPUT_MAX_DEVICES; i++)
        widget_toggle_reset(&st->device_toggles[i]);
    widget_slider_reset(&st->volume);
    widget_slider_reset(&st->pointer_speed);
    widget_slider_reset(&st->key_repeat);
    widget_segment_reset(&st->theme);
    widget_segment_reset(&st->storage);
    widget_button_reset(&st->apply);
    widget_button_reset(&st->def);
    st->wallpaper.hovered = false;
}

static void preferences_event(App *app, const Event *ev)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    PreferencesState *state = &st->state;
    Registry *registry = gui_registry();

    switch (ev->type) {
        case EVT_UNFOCUS:
        case EVT_MOUSE_LEAVE:
            preferences_clear_hover(st);
            app_invalidate_all(app);
            break;

        case EVT_WINDOW_SCROLL:
            // The content scrolled under a stationary pointer: cached widget
            // hover flags were computed against pre-scroll coordinates, so
            // clear them (the next mouse move re-establishes hover) instead
            // of highlighting the wrong row.
            preferences_clear_hover(st);
            break;

        case EVT_MOUSE_MOVE: {
            // Slider drags are global (a drag started in one section continues
            // until release) so they are handled before the section gate.
            if (st->volume.dragging) {
                if (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) {
                    state->volume_level = st->volume.value;
                    publish_system_settings(*state, registry);
                }
                app_invalidate_all(app);
                break;
            }
            if (st->pointer_speed.dragging) {
                if (widget_slider_event(&st->pointer_speed, ev, POINTER_SPEED_MAX) & WIDGET_CHANGED) {
                    state->input_pointer_speed = pointer_speed_to_mult(st->pointer_speed.value);
                    input_set_pointer_speed(state->input_pointer_speed);
                }
                app_invalidate_all(app);
                break;
            }
            if (st->key_repeat.dragging) {
                if (widget_slider_event(&st->key_repeat, ev, 100) & WIDGET_CHANGED) {
                    state->input_repeat_rate = key_repeat_slider_to_rate(st->key_repeat.value);
                    input_set_repeat_rate(state->input_repeat_delay, state->input_repeat_rate);
                }
                app_invalidate_all(app);
                break;
            }
            int previous_nav = st->nav_hover;
            st->nav_hover = widget_hit_rects(st->nav, PREF_SECTION_COUNT, ev->mouse.x, ev->mouse.y);
            bool changed = st->nav_hover != previous_nav;
            // Only dispatch hover for the current section's widgets; the others
            // hold stale rects from their own draw and must not be hit-tested.
            switch (state->section) {
                case PREF_SECTION_APPEARANCE:
                    changed |= (widget_segment_event(&st->theme, ev, 2, nullptr) & WIDGET_CHANGED) != 0;
                    changed |= (widget_field_event(&st->wallpaper, st->wallpaper_rect, ev) & WIDGET_CHANGED) != 0;
                    changed |= (widget_button_event(&st->apply, ev) & WIDGET_CHANGED) != 0;
                    changed |= (widget_button_event(&st->def, ev) & WIDGET_CHANGED) != 0;
                    changed |= (widget_toggle_event(&st->animations, ev) & WIDGET_CHANGED) != 0;
                    changed |= (widget_toggle_event(&st->transparency, ev) & WIDGET_CHANGED) != 0;
                    break;
                case PREF_SECTION_DESKTOP:
                    changed |= (widget_toggle_event(&st->grid, ev) & WIDGET_CHANGED) != 0;
                    changed |= (widget_toggle_event(&st->seconds, ev) & WIDGET_CHANGED) != 0;
                    changed |= (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) != 0;
                    break;
                case PREF_SECTION_NETWORK:
                    changed |= (widget_button_event(&st->renew, ev) & WIDGET_CHANGED) != 0;
                    break;
                case PREF_SECTION_SYSTEM:
                    changed |= (widget_toggle_event(&st->terminal, ev) & WIDGET_CHANGED) != 0;
                    changed |= (widget_segment_event(&st->storage, ev, 3, nullptr) & WIDGET_CHANGED) != 0;
                    break;
                case PREF_SECTION_DEVICES:
                    changed |= (widget_slider_event(&st->pointer_speed, ev, POINTER_SPEED_MAX) & WIDGET_CHANGED) != 0;
                    changed |= (widget_slider_event(&st->key_repeat, ev, 100) & WIDGET_CHANGED) != 0;
                    for (int i = 0; i < state->device_count; i++)
                        changed |= (widget_toggle_event(&st->device_toggles[i], ev) & WIDGET_CHANGED) != 0;
                    break;
            }
            if (changed)
                app_invalidate_all(app);
            break;
        }

        case EVT_MOUSE_DOWN: {
            if (ev->mouse.button != 1)
                break;
            if (st->help.open) {
                if (widget_help_event(&st->help, ev))
                    app_invalidate_all(app);
                break;
            }

            // Navigation is always live (its rects are recomputed every frame).
            int nav_index = widget_hit_rects(st->nav, PREF_SECTION_COUNT, ev->mouse.x, ev->mouse.y);
            if (nav_index >= 0 && nav_index != state->section) {
                state->section = nav_index;
                if (state->section == PREF_SECTION_DEVICES)
                    refresh_input_devices(state);
                if (state->section == PREF_SECTION_NETWORK)
                    refresh_network_status(state);
                // The previous section's widgets keep stale hover flags that
                // must not survive the switch (they would re-light when the
                // user returns until the next mouse move).
                preferences_clear_hover(st);
                app_invalidate_all(app);
                break;
            }

            // Per-section dispatch: only the current section's widgets have
            // valid rects, so stale rects can no longer steal a click.
            switch (state->section) {
                case PREF_SECTION_APPEARANCE: {
                    int seg_index = -1;
                    if (widget_segment_event(&st->theme, ev, 2, &seg_index) & WIDGET_CLICKED) {
                        bool wallpaper_tracks_theme = wallpaper_is_default_family_path(state->wallpaper_path);
                        state->theme_mode = (seg_index == 1) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
                        if (wallpaper_tracks_theme) {
                            safe_copy_text(state->wallpaper_path, sizeof(state->wallpaper_path),
                                           wallpaper_default_path_for_theme(state->theme_mode));
                            widget_field_set(&st->wallpaper, state->wallpaper_path);
                        }
                        apply_settings(state, registry, "Theme updated", "Theme applied for this session");
                        gui_sync_theme_from_registry();
                        app_invalidate_all(app);
                        break;
                    }
                    if (widget_toggle_event(&st->animations, ev) & WIDGET_CLICKED) {
                        state->animations_enabled = !state->animations_enabled;
                        apply_settings(state, registry, "Animations updated", "Animations applied for this session");
                        app_invalidate_all(app);
                        break;
                    }
                    if (widget_toggle_event(&st->transparency, ev) & WIDGET_CLICKED) {
                        state->transparency_level = (state->transparency_level > 200) ? 180 : 255;
                        apply_settings(state, registry, "Transparency updated",
                                       "Transparency applied for this session");
                        app_invalidate_all(app);
                        break;
                    }
                    // Apply/Default press here; the action fires on mouse-up.
                    if (widget_button_event(&st->apply, ev) & WIDGET_CHANGED)
                        app_invalidate_all(app);
                    if (widget_button_event(&st->def, ev) & WIDGET_CHANGED)
                        app_invalidate_all(app);
                    if (widget_field_event(&st->wallpaper, st->wallpaper_rect, ev) & WIDGET_CHANGED)
                        app_invalidate_all(app);
                    break;
                }
                case PREF_SECTION_DESKTOP: {
                    if (widget_toggle_event(&st->grid, ev) & WIDGET_CLICKED) {
                        state->system_flags ^= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
                        apply_settings(state, registry, "Desktop setting updated",
                                       "Desktop setting applied for this session");
                        app_invalidate_all(app);
                        break;
                    }
                    if (widget_toggle_event(&st->seconds, ev) & WIDGET_CLICKED) {
                        state->system_flags ^= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
                        apply_settings(state, registry, "Clock setting updated",
                                       "Clock setting applied for this session");
                        app_invalidate_all(app);
                        break;
                    }
                    if (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) {
                        state->volume_level = st->volume.value;
                        publish_system_settings(*state, registry);
                        app_invalidate_all(app);
                    }
                    break;
                }
                case PREF_SECTION_NETWORK: {
                    if (widget_button_event(&st->renew, ev) & WIDGET_CLICKED) {
                        snprintf(state->status, sizeof(state->status), "Renewing DHCP lease...");
                        app_invalidate_all(app);
                        app_commit(app);

                        const int r = net_renew();
                        refresh_network_status(state);
                        if (r == 0)
                            snprintf(state->status, sizeof(state->status), "DHCP lease renewed");
                        else if (r == -16)
                            snprintf(state->status, sizeof(state->status), "A renew is already in progress");
                        else if (r == -19)
                            snprintf(state->status, sizeof(state->status), "No network device");
                        else if (r == -11)
                            snprintf(state->status, sizeof(state->status), "Network not initialized yet");
                        else
                            snprintf(state->status, sizeof(state->status), "DHCP renew failed (no ACK)");
                        app_invalidate_all(app);
                        break;
                    }
                    break;
                }
                case PREF_SECTION_SYSTEM: {
                    if (widget_toggle_event(&st->terminal, ev) & WIDGET_CLICKED) {
                        state->system_flags ^= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
                        apply_settings(state, registry, "Startup setting updated",
                                       "Startup setting applied for this session");
                        app_invalidate_all(app);
                        break;
                    }
                    int seg_index = -1;
                    if (widget_segment_event(&st->storage, ev, 3, &seg_index) & WIDGET_CLICKED) {
                        if (request_storage_mode_change(registry, seg_index))
                            snprintf(state->status, sizeof(state->status), "Storage Mode update requested");
                        else
                            snprintf(state->status, sizeof(state->status), "Failed to update Storage Mode");
                        app_invalidate_all(app);
                    }
                    break;
                }
                case PREF_SECTION_DEVICES: {
                    if (widget_slider_event(&st->pointer_speed, ev, POINTER_SPEED_MAX) & WIDGET_CHANGED) {
                        state->input_pointer_speed = pointer_speed_to_mult(st->pointer_speed.value);
                        input_set_pointer_speed(state->input_pointer_speed);
                        app_invalidate_all(app);
                        break;
                    }
                    if (widget_slider_event(&st->key_repeat, ev, 100) & WIDGET_CHANGED) {
                        state->input_repeat_rate = key_repeat_slider_to_rate(st->key_repeat.value);
                        input_set_repeat_rate(state->input_repeat_delay, state->input_repeat_rate);
                        app_invalidate_all(app);
                        break;
                    }
                    for (int i = 0; i < state->device_count; i++) {
                        if (widget_toggle_event(&st->device_toggles[i], ev) & WIDGET_CLICKED) {
                            bool now_enabled = !state->devices[i].enabled;
                            state->devices[i].enabled = now_enabled;
                            input_set_device_enabled(state->devices[i].id, now_enabled);
                            snprintf(state->status, sizeof(state->status), "%s %s", state->devices[i].name,
                                     now_enabled ? "enabled" : "disabled");
                            app_invalidate_all(app);
                            break;
                        }
                    }
                    break;
                }
            }
            break;
        }

        case EVT_MOUSE_UP: {
            if (ev->mouse.button != 1)
                break;
            if (st->volume.dragging) {
                if (widget_slider_event(&st->volume, ev, 100) & WIDGET_CLICKED)
                    apply_settings(state, registry, "Volume updated", "Volume applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (st->pointer_speed.dragging) {
                if (widget_slider_event(&st->pointer_speed, ev, POINTER_SPEED_MAX) & WIDGET_CLICKED)
                    apply_settings(state, registry, "Pointer speed updated", "Pointer speed applied for this session");
                app_invalidate_all(app);
                break;
            }
            if (st->key_repeat.dragging) {
                if (widget_slider_event(&st->key_repeat, ev, 100) & WIDGET_CLICKED)
                    apply_settings(state, registry, "Key repeat updated", "Key repeat applied for this session");
                app_invalidate_all(app);
                break;
            }
            // Release-to-apply: the wallpaper change fires on mouse-up so a
            // drag away cancels it.
            if (widget_button_event(&st->apply, ev) & WIDGET_CLICKED) {
                apply_wallpaper(state, registry, state->wallpaper_path);
                widget_field_set(&st->wallpaper, state->wallpaper_path);
                app_invalidate_all(app);
                break;
            }
            if (widget_button_event(&st->def, ev) & WIDGET_CLICKED) {
                apply_wallpaper(state, registry, wallpaper_default_path_for_theme(state->theme_mode));
                widget_field_set(&st->wallpaper, state->wallpaper_path);
                app_invalidate_all(app);
                break;
            }
            break;
        }

        case EVT_KEY_DOWN: {
            if (st->help.open) {
                if (widget_help_event(&st->help, ev))
                    app_invalidate_all(app);
                break;
            }
            // The wallpaper field only exists in the Appearance section; only
            // dispatch it there so a stale rect cannot grab focus elsewhere.
            if (state->section == PREF_SECTION_APPEARANCE) {
                int field_rc = widget_field_event(&st->wallpaper, st->wallpaper_rect, ev);
                if (field_rc & WIDGET_FIELD_ENTER) {
                    apply_wallpaper(state, registry, state->wallpaper_path);
                    widget_field_set(&st->wallpaper, state->wallpaper_path);
                    app_invalidate_all(app);
                } else if (field_rc & WIDGET_CHANGED) {
                    app_invalidate_all(app);
                }
            }
            break;
        }

        default:
            break;
    }
}

static void preferences_idle(App *app)
{
    PreferencesApp *st = (PreferencesApp *)app_user(app);
    PreferencesState *state = &st->state;
    Registry *registry = gui_registry();
    int current_storage_mode =
        registry && registry->storage_mode <= STORAGE_MODE_WRITABLE ? (int)registry->storage_mode : get_storage_mode();
    if (current_storage_mode >= STORAGE_MODE_OFF && current_storage_mode <= STORAGE_MODE_WRITABLE &&
        current_storage_mode != state->storage_mode) {
        state->storage_mode = current_storage_mode;
        app_invalidate_all(app);
    }

    // Debounced device re-enumeration while the Devices tab is open so USB
    // attach/detach is reflected without a syscall every idle tick.
    if (state->section == PREF_SECTION_DEVICES) {
        uint64_t now = get_ticks();
        if (now - state->last_enum_tick >= 500) {
            state->last_enum_tick = now;
            int prev_count = state->device_count;
            refresh_input_devices(state);
            if (state->device_count != prev_count)
                app_invalidate_all(app);
        }
    }

    // The About section shows live runtime stats: refresh once a second while
    // it is on screen.
    if (state->section == PREF_SECTION_ABOUT) {
        uint64_t now = get_ticks();
        if (now - state->about_refresh_tick >= 1000) {
            state->about_refresh_tick = now;
            about_collect_snapshot(&state->about);
            state->about_ready = true;
            app_invalidate_all(app);
        }
    }
}

extern "C" int main()
{
    static PreferencesApp st = {};
    st.nav_hover = -1;
    load_preferences_state(&st.state, nullptr);
    widget_field_init(&st.wallpaper, st.state.wallpaper_path, sizeof(st.state.wallpaper_path));
    st.pointer_speed.value = pointer_mult_to_slider(st.state.input_pointer_speed);
    st.key_repeat.value = key_repeat_rate_to_slider(st.state.input_repeat_rate);
    refresh_input_devices(&st.state);

    AppConfig config = {};
    config.title = "Settings";
    config.width = gui_scaled_metric(760);
    config.height = gui_scaled_metric(460);
    config.min_width = gui_scaled_metric(560);
    config.min_height = gui_scaled_metric(420);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 10;
    config.on_draw = preferences_draw;
    config.on_event = preferences_event;
    config.on_menu = preferences_menu;
    config.on_menus = preferences_menus;
    config.on_settings = preferences_settings;
    config.on_idle = preferences_idle;

    App *app = app_create(&config, &st);
    if (!app)
        return 1;

    // The registry exists once the window is registered: seed widget state
    // from it (theme, volume, wallpaper) before the first frame.
    preferences_sync_from_registry(&st, gui_registry());
    gui_apply_theme(st.state.theme_mode);

    // The menubar's "About uniOS" entry launches us with an open request that
    // jumps straight to the About section.
    char open_path[256];
    if (gui_open_request_take(open_path, sizeof(open_path)) && strcmp(open_path, "about") == 0)
        st.state.section = PREF_SECTION_ABOUT;

    app_invalidate_all(app);
    app_commit(app);
    while (app_pump(app)) {
        app_commit(app);
        if (!app_needs_draw(app) && !st.volume.dragging && !st.pointer_speed.dragging && !st.key_repeat.dragging)
            sleep_ms(config.idle_ms);
    }
    app_destroy(app);
    return 0;
}
