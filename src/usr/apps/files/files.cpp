#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>
#include <unistd.h>

#include "../../libc/syscall.h"
#include "../../libgui/gui.h"

static constexpr int MAX_VOLUMES = 16;
static constexpr int MAX_ROWS = 128;
static constexpr int MAX_PLACES = 5;

struct PlaceEntry
{
    const char *label;
    const char *detail;
    const char *path;
};

static constexpr PlaceEntry k_places[MAX_PLACES] = {
    {"Home", "User storage", "/data"},
    {"Desktop", "Desktop files", "/data/Desktop"},
    {"Documents", "Documents", "/data/Documents"},
    {"Downloads", "Downloads", "/data/Downloads"},
    {"Pictures", "Pictures", "/data/Pictures"},
};

struct FileRow
{
    char name[256];
    char path[512];
    bool is_dir;
    uint64_t size;
};

enum DialogMode
{
    DIALOG_NONE = 0,
    DIALOG_NEW_FOLDER,
    DIALOG_RENAME,
    DIALOG_COPY,
    DIALOG_MOVE,
};

enum MenuKind
{
    MENU_NONE = 0,
    MENU_BACKGROUND,
    MENU_ENTRY,
    MENU_VOLUME,
};

enum MenuCommand
{
    CMD_NONE = -1,
    CMD_OPEN = 0,
    CMD_UP,
    CMD_REFRESH,
    CMD_NEW_FOLDER,
    CMD_RENAME,
    CMD_DELETE,
    CMD_COPY,
    CMD_MOVE,
};

struct AppState
{
    VolumeInfo volumes[MAX_VOLUMES];
    int volume_count;
    int active_volume;
    int storage_mode;
    FileRow rows[MAX_ROWS];
    int row_count;
    int selected_row;
    bool volume_home;
    bool load_failed;
    char current_path[512];
    char status[160];
    uint64_t last_click_ticks;
    int last_click_row;
    DialogMode dialog_mode;
    char dialog_title[64];
    char dialog_input[256];
    MenuKind menu_kind;
    int menu_target_row;
    int menu_target_volume;
    int menu_x;
    int menu_y;
    int menu_w;
    int menu_h;
    int menu_hovered;
    bool needs_redraw;
};

struct LayoutCache
{
    Rect place_rects[MAX_PLACES];
    Rect volume_rects[MAX_VOLUMES];
    Rect row_rects[MAX_ROWS];
    Rect dialog_box;
    Rect dialog_field;
    Rect dialog_ok;
    Rect dialog_cancel;
    Rect menu_rect;
};

static bool rect_contains(Rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void set_status(AppState *state, const char *msg)
{
    if (!state)
        return;
    strncpy(state->status, msg ? msg : "", sizeof(state->status) - 1);
    state->status[sizeof(state->status) - 1] = '\0';
}

static const char *storage_mode_label(int mode)
{
    if (mode == STORAGE_MODE_OFF)
        return "Storage Mode: Off";
    if (mode == STORAGE_MODE_WRITABLE)
        return "Storage Mode: Writable";
    return "Storage Mode: Read-Only";
}

static bool storage_is_writable(const AppState *state)
{
    return state && state->storage_mode == STORAGE_MODE_WRITABLE;
}

static bool path_is_storage_path(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    return strcmp(path, "/data") == 0 || strncmp(path, "/data/", 6) == 0 || strncmp(path, "/vol/", 5) == 0;
}

static int find_data_volume_index(const AppState *state)
{
    if (!state)
        return -1;
    for (int i = 0; i < state->volume_count; i++) {
        if ((state->volumes[i].flags & VOLUME_FLAG_SYSTEM_DATA) != 0)
            return i;
    }
    return -1;
}

static bool is_visible_volume(const VolumeInfo &volume)
{
    return (volume.flags & VOLUME_FLAG_STORAGE_DEVICE) != 0;
}

static int visible_volume_count(const AppState *state)
{
    if (!state)
        return 0;
    int count = 0;
    for (int i = 0; i < state->volume_count; i++) {
        if (is_visible_volume(state->volumes[i]))
            count++;
    }
    return count;
}

static int visible_volume_index_at(const AppState *state, int visible_index)
{
    if (!state || visible_index < 0)
        return -1;
    int count = 0;
    for (int i = 0; i < state->volume_count; i++) {
        if (!is_visible_volume(state->volumes[i]))
            continue;
        if (count == visible_index)
            return i;
        count++;
    }
    return -1;
}

static void join_path(const char *base, const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!base || base[0] == '\0') {
        strncpy(out, name ? name : "", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] != '/' && len + 1 < out_size) {
        out[len++] = '/';
        out[len] = '\0';
    }
    if (name)
        strncat(out, name, out_size - 1 - strlen(out));
}

static void parent_path(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    if (!path || path[0] == '\0' || strcmp(path, "/") == 0) {
        strncpy(out, "/", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        strncpy(out, "/", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    *slash = '\0';
}

static void trim_ascii_whitespace(const char *src, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!src)
        return;
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')
        src++;
    size_t len = strlen(src);
    while (len > 0) {
        char ch = src[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            break;
        len--;
    }
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, src, len);
    out[len] = '\0';
}

static const char *path_basename(const char *path)
{
    if (!path || !path[0])
        return "";
    const char *slash = strrchr(path, '/');
    if (!slash)
        return path;
    return slash[1] ? slash + 1 : slash;
}

static bool path_equals(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static bool validate_simple_name(const char *name)
{
    return name && name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && strchr(name, '/') == nullptr;
}

static bool resolve_destination_path(const AppState *state, const FileRow *row, const char *input, char *dst,
                                     size_t dst_size)
{
    if (!dst || dst_size == 0)
        return false;
    dst[0] = '\0';
    if (!state || !row || !input || !input[0])
        return false;

    if (strchr(input, '/'))
        strncpy(dst, input, dst_size - 1);
    else
        join_path(state->current_path, input, dst, dst_size);
    dst[dst_size - 1] = '\0';

    VNodeStat st = {};
    if (stat(dst, &st) == 0 && st.is_dir) {
        char nested[512];
        join_path(dst, path_basename(row->path), nested, sizeof(nested));
        strncpy(dst, nested, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
    return true;
}

static void reset_click_tracking(AppState *state)
{
    if (!state)
        return;
    state->last_click_row = -1;
    state->last_click_ticks = 0;
}

static bool copy_file_stream(const char *src, const char *dst, char *error, size_t error_size)
{
    if (!src || !dst) {
        snprintf(error, error_size, "invalid copy path");
        return false;
    }
    if (path_equals(src, dst)) {
        snprintf(error, error_size, "source and destination are the same");
        return false;
    }

    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        snprintf(error, error_size, "open failed for %s", src);
        return false;
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmpcopy", dst);
    unlink(tmp_path);
    int out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (out_fd < 0) {
        close(in_fd);
        snprintf(error, error_size, "open failed for %s", tmp_path);
        return false;
    }

    char buffer[4096];
    bool ok = true;
    int n = 0;
    while ((n = read(in_fd, buffer, sizeof(buffer))) > 0) {
        int written_total = 0;
        while (written_total < n) {
            int chunk = write(out_fd, buffer + written_total, (size_t)(n - written_total));
            if (chunk <= 0) {
                ok = false;
                snprintf(error, error_size, "write failed for %s", tmp_path);
                break;
            }
            written_total += chunk;
        }
        if (!ok)
            break;
    }
    if (n < 0) {
        ok = false;
        snprintf(error, error_size, "read failed for %s", src);
    }
    close(in_fd);
    close(out_fd);

    if (!ok) {
        unlink(tmp_path);
        return false;
    }

    VNodeStat st = {};
    if (stat(dst, &st) == 0) {
        if (st.is_dir) {
            snprintf(error, error_size, "destination is a directory");
            unlink(tmp_path);
            return false;
        }
        unlink(dst);
    }

    if (rename(tmp_path, dst) != 0) {
        unlink(tmp_path);
        snprintf(error, error_size, "rename failed for %s", dst);
        return false;
    }
    return true;
}

static bool move_entry(const FileRow *row, const char *dst, char *error, size_t error_size)
{
    if (!row || !dst) {
        snprintf(error, error_size, "invalid move path");
        return false;
    }
    if (path_equals(row->path, dst)) {
        snprintf(error, error_size, "source and destination are the same");
        return false;
    }
    if (rename(row->path, dst) == 0)
        return true;
    if (row->is_dir) {
        snprintf(error, error_size, "cross-volume directory move is unsupported");
        return false;
    }
    if (!copy_file_stream(row->path, dst, error, error_size))
        return false;
    if (unlink(row->path) != 0) {
        unlink(dst);
        snprintf(error, error_size, "source cleanup failed");
        return false;
    }
    return true;
}

static bool delete_selected(const FileRow *row, char *error, size_t error_size)
{
    if (!row) {
        snprintf(error, error_size, "no selection");
        return false;
    }
    if (row->is_dir) {
        if (rmdir(row->path) == 0)
            return true;
        snprintf(error, error_size, "directory is not empty or cannot be removed");
        return false;
    }
    if (unlink(row->path) == 0)
        return true;
    snprintf(error, error_size, "failed to delete file");
    return false;
}

struct MenuEntryDef
{
    GuiMenuItem item;
    MenuCommand command;
};

static int build_menu_entries(const AppState *state, MenuEntryDef *out, int max_count)
{
    if (!state || !out || max_count <= 0)
        return 0;

    int count = 0;
    auto push = [&](const char *label, MenuCommand command, bool enabled, bool separator) {
        if (count >= max_count)
            return;
        out[count].item.label = label;
        out[count].item.enabled = enabled;
        out[count].item.separator = separator;
        out[count].command = command;
        count++;
    };

    if (state->menu_kind == MENU_VOLUME) {
        push("Open Volume", CMD_OPEN, state->menu_target_volume >= 0 && state->menu_target_volume < state->volume_count,
             false);
        push("Refresh", CMD_REFRESH, true, false);
        return count;
    }

    if (state->menu_kind == MENU_ENTRY && state->menu_target_row >= 0 && state->menu_target_row < state->row_count) {
        push(state->rows[state->menu_target_row].is_dir ? "Open Folder" : "Open", CMD_OPEN, true, false);
        push(nullptr, CMD_NONE, false, true);
        push("Rename", CMD_RENAME, storage_is_writable(state), false);
        push("Delete", CMD_DELETE, storage_is_writable(state), false);
        push("Copy To...", CMD_COPY, storage_is_writable(state) && !state->rows[state->menu_target_row].is_dir, false);
        push("Move To...", CMD_MOVE, storage_is_writable(state), false);
        push(nullptr, CMD_NONE, false, true);
        push("Refresh", CMD_REFRESH, true, false);
        push("Up", CMD_UP, true, false);
        return count;
    }

    if (state->menu_kind == MENU_BACKGROUND) {
        if (!state->volume_home) {
            push("New Folder", CMD_NEW_FOLDER, storage_is_writable(state), false);
            push(nullptr, CMD_NONE, false, true);
            push("Refresh", CMD_REFRESH, true, false);
            push("Up", CMD_UP, true, false);
        } else {
            push("Refresh", CMD_REFRESH, true, false);
        }
    }
    return count;
}

static void close_menu(AppState *state)
{
    if (!state)
        return;
    state->menu_kind = MENU_NONE;
    state->menu_target_row = -1;
    state->menu_target_volume = -1;
    state->menu_hovered = -1;
    state->needs_redraw = true;
}

static void refresh_volumes(AppState *state)
{
    int previous_mode = state->storage_mode;
    Registry *registry = gui_registry();
    if (registry && registry->storage_mode <= STORAGE_MODE_WRITABLE)
        state->storage_mode = (int)registry->storage_mode;
    else
        state->storage_mode = get_storage_mode();
    if (state->storage_mode < STORAGE_MODE_OFF || state->storage_mode > STORAGE_MODE_WRITABLE)
        state->storage_mode = STORAGE_MODE_READ_ONLY;
    state->volume_count = get_volumes(state->volumes, MAX_VOLUMES);
    if (state->volume_count < 0)
        state->volume_count = 0;
    if (state->active_volume >= state->volume_count)
        state->active_volume = state->volume_count > 0 ? 0 : -1;
    if (previous_mode != state->storage_mode)
        set_status(state, storage_mode_label(state->storage_mode));
}

static void load_directory(AppState *state)
{
    if (!state)
        return;

    char selected_path[sizeof(state->current_path)] = {};
    if (state->selected_row >= 0 && state->selected_row < state->row_count) {
        strncpy(selected_path, state->rows[state->selected_row].path, sizeof(selected_path) - 1);
        selected_path[sizeof(selected_path) - 1] = '\0';
    }

    state->row_count = 0;
    state->selected_row = -1;
    state->load_failed = false;
    reset_click_tracking(state);
    if (state->volume_home || state->current_path[0] == '\0')
        return;

    VNodeStat dir_stat = {};
    if (stat(state->current_path, &dir_stat) != 0 || !dir_stat.is_dir) {
        state->load_failed = true;
        return;
    }

    int fd = open(state->current_path, O_RDONLY);
    if (fd < 0) {
        state->load_failed = true;
        return;
    }

    while (state->row_count < MAX_ROWS) {
        char name[256] = {};
        if (syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) != 0)
            break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        FileRow &row = state->rows[state->row_count++];
        memset(&row, 0, sizeof(row));
        strncpy(row.name, name, sizeof(row.name) - 1);
        join_path(state->current_path, name, row.path, sizeof(row.path));
        VNodeStat st = {};
        if (stat(row.path, &st) == 0) {
            row.is_dir = st.is_dir;
            row.size = st.size;
        }
        if (selected_path[0] && strcmp(row.path, selected_path) == 0)
            state->selected_row = state->row_count - 1;
    }
    close(fd);
}

static bool ensure_place_directory(AppState *state, const char *path)
{
    if (!state || !path)
        return false;
    VNodeStat st = {};
    if (stat(path, &st) == 0 && st.is_dir)
        return true;
    if (state->storage_mode != STORAGE_MODE_WRITABLE) {
        set_status(state, "Folder is unavailable until storage is writable");
        return false;
    }
    if (mkdir(path) == 0 && stat(path, &st) == 0 && st.is_dir)
        return true;
    set_status(state, "Failed to prepare user folder");
    return false;
}

static void open_data_home(AppState *state)
{
    if (!state)
        return;
    strncpy(state->current_path, "/data", sizeof(state->current_path) - 1);
    state->current_path[sizeof(state->current_path) - 1] = '\0';
    state->volume_home = false;
    state->active_volume = find_data_volume_index(state);
    load_directory(state);
}

static void activate_place(AppState *state, int place_index)
{
    if (!state || place_index < 0 || place_index >= MAX_PLACES)
        return;
    reset_click_tracking(state);
    if (state->storage_mode == STORAGE_MODE_OFF) {
        set_status(state, "Storage is off");
        state->needs_redraw = true;
        return;
    }
    if (place_index > 0) {
        VNodeStat st = {};
        if (stat(k_places[place_index].path, &st) != 0 || !st.is_dir) {
            if (!storage_is_writable(state)) {
                open_data_home(state);
                set_status(state, "Folder is unavailable in read-only mode");
                state->needs_redraw = true;
                return;
            }
            if (!ensure_place_directory(state, k_places[place_index].path)) {
                state->needs_redraw = true;
                return;
            }
        }
    }
    strncpy(state->current_path, k_places[place_index].path, sizeof(state->current_path) - 1);
    state->current_path[sizeof(state->current_path) - 1] = '\0';
    state->volume_home = false;
    state->active_volume = find_data_volume_index(state);
    load_directory(state);
    state->needs_redraw = true;
}

static void select_default_location(AppState *state, bool force_data_home)
{
    if (!state)
        return;
    if (state->storage_mode == STORAGE_MODE_OFF) {
        state->volume_home = true;
        state->current_path[0] = '\0';
        state->row_count = 0;
        state->selected_row = -1;
        state->load_failed = false;
        return;
    }

    int data_index = find_data_volume_index(state);
    if (data_index >= 0 && (force_data_home || state->current_path[0] == '\0' || state->volume_home)) {
        activate_place(state, 0);
        return;
    }

    if (state->current_path[0] != '\0' && path_is_storage_path(state->current_path)) {
        state->volume_home = false;
        load_directory(state);
        return;
    }

    state->volume_home = true;
}

static void enter_volume(AppState *state, int index)
{
    if (!state || index < 0 || index >= state->volume_count)
        return;
    reset_click_tracking(state);
    state->active_volume = index;
    strncpy(state->current_path, state->volumes[index].mount_path, sizeof(state->current_path) - 1);
    state->current_path[sizeof(state->current_path) - 1] = '\0';
    state->volume_home = false;
    load_directory(state);
    state->needs_redraw = true;
}

static void open_dialog(AppState *state, DialogMode mode, const char *title, const char *prefill)
{
    state->dialog_mode = mode;
    strncpy(state->dialog_title, title, sizeof(state->dialog_title) - 1);
    state->dialog_title[sizeof(state->dialog_title) - 1] = '\0';
    strncpy(state->dialog_input, prefill ? prefill : "", sizeof(state->dialog_input) - 1);
    state->dialog_input[sizeof(state->dialog_input) - 1] = '\0';
    state->needs_redraw = true;
}

static void close_dialog(AppState *state)
{
    state->dialog_mode = DIALOG_NONE;
    state->dialog_input[0] = '\0';
    state->needs_redraw = true;
}

static bool confirm_dialog(AppState *state)
{
    if (!state || state->dialog_mode == DIALOG_NONE)
        return false;

    char input[sizeof(state->dialog_input)] = {};
    trim_ascii_whitespace(state->dialog_input, input, sizeof(input));
    char err[160] = {};

    if (state->dialog_mode == DIALOG_NEW_FOLDER) {
        if (!storage_is_writable(state)) {
            set_status(state, "Storage is read-only");
        } else if (!validate_simple_name(input)) {
            set_status(state, "Enter a valid folder name");
        } else {
            char path[512];
            join_path(state->current_path, input, path, sizeof(path));
            if (mkdir(path) == 0) {
                set_status(state, "Folder created");
                load_directory(state);
            } else {
                set_status(state, "Failed to create folder");
            }
        }
        close_dialog(state);
        return true;
    }

    if (state->selected_row < 0 || state->selected_row >= state->row_count) {
        set_status(state, "No item selected");
        close_dialog(state);
        return false;
    }
    if (!storage_is_writable(state)) {
        set_status(state, "Storage is read-only");
        close_dialog(state);
        return false;
    }

    FileRow &row = state->rows[state->selected_row];
    if (state->dialog_mode == DIALOG_RENAME) {
        if (!validate_simple_name(input)) {
            set_status(state, "Enter a valid name");
            close_dialog(state);
            return false;
        }
        char path[512];
        char parent[512];
        parent_path(row.path, parent, sizeof(parent));
        join_path(parent, input, path, sizeof(path));
        if (path_equals(row.path, path)) {
            set_status(state, "Name is unchanged");
        } else if (rename(row.path, path) == 0) {
            set_status(state, "Item renamed");
            load_directory(state);
        } else {
            set_status(state, "Rename failed");
        }
        close_dialog(state);
        return true;
    }

    if (state->dialog_mode == DIALOG_COPY) {
        char dst[512];
        if (!resolve_destination_path(state, &row, input, dst, sizeof(dst))) {
            set_status(state, "Enter a destination path");
        } else if (row.is_dir) {
            set_status(state, "Directory copy is unsupported");
        } else if (copy_file_stream(row.path, dst, err, sizeof(err))) {
            set_status(state, "Copy complete");
            load_directory(state);
        } else {
            set_status(state, err);
        }
        close_dialog(state);
        return true;
    }

    if (state->dialog_mode == DIALOG_MOVE) {
        char dst[512];
        if (!resolve_destination_path(state, &row, input, dst, sizeof(dst))) {
            set_status(state, "Enter a destination path");
        } else if (move_entry(&row, dst, err, sizeof(err))) {
            set_status(state, "Move complete");
            load_directory(state);
        } else {
            set_status(state, err);
        }
        close_dialog(state);
        return true;
    }

    return false;
}

static void draw_volume_home(Surface *win, const GuiAppLayout *layout, AppState *state, LayoutCache *cache)
{
    int volume_rows = visible_volume_count(state);
    int y = layout->body_rect.y + gui_space_2();
    if (state->storage_mode == STORAGE_MODE_OFF || volume_rows == 0) {
        cache->row_rects[0] = gui_rect_make(layout->body_rect.x, y, layout->body_rect.w, gui_app_row_h());
        gui_app_draw_list_row(
            win, cache->row_rects[0].x, cache->row_rects[0].y, cache->row_rects[0].w, cache->row_rects[0].h, "INFO",
            state->storage_mode == STORAGE_MODE_OFF ? "Storage is off" : "No storage volumes available",
            state->storage_mode == STORAGE_MODE_OFF ? "Enable a storage mode to browse /data"
                                                    : "Attach a supported FAT32 volume",
            false, false, false);
        return;
    }
    for (int visible = 0; visible < volume_rows; visible++) {
        int i = visible_volume_index_at(state, visible);
        if (i < 0)
            continue;
        cache->row_rects[visible] = gui_rect_make(layout->body_rect.x, y, layout->body_rect.w, gui_app_row_h());
        gui_app_draw_list_row(
            win, cache->row_rects[visible].x, cache->row_rects[visible].y, cache->row_rects[visible].w,
            cache->row_rects[visible].h, (state->volumes[i].flags & VOLUME_FLAG_SYSTEM_DATA) ? "DATA" : "VOL",
            state->volumes[i].display_name[0] ? state->volumes[i].display_name : state->volumes[i].mount_path,
            state->volumes[i].mount_path, false, false, false);
        y += gui_app_row_h() + gui_space_1();
    }
}

static void draw_dialog(Surface *win, AppState *state, LayoutCache *cache)
{
    if (state->dialog_mode == DIALOG_NONE)
        return;

    int box_w = gui_scaled_metric(360);
    int box_h = gui_scaled_metric(152);
    cache->dialog_box =
        gui_rect_make((int)(win->width - (uint32_t)box_w) / 2, (int)(win->height - (uint32_t)box_h) / 2, box_w, box_h);
    gui_fill_rect(win, 0, 0, (int)win->width, (int)win->height, 0x99000000);
    gui_draw_panel_inset(win, cache->dialog_box.x, cache->dialog_box.y, cache->dialog_box.w, cache->dialog_box.h,
                         g_gui_style.app_surface, g_gui_style.border_focus, g_gui_style.chrome_bg_alt);
    gui_draw_card_header(win, cache->dialog_box.x + 1, cache->dialog_box.y + 1, cache->dialog_box.w - 2,
                         state->dialog_title, nullptr);

    cache->dialog_field =
        gui_rect_make(cache->dialog_box.x + gui_space_2(), cache->dialog_box.y + gui_card_header_h() + gui_space_2(),
                      cache->dialog_box.w - gui_space_4(), gui_app_control_h());
    gui_app_draw_text_field(win, cache->dialog_field.x, cache->dialog_field.y, cache->dialog_field.w,
                            cache->dialog_field.h, state->dialog_input, true, false);

    int btn_w = gui_scaled_metric(88);
    cache->dialog_cancel = gui_rect_make(
        cache->dialog_box.x + cache->dialog_box.w - gui_space_2() - btn_w * 2 - gui_space_1(),
        cache->dialog_box.y + cache->dialog_box.h - gui_space_2() - gui_app_control_h(), btn_w, gui_app_control_h());
    cache->dialog_ok = gui_rect_make(cache->dialog_cancel.x + btn_w + gui_space_1(), cache->dialog_cancel.y, btn_w,
                                     gui_app_control_h());
    gui_app_draw_button(win, cache->dialog_cancel.x, cache->dialog_cancel.y, cache->dialog_cancel.w,
                        cache->dialog_cancel.h, "Cancel", false, false, false);
    gui_app_draw_button(win, cache->dialog_ok.x, cache->dialog_ok.y, cache->dialog_ok.w, cache->dialog_ok.h, "OK", true,
                        false, false);
}

static void draw_menu(Surface *win, AppState *state, LayoutCache *cache)
{
    if (!state || state->menu_kind == MENU_NONE)
        return;

    MenuEntryDef entries[10];
    int count = build_menu_entries(state, entries, 10);
    if (count <= 0)
        return;

    GuiMenuItem items[10];
    for (int i = 0; i < count; i++)
        items[i] = entries[i].item;
    cache->menu_rect = gui_rect_make(state->menu_x, state->menu_y, state->menu_w, state->menu_h);
    gui_draw_popup_menu(win, state->menu_x, state->menu_y, state->menu_w, items, count, state->menu_hovered);
}

static int compute_files_content_height(AppState *state)
{
    int sidebar_h = gui_card_header_h() + gui_space_1() + gui_line_height() + gui_space_1();
    int data_index = find_data_volume_index(state);
    if (data_index >= 0 && state->storage_mode != STORAGE_MODE_OFF)
        sidebar_h += MAX_PLACES * gui_scaled_metric(46) + gui_space_1();
    else
        sidebar_h += gui_scaled_metric(46);
    sidebar_h += gui_card_header_h() + gui_space_1();
    int visible_vols = 0;
    for (int i = 0; i < state->volume_count; i++) {
        if (is_visible_volume(state->volumes[i]))
            visible_vols++;
    }
    sidebar_h += visible_vols * gui_scaled_metric(48);

    int main_h = gui_card_header_h() + gui_space_2();
    if (state->volume_home) {
        int vols = visible_volume_count(state);
        main_h += (vols > 0 ? vols : 1) * (gui_app_row_h() + gui_space_1());
    } else if (state->load_failed) {
        main_h += gui_app_row_h();
    } else {
        main_h += state->row_count * (gui_app_row_h() + 2);
    }

    return sidebar_h > main_h ? sidebar_h : main_h;
}

static void draw_files(Surface *win, AppState *state, LayoutCache *cache)
{
    memset(cache, 0, sizeof(*cache));
    GuiAppLayout layout = gui_app_begin(win);
    int view_w = layout.outer_w + layout.outer_x * 2;
    int body_content_h = compute_files_content_height(state);
    int content_total = layout.body_rect.y + body_content_h + gui_app_outer_padding();
    gui_set_content_size(win, view_w, content_total);

    int sidebar_w = gui_scaled_metric(170);
    int content_x = layout.body_rect.x;
    int content_y = layout.body_rect.y;
    int content_w = layout.body_rect.w;
    int sidebar_x = content_x;
    int main_x = sidebar_x + sidebar_w + gui_app_section_gap();
    int main_w = content_w - sidebar_w - gui_app_section_gap();
    int content_h = layout.body_rect.h;
    if (body_content_h > content_h)
        content_h = body_content_h;

    int scroll_y = (g_my_window) ? g_my_window->scroll_y : 0;
    int sticky_sidebar_y = content_y + scroll_y;

    // 1. Draw non-sticky backgrounds
    gui_draw_panel_inset(win, main_x, content_y, main_w, content_h, g_gui_style.app_surface, g_gui_style.border,
                         g_gui_style.chrome_bg_alt);

    // 2. Draw scrolling content
    int list_y = content_y + gui_card_header_h() + gui_space_2();
    if (state->volume_home) {
        GuiAppLayout home_layout = layout;
        home_layout.body_rect = gui_rect_make(main_x + gui_space_2(), list_y, main_w - gui_space_4(),
                                              content_h - (list_y - content_y) - gui_space_2());
        draw_volume_home(win, &home_layout, state, cache);
    } else if (state->load_failed) {
        gui_app_draw_list_row(win, main_x + gui_space_2(), list_y, main_w - gui_space_4(), gui_app_row_h(), "ERR",
                              "Unable to open directory", state->current_path, false, false, true);
    } else {
        for (int i = 0; i < state->row_count; i++) {
            cache->row_rects[i] = gui_rect_make(main_x + gui_space_2(), list_y + i * (gui_app_row_h() + 2),
                                                main_w - gui_space_4(), gui_app_row_h());
            char detail[96];
            if (state->rows[i].is_dir)
                strncpy(detail, "Directory", sizeof(detail) - 1);
            else
                snprintf(detail, sizeof(detail), "%llu bytes", state->rows[i].size);
            gui_app_draw_list_row(win, cache->row_rects[i].x, cache->row_rects[i].y, cache->row_rects[i].w,
                                  cache->row_rects[i].h, state->rows[i].is_dir ? "DIR" : "FILE", state->rows[i].name,
                                  detail, i == state->selected_row, false, false);
        }
    }

    // 3. Draw sticky overlays
    gui_draw_panel_inset(win, sidebar_x, sticky_sidebar_y, sidebar_w, layout.body_rect.h, g_gui_style.app_surface,
                         g_gui_style.border, g_gui_style.chrome_bg_alt);
    gui_draw_card_header(win, sidebar_x + 1, sticky_sidebar_y + 1, sidebar_w - 2, "Places", nullptr);
    int sy = sticky_sidebar_y + gui_card_header_h() + gui_space_1();
    gui_draw_text_clipped(win, gui_font_default(), sidebar_x + gui_space_2(), sy, sidebar_w - gui_space_4(),
                          storage_mode_label(state->storage_mode), g_gui_style.text_dim, g_gui_style.app_surface);
    sy += gui_line_height() + gui_space_1();
    int data_index = find_data_volume_index(state);
    if (data_index >= 0 && state->storage_mode != STORAGE_MODE_OFF) {
        for (int i = 0; i < MAX_PLACES; i++) {
            cache->place_rects[i] = gui_rect_make(sidebar_x + 1, sy, sidebar_w - 2, gui_scaled_metric(42));
            bool active = !state->volume_home && strcmp(state->current_path, k_places[i].path) == 0;
            gui_app_draw_nav_item(win, cache->place_rects[i].x, cache->place_rects[i].y, cache->place_rects[i].w,
                                  cache->place_rects[i].h, k_places[i].label, k_places[i].detail, active, false);
            sy += gui_scaled_metric(46);
        }
        sy += gui_space_1();
    } else {
        cache->place_rects[0] = gui_rect_make(sidebar_x + 1, sy, sidebar_w - 2, gui_scaled_metric(42));
        gui_app_draw_nav_item(win, cache->place_rects[0].x, cache->place_rects[0].y, cache->place_rects[0].w,
                              cache->place_rects[0].h, "Home", "No data volume available", false, false);
        sy += gui_scaled_metric(46);
    }

    gui_draw_card_header(win, sidebar_x + 1, sy, sidebar_w - 2, "Storage", nullptr);
    sy += gui_card_header_h() + gui_space_1();
    int visible = 0;
    for (int i = 0; i < state->volume_count; i++) {
        if (!is_visible_volume(state->volumes[i]))
            continue;
        cache->volume_rects[visible] = gui_rect_make(sidebar_x + 1, sy, sidebar_w - 2, gui_scaled_metric(44));
        bool active = !state->volume_home && strcmp(state->current_path, state->volumes[i].mount_path) == 0;
        gui_app_draw_nav_item(win, cache->volume_rects[visible].x, cache->volume_rects[visible].y,
                              cache->volume_rects[visible].w, cache->volume_rects[visible].h,
                              state->volumes[i].display_name[0] ? state->volumes[i].display_name
                                                                : state->volumes[i].mount_path,
                              state->volumes[i].mount_path, active, false);
        sy += gui_scaled_metric(48);
        visible++;
    }

    int sticky_main_y = content_y + scroll_y;
    gui_draw_card_header(win, main_x + 1, sticky_main_y + 1, main_w - 2,
                         state->volume_home ? "Browse Storage" : "Directory", nullptr);

    gui_app_draw_header(win, &layout, "Files",
                        state->volume_home ? "Storage, places and recent volumes" : state->current_path,
                        state->status[0] ? state->status : nullptr);

    draw_menu(win, state, cache);
    draw_dialog(win, state, cache);
    gui_blit_to_screen_rect(win, 0, 0, (int)win->width, (int)win->height);
}

static void navigate_up(AppState *state)
{
    if (state->volume_home)
        return;
    reset_click_tracking(state);
    if (state->active_volume >= 0 && state->active_volume < state->volume_count &&
        strcmp(state->current_path, state->volumes[state->active_volume].mount_path) == 0) {
        state->volume_home = true;
        state->current_path[0] = '\0';
        state->row_count = 0;
        state->selected_row = -1;
        state->load_failed = false;
        state->needs_redraw = true;
        return;
    }
    char parent[512];
    parent_path(state->current_path, parent, sizeof(parent));
    if (strcmp(parent, state->current_path) == 0) {
        state->volume_home = true;
        state->current_path[0] = '\0';
    } else {
        strncpy(state->current_path, parent, sizeof(state->current_path) - 1);
        state->current_path[sizeof(state->current_path) - 1] = '\0';
        load_directory(state);
    }
    state->needs_redraw = true;
}

static void activate_row(AppState *state, int index)
{
    if (state->volume_home) {
        int volume_index = visible_volume_index_at(state, index);
        if (volume_index >= 0)
            enter_volume(state, volume_index);
        return;
    }
    if (index < 0 || index >= state->row_count)
        return;
    state->selected_row = index;
    if (!state->rows[index].is_dir) {
        state->needs_redraw = true;
        return;
    }
    strncpy(state->current_path, state->rows[index].path, sizeof(state->current_path) - 1);
    state->current_path[sizeof(state->current_path) - 1] = '\0';
    load_directory(state);
    state->needs_redraw = true;
}

static void open_menu(AppState *state, MenuKind kind, int target_row, int target_volume, int mouse_x, int mouse_y,
                      int canvas_w, int canvas_h)
{
    if (!state)
        return;
    state->menu_kind = kind;
    state->menu_target_row = target_row;
    state->menu_target_volume = target_volume;
    MenuEntryDef entries[10];
    int count = build_menu_entries(state, entries, 10);
    if (count <= 0) {
        close_menu(state);
        return;
    }

    GuiMenuItem items[10];
    for (int i = 0; i < count; i++)
        items[i] = entries[i].item;
    state->menu_w = gui_popup_menu_width(items, count, gui_scaled_metric(170));
    state->menu_h = gui_popup_menu_height(items, count);
    state->menu_x = mouse_x;
    state->menu_y = mouse_y;
    if (state->menu_x + state->menu_w > canvas_w)
        state->menu_x = canvas_w - state->menu_w - gui_space_1();
    if (state->menu_y + state->menu_h > canvas_h)
        state->menu_y = canvas_h - state->menu_h - gui_space_1();
    if (state->menu_x < 0)
        state->menu_x = 0;
    if (state->menu_y < 0)
        state->menu_y = 0;
    state->menu_hovered =
        gui_popup_menu_hit_test(items, count, state->menu_x, state->menu_y, state->menu_w, mouse_x, mouse_y);
    state->needs_redraw = true;
}

static void update_menu_hover(AppState *state, int mouse_x, int mouse_y)
{
    if (!state || state->menu_kind == MENU_NONE)
        return;
    MenuEntryDef entries[10];
    int count = build_menu_entries(state, entries, 10);
    GuiMenuItem items[10];
    for (int i = 0; i < count; i++)
        items[i] = entries[i].item;
    int hovered = gui_popup_menu_hit_test(items, count, state->menu_x, state->menu_y, state->menu_w, mouse_x, mouse_y);
    if (hovered == state->menu_hovered)
        return;
    state->menu_hovered = hovered;
    state->needs_redraw = true;
}

static void execute_menu_command(AppState *state, MenuCommand command)
{
    if (!state || command == CMD_NONE)
        return;

    if (command == CMD_OPEN) {
        if (state->menu_kind == MENU_VOLUME) {
            enter_volume(state, state->menu_target_volume);
        } else if (state->menu_kind == MENU_ENTRY) {
            activate_row(state, state->menu_target_row);
        }
        return;
    }

    if (command == CMD_REFRESH) {
        refresh_volumes(state);
        select_default_location(state, false);
        if (!state->volume_home)
            load_directory(state);
        state->needs_redraw = true;
        return;
    }

    if (command == CMD_UP) {
        navigate_up(state);
        return;
    }

    if (command == CMD_NEW_FOLDER) {
        open_dialog(state, DIALOG_NEW_FOLDER, "New Folder", "");
        return;
    }

    if (state->menu_target_row < 0 || state->menu_target_row >= state->row_count)
        return;
    state->selected_row = state->menu_target_row;

    if (command == CMD_RENAME) {
        open_dialog(state, DIALOG_RENAME, "Rename", state->rows[state->selected_row].name);
        return;
    }

    if (command == CMD_DELETE) {
        char err[160] = {};
        if (delete_selected(&state->rows[state->selected_row], err, sizeof(err))) {
            set_status(state, "Item deleted");
            load_directory(state);
        } else {
            set_status(state, err[0] ? err : "Delete failed");
        }
        state->needs_redraw = true;
        return;
    }

    if (command == CMD_COPY) {
        open_dialog(state, DIALOG_COPY, "Copy To", state->rows[state->selected_row].name);
        return;
    }

    if (command == CMD_MOVE) {
        open_dialog(state, DIALOG_MOVE, "Move To", state->rows[state->selected_row].name);
    }
}

extern "C" int main()
{
    Surface win = gui_register_window_ex("Files", (uint32_t)gui_scaled_metric(860), (uint32_t)gui_scaled_metric(540),
                                         WIN_FLAG_RESIZABLE);
    if (!win.buffer)
        return 1;
    gui_window_set_min_size(gui_scaled_metric(640), gui_scaled_metric(420));
    gui_sync_theme_from_registry();
    gui_request_focus();

    AppState *state = static_cast<AppState *>(malloc(sizeof(AppState)));
    LayoutCache *cache = static_cast<LayoutCache *>(malloc(sizeof(LayoutCache)));
    if (!state || !cache)
        return 1;
    memset(state, 0, sizeof(AppState));
    memset(cache, 0, sizeof(LayoutCache));
    state->active_volume = -1;
    state->selected_row = -1;
    state->last_click_row = -1;
    state->menu_target_row = -1;
    state->menu_target_volume = -1;
    state->menu_hovered = -1;
    state->volume_home = true;
    state->needs_redraw = true;
    state->storage_mode = STORAGE_MODE_READ_ONLY;
    set_status(state, storage_mode_label(state->storage_mode));
    refresh_volumes(state);
    select_default_location(state, true);

    Registry *registry = gui_registry();
    uint32_t self_pid = (uint32_t)syscall1(SYS_GETPID, 0);
    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;

    while (true) {
        Event ev = {};
        if (state->menu_kind != MENU_NONE && registry && registry->focused_owner_pid != self_pid) {
            close_menu(state);
        }
        while (poll_event(&ev) > 0) {
            if (ev.type == EVT_WINDOW_CLOSE)
                return 0;
            if (ev.type == EVT_WINDOW_RESIZE && gui_sync_window_size(&win) > 0)
                state->needs_redraw = true;

            if (ev.type == EVT_MOUSE_MOVE) {
                update_menu_hover(state, ev.mouse.x, ev.mouse.y);
            }

            if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 1) {
                if (state->dialog_mode != DIALOG_NONE) {
                    if (rect_contains(cache->dialog_ok, ev.mouse.x, ev.mouse.y))
                        confirm_dialog(state);
                    else if (rect_contains(cache->dialog_cancel, ev.mouse.x, ev.mouse.y))
                        close_dialog(state);
                    continue;
                }

                if (state->menu_kind != MENU_NONE) {
                    MenuEntryDef entries[10];
                    int count = build_menu_entries(state, entries, 10);
                    GuiMenuItem items[10];
                    for (int i = 0; i < count; i++)
                        items[i] = entries[i].item;
                    int menu_index = gui_popup_menu_hit_test(items, count, state->menu_x, state->menu_y, state->menu_w,
                                                             ev.mouse.x, ev.mouse.y);
                    if (menu_index >= 0) {
                        MenuCommand command = entries[menu_index].command;
                        close_menu(state);
                        execute_menu_command(state, command);
                        continue;
                    }
                    close_menu(state);
                }

                bool handled_left_click = false;
                for (int i = 0; i < MAX_PLACES; i++) {
                    if (gui_rect_is_empty(cache->place_rects[i]))
                        continue;
                    if (rect_contains(cache->place_rects[i], ev.mouse.x, ev.mouse.y)) {
                        activate_place(state, i);
                        handled_left_click = true;
                        break;
                    }
                }
                if (handled_left_click)
                    continue;

                int sidebar_volume_count = visible_volume_count(state);
                for (int i = 0; i < sidebar_volume_count; i++) {
                    if (rect_contains(cache->volume_rects[i], ev.mouse.x, ev.mouse.y)) {
                        int volume_index = visible_volume_index_at(state, i);
                        if (volume_index >= 0)
                            enter_volume(state, volume_index);
                        handled_left_click = true;
                        break;
                    }
                }
                if (handled_left_click)
                    continue;

                int row_count = state->volume_home ? visible_volume_count(state) : state->row_count;
                for (int i = 0; i < row_count; i++) {
                    if (!rect_contains(cache->row_rects[i], ev.mouse.x, ev.mouse.y))
                        continue;
                    uint64_t now = get_ticks();
                    bool double_click = (state->last_click_row == i) && (now - state->last_click_ticks < 400);
                    state->last_click_row = i;
                    state->last_click_ticks = now;
                    if (state->volume_home) {
                        if (double_click)
                            activate_row(state, i);
                    } else {
                        state->selected_row = i;
                        if (double_click)
                            activate_row(state, i);
                        state->needs_redraw = true;
                    }
                    handled_left_click = true;
                    break;
                }
            } else if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 2) {
                if (state->dialog_mode != DIALOG_NONE)
                    continue;

                int sidebar_volume_count = visible_volume_count(state);
                for (int i = 0; i < sidebar_volume_count; i++) {
                    if (!rect_contains(cache->volume_rects[i], ev.mouse.x, ev.mouse.y))
                        continue;
                    int volume_index = visible_volume_index_at(state, i);
                    open_menu(state, MENU_VOLUME, -1, volume_index, ev.mouse.x, ev.mouse.y, (int)win.width,
                              (int)win.height);
                    goto handled_right_click;
                }

                if (!state->volume_home) {
                    for (int i = 0; i < state->row_count; i++) {
                        if (!rect_contains(cache->row_rects[i], ev.mouse.x, ev.mouse.y))
                            continue;
                        state->selected_row = i;
                        open_menu(state, MENU_ENTRY, i, -1, ev.mouse.x, ev.mouse.y, (int)win.width, (int)win.height);
                        goto handled_right_click;
                    }
                } else {
                    for (int i = 0; i < visible_volume_count(state); i++) {
                        if (!rect_contains(cache->row_rects[i], ev.mouse.x, ev.mouse.y))
                            continue;
                        int volume_index = visible_volume_index_at(state, i);
                        open_menu(state, MENU_VOLUME, -1, volume_index, ev.mouse.x, ev.mouse.y, (int)win.width,
                                  (int)win.height);
                        goto handled_right_click;
                    }
                }

                open_menu(state, MENU_BACKGROUND, -1, -1, ev.mouse.x, ev.mouse.y, (int)win.width, (int)win.height);
            handled_right_click:
                state->needs_redraw = true;
            }

            if (ev.type == EVT_KEY_DOWN) {
                if (state->menu_kind != MENU_NONE) {
                    close_menu(state);
                }
                if (state->dialog_mode != DIALOG_NONE) {
                    if (ev.key.c == '\n' || ev.key.c == '\r')
                        confirm_dialog(state);
                    else if (ev.key.c == 27)
                        close_dialog(state);
                    else if (ev.key.c == '\b' || ev.key.c == 127) {
                        size_t len = strlen(state->dialog_input);
                        if (len > 0)
                            state->dialog_input[len - 1] = '\0';
                        state->needs_redraw = true;
                    } else if (ev.key.c >= 32) {
                        size_t len = strlen(state->dialog_input);
                        if (len + 1 < sizeof(state->dialog_input)) {
                            state->dialog_input[len] = ev.key.c;
                            state->dialog_input[len + 1] = '\0';
                            state->needs_redraw = true;
                        }
                    }
                } else if (ev.key.c == '\n' || ev.key.c == '\r') {
                    if (state->selected_row >= 0)
                        activate_row(state, state->selected_row);
                } else if (ev.key.c == 8 || ev.key.c == 127) {
                    navigate_up(state);
                }
            }
        }

        registry = gui_registry();
        int current_storage_mode = registry && registry->storage_mode <= STORAGE_MODE_WRITABLE
                                       ? (int)registry->storage_mode
                                       : get_storage_mode();
        if (current_storage_mode != state->storage_mode) {
            refresh_volumes(state);
            select_default_location(state, false);
            state->needs_redraw = true;
        }
        if (registry && registry->settings_generation != last_settings_generation) {
            last_settings_generation = registry->settings_generation;
            if (gui_sync_theme_from_registry())
                state->needs_redraw = true;
        }

        if (state->needs_redraw) {
            draw_files(&win, state, cache);
            state->needs_redraw = false;
        } else {
            sleep_ms(35);
        }
    }
}
