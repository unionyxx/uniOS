#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>
#include <unistd.h>

#include "../../libapp/app.h"
#include "../../libapp/widgets.h"
#include "../../libc/config_utils.h"
#include "../../libc/syscall.h"
#include "../../libc/vec.h"
#include "../../libgui/gui.h"
#include "../../libmedia/media_image.h"

static constexpr int MAX_VOLUMES = 16;
// Safety bound on a single directory listing so a hostile or corrupt volume
// cannot exhaust memory; ordinary directories are far smaller.
static constexpr int MAX_LIST_ROWS = 100000;
static constexpr int MAX_PLACES = 5;
static constexpr int FILES_ICON_SIZE_PX = 48;

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

static constexpr GuiGlyphKind k_place_glyphs[MAX_PLACES] = {
    GUI_GLYPH_HOME, GUI_GLYPH_DESKTOP, GUI_GLYPH_DOCUMENTS, GUI_GLYPH_DOWNLOADS, GUI_GLYPH_PICTURES,
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
    DIALOG_HELP,
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

// Menubar command IDs (dispatched through WindowEntry.menu_command_id).
enum FilesMenuId
{
    FILES_MENU_NEW_FOLDER = 1,
    FILES_MENU_RENAME,
    FILES_MENU_DELETE,
    FILES_MENU_REFRESH,
    FILES_MENU_UP,
    FILES_MENU_TOGGLE_SIDEBAR,
    FILES_MENU_TOGGLE_VIEW,
    FILES_MENU_GO_PLACE = 0x20,  // + place index
    FILES_MENU_GO_VOLUME = 0x40, // + visible volume index
    FILES_MENU_HELP = 0x80,
};

namespace {
struct ThumbCache
{
    char path[512];
    media_image img;
    bool tried;
};

struct AppState
{
    VolumeInfo volumes[MAX_VOLUMES];
    int volume_count;
    int active_volume;
    int storage_mode;
    Vec<FileRow> rows;
    int selected_row;
    bool volume_home;
    bool load_failed;
    char current_path[512];
    char status[160];
    uint64_t last_click_ticks;
    int last_click_row;
    DialogMode dialog_mode;
    char dialog_title[64];
    WidgetDialog dialog;
    MenuKind menu_kind;
    int menu_target_row;
    int menu_target_volume;
    WidgetPopup popup;
    GuiMenuItem popup_items[10];
    MenuCommand popup_commands[10];
    int popup_count;
    bool needs_redraw;
    // Pointer/hover tracking (content-space coordinates).
    int mouse_x;
    int mouse_y;
    bool have_mouse;
    // Last WM-side scroll offsets, used to rebase the pointer position when
    // the view scrolls under a stationary mouse.
    int last_scroll_x;
    int last_scroll_y;
    // Signature of the region under the pointer; moves that keep it unchanged
    // do not repaint (hover washes are position-derived, not stateful).
    int hover_region;
    int selected_volume_row;
    char window_title[96];
    bool show_sidebar;
    bool icon_view;
    Vec<ThumbCache> thumbs;
};
} // namespace

struct LayoutCache
{
    Rect place_rects[MAX_PLACES];
    Rect volume_rects[MAX_VOLUMES];
    Vec<Rect> row_rects;
    Rect view_switch;
};

static bool rect_contains(Rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

// Human-readable size for the list-view Size column.
static void files_format_size(uint64_t bytes, char *out, size_t out_size)
{
    if (bytes < 1024ull) {
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1048576ull) {
        uint64_t kb10 = (bytes * 10ull) >> 10;
        snprintf(out, out_size, "%llu.%llu KB", (unsigned long long)(kb10 / 10ull), (unsigned long long)(kb10 % 10ull));
    } else if (bytes < 1073741824ull) {
        uint64_t mb10 = (bytes * 10ull) >> 20;
        snprintf(out, out_size, "%llu.%llu MB", (unsigned long long)(mb10 / 10ull), (unsigned long long)(mb10 % 10ull));
    } else {
        uint64_t gb10 = (bytes * 10ull) >> 30;
        snprintf(out, out_size, "%llu.%llu GB", (unsigned long long)(gb10 / 10ull), (unsigned long long)(gb10 % 10ull));
    }
}

// Breadcrumb label for the header toolbar (e.g. "Home > Documents").
static void files_breadcrumb(const AppState *state, char *out, size_t out_size)
{
    if (!state || !out || out_size == 0)
        return;
    if (state->volume_home) {
        snprintf(out, out_size, "Storage");
        return;
    }
    const char *path = state->current_path;
    char root_label[96];
    const char *rest = nullptr;
    root_label[0] = '\0';
    if (strncmp(path, "/data", 5) == 0 && (path[5] == '\0' || path[5] == '/')) {
        snprintf(root_label, sizeof(root_label), "Home");
        rest = path + 5;
    } else {
        for (int i = 0; i < state->volume_count; i++) {
            const VolumeInfo &v = state->volumes[i];
            size_t len = strlen(v.mount_path);
            if (len > 0 && strncmp(path, v.mount_path, len) == 0 && (path[len] == '\0' || path[len] == '/')) {
                snprintf(root_label, sizeof(root_label), "%s", v.display_name[0] ? v.display_name : v.mount_path);
                rest = path + len;
                break;
            }
        }
    }
    if (!root_label[0]) {
        snprintf(out, out_size, "%s", path);
        return;
    }
    if (!rest || !*rest || strcmp(rest, "/") == 0) {
        snprintf(out, out_size, "%s", root_label);
        return;
    }
    if (*rest == '/')
        rest++;
    char segments[192];
    size_t seg_len = 0;
    segments[0] = '\0';
    const char *p = rest;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n > 0) {
            if (seg_len > 0 && seg_len + 3 < sizeof(segments)) {
                memcpy(segments + seg_len, " > ", 3);
                seg_len += 3;
            }
            if (seg_len + n < sizeof(segments)) {
                memcpy(segments + seg_len, p, n);
                seg_len += n;
            }
        }
        segments[seg_len] = '\0';
        if (!slash)
            break;
        p = slash + 1;
    }
    snprintf(out, out_size, "%s > %s", root_label, segments);
}

static void set_status(AppState *state, const char *msg)
{
    if (!state)
        return;
    strncpy(state->status, msg ? msg : "", sizeof(state->status) - 1);
    state->status[sizeof(state->status) - 1] = '\0';
}

static int row_count_of(const AppState *state)
{
    return state ? (int)state->rows.size() : 0;
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

// Window title for the browsed location. At a storage root the raw last path
// segment is a device/internal name ("data", "ata0p1"), so the root's friendly
// label (Home / the volume name) is used instead; real folders below a root keep
// their own name.
static void files_location_title(const AppState *state, char *out, size_t out_size)
{
    if (!state || !out || out_size == 0)
        return;
    if (state->volume_home) {
        snprintf(out, out_size, "Storage");
        return;
    }
    const char *path = state->current_path;
    if (strncmp(path, "/data", 5) == 0 && (path[5] == '\0' || path[5] == '/')) {
        if (path[5] == '\0') {
            snprintf(out, out_size, "Home");
            return;
        }
    } else {
        for (int i = 0; i < state->volume_count; i++) {
            const VolumeInfo &v = state->volumes[i];
            size_t len = strlen(v.mount_path);
            if (len > 0 && strncmp(path, v.mount_path, len) == 0 && (path[len] == '\0' || path[len] == '/')) {
                if (path[len] == '\0') {
                    snprintf(out, out_size, "%s", v.display_name[0] ? v.display_name : v.mount_path);
                    return;
                }
                break;
            }
        }
    }
    const char *label = state->load_failed ? state->current_path : path_basename(path);
    if (!label || !label[0] || strcmp(label, "/") == 0)
        snprintf(out, out_size, "Files");
    else
        snprintf(out, out_size, "%s", label);
}

static bool suffix_match_icase(const char *name, const char *suffix)
{
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (name_len < suffix_len)
        return false;
    const char *p = name + name_len - suffix_len;
    for (size_t i = 0; i < suffix_len; i++) {
        char a = p[i];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (a != suffix[i])
            return false;
    }
    return true;
}

static bool name_is_image(const char *name)
{
    if (!name)
        return false;
    return suffix_match_icase(name, ".png") || suffix_match_icase(name, ".jpg") || suffix_match_icase(name, ".jpeg") ||
           suffix_match_icase(name, ".bmp") || suffix_match_icase(name, ".gif") || suffix_match_icase(name, ".qoi");
}

static bool name_is_audio(const char *name)
{
    if (!name)
        return false;
    return suffix_match_icase(name, ".wav");
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
    int out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

    // Put the staged temp file in place. rename() is the authoritative
    // step; on FAT32 it refuses to overwrite, so a failed rename is retried
    // after unlinking the destination. unlink() itself refuses directories,
    // and no stat() gates a mutation here, so there is no check/use race.
    // The stat below only picks an error message once every mutation failed.
    int placed = rename(tmp_path, dst);
    if (placed != 0 && unlink(dst) == 0)
        placed = rename(tmp_path, dst);
    if (placed != 0) {
        unlink(tmp_path);
        VNodeStat st = {};
        if (stat(dst, &st) == 0 && st.is_dir)
            snprintf(error, error_size, "destination is a directory");
        else
            snprintf(error, error_size, "rename failed for %s", dst);
        return false;
    }
    return true;
}

static bool path_is_within(const char *child, const char *parent)
{
    if (!child || !parent || !parent[0])
        return false;
    size_t len = strlen(parent);
    if (strncmp(child, parent, len) != 0)
        return false;
    return child[len] == '\0' || child[len] == '/';
}

// Userspace stacks are 32 KB and each recursion level needs ~1.4 KB of
// path/stat buffers, so keep the tree depth bounded well below that.
static constexpr int MAX_TREE_DEPTH = 12;

static bool delete_directory_tree(const char *path, int depth, char *error, size_t error_size);

static bool copy_directory_tree(const char *src, const char *dst, int depth, char *error, size_t error_size)
{
    if (!src || !dst || depth > MAX_TREE_DEPTH) {
        snprintf(error, error_size, "invalid or too deep copy");
        return false;
    }
    if (path_is_within(dst, src)) {
        snprintf(error, error_size, "destination is inside source");
        return false;
    }

    // Open src before stat-ing it: a stat() -> open() sequence on the same
    // path is a TOCTOU race, so the fd is opened first and its type is
    // verified afterwards (open() succeeds for directories and files alike).
    int fd = open(src, O_RDONLY);
    if (fd < 0) {
        snprintf(error, error_size, "open failed for %s", src);
        return false;
    }
    VNodeStat st = {};
    if (stat(src, &st) != 0 || !st.is_dir) {
        close(fd);
        snprintf(error, error_size, "source is not a directory");
        return false;
    }
    VNodeStat dst_st = {};
    if (stat(dst, &dst_st) == 0) {
        close(fd);
        snprintf(error, error_size, "destination already exists");
        return false;
    }
    if (mkdir(dst) != 0) {
        close(fd);
        snprintf(error, error_size, "mkdir failed for %s", dst);
        return false;
    }

    bool ok = true;
    while (true) {
        char name[256] = {};
        if (syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) != 0)
            break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        char child_src[512];
        char child_dst[512];
        join_path(src, name, child_src, sizeof(child_src));
        join_path(dst, name, child_dst, sizeof(child_dst));

        VNodeStat child_st = {};
        if (stat(child_src, &child_st) != 0) {
            snprintf(error, error_size, "stat failed for %s", child_src);
            ok = false;
            break;
        }
        if (child_st.is_dir) {
            if (!copy_directory_tree(child_src, child_dst, depth + 1, error, error_size)) {
                ok = false;
                break;
            }
        } else if (!copy_file_stream(child_src, child_dst, error, error_size)) {
            ok = false;
            break;
        }
    }
    close(fd);

    if (!ok)
        delete_directory_tree(dst, depth, nullptr, 0); // best-effort cleanup
    return ok;
}

static bool delete_directory_tree(const char *path, int depth, char *error, size_t error_size)
{
    if (!path || depth > MAX_TREE_DEPTH) {
        if (error)
            snprintf(error, error_size, "invalid or too deep delete");
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (error)
            snprintf(error, error_size, "open failed for %s", path);
        return false;
    }

    bool ok = true;
    while (true) {
        char name[256] = {};
        if (syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) != 0)
            break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        char child[512];
        join_path(path, name, child, sizeof(child));
        VNodeStat child_st = {};
        if (stat(child, &child_st) == 0 && child_st.is_dir) {
            if (!delete_directory_tree(child, depth + 1, error, error_size)) {
                ok = false;
                break;
            }
        } else if (unlink(child) != 0) {
            if (error)
                snprintf(error, error_size, "unlink failed for %s", child);
            ok = false;
            break;
        }
    }
    close(fd);

    if (!ok)
        return false;
    if (rmdir(path) != 0) {
        if (error)
            snprintf(error, error_size, "rmdir failed for %s", path);
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
        // Cross-volume: fall back to recursive copy + delete of the source.
        if (!copy_directory_tree(row->path, dst, 0, error, error_size))
            return false;
        if (!delete_directory_tree(row->path, 0, nullptr, 0)) {
            snprintf(error, error_size, "source cleanup failed");
            return false;
        }
        return true;
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

    if (state->menu_kind == MENU_ENTRY && state->menu_target_row >= 0 && state->menu_target_row < row_count_of(state)) {
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
    widget_popup_close(&state->popup);
    state->needs_redraw = true;
}

static void refresh_volumes(AppState *state)
{
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
}

static void clear_thumbs(AppState *state)
{
    if (!state)
        return;
    for (size_t i = 0; i < state->thumbs.size(); i++)
        media_image_free(&state->thumbs[i].img);
    state->thumbs.clear();
}

// Sort entries into display order: directories first, then files, each group
// by name. Heapsort (worst-case O(n log n), no recursion) — directory order
// from GETDENTS is creation order and would degrade quicksort.
static bool files_row_before(const FileRow &a, const FileRow &b)
{
    if (a.is_dir != b.is_dir)
        return a.is_dir;
    return strcmp(a.name, b.name) < 0;
}

static void files_rows_sift_down(Vec<FileRow> &rows, int start, int end)
{
    int root = start;
    while (root * 2 + 1 <= end) {
        int child = root * 2 + 1;
        if (child + 1 <= end && files_row_before(rows[root * 2 + 1], rows[child]))
            child++;
        if (files_row_before(rows[root], rows[child]))
            return;
        FileRow tmp = rows[root];
        rows[root] = rows[child];
        rows[child] = tmp;
        root = child;
    }
}

static void files_rows_sort(Vec<FileRow> &rows)
{
    int count = (int)rows.size();
    if (count < 2)
        return;
    for (int start = count / 2 - 1; start >= 0; start--)
        files_rows_sift_down(rows, start, count - 1);
    for (int end = count - 1; end > 0; end--) {
        FileRow tmp = rows[end];
        rows[end] = rows[0];
        rows[0] = tmp;
        files_rows_sift_down(rows, 0, end - 1);
    }
}

static void load_directory(AppState *state)
{
    if (!state)
        return;

    char selected_path[sizeof(state->current_path)] = {};
    if (state->selected_row >= 0 && state->selected_row < row_count_of(state)) {
        strncpy(selected_path, state->rows[state->selected_row].path, sizeof(selected_path) - 1);
        selected_path[sizeof(selected_path) - 1] = '\0';
    }

    state->rows.clear();
    state->selected_row = -1;
    state->load_failed = false;
    clear_thumbs(state);
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

    while (row_count_of(state) < MAX_LIST_ROWS) {
        char name[256] = {};
        if (syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) != 0)
            break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        FileRow row = {};
        strncpy(row.name, name, sizeof(row.name) - 1);
        join_path(state->current_path, name, row.path, sizeof(row.path));
        VNodeStat st = {};
        if (stat(row.path, &st) == 0) {
            row.is_dir = st.is_dir;
            row.size = st.size;
        }
        if (selected_path[0] && strcmp(row.path, selected_path) == 0)
            state->selected_row = row_count_of(state);
        if (!state->rows.push(row))
            break;
    }
    close(fd);

    // Deterministic display order (directories, then files, by name) and the
    // selection restored against the sorted rows.
    files_rows_sort(state->rows);
    state->selected_row = -1;
    if (selected_path[0]) {
        for (size_t i = 0; i < state->rows.size(); i++) {
            if (strcmp(state->rows[i].path, selected_path) == 0) {
                state->selected_row = (int)i;
                break;
            }
        }
    }

    // Keep one thumbnail slot per row so refresh_thumb can index by row. If the
    // table cannot be grown, drop the rows it would not cover so row indexing
    // stays in bounds.
    if (!state->thumbs.resize(state->rows.size())) {
        while (state->rows.size() > state->thumbs.size())
            state->rows.pop();
    }
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
        state->rows.clear();
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
    bool is_help = mode == DIALOG_HELP;
    widget_dialog_open(&state->dialog, !is_help, !is_help, prefill);
    state->needs_redraw = true;
}

static void close_dialog(AppState *state)
{
    state->dialog_mode = DIALOG_NONE;
    widget_dialog_close(&state->dialog);
    state->needs_redraw = true;
}

static bool confirm_dialog(AppState *state)
{
    if (!state || state->dialog_mode == DIALOG_NONE)
        return false;

    if (state->dialog_mode == DIALOG_HELP) {
        close_dialog(state);
        return true;
    }

    char input[256] = {};
    trim_ascii_whitespace(widget_dialog_input(&state->dialog), input, sizeof(input));
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

    if (state->selected_row < 0 || state->selected_row >= row_count_of(state)) {
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
            if (copy_directory_tree(row.path, dst, 0, err, sizeof(err))) {
                set_status(state, "Copy complete");
                load_directory(state);
            } else {
                set_status(state, err);
            }
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
    int y = layout->body_rect.y;
    // row_rects was cleared by the caller and is only resized on the icon/list
    // paths; size it here too so the indexed writes below stay in bounds.
    int needed = (state->storage_mode == STORAGE_MODE_OFF || volume_rows == 0) ? 1 : volume_rows;
    if (!cache->row_rects.resize(needed))
        return;
    if (state->storage_mode == STORAGE_MODE_OFF || volume_rows == 0) {
        cache->row_rects[0] = gui_rect_make(layout->body_rect.x, y, layout->body_rect.w, gui_app_row_h());
        gui_app_draw_list_row(
            win, cache->row_rects[0].x, cache->row_rects[0].y, cache->row_rects[0].w, cache->row_rects[0].h,
            GUI_GLYPH_INFO, state->storage_mode == STORAGE_MODE_OFF ? "Storage is off" : "No storage volumes available",
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
        bool hovered = state->have_mouse && state->menu_kind == MENU_NONE &&
                       rect_contains(cache->row_rects[visible], state->mouse_x, state->mouse_y);
        gui_app_draw_list_row(win, cache->row_rects[visible].x, cache->row_rects[visible].y,
                              cache->row_rects[visible].w, cache->row_rects[visible].h, GUI_GLYPH_DRIVE,
                              state->volumes[i].display_name[0] ? state->volumes[i].display_name
                                                                : state->volumes[i].mount_path,
                              state->volumes[i].mount_path, visible == state->selected_volume_row, hovered, false);
        y += gui_app_row_h() + gui_app_row_gap();
    }
}

static void draw_dialog(Surface *win, AppState *state, LayoutCache *cache)
{
    (void)cache;
    if (state->dialog_mode == DIALOG_NONE)
        return;

    static const char *tips[] = {
        "Click a folder to select it, Enter opens it.",     "Backspace or Left arrow goes up one level.",
        "Right-click items to copy, move, rename, delete.", "Edit > Cut/Copy then Paste moves items between folders.",
        "Storage home lists every mounted volume.",
    };

    int scroll_y = (g_my_window) ? g_my_window->scroll_y : 0;
    // Layout against the visible viewport, not the (grown) content canvas:
    // with a scrolled list the canvas is much taller than the window and the
    // dialog would center far below the fold.
    int view_w = (g_my_window && g_my_window->w > 0) ? (int)g_my_window->w : (int)win->width;
    int view_h = (g_my_window && g_my_window->h > 0) ? (int)g_my_window->h : (int)win->height;
    bool is_help = state->dialog_mode == DIALOG_HELP;

    widget_dialog_draw(win, &state->dialog, view_w, view_h, scroll_y, state->dialog_title, is_help ? tips : nullptr,
                       is_help ? (int)(sizeof(tips) / sizeof(tips[0])) : 0, is_help ? "Close" : "OK",
                       is_help ? nullptr : "Cancel");
}

static void draw_menu(Surface *win, AppState *state, LayoutCache *cache)
{
    (void)cache;
    if (!state || state->menu_kind == MENU_NONE)
        return;
    widget_popup_draw(win, &state->popup);
}

static inline int files_icon_cell_w()
{
    return gui_scaled_metric(84);
}

static inline int files_icon_cell_h()
{
    return gui_scaled_metric(90);
}

static inline int files_icon_size()
{
    return gui_scaled_metric(FILES_ICON_SIZE_PX);
}

static inline int files_sidebar_w()
{
    return gui_scaled_metric(200);
}

static inline int files_nav_pitch()
{
    return gui_app_nav_h() + gui_app_row_gap();
}

static inline int files_icon_grid_gap()
{
    return gui_scaled_metric(16);
}

// Column count for the icon grid: cells plus the gaps between them must fit,
// so the last column is never cropped at the right edge. Shared by the content
// height computation and the draw path so they can never diverge.
static int files_icon_grid_cols(int main_w)
{
    int cell_w = files_icon_cell_w();
    int gap = files_icon_grid_gap();
    int usable_w = main_w - gui_scaled_metric(32);
    if (cell_w <= 0 || usable_w <= 0)
        return 1;
    int cols = (usable_w + gap) / (cell_w + gap);
    return cols < 1 ? 1 : cols;
}

// Shape-based fallback icon: folder = tab + body, file = page with text
// lines. Used only when the per-type .uoic thumbnail assets fail to load.
static void draw_shape_icon(Surface *win, int x, int y, int size, bool is_dir, uint32_t fg, uint32_t detail)
{
    if (!win || size <= 0)
        return;
    int r = size / 8;
    if (r < 2)
        r = 2;
    if (is_dir) {
        int tab_w = size * 42 / 100;
        int tab_h = size * 18 / 100;
        gui_fill_rounded_rect(win, x, y, tab_w, tab_h + r, r, fg);
        gui_fill_rounded_rect(win, x, y + tab_h, size, size - tab_h, r, fg);
    } else {
        int page_w = size * 76 / 100;
        int page_x = x + (size - page_w) / 2;
        gui_fill_rounded_rect(win, page_x, y, page_w, size, r, fg);
        int line_w = page_w * 56 / 100;
        int line_x = page_x + (page_w - line_w) / 2;
        int line_h = size / 16;
        if (line_h < 2)
            line_h = 2;
        int gap_v = size / 5;
        int line_y = y + size * 32 / 100;
        for (int i = 0; i < 3 && line_y + line_h <= y + size - r; i++) {
            gui_fill_rect(win, line_x, line_y, line_w, line_h, detail);
            line_y += gap_v;
        }
    }
}

enum
{
    FILES_TYPE_ICON_FOLDER = 0,
    FILES_TYPE_ICON_FILE = 1,
    FILES_TYPE_ICON_IMAGE = 2,
    FILES_TYPE_ICON_COUNT = 3
};

static Surface g_type_icons[FILES_TYPE_ICON_COUNT] = {};
static bool g_type_icon_attempted[FILES_TYPE_ICON_COUNT] = {};

// Per-type thumbnail assets (folder / generic file / image file), loaded once
// at icon-view size. Falls back to draw_shape_icon when an asset is missing.
static const Surface *files_type_icon(int kind)
{
    if (kind < 0 || kind >= FILES_TYPE_ICON_COUNT)
        return nullptr;
    if (!g_type_icon_attempted[kind]) {
        g_type_icon_attempted[kind] = true;
        static const char *const names[FILES_TYPE_ICON_COUNT] = {"folder", "file", "file-image"};
        char path[64];
        snprintf(path, sizeof(path), "/usr/share/appicons/%s.uoic", names[kind]);
        if (!gui_load_uoic(path, FILES_ICON_SIZE_PX, (uint32_t)gui_ui_scale_pct(), &g_type_icons[kind]))
            memset(&g_type_icons[kind], 0, sizeof(g_type_icons[kind]));
    }
    return g_type_icons[kind].buffer ? &g_type_icons[kind] : nullptr;
}

static inline uint32_t type_icon_blend_channel(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, uint32_t fx,
                                               uint32_t fy)
{
    uint32_t inv_fx = 256u - fx;
    uint32_t inv_fy = 256u - fy;
    uint32_t top = c00 * inv_fx + c10 * fx;
    uint32_t bot = c01 * inv_fx + c11 * fx;
    return (top * inv_fy + bot * fy + 32768u) >> 16;
}

// Bilinear-scaled blit of a premultiplied-alpha UOIC surface onto the opaque
// window canvas. The loader picks the smallest frame covering the scaled size,
// which is not necessarily an exact match, so the draw scales to icon_size.
static void draw_type_icon(Surface *win, const Surface *icon, int x, int y, int size)
{
    if (!win || !win->buffer || !icon || !icon->buffer || size <= 0 || icon->width == 0 || icon->height == 0)
        return;

    int clip_left = x < 0 ? 0 : x;
    int clip_top = y < 0 ? 0 : y;
    int clip_right = x + size > (int)win->width ? (int)win->width : x + size;
    int clip_bottom = y + size > (int)win->height ? (int)win->height : y + size;
    if (clip_left >= clip_right || clip_top >= clip_bottom)
        return;

    uint32_t dst_stride = win->pitch / 4u;
    uint32_t src_stride = icon->pitch / 4u;
    uint32_t src_w = icon->width;
    uint32_t src_h = icon->height;

    for (int py = clip_top; py < clip_bottom; py++) {
        int local_y = py - y;
        uint64_t src_y_fp = ((uint64_t)local_y * (uint64_t)src_h * 65536u) / (uint32_t)size;
        uint32_t sy0 = (uint32_t)(src_y_fp >> 16);
        uint32_t frac_y = ((uint32_t)src_y_fp >> 8) & 0xFFu;
        uint32_t sy1 = sy0 + 1u < src_h ? sy0 + 1u : sy0;

        uint32_t *dst_row = &win->buffer[(size_t)py * dst_stride];
        const uint32_t *src_row0 = &icon->buffer[(size_t)sy0 * src_stride];
        const uint32_t *src_row1 = &icon->buffer[(size_t)sy1 * src_stride];

        for (int px = clip_left; px < clip_right; px++) {
            int local_x = px - x;
            uint64_t src_x_fp = ((uint64_t)local_x * (uint64_t)src_w * 65536u) / (uint32_t)size;
            uint32_t sx0 = (uint32_t)(src_x_fp >> 16);
            uint32_t frac_x = ((uint32_t)src_x_fp >> 8) & 0xFFu;
            uint32_t sx1 = sx0 + 1u < src_w ? sx0 + 1u : sx0;

            uint32_t p00 = src_row0[sx0];
            uint32_t p10 = src_row0[sx1];
            uint32_t p01 = src_row1[sx0];
            uint32_t p11 = src_row1[sx1];

            uint32_t a = type_icon_blend_channel(p00 >> 24, p10 >> 24, p01 >> 24, p11 >> 24, frac_x, frac_y);
            if (a == 0)
                continue;
            uint32_t r = type_icon_blend_channel((p00 >> 16) & 0xFFu, (p10 >> 16) & 0xFFu, (p01 >> 16) & 0xFFu,
                                                 (p11 >> 16) & 0xFFu, frac_x, frac_y);
            uint32_t g = type_icon_blend_channel((p00 >> 8) & 0xFFu, (p10 >> 8) & 0xFFu, (p01 >> 8) & 0xFFu,
                                                 (p11 >> 8) & 0xFFu, frac_x, frac_y);
            uint32_t b = type_icon_blend_channel(p00 & 0xFFu, p10 & 0xFFu, p01 & 0xFFu, p11 & 0xFFu, frac_x, frac_y);
            if (a > 255u)
                a = 255u;

            uint32_t &dst = dst_row[px];
            if (a == 255u) {
                dst = 0xFF000000u | (r << 16) | (g << 8) | b;
            } else {
                uint32_t ia = 255u - a;
                uint32_t dr = (((dst >> 16) & 0xFFu) * ia + 127u) / 255u;
                uint32_t dg = (((dst >> 8) & 0xFFu) * ia + 127u) / 255u;
                uint32_t db = ((dst & 0xFFu) * ia + 127u) / 255u;
                dst = 0xFF000000u | ((r + dr) << 16) | ((g + dg) << 8) | (b + db);
            }
        }
    }
}

static constexpr uint64_t THUMB_MAX_FILE_BYTES = 8ull * 1024 * 1024;

static void blend_thumb_onto(Surface *win, int x0, int y0, const media_image *img)
{
    if (!img->pixels)
        return;
    uint32_t stride = win->pitch / 4;
    for (int y = 0; y < img->height; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= static_cast<int>(win->height))
            continue;
        const uint32_t *src = img->pixels + static_cast<uint64_t>(y) * img->width;
        uint32_t *dst = win->buffer + static_cast<uint64_t>(dy) * stride;
        for (int x = 0; x < img->width; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= static_cast<int>(win->width))
                continue;
            uint32_t p = src[x];
            uint32_t a = p >> 24;
            if (a == 255) {
                dst[dx] = p;
                continue;
            }
            if (a == 0)
                continue;
            uint32_t d = dst[dx];
            uint32_t ia = 255 - a;
            uint32_t r = (((p >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia + 127) / 255;
            uint32_t g = (((p >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia + 127) / 255;
            uint32_t b = ((p & 0xFF) * a + (d & 0xFF) * ia + 127) / 255;
            dst[dx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

// Decode a directory entry into a cached icon-sized thumbnail. Called lazily
// from the draw path so only visible cells pay the decode cost, and only once
// per entry per directory load.
static void refresh_thumb(AppState *state, int index)
{
    ThumbCache *thumb = &state->thumbs[index];
    media_image_free(&thumb->img);
    thumb->tried = true;
    strncpy(thumb->path, state->rows[index].path, sizeof(thumb->path) - 1);
    thumb->path[sizeof(thumb->path) - 1] = '\0';

    const FileRow &row = state->rows[index];
    if (row.size == 0 || row.size > THUMB_MAX_FILE_BYTES)
        return;
    uint8_t *data = nullptr;
    uint32_t size = 0;
    if (!gui_load_file(row.path, &data, &size))
        return;
    media_image full = {};
    bool ok = media_image_decode(data, size, &full);
    free(data);
    if (!ok)
        return;

    int box = gui_scaled_metric(80);
    int tw = 1, th = 1;
    if (full.width > 0 && full.height > 0) {
        if (full.width >= full.height) {
            tw = box;
            th = static_cast<int>(static_cast<uint64_t>(full.height) * box / full.width);
        } else {
            th = box;
            tw = static_cast<int>(static_cast<uint64_t>(full.width) * box / full.height);
        }
    }
    if (tw < 1)
        tw = 1;
    if (th < 1)
        th = 1;
    if (!media_image_scale(&full, &thumb->img, tw, th))
        media_image_free(&thumb->img);
    media_image_free(&full);
}

static void draw_file_icon_cell(Surface *win, AppState *state, const Rect &cell, int index)
{
    if (!win || !state || index < 0 || index >= row_count_of(state))
        return;
    const FileRow &row = state->rows[index];
    bool selected = index == state->selected_row;
    bool hovered = state->have_mouse && state->menu_kind == MENU_NONE && state->dialog_mode == DIALOG_NONE &&
                   rect_contains(cell, state->mouse_x, state->mouse_y);

    int icon_size = files_icon_size();
    int icon_x = cell.x + (cell.w - icon_size) / 2;
    int icon_y = cell.y + gui_space_2();

    bool drew_thumb = false;
    if (!row.is_dir && name_is_image(row.name)) {
        ThumbCache *thumb = &state->thumbs[index];
        bool cell_visible = true;
        if (g_my_window) {
            // Visibility against the viewport (window height), not the grown
            // content canvas — otherwise every cell counts as visible and the
            // whole directory is decoded eagerly.
            int view_top = g_my_window->scroll_y;
            int view_h = g_my_window->h > 0 ? (int)g_my_window->h : (int)win->height;
            int view_bottom = view_top + view_h;
            cell_visible = cell.y + cell.h > view_top && cell.y < view_bottom;
        }
        if ((!thumb->tried || strcmp(thumb->path, row.path) != 0) && cell_visible)
            refresh_thumb(state, index);
        if (thumb->img.pixels) {
            int pad = gui_scaled_metric(2);
            gui_fill_rounded_rect(win, icon_x - pad, icon_y - pad, icon_size + pad * 2, icon_size + pad * 2,
                                  gui_radius_xs(), g_gui_style.app_surface_alt);
            gui_draw_rounded_rect(win, icon_x - pad, icon_y - pad, icon_size + pad * 2, icon_size + pad * 2,
                                  gui_radius_xs(), g_gui_style.border);
            int tx = icon_x + (icon_size - thumb->img.width) / 2;
            int ty = icon_y + (icon_size - thumb->img.height) / 2;
            blend_thumb_onto(win, tx, ty, &thumb->img);
            drew_thumb = true;
        }
    }
    if (!drew_thumb) {
        int kind = row.is_dir ? FILES_TYPE_ICON_FOLDER
                              : (name_is_image(row.name) ? FILES_TYPE_ICON_IMAGE : FILES_TYPE_ICON_FILE);
        const Surface *type_icon = files_type_icon(kind);
        if (type_icon) {
            draw_type_icon(win, type_icon, icon_x, icon_y, icon_size);
        } else {
            uint32_t icon_color = row.is_dir ? g_gui_style.accent : g_gui_style.text_dim;
            draw_shape_icon(win, icon_x, icon_y, icon_size, row.is_dir, icon_color, g_gui_style.app_surface);
        }
    }

    int label_y = icon_y + icon_size + gui_space_1();
    int max_label_w = cell.w - gui_space_2();
    int label_w = gui_measure_text(gui_font_default(), row.name);
    int label_x = cell.x + (cell.w - label_w) / 2;
    if (label_w > max_label_w) {
        label_x = cell.x + gui_space_1();
        label_w = max_label_w;
    }
    // Selected and hovered items highlight the label only — a capsule behind
    // the name, never a wash over the whole cell. Hover uses the neutral wash;
    // selection the accent tint, so both states share one shape.
    if (selected || hovered) {
        int pad = gui_scaled_metric(3);
        int capsule_h = gui_line_height() + gui_scaled_metric(2);
        int capsule_y = label_y - gui_scaled_metric(1);
        uint32_t capsule_color = selected ? g_gui_style.accent_soft : gui_hover_wash_color();
        gui_fill_rounded_rect(win, label_x - pad, capsule_y, label_w + pad * 2, capsule_h, capsule_h / 2,
                              capsule_color);
    }
    gui_draw_text_clipped(win, gui_font_default(), label_x, label_y, label_w, row.name, g_gui_style.text,
                          selected ? g_gui_style.accent_soft : 0);
}

// Table-style list row: icon + name (flex), then right-aligned Type (100px) and
// Size (80px) columns. On narrow rows the meta columns drop out one at a time
// (size first, then type) so the name never collides with them. Selection and
// hover are tonal washes, not framed boxes.
static void files_draw_table_row(Surface *win, const Rect *r, const FileRow *row, bool selected, bool hovered)
{
    if (!win || !r || !row || r->w <= 0 || r->h <= 0)
        return;
    const int pad = gui_space_1_5();
    int rad = gui_radius_sm();
    uint32_t wash = selected ? g_gui_style.accent_soft : (hovered ? gui_hover_wash_color() : 0);
    if (wash)
        gui_fill_rounded_rect(win, r->x, r->y, r->w, r->h, rad, wash);

    int text_y = gui_align_text_y(gui_font_default(), r->y, r->h);
    uint32_t meta_fg = g_gui_style.text_muted;
    int right_pad = gui_space_1_5();
    int icon_size = gui_glyph_std_size();
    int icon_x = r->x + pad;
    int name_x = icon_x + icon_size + gui_space_1();
    // The name keeps at least this much room before a meta column may render.
    int name_min_end = name_x + gui_scaled_metric(100);

    char size_buf[32];
    if (row->is_dir)
        size_buf[0] = '\0';
    else
        files_format_size(row->size, size_buf, sizeof(size_buf));

    int size_col_w = gui_scaled_metric(80);
    int type_col_w = gui_scaled_metric(100);
    int size_x = r->x + r->w - right_pad - size_col_w;
    bool show_size = size_buf[0] && size_x >= name_min_end;
    int type_x = show_size ? size_x - gui_space_1() - type_col_w : r->x + r->w - right_pad - type_col_w;
    bool show_type = type_x >= name_min_end;

    if (show_type) {
        const char *type_label =
            row->is_dir ? "Directory"
                        : (name_is_image(row->name) ? "Image" : (name_is_audio(row->name) ? "Audio" : "File"));
        int type_w = gui_measure_text(gui_font_default(), type_label);
        int type_align_x = type_x + type_col_w - type_w;
        if (type_align_x < type_x)
            type_align_x = type_x;
        gui_draw_text_clipped(win, gui_font_default(), type_align_x, text_y, type_col_w, type_label, meta_fg, 0);
    }

    if (show_size) {
        int size_w = gui_measure_text(gui_font_default(), size_buf);
        int size_align_x = size_x + size_col_w - size_w;
        if (size_align_x < size_x)
            size_align_x = size_x;
        gui_draw_text_clipped(win, gui_font_default(), size_align_x, text_y, size_col_w, size_buf, meta_fg, 0);
    }

    GuiGlyphKind glyph =
        row->is_dir ? GUI_GLYPH_FOLDER
                    : (name_is_image(row->name) ? GUI_GLYPH_FILE_IMAGE
                                                : (name_is_audio(row->name) ? GUI_GLYPH_FILE : GUI_GLYPH_FILE_TEXT));
    gui_draw_glyph(win, icon_x, r->y + (r->h - icon_size) / 2, icon_size, glyph,
                   selected ? g_gui_style.accent : g_gui_style.text_dim);

    int name_end = show_size ? size_x : (show_type ? type_x : r->x + r->w - right_pad);
    int name_w = name_end - gui_space_1() - name_x;
    if (name_w < 0)
        name_w = 0;
    gui_draw_text_clipped(win, gui_font_default(), name_x, text_y, name_w, row->name, g_gui_style.text, 0);
}

static int compute_files_content_height(AppState *state, int content_w)
{
    int sidebar_h = 0;
    if (state->show_sidebar) {
        // Headerbar deadzone (traffic lights) + pad + PLACES caption + gap.
        sidebar_h = gui_headerbar_h() + gui_space_1() + gui_line_height() + gui_scaled_metric(4);
        int data_index = find_data_volume_index(state);
        if (data_index >= 0 && state->storage_mode != STORAGE_MODE_OFF)
            sidebar_h += MAX_PLACES * files_nav_pitch() + gui_space_1();
        else
            sidebar_h += files_nav_pitch() + gui_space_1();
        sidebar_h += gui_line_height() + gui_scaled_metric(4); // STORAGE caption
        int visible_vols = 0;
        for (int i = 0; i < state->volume_count; i++) {
            if (is_visible_volume(state->volumes[i]))
                visible_vols++;
        }
        sidebar_h += visible_vols * files_nav_pitch() + gui_space_1();
    }

    int sidebar_w = state->show_sidebar ? files_sidebar_w() : 0;
    int main_w = content_w - sidebar_w;

    int main_h = gui_space_1();
    if (state->volume_home) {
        int vols = visible_volume_count(state);
        main_h += (vols > 0 ? vols : 1) * (gui_app_row_h() + gui_app_row_gap());
    } else if (state->load_failed) {
        main_h += gui_app_row_h();
    } else if (state->icon_view) {
        int cols = files_icon_grid_cols(main_w);
        int rows = (row_count_of(state) + cols - 1) / cols;
        main_h +=
            rows * files_icon_cell_h() + (rows > 1 ? (rows - 1) * files_icon_grid_gap() : 0) + gui_scaled_metric(32);
    } else {
        main_h += row_count_of(state) * (gui_app_row_h() + gui_app_row_gap());
    }
    main_h += gui_space_1();

    return sidebar_h > main_h ? sidebar_h : main_h;
}

static void draw_files(App *app, Surface *win, AppState *state, LayoutCache *cache)
{
    memset(cache->place_rects, 0, sizeof(cache->place_rects));
    memset(cache->volume_rects, 0, sizeof(cache->volume_rects));
    cache->row_rects.clear();
    GuiAppLayout layout = gui_app_begin(win);
    int view_w = layout.outer_w + layout.outer_x * 2;
    int view_h = layout.outer_h + layout.outer_y + gui_app_outer_padding();

    int sidebar_w = state->show_sidebar ? files_sidebar_w() : 0;
    // Edge-to-edge split: the sidebar is flush against the window frame and the
    // content column fills the rest — no floating inner panels.
    int main_x = sidebar_w;
    int main_w = view_w - sidebar_w;
    if (main_w < 0)
        main_w = 0;
    // The sticky toolbar IS the unified headerbar band over the content column.
    int toolbar_h = gui_headerbar_h();

    int body_content_h = compute_files_content_height(state, main_w);
    int content_total = toolbar_h + body_content_h + gui_space_1();
    app_set_content_size(app, view_w, content_total);

    int scroll_y = (g_my_window) ? g_my_window->scroll_y : 0;
    int sticky_sidebar_y = scroll_y;

    // 1. Sidebar surface: full visible height, flush left, split from the
    // content column by a single hairline divider.
    if (state->show_sidebar && sidebar_w > 0) {
        gui_fill_rect(win, 0, scroll_y, sidebar_w, view_h, g_gui_style.app_surface);
        gui_draw_separator_v(win, sidebar_w - 1, scroll_y, view_h, gui_hairline_color());
    }

    // 2. Draw scrolling content
    int list_y = toolbar_h + gui_space_0_5();
    if (state->volume_home) {
        GuiAppLayout home_layout = layout;
        home_layout.body_rect = gui_rect_make(main_x + gui_space_1(), list_y, main_w - gui_space_2(), body_content_h);
        draw_volume_home(win, &home_layout, state, cache);
    } else if (state->load_failed) {
        gui_app_draw_list_row(win, main_x + gui_space_1(), list_y, main_w - gui_space_2(), gui_app_row_h(),
                              GUI_GLYPH_WARNING, "Unable to open directory", state->current_path, false, false, true);
    } else if (state->icon_view) {
        int cell_w = files_icon_cell_w();
        int cell_h = files_icon_cell_h();
        int grid_gap = files_icon_grid_gap();
        int cols = files_icon_grid_cols(main_w);
        // Center the grid block inside the content column so the margins on
        // both sides are even instead of crowding the left edge.
        int grid_total_w = cols * cell_w + (cols - 1) * grid_gap;
        int grid_x = main_x + (main_w - grid_total_w) / 2;
        if (grid_x < main_x + gui_scaled_metric(16))
            grid_x = main_x + gui_scaled_metric(16);
        int icon_count = row_count_of(state);
        if (!cache->row_rects.resize(icon_count))
            icon_count = 0;
        for (int i = 0; i < icon_count; i++) {
            int col = i % cols;
            int grid_row = i / cols;
            cache->row_rects[i] = gui_rect_make(grid_x + col * (cell_w + grid_gap),
                                                list_y + grid_row * (cell_h + grid_gap), cell_w, cell_h);
            draw_file_icon_cell(win, state, cache->row_rects[i], i);
        }
    } else {
        int list_count = row_count_of(state);
        if (!cache->row_rects.resize(list_count))
            list_count = 0;
        for (int i = 0; i < list_count; i++) {
            cache->row_rects[i] =
                gui_rect_make(main_x + gui_space_1(), list_y + i * (gui_app_row_h() + gui_app_row_gap()),
                              main_w - gui_space_2(), gui_app_row_h());
            bool hovered = state->have_mouse && state->menu_kind == MENU_NONE && state->dialog_mode == DIALOG_NONE &&
                           rect_contains(cache->row_rects[i], state->mouse_x, state->mouse_y);
            files_draw_table_row(win, &cache->row_rects[i], &state->rows[i], i == state->selected_row, hovered);
        }
    }

    // 2b. Empty state: a loaded-but-empty directory gets a muted hint row
    // instead of blank space (failure/off states render elsewhere).
    if (row_count_of(state) == 0 && !state->volume_home && !state->load_failed &&
        state->storage_mode != STORAGE_MODE_OFF && state->current_path[0]) {
        Rect empty_rect = gui_rect_make(main_x + gui_space_1(), list_y, main_w - gui_space_2(), gui_app_row_h());
        gui_app_draw_list_row(win, empty_rect.x, empty_rect.y, empty_rect.w, empty_rect.h, GUI_GLYPH_FOLDER,
                              "Folder is empty", "No items in this location", false, false, true);
    }

    // 3. Sticky sidebar content (the surface itself is already painted).
    if (state->show_sidebar) {
        int pill_inset = gui_scaled_metric(8);
        int item_x = pill_inset;
        int item_w = sidebar_w - pill_inset * 2;
        int caption_x = gui_scaled_metric(10);
        int caption_w = sidebar_w - gui_scaled_metric(20);
        int sy = sticky_sidebar_y + gui_headerbar_h() + gui_space_1();
        gui_app_draw_section_caption(win, caption_x, sy, caption_w, "Places");
        sy += gui_line_height() + gui_scaled_metric(4);
        int data_index = find_data_volume_index(state);
        if (data_index >= 0 && state->storage_mode != STORAGE_MODE_OFF) {
            for (int i = 0; i < MAX_PLACES; i++) {
                cache->place_rects[i] = gui_rect_make(item_x, sy, item_w, gui_app_nav_h());
                bool active = !state->volume_home && strcmp(state->current_path, k_places[i].path) == 0;
                bool hovered = state->have_mouse && state->menu_kind == MENU_NONE &&
                               state->dialog_mode == DIALOG_NONE &&
                               rect_contains(cache->place_rects[i], state->mouse_x, state->mouse_y);
                gui_app_draw_nav_item(win, cache->place_rects[i].x, cache->place_rects[i].y, cache->place_rects[i].w,
                                      cache->place_rects[i].h, k_place_glyphs[i], k_places[i].label, active, hovered);
                sy += files_nav_pitch();
            }
            sy += gui_space_1();
        } else {
            cache->place_rects[0] = gui_rect_make(item_x, sy, item_w, gui_app_nav_h());
            gui_app_draw_nav_item(win, cache->place_rects[0].x, cache->place_rects[0].y, cache->place_rects[0].w,
                                  cache->place_rects[0].h, GUI_GLYPH_HOME, "Home", false, false);
            sy += files_nav_pitch() + gui_space_1();
        }

        gui_app_draw_section_caption(win, caption_x, sy, caption_w, "Storage");
        sy += gui_line_height() + gui_scaled_metric(4);
        int visible = 0;
        for (int i = 0; i < state->volume_count; i++) {
            if (!is_visible_volume(state->volumes[i]))
                continue;
            cache->volume_rects[visible] = gui_rect_make(item_x, sy, item_w, gui_app_nav_h());
            bool active = !state->volume_home && strcmp(state->current_path, state->volumes[i].mount_path) == 0;
            bool hovered = state->have_mouse && state->menu_kind == MENU_NONE && state->dialog_mode == DIALOG_NONE &&
                           rect_contains(cache->volume_rects[visible], state->mouse_x, state->mouse_y);
            gui_app_draw_nav_item(win, cache->volume_rects[visible].x, cache->volume_rects[visible].y,
                                  cache->volume_rects[visible].w, cache->volume_rects[visible].h, GUI_GLYPH_DRIVE,
                                  state->volumes[i].display_name[0] ? state->volumes[i].display_name
                                                                    : state->volumes[i].mount_path,
                                  active, hovered);
            sy += files_nav_pitch();
            visible++;
        }
    } // show_sidebar

    // 4. Sticky content toolbar: breadcrumb on the left, status + view switcher
    // on the right — one continuous band at the top of the content column.
    {
        gui_fill_rect(win, main_x, scroll_y, main_w, toolbar_h, g_gui_style.app_bg);

        // Without a sidebar the content column starts at the window's left edge
        // under the traffic lights; keep the path bar clear of them.
        int text_x = main_x > 0 ? main_x + gui_space_2() : gui_traffic_lights_w() + gui_space_1();

        int switch_w = gui_scaled_metric(150);
        int switch_h = gui_app_control_h();
        int switch_x = view_w - switch_w - gui_space_1_5();
        int switch_y = scroll_y + (toolbar_h - switch_h) / 2;
        cache->view_switch = gui_rect_make(switch_x, switch_y, switch_w, switch_h);

        // Transient status messages sit right-aligned beside the view switcher
        // and never replace the path bar.
        int status_reserved = 0;
        if (state->status[0]) {
            int status_w = gui_measure_text(gui_font_default(), state->status);
            status_reserved = status_w + gui_space_2();
            int status_y = gui_align_text_y(gui_font_default(), scroll_y, toolbar_h);
            gui_draw_text_clipped(win, gui_font_default(), switch_x - gui_space_1() - status_w, status_y, status_w,
                                  state->status, g_gui_style.text_muted, g_gui_style.app_bg);
        }

        // The path bar (breadcrumb) is always visible.
        char breadcrumb[256];
        files_breadcrumb(state, breadcrumb, sizeof(breadcrumb));
        int bc_y = gui_align_text_y(gui_font_title(), scroll_y, toolbar_h);
        int bc_w = switch_x - gui_space_1() - status_reserved - text_x;
        if (bc_w < 0)
            bc_w = 0;
        gui_draw_text_clipped(win, gui_font_title(), text_x, bc_y, bc_w, breadcrumb, g_gui_style.text,
                              g_gui_style.app_bg);

        const char *view_labels[2] = {"List", "Icons"};
        int switch_hover = -1;
        if (state->have_mouse && state->menu_kind == MENU_NONE && state->dialog_mode == DIALOG_NONE &&
            rect_contains(cache->view_switch, state->mouse_x, state->mouse_y)) {
            int seg_w = cache->view_switch.w / 2;
            switch_hover = (state->mouse_x - cache->view_switch.x) >= seg_w ? 1 : 0;
        }
        gui_app_draw_segmented_choice(win, switch_x, switch_y, switch_w, switch_h, view_labels, 2,
                                      state->icon_view ? 1 : 0, switch_hover);

        // The view switcher is an interactive headerbar control; exempt it from
        // the window-drag zone so it stays clickable.
        gui_window_set_header_input(&cache->view_switch, 1);
    }

    // Keep the titlebar in sync with the location being browsed.
    char location[96];
    files_location_title(state, location, sizeof(location));
    char title[96];
    if (strcmp(location, "Files") == 0)
        snprintf(title, sizeof(title), "Files");
    else
        snprintf(title, sizeof(title), "%s - Files", location);
    if (strcmp(title, state->window_title) != 0) {
        strncpy(state->window_title, title, sizeof(state->window_title) - 1);
        state->window_title[sizeof(state->window_title) - 1] = '\0';
        gui_set_window_title(state->window_title);
    }

    draw_menu(win, state, cache);
    draw_dialog(win, state, cache);
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
        state->rows.clear();
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

static void open_in_image_viewer(AppState *state, const FileRow *row)
{
    if (!state || !row)
        return;
    if (!gui_open_request_submit(row->path)) {
        set_status(state, "Open request failed");
        return;
    }
    int pid = fork();
    if (pid == 0) {
        exec("/bin/imageviewer.elf");
        exit(1);
    }
    if (pid < 0) {
        set_status(state, "Launch failed");
        return;
    }
    char msg[sizeof(state->status)];
    snprintf(msg, sizeof(msg), "Opening %s", row->name);
    set_status(state, msg);
}

static void open_in_music_player(AppState *state, const FileRow *row)
{
    if (!state || !row)
        return;
    if (!gui_open_request_submit(row->path)) {
        set_status(state, "Open request failed");
        return;
    }
    int pid = fork();
    if (pid == 0) {
        exec("/bin/musicplayer.elf");
        exit(1);
    }
    if (pid < 0) {
        set_status(state, "Launch failed");
        return;
    }
    char msg[sizeof(state->status)];
    snprintf(msg, sizeof(msg), "Opening %s", row->name);
    set_status(state, msg);
}

static void activate_row(AppState *state, int index)
{
    if (state->volume_home) {
        int volume_index = visible_volume_index_at(state, index);
        if (volume_index >= 0)
            enter_volume(state, volume_index);
        return;
    }
    if (index < 0 || index >= row_count_of(state))
        return;
    state->selected_row = index;
    if (!state->rows[index].is_dir) {
        if (name_is_image(state->rows[index].name))
            open_in_image_viewer(state, &state->rows[index]);
        else if (name_is_audio(state->rows[index].name))
            open_in_music_player(state, &state->rows[index]);
        state->needs_redraw = true;
        return;
    }
    strncpy(state->current_path, state->rows[index].path, sizeof(state->current_path) - 1);
    state->current_path[sizeof(state->current_path) - 1] = '\0';
    load_directory(state);
    state->needs_redraw = true;
}

// Rebuild the popup's item list in place; the widget keeps pointing at it.
static int files_popup_build_items(AppState *state)
{
    MenuEntryDef entries[10];
    int count = build_menu_entries(state, entries, 10);
    for (int i = 0; i < count; i++) {
        state->popup_items[i] = entries[i].item;
        state->popup_commands[i] = entries[i].command;
    }
    state->popup_count = count;
    return count;
}

static void open_menu(AppState *state, MenuKind kind, int target_row, int target_volume, int mouse_x, int mouse_y,
                      int canvas_w, int view_y, int view_h)
{
    if (!state)
        return;
    state->menu_kind = kind;
    state->menu_target_row = target_row;
    state->menu_target_volume = target_volume;
    int count = files_popup_build_items(state);
    if (count <= 0) {
        close_menu(state);
        return;
    }

    // widget_popup_open clamps to the canvas width and the visible viewport
    // (not the full content height), so the menu never opens below the fold.
    widget_popup_open(&state->popup, state->popup_items, count, mouse_x, mouse_y, canvas_w, view_y, view_h,
                      gui_scaled_metric(170));
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

    if (state->menu_target_row < 0 || state->menu_target_row >= row_count_of(state))
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

static bool files_has_selection(const AppState *state)
{
    return !state->volume_home && state->selected_row >= 0 && state->selected_row < row_count_of(state);
}

static void files_publish_menus(AppState *state)
{
    MenuModel model;
    gui_menu_model_reset(&model);
    bool writable = storage_is_writable(state);
    bool has_sel = files_has_selection(state);
    bool in_dir = !state->volume_home;

    int file = gui_menu_model_add_menu(&model, "File");
    gui_menu_model_add_item(&model, file, "New Folder...", FILES_MENU_NEW_FOLDER,
                            writable && in_dir ? 0 : MENU_FLAG_DISABLED, "Ctrl+N");
    gui_menu_model_add_separator(&model, file);
    gui_menu_model_add_item(&model, file, "Rename...", FILES_MENU_RENAME, has_sel && writable ? 0 : MENU_FLAG_DISABLED,
                            nullptr);
    gui_menu_model_add_item(&model, file, "Delete", FILES_MENU_DELETE, has_sel && writable ? 0 : MENU_FLAG_DISABLED,
                            nullptr);

    int edit = app_menus_add_edit(&model, APP_EDIT_CUT | APP_EDIT_COPY | APP_EDIT_PASTE,
                                  (has_sel && writable ? APP_EDIT_CUT : 0) | (has_sel ? APP_EDIT_COPY : 0) |
                                      (writable && in_dir ? APP_EDIT_PASTE : 0));
    (void)edit;

    int view = gui_menu_model_add_menu(&model, "View");
    gui_menu_model_add_item(&model, view, "Show Sidebar", FILES_MENU_TOGGLE_SIDEBAR,
                            state->show_sidebar ? MENU_FLAG_CHECKED : 0, nullptr);
    gui_menu_model_add_item(&model, view, "Show as Icons", FILES_MENU_TOGGLE_VIEW,
                            state->icon_view ? MENU_FLAG_CHECKED : 0, nullptr);
    gui_menu_model_add_separator(&model, view);
    gui_menu_model_add_item(&model, view, "Refresh", FILES_MENU_REFRESH, 0, nullptr);

    int go = gui_menu_model_add_menu(&model, "Go");
    gui_menu_model_add_item(&model, go, "Up", FILES_MENU_UP, in_dir ? 0 : MENU_FLAG_DISABLED, nullptr);
    gui_menu_model_add_separator(&model, go);
    for (int i = 0; i < MAX_PLACES; i++) {
        bool checked = in_dir && path_equals(state->current_path, k_places[i].path);
        gui_menu_model_add_item(&model, go, k_places[i].label, FILES_MENU_GO_PLACE + (uint32_t)i,
                                checked ? MENU_FLAG_CHECKED : 0, nullptr);
    }
    int vol_count = visible_volume_count(state);
    if (vol_count > 0) {
        gui_menu_model_add_separator(&model, go);
        for (int i = 0; i < vol_count && i < 8; i++) {
            int vi = visible_volume_index_at(state, i);
            if (vi < 0)
                continue;
            bool checked = state->active_volume == vi;
            gui_menu_model_add_item(&model, go, state->volumes[vi].display_name, FILES_MENU_GO_VOLUME + (uint32_t)i,
                                    checked ? MENU_FLAG_CHECKED : 0, nullptr);
        }
    }

    app_menus_add_help(&model, FILES_MENU_HELP);

    gui_menu_publish(&model);
}

static bool files_clipboard_path(const char *prefix, char *out, size_t out_size)
{
    char clip[640];
    size_t len = 0;
    if (!gui_clipboard_paste(clip, sizeof(clip), &len))
        return false;
    size_t plen = strlen(prefix);
    if (len < plen + 1 || strncmp(clip, prefix, plen) != 0)
        return false;
    strncpy(out, clip + plen, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

static void files_cut_or_copy(AppState *state, bool cut)
{
    if (!files_has_selection(state))
        return;
    const FileRow &row = state->rows[state->selected_row];
    char text[600];
    snprintf(text, sizeof(text), "%s:%s", cut ? "cut" : "copy", row.path);
    if (gui_clipboard_copy(text, strlen(text))) {
        char msg[300];
        snprintf(msg, sizeof(msg), "%s %s", cut ? "Cut" : "Copied", row.name);
        set_status(state, msg);
    } else {
        set_status(state, "Clipboard unavailable");
    }
}

static void files_paste(AppState *state)
{
    if (state->volume_home || !storage_is_writable(state))
        return;
    char src[512];
    bool cut;
    if (files_clipboard_path("cut:", src, sizeof(src)))
        cut = true;
    else if (files_clipboard_path("copy:", src, sizeof(src)))
        cut = false;
    else {
        set_status(state, "Clipboard has no file");
        return;
    }
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    if (!base[0] || !validate_simple_name(base)) {
        set_status(state, "Clipboard has no file");
        return;
    }
    if (path_is_within(state->current_path, src)) {
        set_status(state, "Cannot paste a folder into itself");
        return;
    }
    struct VNodeStat st = {};
    if (stat(src, &st) != 0) {
        set_status(state, "Clipboard item no longer exists");
        return;
    }
    FileRow row = {};
    strncpy(row.path, src, sizeof(row.path) - 1);
    row.is_dir = st.is_dir;
    char dst[512];
    join_path(state->current_path, base, dst, sizeof(dst));
    char err[160] = {};
    bool ok;
    if (cut)
        ok = move_entry(&row, dst, err, sizeof(err));
    else if (row.is_dir)
        ok = copy_directory_tree(row.path, dst, 0, err, sizeof(err));
    else
        ok = copy_file_stream(row.path, dst, err, sizeof(err));
    if (ok) {
        set_status(state, cut ? "Move complete" : "Copy complete");
        load_directory(state);
    } else {
        set_status(state, err[0] ? err : "Paste failed");
    }
}

static void files_handle_menu_command(AppState *state, uint32_t cmd)
{
    if (!state || cmd == 0)
        return;
    bool writable = storage_is_writable(state);
    bool has_sel = files_has_selection(state);

    if (cmd == FILES_MENU_NEW_FOLDER) {
        if (writable && !state->volume_home)
            open_dialog(state, DIALOG_NEW_FOLDER, "New Folder", "");
        return;
    }
    if (cmd == FILES_MENU_RENAME) {
        if (has_sel && writable)
            open_dialog(state, DIALOG_RENAME, "Rename", state->rows[state->selected_row].name);
        return;
    }
    if (cmd == FILES_MENU_DELETE) {
        if (!has_sel || !writable)
            return;
        char err[160] = {};
        if (delete_selected(&state->rows[state->selected_row], err, sizeof(err))) {
            set_status(state, "Item deleted");
            load_directory(state);
        } else {
            set_status(state, err[0] ? err : "Delete failed");
        }
        return;
    }
    if (cmd == APP_CMD_CUT) {
        if (has_sel && writable)
            files_cut_or_copy(state, true);
        return;
    }
    if (cmd == APP_CMD_COPY) {
        if (has_sel)
            files_cut_or_copy(state, false);
        return;
    }
    if (cmd == APP_CMD_PASTE) {
        files_paste(state);
        return;
    }
    if (cmd == FILES_MENU_REFRESH) {
        refresh_volumes(state);
        select_default_location(state, false);
        if (!state->volume_home)
            load_directory(state);
        return;
    }
    if (cmd == FILES_MENU_TOGGLE_SIDEBAR) {
        state->show_sidebar = !state->show_sidebar;
        app_setting_save_int("files_sidebar", state->show_sidebar ? 1 : 0);
        state->needs_redraw = true;
        return;
    }
    if (cmd == FILES_MENU_TOGGLE_VIEW) {
        state->icon_view = !state->icon_view;
        app_setting_save_int("files_view_mode", state->icon_view ? 1 : 0);
        state->needs_redraw = true;
        return;
    }
    if (cmd == FILES_MENU_UP) {
        if (!state->volume_home)
            navigate_up(state);
        return;
    }
    if (cmd >= FILES_MENU_GO_PLACE && cmd < FILES_MENU_GO_VOLUME) {
        int index = (int)(cmd - FILES_MENU_GO_PLACE);
        if (index >= 0 && index < MAX_PLACES)
            activate_place(state, index);
        return;
    }
    if (cmd >= FILES_MENU_GO_VOLUME && cmd < FILES_MENU_HELP) {
        int index = visible_volume_index_at(state, (int)(cmd - FILES_MENU_GO_VOLUME));
        if (index >= 0)
            enter_volume(state, index);
        return;
    }
    if (cmd == FILES_MENU_HELP) {
        open_dialog(state, DIALOG_HELP, "Files Help", "");
        return;
    }
}

struct FilesApp
{
    AppState state;
    LayoutCache cache;
};

static void files_draw(App *app, Surface *canvas)
{
    FilesApp *st = (FilesApp *)app_user(app);
    draw_files(app, canvas, &st->state, &st->cache);
    st->state.needs_redraw = false;
}

static void files_menus(App *app)
{
    FilesApp *st = (FilesApp *)app_user(app);
    files_publish_menus(&st->state);
}

static void files_menu(App *app, uint32_t cmd)
{
    FilesApp *st = (FilesApp *)app_user(app);
    files_handle_menu_command(&st->state, cmd);
    // Menubar commands mutate state (dialogs, delete, refresh, clipboard,
    // status) outside the mouse paths that set the flag themselves.
    st->state.needs_redraw = true;
}

static void files_idle(App *app)
{
    FilesApp *st = (FilesApp *)app_user(app);
    AppState *state = &st->state;

    Registry *registry = gui_registry();
    int current_storage_mode =
        registry && registry->storage_mode <= STORAGE_MODE_WRITABLE ? (int)registry->storage_mode : get_storage_mode();
    if (current_storage_mode != state->storage_mode) {
        refresh_volumes(state);
        select_default_location(state, false);
        state->needs_redraw = true;
    }

    // The menubar model tracks selection, view toggles and mounted volumes;
    // keep it fresh while this window owns the focus.
    if (app_focused(app) && registry && registry->focused_owner_pid == (uint32_t)syscall1(SYS_GETPID, 0))
        files_publish_menus(state);

    if (state->needs_redraw)
        app_request_draw(app);
}

static void files_dialog_event(FilesApp *st, const Event *ev)
{
    AppState *state = &st->state;
    int rc = widget_dialog_event(&state->dialog, ev);
    int action = rc & (WIDGET_DIALOG_CONFIRM | WIDGET_DIALOG_CANCEL | WIDGET_DIALOG_DISMISS);
    if (action == WIDGET_DIALOG_CONFIRM)
        confirm_dialog(state);
    else if (action == WIDGET_DIALOG_CANCEL || action == WIDGET_DIALOG_DISMISS)
        close_dialog(state);
    if (rc)
        state->needs_redraw = true;
}

// Signature of the interactive region under the pointer. All hover washes in
// the file view are derived at draw time from the pointer position, so a move
// that keeps this signature unchanged cannot change any pixel.
static int files_hover_region(const AppState *state, const LayoutCache *cache)
{
    if (!state->have_mouse)
        return -1;
    if (!gui_rect_is_empty(cache->view_switch) && rect_contains(cache->view_switch, state->mouse_x, state->mouse_y))
        return 0x40000 + (state->mouse_x >= cache->view_switch.x + cache->view_switch.w / 2 ? 1 : 0);
    for (int i = 0; i < MAX_PLACES; i++) {
        if (gui_rect_is_empty(cache->place_rects[i]))
            continue;
        if (rect_contains(cache->place_rects[i], state->mouse_x, state->mouse_y))
            return 0x10000 + i;
    }
    for (int i = 0; i < MAX_VOLUMES; i++) {
        if (gui_rect_is_empty(cache->volume_rects[i]))
            continue;
        if (rect_contains(cache->volume_rects[i], state->mouse_x, state->mouse_y))
            return 0x20000 + i;
    }
    int rows = (int)cache->row_rects.size();
    if (rows > 0x10000)
        rows = 0x10000;
    for (int i = 0; i < rows; i++) {
        if (rect_contains(cache->row_rects[i], state->mouse_x, state->mouse_y))
            return i;
    }
    return 0x30000;
}

static void files_event(App *app, const Event *ev)
{
    FilesApp *st = (FilesApp *)app_user(app);
    AppState *state = &st->state;
    LayoutCache *cache = &st->cache;
    Surface *win = app_canvas(app);

    switch (ev->type) {
        case EVT_UNFOCUS:
            if (state->menu_kind != MENU_NONE)
                close_menu(state);
            state->have_mouse = false;
            state->hover_region = -1;
            state->needs_redraw = true;
            break;

        case EVT_MOUSE_LEAVE:
            state->have_mouse = false;
            state->hover_region = -1;
            state->needs_redraw = true;
            break;

        case EVT_WINDOW_SCROLL:
            // The view scrolled under a stationary pointer: the pointer's
            // content-space position moved by the scroll delta, so rebase the
            // tracked hover coordinates (libapp already invalidated all).
            state->mouse_y += ev->scroll.scroll_y - state->last_scroll_y;
            state->mouse_x += ev->scroll.scroll_x - state->last_scroll_x;
            state->last_scroll_y = ev->scroll.scroll_y;
            state->last_scroll_x = ev->scroll.scroll_x;
            state->hover_region = files_hover_region(state, cache);
            break;

        case EVT_MOUSE_MOVE: {
            state->mouse_x = ev->mouse.x;
            state->mouse_y = ev->mouse.y;
            state->have_mouse = true;
            if (state->menu_kind != MENU_NONE) {
                if (widget_popup_event(&state->popup, ev) == WIDGET_POPUP_HOVERED)
                    state->needs_redraw = true;
                break;
            }
            // Repaint only when the hovered region actually changed: the
            // hover washes are position-derived, so moves within one region
            // are visually identical and a full-content repaint is waste.
            int region = files_hover_region(state, cache);
            if (region != state->hover_region) {
                state->hover_region = region;
                state->needs_redraw = true;
            }
            break;
        }

        case EVT_MOUSE_UP: {
            if (ev->mouse.button != 1)
                break;
            if (state->dialog_mode != DIALOG_NONE) {
                files_dialog_event(st, ev);
                break;
            }
            break;
        }

        case EVT_MOUSE_DOWN: {
            if (ev->mouse.button == 1) {
                state->mouse_x = ev->mouse.x;
                state->mouse_y = ev->mouse.y;
                state->have_mouse = true;

                if (state->dialog_mode != DIALOG_NONE) {
                    files_dialog_event(st, ev);
                    break;
                }

                if (state->menu_kind != MENU_NONE) {
                    int rc = widget_popup_event(&state->popup, ev);
                    if (rc >= 0) {
                        MenuCommand command = state->popup_commands[rc];
                        // Execute before closing: the command dispatches on
                        // menu_kind/menu_target_row, which close_menu resets.
                        execute_menu_command(state, command);
                        close_menu(state);
                    } else if (rc == WIDGET_POPUP_DISMISSED) {
                        close_menu(state);
                    }
                    break;
                }

                // Header view switcher (List | Icons).
                if (!gui_rect_is_empty(cache->view_switch) &&
                    rect_contains(cache->view_switch, ev->mouse.x, ev->mouse.y)) {
                    bool want_icons = ev->mouse.x >= cache->view_switch.x + cache->view_switch.w / 2;
                    if (want_icons != state->icon_view) {
                        state->icon_view = want_icons;
                        app_setting_save_int("files_view_mode", state->icon_view ? 1 : 0);
                        state->needs_redraw = true;
                    }
                    break;
                }

                bool handled_left_click = false;
                for (int i = 0; i < MAX_PLACES; i++) {
                    if (gui_rect_is_empty(cache->place_rects[i]))
                        continue;
                    if (rect_contains(cache->place_rects[i], ev->mouse.x, ev->mouse.y)) {
                        activate_place(state, i);
                        handled_left_click = true;
                        break;
                    }
                }
                if (handled_left_click)
                    break;

                int sidebar_volume_count = visible_volume_count(state);
                for (int i = 0; i < sidebar_volume_count; i++) {
                    if (rect_contains(cache->volume_rects[i], ev->mouse.x, ev->mouse.y)) {
                        int volume_index = visible_volume_index_at(state, i);
                        if (volume_index >= 0)
                            enter_volume(state, volume_index);
                        handled_left_click = true;
                        break;
                    }
                }
                if (handled_left_click)
                    break;

                int row_count = state->volume_home ? visible_volume_count(state) : row_count_of(state);
                // The rect Vec still reflects the last draw; state->rows may
                // have grown inside this same event batch (a directory load),
                // so never index past the cached geometry.
                if (row_count > (int)cache->row_rects.size())
                    row_count = (int)cache->row_rects.size();
                for (int i = 0; i < row_count; i++) {
                    if (!rect_contains(cache->row_rects[i], ev->mouse.x, ev->mouse.y))
                        continue;
                    uint64_t now = get_ticks();
                    bool double_click = (state->last_click_row == i) && (now - state->last_click_ticks < 400);
                    state->last_click_row = i;
                    state->last_click_ticks = now;
                    if (state->volume_home) {
                        state->selected_volume_row = i;
                        state->needs_redraw = true;
                        if (double_click)
                            activate_row(state, i);
                    } else {
                        state->selected_row = i;
                        if (double_click)
                            activate_row(state, i);
                        state->needs_redraw = true;
                    }
                    break;
                }
            } else if (ev->mouse.button == 2) {
                if (state->dialog_mode != DIALOG_NONE)
                    break;

                int view_y = app_scroll_y(app);
                // Clamp popups to the visible viewport (the canvas is grown
                // to the content height and would defeat the clamp).
                int view_h = app_view_h(app);

                int sidebar_volume_count = visible_volume_count(state);
                bool opened = false;
                for (int i = 0; i < sidebar_volume_count && !opened; i++) {
                    if (!rect_contains(cache->volume_rects[i], ev->mouse.x, ev->mouse.y))
                        continue;
                    int volume_index = visible_volume_index_at(state, i);
                    open_menu(state, MENU_VOLUME, -1, volume_index, ev->mouse.x, ev->mouse.y, (int)win->width, view_y,
                              view_h);
                    opened = true;
                }

                if (!opened && !state->volume_home) {
                    int hit_count = row_count_of(state);
                    if (hit_count > (int)cache->row_rects.size())
                        hit_count = (int)cache->row_rects.size();
                    for (int i = 0; i < hit_count && !opened; i++) {
                        if (!rect_contains(cache->row_rects[i], ev->mouse.x, ev->mouse.y))
                            continue;
                        state->selected_row = i;
                        open_menu(state, MENU_ENTRY, i, -1, ev->mouse.x, ev->mouse.y, (int)win->width, view_y, view_h);
                        opened = true;
                    }
                } else if (!opened && state->volume_home) {
                    int hit_count = visible_volume_count(state);
                    if (hit_count > (int)cache->row_rects.size())
                        hit_count = (int)cache->row_rects.size();
                    for (int i = 0; i < hit_count && !opened; i++) {
                        if (!rect_contains(cache->row_rects[i], ev->mouse.x, ev->mouse.y))
                            continue;
                        int volume_index = visible_volume_index_at(state, i);
                        open_menu(state, MENU_VOLUME, -1, volume_index, ev->mouse.x, ev->mouse.y, (int)win->width,
                                  view_y, view_h);
                        opened = true;
                    }
                }

                if (!opened)
                    open_menu(state, MENU_BACKGROUND, -1, -1, ev->mouse.x, ev->mouse.y, (int)win->width, view_y,
                              view_h);
                state->needs_redraw = true;
            }
            break;
        }

        case EVT_KEY_DOWN: {
            if (state->menu_kind != MENU_NONE)
                close_menu(state);
            if (state->dialog_mode == DIALOG_NONE && ev->key.c > 0 && ev->key.c < 32) {
                bool handled = true;
                if (ev->key.c == 14) { // Ctrl+N
                    files_handle_menu_command(state, FILES_MENU_NEW_FOLDER);
                } else {
                    uint32_t shortcut = app_edit_shortcut(ev);
                    if (shortcut == APP_CMD_CUT || shortcut == APP_CMD_COPY || shortcut == APP_CMD_PASTE)
                        files_handle_menu_command(state, shortcut);
                    else
                        handled = false;
                }
                if (handled) {
                    state->needs_redraw = true;
                    break;
                }
            }
            if (state->dialog_mode != DIALOG_NONE) {
                files_dialog_event(st, ev);
                break;
            }
            if (ev->key.c == '\n' || ev->key.c == '\r') {
                if (state->volume_home) {
                    if (state->selected_volume_row >= 0)
                        activate_row(state, state->selected_volume_row);
                } else if (state->selected_row >= 0) {
                    activate_row(state, state->selected_row);
                }
            } else if (ev->key.c == 8 || ev->key.c == 127) {
                navigate_up(state);
            } else {
                uint8_t key = (uint8_t)ev->key.c;
                int row_count = state->volume_home ? visible_volume_count(state) : row_count_of(state);
                int *selection = state->volume_home ? &state->selected_volume_row : &state->selected_row;
                if (key == 0x80) { // Up
                    if (row_count > 0) {
                        *selection = (*selection <= 0) ? row_count - 1 : *selection - 1;
                        state->needs_redraw = true;
                    }
                } else if (key == 0x81) { // Down
                    if (row_count > 0) {
                        *selection = (*selection >= row_count - 1) ? 0 : *selection + 1;
                        state->needs_redraw = true;
                    }
                } else if (key == 0x82) { // Left = navigate up
                    navigate_up(state);
                } else if (key == 0x83) { // Right = open selection
                    if (state->volume_home) {
                        if (state->selected_volume_row >= 0)
                            activate_row(state, state->selected_volume_row);
                    } else if (state->selected_row >= 0) {
                        activate_row(state, state->selected_row);
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

extern "C" int main()
{
    static FilesApp st = {};
    AppState *state = &st.state;

    state->active_volume = -1;
    state->selected_row = -1;
    state->last_click_row = -1;
    state->menu_target_row = -1;
    state->menu_target_volume = -1;
    state->volume_home = true;
    state->needs_redraw = true;
    state->storage_mode = STORAGE_MODE_READ_ONLY;
    state->mouse_x = -1;
    state->mouse_y = -1;
    state->have_mouse = false;
    state->selected_volume_row = -1;
    state->window_title[0] = '\0';
    state->hover_region = -1; // 0 is a valid region key (first row)
    state->show_sidebar = app_setting_load_int("files_sidebar", 1) != 0;
    state->icon_view = app_setting_load_int("files_view_mode", 1) != 0;
    refresh_volumes(state);
    select_default_location(state, true);

    AppConfig config = {};
    config.title = "Files";
    config.width = gui_scaled_metric(920);
    config.height = gui_scaled_metric(560);
    config.min_width = gui_scaled_metric(640);
    config.min_height = gui_scaled_metric(420);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 35;
    config.on_draw = files_draw;
    config.on_event = files_event;
    config.on_menu = files_menu;
    config.on_menus = files_menus;
    config.on_idle = files_idle;

    App *app = app_create(&config, &st);
    if (!app)
        return 1;

    app_invalidate_all(app);
    while (app_pump(app)) {
        app_commit(app);
        if (!app_needs_draw(app))
            sleep_ms(config.idle_ms);
    }
    clear_thumbs(state);
    app_destroy(app);
    return 0;
}
