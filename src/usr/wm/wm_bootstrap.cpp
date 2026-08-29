#include "../libgui/gui_pixops.h"
#include "wm_damage.h"
#include "wm_input.h"
#include "wm_main.h"
#include "wm_metrics.h"
#include "wm_overlays.h"
#include "wm_present.h"
#include "wm_render.h"
#include "wm_settings.h"
#include "wm_window.h"

Registry *wm_bootstrap()
{
    g_screen = gui_init_framebuffer();
    if (!g_screen.buffer)
        return nullptr;

    {
        if (display_get_caps(&g_display_caps) == 0) {
            g_display_copy_path = (g_display_caps.flags & DISPLAY_FLAG_USES_COPY_PATH) != 0 &&
                                  (g_display_caps.flags & DISPLAY_FLAG_HAS_PAGE_FLIP) == 0;
        }
        // Shell blur runs on page-flip backends and on copy-path backends that
        // still expose a compositor (e.g. the UEFI GOP framebuffer). It is only
        // suppressed on a truly compositor-less copy path, where there is nothing
        // to amortize the translucent shell surfaces against.
        g_shell_blur_available = !g_display_copy_path || (g_display_caps.flags & DISPLAY_FLAG_HAS_COMPOSITOR) != 0;
    }

    int reg_shm = static_cast<int>(syscall1(SYS_SHM_GET, (sizeof(Registry) + 0xFFFu) & ~0xFFFu));
    if (reg_shm < 0)
        return nullptr;

    uint64_t reg_ptr = syscall1(SYS_SHM_MAP, static_cast<uint64_t>(reg_shm));
    if (reg_ptr == 0 || reg_ptr == static_cast<uint64_t>(-1))
        return nullptr;

    Registry *registry = reinterpret_cast<Registry *>(reg_ptr);
    memset(registry, 0, (sizeof(Registry) + 0xFFFu) & ~0xFFFu);
    registry->mb_shm_id = WIN_SHM_INVALID;
    registry->dk_shm_id = WIN_SHM_INVALID;
    registry->mb_blur_shm_id = WIN_SHM_INVALID;
    registry->dk_blur_shm_id = WIN_SHM_INVALID;
    registry->focused_window = -1;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        memset(&registry->windows[i], 0, sizeof(WindowEntry));
        registry->windows[i].shm_id = WIN_SHM_INVALID;
        registry->windows[i].state = WIN_HIDDEN;
    }

    syscall1(SYS_GUI_REGISTER_WM, 0);
    RuntimeGuiSettings runtime_settings = load_runtime_settings();
    g_system_flags = runtime_settings.system_flags;
    bool run_pixops_selftest = (g_system_flags & SYSTEM_FLAG_WM_PIXEL_SELFTEST) != 0;
#ifdef DEBUG
    run_pixops_selftest = true;
#endif
    if (run_pixops_selftest) {
        bool pixops_ok = gui_pixops_self_test();
        LOG_INFO("wm", "pixel op self-test: %s", pixops_ok ? "PASS" : "FAIL");
        if (!pixops_ok)
            LOG_ERROR("wm", "SIMD pixel ops diverge from scalar reference; rendering may be corrupt");
    }
    g_control_center.network_enabled = runtime_settings.ethernet_enabled;
    g_control_center.animations_enabled = runtime_settings.animations_enabled;
    g_control_center.transparency_level = runtime_settings.transparency_level;
    g_control_center.volume = runtime_settings.volume_level;
    gui_apply_theme(runtime_settings.theme_mode);
    refresh_wm_metrics();

    // Identity alias: on a synchronous copy-path backend the scene buffer can
    // be the present buffer itself — compose draws directly into the buffer
    // the kernel scans out from, eliminating the scene->present blit.
    bool synchronous_copy_backend = g_display_copy_path && (g_display_caps.flags & DISPLAY_FLAG_HAS_COMPOSITOR) != 0 &&
                                    (g_display_caps.flags & DISPLAY_FLAG_SYNCHRONOUS_PRESENT) != 0;
    if (synchronous_copy_backend) {
        DisplayBufferCreate create = {};
        create.width = g_screen.width;
        create.height = g_screen.height;
        create.pixel_format = DISPLAY_PIXEL_FORMAT_XRGB8888;
        create.flags = DISPLAY_BUFFER_FLAG_CPU_VISIBLE | DISPLAY_BUFFER_FLAG_LINEAR | DISPLAY_BUFFER_FLAG_RENDER_TARGET;
        if (display_buffer_create(&create) == 0 && create.handle != 0) {
            DisplayBufferMap map = {};
            map.handle = create.handle;
            if (display_buffer_map(&map) != 0 || map.address == 0 || map.stride < g_screen.width) {
                display_buffer_destroy(create.handle);
            } else {
                PresentBufferSlot &slot = g_presentbuffer_slots[g_presentbuffer_slot_count++];
                memset(&slot, 0, sizeof(PresentBufferSlot));
                slot.surface.width = g_screen.width;
                slot.surface.height = g_screen.height;
                slot.surface.pitch = map.stride * 4u;
                slot.surface.buffer = reinterpret_cast<uint32_t *>(map.address);
                slot.handle = create.handle;
                slot.in_flight_sequence = 0;
                g_backbuffer = slot.surface;
                wm_scene_mark_presentbuffer();
            }
        }
    }

    if (!g_scene_is_presentbuffer) {
        g_backbuffer = gui_create_surface(g_screen.width, g_screen.height);
        if (!g_backbuffer.buffer)
            return nullptr;

        if ((g_display_caps.flags & DISPLAY_FLAG_HAS_COMPOSITOR) != 0) {
            uint32_t requested_present_slots = g_display_copy_path ? 2u : MAX_PRESENT_BUFFER_SLOTS;
            for (uint32_t i = 0; i < requested_present_slots; i++) {
                DisplayBufferCreate create = {};
                create.width = g_screen.width;
                create.height = g_screen.height;
                create.pixel_format = DISPLAY_PIXEL_FORMAT_XRGB8888;
                create.flags =
                    DISPLAY_BUFFER_FLAG_CPU_VISIBLE | DISPLAY_BUFFER_FLAG_LINEAR | DISPLAY_BUFFER_FLAG_RENDER_TARGET;
                if (!g_display_copy_path)
                    create.flags |= DISPLAY_BUFFER_FLAG_SCANOUT;

                if (display_buffer_create(&create) != 0)
                    break;

                DisplayBufferMap map = {};
                map.handle = create.handle;
                if (display_buffer_map(&map) != 0 || map.address == 0 || map.stride < g_screen.width) {
                    display_buffer_destroy(create.handle);
                    break;
                }

                PresentBufferSlot &slot = g_presentbuffer_slots[g_presentbuffer_slot_count++];
                memset(&slot, 0, sizeof(PresentBufferSlot));
                slot.surface.width = g_screen.width;
                slot.surface.height = g_screen.height;
                slot.surface.pitch = map.stride * 4u;
                slot.surface.buffer = reinterpret_cast<uint32_t *>(map.address);
                slot.handle = create.handle;
                slot.in_flight_sequence = 0;
            }
        }
    }

    if (!g_scene_is_presentbuffer && g_presentbuffer_slot_count == 0) {
        PresentBufferSlot &slot = g_presentbuffer_slots[0];
        memset(&slot, 0, sizeof(PresentBufferSlot));
        slot.surface.width = g_screen.width;
        slot.surface.height = g_screen.height;
        slot.surface.pitch = g_screen.pitch;
        slot.surface.buffer =
            static_cast<uint32_t *>(malloc(static_cast<size_t>(g_screen.pitch) * static_cast<size_t>(g_screen.height)));
        slot.handle = 0;
        slot.in_flight_sequence = 0;
        if (slot.surface.buffer)
            g_presentbuffer_slot_count = 1;
    }
    if (g_presentbuffer_slot_count == 0)
        return nullptr;

    sync_presentbuffer_alias_from_active_slot();
    if (!g_presentbuffer.buffer)
        return nullptr;

    init_wallpaper();

    uint32_t dock_w = static_cast<uint32_t>(shell_dock_window_w(SHELL_DOCK_ITEM_COUNT));
    uint32_t dock_h = static_cast<uint32_t>(shell_dock_window_h());
    int menubar_h = wm_menubar_h();

    uint64_t mb_size = (uint64_t)g_screen.width * gui_system_menubar_canvas_h() * 4;
    uint64_t dk_size = (uint64_t)dock_w * dock_h * 4;
    uint64_t mb_blur_size = (uint64_t)g_screen.width * static_cast<uint32_t>(menubar_h) * 4;
    uint64_t dk_blur_size = (uint64_t)dock_w * dock_h * 4;
    if (mb_size > UINT32_MAX || dk_size > UINT32_MAX || mb_blur_size > UINT32_MAX || dk_blur_size > UINT32_MAX)
        return nullptr;

    int mb_shm = syscall1(SYS_SHM_GET, mb_size);
    int dk_shm = syscall1(SYS_SHM_GET, dk_size);
    int mb_blur_shm = syscall1(SYS_SHM_GET, mb_blur_size);
    int dk_blur_shm = syscall1(SYS_SHM_GET, dk_blur_size);

    registry->mb_shm_id = mb_shm;
    registry->dk_shm_id = dk_shm;
    registry->mb_blur_shm_id = mb_blur_shm;
    registry->dk_blur_shm_id = dk_blur_shm;
    registry->theme_mode = runtime_settings.theme_mode;
    registry->system_flags = runtime_settings.system_flags;
    registry->ethernet_enabled = runtime_settings.ethernet_enabled;
    registry->ethernet_use_dhcp = runtime_settings.ethernet_use_dhcp;
    registry->animations_enabled = runtime_settings.animations_enabled;
    registry->transparency_level = runtime_settings.transparency_level;
    registry->volume_level = runtime_settings.volume_level;
    registry->storage_mode = get_storage_mode();
    registry->storage_request_mode = registry->storage_mode;
    registry->wallpaper_status = WALLPAPER_STATUS_SOLID;
    registry->window_count = 2;

    WindowEntry &we0 = registry->windows[0];
    memset(&we0, 0, sizeof(WindowEntry));
    we0.shm_id = mb_shm;
    we0.x = 0;
    we0.y = 0;
    we0.w = static_cast<int>(g_screen.width);
    we0.h = menubar_h;
    we0.restore_x = we0.x;
    we0.restore_y = we0.y;
    we0.restore_w = we0.w;
    we0.restore_h = we0.h;
    we0.buffer_w = we0.w;
    we0.buffer_h = gui_system_menubar_canvas_h();
    we0.min_w = we0.w;
    we0.min_h = we0.h;
    we0.flags = WIN_FLAG_TRANSPARENT | WIN_FLAG_SYSTEM;
    we0.state = WIN_NORMAL;
    we0.active = true;
    we0.ready = true;
    strncpy(we0.title, "Menubar", 63);
    we0.title[63] = '\0';
    damage_reset(&we0.damage);

    WindowEntry &we1 = registry->windows[1];
    memset(&we1, 0, sizeof(WindowEntry));
    we1.shm_id = dk_shm;
    we1.x = static_cast<int>(g_screen.width - dock_w) / 2;
    we1.y = static_cast<int>(g_screen.height - dock_h - shell_dock_bottom_inset());
    we1.w = static_cast<int>(dock_w);
    we1.h = static_cast<int>(dock_h);
    we1.restore_x = we1.x;
    we1.restore_y = we1.y;
    we1.restore_w = we1.w;
    we1.restore_h = we1.h;
    we1.buffer_w = we1.w;
    we1.buffer_h = we1.h;
    we1.min_w = we1.w;
    we1.min_h = we1.h;
    we1.flags = WIN_FLAG_TRANSPARENT | WIN_FLAG_SYSTEM;
    we1.state = WIN_NORMAL;
    we1.active = true;
    we1.ready = true;
    strncpy(we1.title, "Dock", 63);
    we1.title[63] = '\0';
    damage_reset(&we1.damage);

    add_win_internal(mb_shm, 0, 0, static_cast<int>(g_screen.width), menubar_h, "Menubar", &registry->windows[0].damage,
                     &registry->windows[0], true);
    add_win_internal(dk_shm, static_cast<int>(g_screen.width - dock_w) / 2,
                     static_cast<int>(g_screen.height - dock_h - shell_dock_bottom_inset()), dock_w, dock_h, "Dock",
                     &registry->windows[1].damage, &registry->windows[1], true);

    syscall1(SYS_SET_QUIET, 1);
    smp_wmb();

    // Shell blur is suppressed only on a truly compositor-less copy path (no
    // page flip and no compositor to amortize translucent shell surfaces).
    // Backends with a compositor or page flip — including the UEFI GOP
    // framebuffer used on real hardware — run the normal lazy blur path.
    if (!g_shell_blur_available || !init_shell_blur_buffers(registry, dock_w, dock_h)) {
        registry->mb_blur_generation = 0;
        registry->dk_blur_generation = 0;
    }

    g_input.mouse_x = g_screen.width / 2;
    g_input.mouse_y = g_screen.height / 2;
    reload_wallpaper(registry, false);
    sync_storage_prompt_state(false);
    ensure_default_user_storage_layout();

    gui_blit_rect(&g_backbuffer, &g_wallpaper, 0, 0, 0, 0, g_screen.width, g_screen.height);
    capture_shell_backdrop_for_rect({0, 0, static_cast<int>(g_screen.width), static_cast<int>(g_screen.height)},
                                    registry);
    // The shell blur is the most expensive part of startup. The first frame
    // uses the desktop background immediately; the normal compositor loop
    // resolves the marked blur surfaces before its next present.
    if (!g_scene_is_presentbuffer)
        select_presentbuffer_slot_for_frame();

    if (!g_scene_is_presentbuffer)
        gui_blit_rect(&g_presentbuffer, &g_backbuffer, 0, 0, 0, 0, g_screen.width, g_screen.height);
    gui_draw_cursor_kind(&g_presentbuffer, g_input.mouse_x, g_input.mouse_y, g_input.cursor_kind);
    wm_scene_mark_cursor_baked();

    DirtyRect init_pres = {0, 0, static_cast<int>(g_screen.width), static_cast<int>(g_screen.height)};
    uint32_t first_seq = present_frame(&g_presentbuffer, &init_pres, 1, 1, 0, 0, 0);

    if (first_seq && g_presentbuffer_slot_count) {
        g_presentbuffer_slots[g_presentbuffer_active_slot].in_flight_sequence = first_seq;
    }

    mark_other_presentbuffer_slots_stale(&init_pres, 1, g_presentbuffer_active_slot);
    wm_present_init_sequences(first_seq);

    // Ensure deferred shell blur is built on the next compositor frame even
    // when no input or application damage arrives after boot.
    enqueue_damage_rect(0, 0, static_cast<int>(g_screen.width), menubar_h);
    enqueue_damage_rect(registry->windows[1].x, registry->windows[1].y, registry->windows[1].w, registry->windows[1].h);

    smp_wmb();
    registry->magic = REGISTRY_MAGIC;
    smp_wmb();

    LOG_INFO("wm", "first desktop frame submitted at %llu ms", static_cast<unsigned long long>(get_ticks()));
    wm_push_notification("uniOS", "System successfully booted.");

    wm_registry_sync_init(registry, runtime_settings.theme_mode);
    sync_control_center_state_from_registry(registry);

    wm_tsc_to_us(1);

    return registry;
}
