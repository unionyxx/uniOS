#include "wm_render.h"
#include "wm_input.h"
#include "wm_present.h"
#include "wm_window.h"
#include "wm_damage.h"
#include "wm_metrics.h"

static bool g_menubar_blur_dirty = false;
static bool g_dock_blur_dirty = false;
static uint64_t g_last_blur_vblank = 0;

// Blur source dirty rect tracking - must be declared before first use in mark_shell_blur_dirty
static DirtyRect g_menubar_blur_dirty_rects[MAX_DIRTY_RECTS];
static int g_menubar_blur_dirty_count = 0;
static DirtyRect g_dock_blur_dirty_rects[MAX_DIRTY_RECTS];
static int g_dock_blur_dirty_count = 0;

static void add_blur_dirty_rect(DirtyRect *rects, int *count, const DirtyRect &r)
{
    DirtyRect clip = r;
    if (!clip_dirty_rect_to_screen(clip))
        return;
    if (*count < MAX_DIRTY_RECTS) {
        rects[(*count)++] = clip;
    } else {
        if (*count == 1) {
            rects[0] = rect_union(rects[0], clip);
        }
    }
}

static void clear_blur_dirty_rects(DirtyRect * /*rects*/, int *count)
{
    *count = 0;
}

static void compose_desktop_for_blur(Surface *dst, const DirtyRect &clip, int offset_x, int offset_y)
{
    DirtyRect shifted_clip = {clip.x - offset_x, clip.y - offset_y, clip.w, clip.h};
    int start_index = -1;
    bool covered = false;

    for (int i = g_window_count - 1; i >= WM_FIRST_USER_WINDOW; --i) {
        if (!g_window_visible_cache[i] || g_windows[i].transparent || !g_windows[i].buffer)
            continue;
        DirtyRect outer = window_occlusion_bounds(g_windows[i]);
        if (clip.x >= outer.x && clip.y >= outer.y && clip.x + clip.w <= outer.x + outer.w &&
            clip.y + clip.h <= outer.y + outer.h) {
            start_index = i;
            covered = true;
            break;
        }
    }

    if (!covered) {
        gui_blit_rect(dst, &g_wallpaper, shifted_clip.x, shifted_clip.y, clip.x, clip.y, clip.w, clip.h);
    }

    start_index = (start_index < WM_FIRST_USER_WINDOW) ? WM_FIRST_USER_WINDOW : start_index;

    for (int i = start_index; i < g_window_count; ++i) {
        if (!g_window_visible_cache[i] || !g_windows[i].buffer || g_windows[i].transparent ||
            !dirty_rects_intersect(clip, g_window_outer_cache[i]))
            continue;
        Window local = g_windows[i];
        local.x -= offset_x;
        local.y -= offset_y;
        draw_window_client_clipped(dst, local, shifted_clip);
    }

    for (int i = start_index; i < g_window_count; ++i) {
        if (!g_window_visible_cache[i] || !g_windows[i].buffer || !g_windows[i].transparent ||
            !dirty_rects_intersect(clip, g_window_outer_cache[i]))
            continue;
        Window local = g_windows[i];
        local.x -= offset_x;
        local.y -= offset_y;
        draw_window_client_clipped(dst, local, shifted_clip);
    }
}

static void mark_shell_blur_dirty(Registry *registry, const DirtyRect &screen_rect)
{
    if (!registry || !g_backbuffer.buffer)
        return;

    DirtyRect menubar_rect = {0, 0, static_cast<int>(g_screen.width), wm_menubar_h()};
    DirtyRect overlap = {};

    if (g_menubar_blur_source.buffer && rect_intersection(screen_rect, menubar_rect, &overlap)) {
        compose_desktop_for_blur(&g_menubar_blur_source, overlap, 0, 0);
        g_menubar_blur_dirty = true;
        add_blur_dirty_rect(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count, overlap);
    }

    if (g_dock_blur_source.buffer && registry->window_count > 1) {
        DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                               registry->windows[1].h};
        if (clip_dirty_rect_to_screen(dock_rect) && rect_intersection(screen_rect, dock_rect, &overlap)) {
            compose_desktop_for_blur(&g_dock_blur_source, overlap, dock_rect.x, dock_rect.y);
            g_dock_blur_dirty = true;
            add_blur_dirty_rect(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count, overlap);
        }
    }
}

void recapture_shell_blur_sources(Registry *registry)
{
    if (!registry || !g_backbuffer.buffer)
        return;

    // Re-capture full menubar source
    if (g_menubar_blur_source.buffer) {
        int menubar_h = wm_menubar_h();
        DirtyRect full_menubar = {0, 0, static_cast<int>(g_screen.width), menubar_h};
        compose_desktop_for_blur(&g_menubar_blur_source, full_menubar, 0, 0);
        g_menubar_blur_dirty = true;
        clear_blur_dirty_rects(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count);
    }

    // Re-capture full dock source
    if (g_dock_blur_source.buffer && registry->window_count > 1) {
        DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                               registry->windows[1].h};
        if (clip_dirty_rect_to_screen(dock_rect)) {
            compose_desktop_for_blur(&g_dock_blur_source, dock_rect, dock_rect.x, dock_rect.y);
            g_dock_blur_dirty = true;
            clear_blur_dirty_rects(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count);
        }
    }
}

bool init_shell_blur_buffers(Registry *registry, uint32_t dock_w, uint32_t dock_h)
{
    if (!registry || !gui_shm_id_is_valid(registry->mb_blur_shm_id) || !gui_shm_id_is_valid(registry->dk_blur_shm_id))
        return false;

    int menubar_h = wm_menubar_h();
    if (menubar_h <= 0 || dock_w == 0 || dock_h == 0)
        return false;

    uint64_t mb_map = syscall1(SYS_SHM_MAP, static_cast<uint64_t>(registry->mb_blur_shm_id));
    uint64_t dk_map = syscall1(SYS_SHM_MAP, static_cast<uint64_t>(registry->dk_blur_shm_id));

    if (mb_map == 0 || mb_map == static_cast<uint64_t>(-1) || dk_map == 0 || dk_map == static_cast<uint64_t>(-1)) {
        if (mb_map != 0 && mb_map != static_cast<uint64_t>(-1))
            syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->mb_blur_shm_id));
        if (dk_map != 0 && dk_map != static_cast<uint64_t>(-1))
            syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->dk_blur_shm_id));
        return false;
    }

    gui_destroy_surface(&g_menubar_blur_source);
    gui_destroy_surface(&g_dock_blur_source);
    g_menubar_blur_source = gui_create_surface(g_screen.width, static_cast<uint32_t>(menubar_h));
    g_dock_blur_source = gui_create_surface(dock_w, dock_h);

    if (!g_menubar_blur_source.buffer || !g_dock_blur_source.buffer) {
        gui_destroy_surface(&g_menubar_blur_source);
        gui_destroy_surface(&g_dock_blur_source);
        syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->mb_blur_shm_id));
        syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->dk_blur_shm_id));
        g_menubar_blur = {};
        g_dock_blur = {};
        return false;
    }

    g_menubar_blur.buffer = reinterpret_cast<uint32_t *>(mb_map);
    g_menubar_blur.width = g_screen.width;
    g_menubar_blur.height = static_cast<uint32_t>(menubar_h);
    g_menubar_blur.pitch = g_screen.width * 4u;
    g_menubar_blur.owns_buffer = false;

    g_dock_blur.buffer = reinterpret_cast<uint32_t *>(dk_map);
    g_dock_blur.width = dock_w;
    g_dock_blur.height = dock_h;
    g_dock_blur.pitch = dock_w * 4u;
    g_dock_blur.owns_buffer = false;

    memset(g_menubar_blur.buffer, 0, static_cast<size_t>(g_menubar_blur.pitch) * g_menubar_blur.height);
    memset(g_dock_blur.buffer, 0, static_cast<size_t>(g_dock_blur.pitch) * g_dock_blur.height);
    memset(g_menubar_blur_source.buffer, 0,
           static_cast<size_t>(g_menubar_blur_source.pitch) * g_menubar_blur_source.height);
    memset(g_dock_blur_source.buffer, 0, static_cast<size_t>(g_dock_blur_source.pitch) * g_dock_blur_source.height);

    g_menubar_blur_dirty = true;
    g_dock_blur_dirty = true;
    return true;
}

void capture_shell_backdrop_for_rect(const DirtyRect &rect, Registry *registry)
{
    mark_shell_blur_dirty(registry, rect);
}

static inline bool rect_touch_or_overlap(const DirtyRect &a, const DirtyRect &b)
{
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return false;
    return a.x <= b.x + b.w && a.x + a.w >= b.x && a.y <= b.y + b.h && a.y + a.h >= b.y;
}

void flush_shell_blur_updates(Registry *registry)
{
    if (!registry)
        return;

    if (registry->transparency_level >= 255) {
        registry->mb_blur_generation = 0;
        registry->dk_blur_generation = 0;
        g_menubar_blur_dirty = false;
        g_dock_blur_dirty = false;
        clear_blur_dirty_rects(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count);
        clear_blur_dirty_rects(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count);
        asm volatile("sfence" ::: "memory");
        return;
    }

    bool active_drag = g_input.pointer_down && g_input.drag_index >= 2;
    bool hover_resize = g_input.hover_resize_edges != RESIZE_NONE;

    static bool s_was_dragging = false;
    if (active_drag || hover_resize) {
        s_was_dragging = true;
        return;
    }
    if (s_was_dragging) {
        s_was_dragging = false;

        int menubar_h = wm_menubar_h();
        DirtyRect menubar_rect = {0, 0, static_cast<int>(g_screen.width), menubar_h};

        DirtyRect dock_rect = {};
        bool has_dock = false;
        if (g_dock_blur_source.buffer && registry->window_count > 1) {
            const WindowEntry &we1 = registry->windows[1];
            dock_rect = {we1.x, we1.y, we1.w, we1.h};
            has_dock = true;
        }

        bool intersects_menubar = false;
        bool intersects_dock = false;
        for (int i = WM_FIRST_USER_WINDOW; i < g_window_count; i++) {
            if (is_window_visible(g_windows[i])) {
                DirtyRect w_rect = window_outer_bounds(g_windows[i]);

                if (rect_touch_or_overlap(w_rect, menubar_rect)) {
                    intersects_menubar = true;
                }
                if (has_dock && rect_touch_or_overlap(w_rect, dock_rect)) {
                    intersects_dock = true;
                }
            }
        }

        if (intersects_menubar) {
            g_menubar_blur_dirty = true;
            add_blur_dirty_rect(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count, menubar_rect);
        }
        if (intersects_dock) {
            g_dock_blur_dirty = true;
            add_blur_dirty_rect(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count, dock_rect);
        }
    }

    g_last_blur_vblank = g_display_queue.vblank_count;
    bool is_light = registry->theme_mode == GUI_THEME_LIGHT;

    // Always process both surfaces every frame - no stagger
    if (g_menubar_blur_dirty && g_menubar_blur.buffer && g_menubar_blur_source.buffer) {
        if (g_menubar_blur_dirty_count > 0) {
            // Recompose only dirty regions
            for (int i = 0; i < g_menubar_blur_dirty_count; i++) {
                compose_desktop_for_blur(&g_menubar_blur_source, g_menubar_blur_dirty_rects[i], 0, 0);
            }
            clear_blur_dirty_rects(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count);
        } else {
            // Full recomposition
            int menubar_h = wm_menubar_h();
            DirtyRect full = {0, 0, static_cast<int>(g_screen.width), menubar_h};
            compose_desktop_for_blur(&g_menubar_blur_source, full, 0, 0);
        }
        blur_surface_material(&g_menubar_blur_source, &g_menubar_blur, 48.0f, is_light ? 85 : 80, is_light ? 8 : 12);
        registry->mb_blur_generation = registry->mb_blur_generation + 1u;
        g_menubar_blur_dirty = false;
    }

    if (g_dock_blur_dirty && g_dock_blur.buffer && g_dock_blur_source.buffer) {
        if (g_dock_blur_dirty_count > 0) {
            // Recompose only dirty regions
            DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                                   registry->windows[1].h};
            clip_dirty_rect_to_screen(dock_rect);
            for (int i = 0; i < g_dock_blur_dirty_count; i++) {
                compose_desktop_for_blur(&g_dock_blur_source, g_dock_blur_dirty_rects[i], dock_rect.x, dock_rect.y);
            }
            clear_blur_dirty_rects(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count);
        } else {
            // Full recomposition
            DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                                   registry->windows[1].h};
            clip_dirty_rect_to_screen(dock_rect);
            compose_desktop_for_blur(&g_dock_blur_source, dock_rect, dock_rect.x, dock_rect.y);
        }
        blur_surface_material(&g_dock_blur_source, &g_dock_blur, 36.0f, is_light ? 82 : 78, is_light ? 8 : 10);
        registry->dk_blur_generation = registry->dk_blur_generation + 1u;
        g_dock_blur_dirty = false;
    }

    asm volatile("sfence" ::: "memory");
}

