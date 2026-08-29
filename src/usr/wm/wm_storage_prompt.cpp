#include "wm_core.h"

StoragePromptLayout storage_prompt_layout()
{
    StoragePromptLayout L = {};
    int bw = gui_scaled_metric(540);
    int bh = gui_scaled_metric(276);
    int btn_w = gui_scaled_metric(136);
    int btn_h = gui_app_control_h();
    int gap = gui_space_1();
    int outer_pad = gui_space_2();
    L.box = {(int)(g_screen.width - bw) / 2, (int)(g_screen.height - bh) / 2, bw, bh};
    int by = L.box.y + L.box.h - outer_pad - btn_h;
    L.writable_button = {L.box.x + L.box.w - outer_pad - btn_w, by, btn_w, btn_h};
    L.readonly_button = {L.writable_button.x - gap - btn_w, by, btn_w, btn_h};
    L.off_button = {L.readonly_button.x - gap - btn_w, by, btn_w, btn_h};
    return L;
}

void sync_storage_prompt_state(bool force)
{
    int mode = get_storage_mode();
    bool was = g_storage_prompt.visible;
    if (mode == STORAGE_MODE_WRITABLE && !force) {
        g_storage_prompt.visible = false;
        g_storage_prompt.dismissed = false;
        g_storage_prompt.hovered_button = -1;
    } else if (force || !g_storage_prompt.dismissed) {
        g_storage_prompt.visible = true;
        g_storage_prompt.hovered_button = -1;
    }
    if (g_storage_prompt.visible)
        clear_hover_feedback_state();
    if (was != g_storage_prompt.visible || force)
        enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
}

void open_storage_prompt()
{
    close_context_menu();
    g_storage_prompt.dismissed = false;
    sync_storage_prompt_state(true);
}

void dismiss_storage_prompt()
{
    if (!g_storage_prompt.visible)
        return;
    g_storage_prompt.visible = false;
    g_storage_prompt.dismissed = true;
    g_storage_prompt.hovered_button = -1;
    enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
}

void update_storage_prompt_hover(int mx, int my)
{
    if (!g_storage_prompt.visible)
        return;
    StoragePromptLayout L = storage_prompt_layout();
    int hov =
        point_in_rect(L.off_button, mx, my)
            ? 0
            : (point_in_rect(L.readonly_button, mx, my) ? 1 : (point_in_rect(L.writable_button, mx, my) ? 2 : -1));
    if (hov != g_storage_prompt.hovered_button) {
        g_storage_prompt.hovered_button = hov;
        enqueue_damage_rect(L.box.x, L.box.y, L.box.w, L.box.h);
    }
}

static void ensure_default_storage_file(const char *path, const char *fallback_path)
{
    if (!path || !fallback_path)
        return;
    int probe = open(path, O_RDONLY);
    if (probe >= 0) {
        close(probe);
        return;
    }

    int in = open(fallback_path, O_RDONLY);
    if (in < 0)
        return;
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return;
    }

    char buf[512];
    bool ok = true;
    while (true) {
        int n = read(in, buf, sizeof(buf));
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        if (write(out, buf, (size_t)n) != n) {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);
    if (!ok)
        unlink(path);
}

void ensure_default_user_storage_layout()
{
    if (get_storage_mode() != STORAGE_MODE_WRITABLE)
        return;
    VNodeStat st = {};
    if (stat("/data", &st) != 0 || !st.is_dir)
        return;
    struct StandardDir
    {
        const char *c;
    } dirs[] = {{"/data/Desktop"}, {"/data/Documents"}, {"/data/Downloads"}, {"/data/Pictures"}};
    for (auto d : dirs) {
        if (stat(d.c, &st) == 0 && st.is_dir)
            continue;
        mkdir(d.c);
    }
    ensure_default_storage_file(SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH);
    ensure_default_storage_file(WALLPAPER_CONFIG_PATH, WALLPAPER_BOOTSTRAP_CONFIG_PATH);
}

bool apply_storage_mode_request(Registry *registry, int mode)
{
    if (!registry || mode < STORAGE_MODE_OFF || mode > STORAGE_MODE_WRITABLE || set_storage_mode(mode) != 0)
        return false;
    if (mode == STORAGE_MODE_WRITABLE)
        ensure_default_user_storage_layout();
    registry->storage_mode = mode;
    registry->storage_request_mode = mode;
    asm volatile("sfence" ::: "memory");
    sync_storage_prompt_state(false);
    return true;
}

bool activate_storage_prompt_button(Registry *registry, int mx, int my)
{
    if (!g_storage_prompt.visible)
        return false;
    StoragePromptLayout L = storage_prompt_layout();
    if (point_in_rect(L.off_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_OFF);
        dismiss_storage_prompt();
        return true;
    }
    if (point_in_rect(L.readonly_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_READ_ONLY);
        dismiss_storage_prompt();
        return true;
    }
    if (point_in_rect(L.writable_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_WRITABLE);
        dismiss_storage_prompt();
        return true;
    }
    return false;
}

