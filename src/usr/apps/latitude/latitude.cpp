#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>
#include <unistd.h>

#include "../../libc/syscall.h"
#include "../../libgui/gui.h"

static constexpr int MAX_LINES = 2048;
static constexpr int MAX_LINE_LEN = 512;
static constexpr int MAX_PROJECT_ROWS = 160;
static constexpr int MAX_OUTLINE_ROWS = 96;
static constexpr int MAX_STATUS_LEN = 160;
static constexpr int DEFAULT_TAB_SPACES = 4;
static constexpr uint64_t MAX_OPEN_TEXT_BYTES = 512ull * 1024ull;
static constexpr int TEXT_SNIFF_BYTES = 1024;

static constexpr uint8_t KEY_UP_ARROW = 0x80;
static constexpr uint8_t KEY_DOWN_ARROW = 0x81;
static constexpr uint8_t KEY_LEFT_ARROW = 0x82;
static constexpr uint8_t KEY_RIGHT_ARROW = 0x83;
static constexpr uint8_t KEY_HOME = 0x84;
static constexpr uint8_t KEY_END = 0x85;
static constexpr uint8_t KEY_DELETE = 0x86;
static constexpr uint8_t KEY_PAGEUP = 0x87;
static constexpr uint8_t KEY_PAGEDOWN = 0x88;
static constexpr uint8_t KEY_SHIFT_LEFT = 0x90;
static constexpr uint8_t KEY_SHIFT_RIGHT = 0x91;

enum Language
{
    LANG_TEXT = 0,
    LANG_CPP,
    LANG_JS,
    LANG_PYTHON,
    LANG_RUST,
    LANG_HTML,
    LANG_CSS,
    LANG_JSON,
    LANG_MARKDOWN,
    LANG_SHELL,
};

enum FocusPane
{
    FOCUS_EDITOR = 0,
    FOCUS_PROJECT,
    FOCUS_PATH,
    FOCUS_SEARCH,
};

enum HoverTarget
{
    HOVER_NONE = 0,
    HOVER_PROJECT_ROW,
    HOVER_OUTLINE_ROW,
    HOVER_NEW_FILE,
    HOVER_SAVE,
    HOVER_RELOAD,
    HOVER_PATH,
    HOVER_SEARCH,
    HOVER_EDITOR,
};

enum FileKind
{
    FILE_KIND_TEXT = 0,
    FILE_KIND_CODE,
    FILE_KIND_CONFIG,
    FILE_KIND_BINARY,
    FILE_KIND_EXECUTABLE,
    FILE_KIND_DISK_IMAGE,
    FILE_KIND_IMAGE,
    FILE_KIND_ARCHIVE,
    FILE_KIND_LARGE,
    FILE_KIND_UNKNOWN,
};

struct TextLine
{
    uint16_t len;
    char text[MAX_LINE_LEN];
};

struct TextBuffer
{
    TextLine lines[MAX_LINES];
    int line_count;
    char path[512];
    char title[128];
    Language language;
    bool modified;
    bool truncated;
};

struct ProjectRow
{
    char name[128];
    char path[512];
    FileKind kind;
    bool is_dir;
    bool parent;
};

struct OutlineRow
{
    char label[96];
    char badge[12];
    int line;
};

struct LatitudeRects
{
    Rect project_rows[MAX_PROJECT_ROWS];
    Rect outline_rows[MAX_OUTLINE_ROWS];
    Rect new_button;
    Rect save_button;
    Rect reload_button;
    Rect path_field;
    Rect search_field;
    Rect editor_rect;
    Rect sidebar_rect;
    Rect editor_panel;
};

struct AppState
{
    TextBuffer *buffer;
    ProjectRow project_rows[MAX_PROJECT_ROWS];
    int project_count;
    int project_selected;
    int project_hovered;
    int project_first_row;
    char project_path[512];

    OutlineRow outline_rows[MAX_OUTLINE_ROWS];
    int outline_count;
    int outline_hovered;
    int outline_first_row;

    int cursor_line;
    int cursor_col;
    int desired_col;
    int first_line;
    int first_col;
    int visible_lines;
    int visible_cols;

    FocusPane focus;
    HoverTarget hovered;
    char path_input[512];
    char search[96];
    bool path_focused;
    bool search_focused;

    uint64_t last_project_click_ticks;
    int last_project_click_row;
    uint64_t last_editor_click_ticks;
    int last_editor_click_line;

    char status[MAX_STATUS_LEN];
    uint32_t last_settings_generation;
    bool needs_redraw;
};

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

static bool point_in_rect(const Rect &rect, int x, int y)
{
    return rect.w > 0 && rect.h > 0 && x >= rect.x && y >= rect.y && (int64_t)x < (int64_t)rect.x + rect.w &&
           (int64_t)y < (int64_t)rect.y + rect.h;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_ident(char c)
{
    return is_alpha(c) || is_digit(c);
}

static bool starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix)
        return false;
    while (*prefix) {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}

static bool ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix)
        return false;
    size_t s_len = strlen(s);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > s_len)
        return false;
    return strcmp(s + s_len - suffix_len, suffix) == 0;
}

static const char *path_basename(const char *path)
{
    if (!path || !path[0])
        return "untitled";
    const char *slash = strrchr(path, '/');
    if (!slash)
        return path;
    return slash[1] ? slash + 1 : slash;
}

static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;

    const char *safe_src = src ? src : "";
    size_t i = 0;
    while (i + 1 < dst_size && safe_src[i]) {
        dst[i] = safe_src[i];
        i++;
    }
    dst[i] = '\0';
}

static void join_path(const char *base, const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!base || !base[0]) {
        copy_cstr(out, out_size, name);
        return;
    }
    copy_cstr(out, out_size, base);
    size_t len = strlen(out);
    if (len > 1 && out[len - 1] == '/')
        out[len - 1] = '\0';
    if (strcmp(out, "/") != 0)
        strncat(out, "/", out_size - strlen(out) - 1);
    strncat(out, name ? name : "", out_size - strlen(out) - 1);
}

static void parent_path(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        copy_cstr(out, out_size, "/");
        return;
    }
    copy_cstr(out, out_size, path);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[len - 1] = '\0';
        len--;
    }
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        copy_cstr(out, out_size, "/");
        return;
    }
    *slash = '\0';
}

static const char *file_ext(const char *path)
{
    const char *name = path_basename(path);
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return "";
    return dot + 1;
}

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static bool str_equals_ci(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool ext_equals(const char *path, const char *ext)
{
    return str_equals_ci(file_ext(path), ext);
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    if (!path || !prefix)
        return false;
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0;
}

static bool path_is_storage_path(const char *path)
{
    if (!path || !path[0])
        return false;
    return strcmp(path, "/data") == 0 || path_has_prefix(path, "/data/") || path_has_prefix(path, "/vol/");
}

static bool storage_path_is_read_only(const char *path)
{
    return path_is_storage_path(path) && get_storage_mode() != STORAGE_MODE_WRITABLE;
}

static bool parent_directory_is_available(const char *path, char *parent, size_t parent_size)
{
    if (!path || !path[0] || !parent || parent_size == 0)
        return false;
    parent_path(path, parent, parent_size);
    VNodeStat st = {};
    return stat(parent, &st) == 0 && st.is_dir;
}

static int open_file_for_save(const char *path)
{
    if (!path || !path[0])
        return -1;

    // O_CREAT needs a creation mode. Go through the raw syscall wrapper so we can
    // supply it even on libc builds whose open() shim only exposes two arguments.
    return (int)syscall3(SYS_OPEN, (uint64_t)path, (uint64_t)(O_WRONLY | O_CREAT | O_TRUNC), 0644);
}

static Language detect_language(const char *path)
{
    const char *name = path_basename(path);
    if (ext_equals(path, "c") || ext_equals(path, "h") || ext_equals(path, "cpp") || ext_equals(path, "hpp") ||
        ext_equals(path, "cc") || ext_equals(path, "hh"))
        return LANG_CPP;
    if (ext_equals(path, "js") || ext_equals(path, "jsx") || ext_equals(path, "ts") || ext_equals(path, "tsx"))
        return LANG_JS;
    if (ext_equals(path, "py"))
        return LANG_PYTHON;
    if (ext_equals(path, "rs"))
        return LANG_RUST;
    if (ext_equals(path, "html") || ext_equals(path, "htm") || ext_equals(path, "xml"))
        return LANG_HTML;
    if (ext_equals(path, "css"))
        return LANG_CSS;
    if (ext_equals(path, "json"))
        return LANG_JSON;
    if (ext_equals(path, "md") || ext_equals(path, "markdown"))
        return LANG_MARKDOWN;
    if (ext_equals(path, "sh") || str_equals_ci(name, "meson.build") || str_equals_ci(name, "Makefile") ||
        str_equals_ci(name, "Dockerfile"))
        return LANG_SHELL;
    return LANG_TEXT;
}

static const char *language_label(Language language)
{
    switch (language) {
        case LANG_CPP:
            return "C++";
        case LANG_JS:
            return "JS/TS";
        case LANG_PYTHON:
            return "Python";
        case LANG_RUST:
            return "Rust";
        case LANG_HTML:
            return "HTML";
        case LANG_CSS:
            return "CSS";
        case LANG_JSON:
            return "JSON";
        case LANG_MARKDOWN:
            return "Markdown";
        case LANG_SHELL:
            return "Shell";
        case LANG_TEXT:
        default:
            return "Text";
    }
}

static bool extension_is_text_like(const char *path)
{
    return ext_equals(path, "txt") || ext_equals(path, "log") || ext_equals(path, "md") ||
           ext_equals(path, "markdown") || ext_equals(path, "json") || ext_equals(path, "xml") ||
           ext_equals(path, "html") || ext_equals(path, "htm") || ext_equals(path, "css") || ext_equals(path, "js") ||
           ext_equals(path, "jsx") || ext_equals(path, "ts") || ext_equals(path, "tsx") || ext_equals(path, "py") ||
           ext_equals(path, "rs") || ext_equals(path, "c") || ext_equals(path, "h") || ext_equals(path, "cpp") ||
           ext_equals(path, "hpp") || ext_equals(path, "cc") || ext_equals(path, "hh") || ext_equals(path, "sh") ||
           ext_equals(path, "ini") || ext_equals(path, "cfg") || ext_equals(path, "conf") || ext_equals(path, "toml") ||
           ext_equals(path, "yaml") || ext_equals(path, "yml") || ext_equals(path, "csv") || ext_equals(path, "s") ||
           ext_equals(path, "asm");
}

static FileKind classify_file(const char *path, bool is_dir, uint64_t size)
{
    if (is_dir)
        return FILE_KIND_UNKNOWN;
    if (ext_equals(path, "elf") || ext_equals(path, "efi") || ext_equals(path, "exe"))
        return FILE_KIND_EXECUTABLE;
    if (ext_equals(path, "img") || ext_equals(path, "iso") || ext_equals(path, "dmg"))
        return FILE_KIND_DISK_IMAGE;
    if (ext_equals(path, "bin") || ext_equals(path, "o") || ext_equals(path, "a") || ext_equals(path, "so") ||
        ext_equals(path, "uoic") || ext_equals(path, "uowp") || ext_equals(path, "uof") || ext_equals(path, "ttf") ||
        ext_equals(path, "otf") || ext_equals(path, "dat"))
        return FILE_KIND_BINARY;
    if (ext_equals(path, "png") || ext_equals(path, "jpg") || ext_equals(path, "jpeg") || ext_equals(path, "gif") ||
        ext_equals(path, "bmp") || ext_equals(path, "webp"))
        return FILE_KIND_IMAGE;
    if (ext_equals(path, "zip") || ext_equals(path, "tar") || ext_equals(path, "gz") || ext_equals(path, "7z"))
        return FILE_KIND_ARCHIVE;
    if (size > MAX_OPEN_TEXT_BYTES)
        return FILE_KIND_LARGE;
    if (ext_equals(path, "cfg") || ext_equals(path, "conf") || ext_equals(path, "ini") || ext_equals(path, "toml") ||
        ext_equals(path, "yaml") || ext_equals(path, "yml"))
        return FILE_KIND_CONFIG;
    if (detect_language(path) != LANG_TEXT)
        return FILE_KIND_CODE;
    if (extension_is_text_like(path))
        return FILE_KIND_TEXT;
    return FILE_KIND_UNKNOWN;
}

static bool file_kind_opens_as_text(FileKind kind)
{
    return kind == FILE_KIND_TEXT || kind == FILE_KIND_CODE || kind == FILE_KIND_CONFIG || kind == FILE_KIND_UNKNOWN;
}

static const char *file_kind_badge(FileKind kind)
{
    switch (kind) {
        case FILE_KIND_CODE:
            return "CODE";
        case FILE_KIND_CONFIG:
            return "CFG";
        case FILE_KIND_EXECUTABLE:
            return "APP";
        case FILE_KIND_DISK_IMAGE:
            return "IMG";
        case FILE_KIND_IMAGE:
            return "PIC";
        case FILE_KIND_ARCHIVE:
            return "ZIP";
        case FILE_KIND_LARGE:
            return "BIG";
        case FILE_KIND_BINARY:
            return "BIN";
        case FILE_KIND_TEXT:
            return "TXT";
        case FILE_KIND_UNKNOWN:
        default:
            return "FILE";
    }
}

static const char *file_kind_label(const char *path, FileKind kind)
{
    switch (kind) {
        case FILE_KIND_CODE:
            return language_label(detect_language(path));
        case FILE_KIND_CONFIG:
            return "Config";
        case FILE_KIND_EXECUTABLE:
            return "Executable";
        case FILE_KIND_DISK_IMAGE:
            return "Disk Image";
        case FILE_KIND_IMAGE:
            return "Image";
        case FILE_KIND_ARCHIVE:
            return "Archive";
        case FILE_KIND_LARGE:
            return "Large File";
        case FILE_KIND_BINARY:
            return "Binary";
        case FILE_KIND_TEXT:
            return "Text";
        case FILE_KIND_UNKNOWN:
        default:
            return "File";
    }
}

static void set_status(AppState *state, const char *status)
{
    if (!state)
        return;
    copy_cstr(state->status, sizeof(state->status), status);
}

static void sync_path_input(AppState *state)
{
    if (!state || !state->buffer)
        return;
    copy_cstr(state->path_input, sizeof(state->path_input), state->buffer->path);
}

static void clear_field_focus(AppState *state)
{
    if (!state)
        return;
    state->path_focused = false;
    state->search_focused = false;
}

static bool request_storage_mode_change(Registry *registry, int new_mode)
{
    if (!registry || new_mode < STORAGE_MODE_OFF || new_mode > STORAGE_MODE_WRITABLE)
        return false;
    registry->storage_request_mode = (uint32_t)new_mode;
    asm volatile("sfence" ::: "memory");
    registry->storage_request_generation = registry->storage_request_generation + 1u;
    asm volatile("sfence" ::: "memory");
    return true;
}

static bool ensure_writable_storage(AppState *state, const char *path)
{
    if (!path_is_storage_path(path))
        return true;
    if (get_storage_mode() == STORAGE_MODE_WRITABLE)
        return true;

    request_storage_mode_change(gui_registry(), STORAGE_MODE_WRITABLE);
    for (int i = 0; i < 12; i++) {
        sleep_ms(20);
        if (get_storage_mode() == STORAGE_MODE_WRITABLE)
            return true;
    }

    int mode = get_storage_mode();
    if (mode == STORAGE_MODE_OFF)
        set_status(state, "Storage is off; switch Storage Mode to Writable");
    else
        set_status(state, "Writable storage requested; press Save again after it switches");
    return false;
}

static bool resolve_input_path(AppState *state, const char *input, char *out, size_t out_size)
{
    if (!state || !input || !out || out_size == 0)
        return false;
    out[0] = '\0';
    while (is_space(*input))
        input++;
    size_t len = strlen(input);
    while (len > 0 && is_space(input[len - 1]))
        len--;
    if (len == 0)
        return false;

    char trimmed[512];
    size_t copy_len = len < sizeof(trimmed) - 1 ? len : sizeof(trimmed) - 1;
    memcpy(trimmed, input, copy_len);
    trimmed[copy_len] = '\0';

    if (trimmed[0] == '/') {
        copy_cstr(out, out_size, trimmed);
    } else {
        const char *base = state->project_path[0] ? state->project_path : "/data";
        join_path(base, trimmed, out, out_size);
    }
    return out[0] != '\0';
}

static void set_buffer_path(AppState *state, const char *path, bool mark_dirty)
{
    if (!state || !state->buffer || !path || !path[0])
        return;
    bool changed = strcmp(state->buffer->path, path) != 0;
    copy_cstr(state->buffer->path, sizeof(state->buffer->path), path);
    copy_cstr(state->buffer->title, sizeof(state->buffer->title), path_basename(state->buffer->path));
    state->buffer->language = detect_language(state->buffer->path);
    sync_path_input(state);
    if (mark_dirty && changed)
        state->buffer->modified = true;
}

static bool set_buffer_path_from_input(AppState *state, bool mark_dirty)
{
    char resolved[512];
    if (!resolve_input_path(state, state ? state->path_input : nullptr, resolved, sizeof(resolved))) {
        set_status(state, "Enter a file path");
        if (state)
            state->needs_redraw = true;
        return false;
    }

    VNodeStat st = {};
    if (stat(resolved, &st) == 0 && st.is_dir) {
        set_status(state, "That path is a folder");
        if (state)
            state->needs_redraw = true;
        return false;
    } else if (stat(resolved, &st) == 0) {
        FileKind kind = classify_file(resolved, false, st.size);
        if (!file_kind_opens_as_text(kind)) {
            set_status(state, "Refusing to overwrite a non-text file");
            if (state)
                state->needs_redraw = true;
            return false;
        }
    }

    set_buffer_path(state, resolved, mark_dirty);
    return true;
}

static void reset_buffer(TextBuffer *buffer, const char *path)
{
    if (!buffer)
        return;
    memset(buffer, 0, sizeof(TextBuffer));
    buffer->line_count = 1;
    buffer->lines[0].len = 0;
    buffer->lines[0].text[0] = '\0';
    copy_cstr(buffer->path, sizeof(buffer->path), path);
    copy_cstr(buffer->title, sizeof(buffer->title), path_basename(buffer->path));
    buffer->language = detect_language(buffer->path);
    buffer->modified = false;
    buffer->truncated = false;
}

static void build_outline(AppState *state);
static void load_project(AppState *state, const char *path);

static void mark_modified(AppState *state)
{
    if (!state || !state->buffer)
        return;
    state->buffer->modified = true;
    state->needs_redraw = true;
}

static TextLine *current_line(AppState *state)
{
    if (!state || !state->buffer || state->cursor_line < 0 || state->cursor_line >= state->buffer->line_count)
        return nullptr;
    return &state->buffer->lines[state->cursor_line];
}

static void clamp_cursor(AppState *state)
{
    if (!state || !state->buffer)
        return;
    if (state->buffer->line_count < 1)
        state->buffer->line_count = 1;
    state->cursor_line = clamp_int(state->cursor_line, 0, state->buffer->line_count - 1);
    TextLine &line = state->buffer->lines[state->cursor_line];
    state->cursor_col = clamp_int(state->cursor_col, 0, line.len);
}

static void ensure_cursor_visible(AppState *state)
{
    if (!state)
        return;
    if (state->visible_lines <= 0)
        state->visible_lines = 1;
    if (state->visible_cols <= 0)
        state->visible_cols = 1;
    if (state->cursor_line < state->first_line)
        state->first_line = state->cursor_line;
    if (state->cursor_line >= state->first_line + state->visible_lines)
        state->first_line = state->cursor_line - state->visible_lines + 1;
    if (state->first_line < 0)
        state->first_line = 0;

    if (state->cursor_col < state->first_col)
        state->first_col = state->cursor_col;
    if (state->cursor_col >= state->first_col + state->visible_cols)
        state->first_col = state->cursor_col - state->visible_cols + 1;
    if (state->first_col < 0)
        state->first_col = 0;
}

static bool line_insert_chars(TextLine *line, int col, const char *text, int count)
{
    if (!line || !text || count <= 0)
        return false;
    col = clamp_int(col, 0, line->len);
    if (line->len + count >= MAX_LINE_LEN)
        return false;
    memmove(line->text + col + count, line->text + col, (size_t)(line->len - col + 1));
    memcpy(line->text + col, text, (size_t)count);
    line->len = (uint16_t)(line->len + count);
    return true;
}

static bool line_delete_range(TextLine *line, int col, int count)
{
    if (!line || count <= 0)
        return false;
    if (col < 0 || col >= line->len)
        return false;
    if (col + count > line->len)
        count = line->len - col;
    memmove(line->text + col, line->text + col + count, (size_t)(line->len - col - count + 1));
    line->len = (uint16_t)(line->len - count);
    return true;
}

static bool insert_line(AppState *state, int index)
{
    if (!state || !state->buffer)
        return false;
    TextBuffer *buffer = state->buffer;
    if (buffer->line_count >= MAX_LINES)
        return false;
    index = clamp_int(index, 0, buffer->line_count);
    memmove(&buffer->lines[index + 1], &buffer->lines[index], sizeof(TextLine) * (size_t)(buffer->line_count - index));
    memset(&buffer->lines[index], 0, sizeof(TextLine));
    buffer->line_count++;
    return true;
}

static bool delete_line(AppState *state, int index)
{
    if (!state || !state->buffer || state->buffer->line_count <= 1)
        return false;
    TextBuffer *buffer = state->buffer;
    if (index < 0 || index >= buffer->line_count)
        return false;
    memmove(&buffer->lines[index], &buffer->lines[index + 1],
            sizeof(TextLine) * (size_t)(buffer->line_count - index - 1));
    buffer->line_count--;
    return true;
}

static char matching_close(char c)
{
    if (c == '(')
        return ')';
    if (c == '[')
        return ']';
    if (c == '{')
        return '}';
    if (c == '"')
        return '"';
    if (c == '\'')
        return '\'';
    return 0;
}

static char matching_open(char c)
{
    if (c == ')')
        return '(';
    if (c == ']')
        return '[';
    if (c == '}')
        return '{';
    if (c == '"')
        return '"';
    if (c == '\'')
        return '\'';
    return 0;
}

static bool is_closing_char(char c)
{
    return c == ')' || c == ']' || c == '}' || c == '"' || c == '\'';
}

static int leading_indent(const TextLine &line)
{
    int count = 0;
    while (count < line.len && (line.text[count] == ' ' || line.text[count] == '\t'))
        count++;
    return count;
}

static void insert_text_char(AppState *state, char c)
{
    TextLine *line = current_line(state);
    if (!line)
        return;

    if (is_closing_char(c) && state->cursor_col < line->len && line->text[state->cursor_col] == c) {
        state->cursor_col++;
        state->desired_col = state->cursor_col;
        ensure_cursor_visible(state);
        state->needs_redraw = true;
        return;
    }

    char close = matching_close(c);
    if (close != 0) {
        char pair[2] = {c, close};
        if (line_insert_chars(line, state->cursor_col, pair, 2)) {
            state->cursor_col++;
            state->desired_col = state->cursor_col;
            mark_modified(state);
            ensure_cursor_visible(state);
        }
        return;
    }

    if (line_insert_chars(line, state->cursor_col, &c, 1)) {
        state->cursor_col++;
        state->desired_col = state->cursor_col;
        mark_modified(state);
        ensure_cursor_visible(state);
    }
}

static void insert_spaces(AppState *state, int count)
{
    for (int i = 0; i < count; i++)
        insert_text_char(state, ' ');
}

static void insert_newline(AppState *state)
{
    if (!state || !state->buffer)
        return;
    TextBuffer *buffer = state->buffer;
    TextLine &line = buffer->lines[state->cursor_line];
    int indent = leading_indent(line);
    bool opener_before = state->cursor_col > 0 && matching_close(line.text[state->cursor_col - 1]) != 0;
    bool closer_after = state->cursor_col < line.len && matching_open(line.text[state->cursor_col]) != 0;
    int new_indent = indent + (opener_before ? DEFAULT_TAB_SPACES : 0);

    char after[MAX_LINE_LEN];
    int after_len = line.len - state->cursor_col;
    if (after_len < 0)
        after_len = 0;
    memcpy(after, line.text + state->cursor_col, (size_t)after_len);
    after[after_len] = '\0';
    line.len = (uint16_t)state->cursor_col;
    line.text[line.len] = '\0';

    if (closer_after && opener_before && buffer->line_count + 2 <= MAX_LINES) {
        if (!insert_line(state, state->cursor_line + 1))
            return;
        TextLine &middle = buffer->lines[state->cursor_line + 1];
        int middle_indent = min_int(new_indent, MAX_LINE_LEN - 1);
        for (int i = 0; i < middle_indent; i++)
            middle.text[i] = ' ';
        middle.text[middle_indent] = '\0';
        middle.len = (uint16_t)middle_indent;

        if (!insert_line(state, state->cursor_line + 2))
            return;
        TextLine &closing = buffer->lines[state->cursor_line + 2];
        int close_indent = min_int(indent, MAX_LINE_LEN - 1);
        for (int i = 0; i < close_indent; i++)
            closing.text[i] = ' ';
        int copy_len = min_int(after_len, MAX_LINE_LEN - close_indent - 1);
        memcpy(closing.text + close_indent, after, (size_t)copy_len);
        closing.len = (uint16_t)(close_indent + copy_len);
        closing.text[closing.len] = '\0';

        state->cursor_line++;
        state->cursor_col = middle_indent;
    } else {
        if (!insert_line(state, state->cursor_line + 1))
            return;
        TextLine &next = buffer->lines[state->cursor_line + 1];
        int use_indent = min_int(new_indent, MAX_LINE_LEN - 1);
        for (int i = 0; i < use_indent; i++)
            next.text[i] = ' ';
        int copy_len = min_int(after_len, MAX_LINE_LEN - use_indent - 1);
        memcpy(next.text + use_indent, after, (size_t)copy_len);
        next.len = (uint16_t)(use_indent + copy_len);
        next.text[next.len] = '\0';
        state->cursor_line++;
        state->cursor_col = use_indent;
    }

    state->desired_col = state->cursor_col;
    mark_modified(state);
    build_outline(state);
    ensure_cursor_visible(state);
}

static void backspace(AppState *state)
{
    if (!state || !state->buffer)
        return;
    TextLine *line = current_line(state);
    if (!line)
        return;
    if (state->cursor_col > 0) {
        char before = line->text[state->cursor_col - 1];
        char after = state->cursor_col < line->len ? line->text[state->cursor_col] : 0;
        char close = matching_close(before);
        if (close != 0 && close == after) {
            line_delete_range(line, state->cursor_col - 1, 2);
            state->cursor_col--;
        } else {
            line_delete_range(line, state->cursor_col - 1, 1);
            state->cursor_col--;
        }
        state->desired_col = state->cursor_col;
        mark_modified(state);
        build_outline(state);
        ensure_cursor_visible(state);
        return;
    }

    if (state->cursor_line > 0) {
        TextLine &prev = state->buffer->lines[state->cursor_line - 1];
        if ((int)prev.len + (int)line->len > MAX_LINE_LEN - 1)
            return;
        int old_len = prev.len;
        memcpy(prev.text + prev.len, line->text, (size_t)line->len + 1);
        prev.len = (uint16_t)(prev.len + line->len);
        delete_line(state, state->cursor_line);
        state->cursor_line--;
        state->cursor_col = old_len;
        state->desired_col = state->cursor_col;
        mark_modified(state);
        build_outline(state);
        ensure_cursor_visible(state);
    }
}

static void delete_forward(AppState *state)
{
    if (!state || !state->buffer)
        return;
    TextLine *line = current_line(state);
    if (!line)
        return;
    if (state->cursor_col < line->len) {
        line_delete_range(line, state->cursor_col, 1);
        mark_modified(state);
        build_outline(state);
        ensure_cursor_visible(state);
        return;
    }
    if (state->cursor_line + 1 < state->buffer->line_count) {
        TextLine &next = state->buffer->lines[state->cursor_line + 1];
        if ((int)line->len + (int)next.len > MAX_LINE_LEN - 1)
            return;
        memcpy(line->text + line->len, next.text, (size_t)next.len + 1);
        line->len = (uint16_t)(line->len + next.len);
        delete_line(state, state->cursor_line + 1);
        mark_modified(state);
        build_outline(state);
        ensure_cursor_visible(state);
    }
}

static void move_cursor(AppState *state, int line_delta, int col_delta, bool preserve_desired)
{
    if (!state || !state->buffer)
        return;
    if (!preserve_desired)
        state->desired_col = state->cursor_col;
    if (line_delta != 0) {
        state->cursor_line = clamp_int(state->cursor_line + line_delta, 0, state->buffer->line_count - 1);
        int desired = preserve_desired ? state->desired_col : state->cursor_col;
        state->cursor_col = clamp_int(desired, 0, state->buffer->lines[state->cursor_line].len);
    }
    if (col_delta != 0) {
        state->cursor_col += col_delta;
        while (state->cursor_col < 0 && state->cursor_line > 0) {
            state->cursor_line--;
            state->cursor_col += state->buffer->lines[state->cursor_line].len + 1;
        }
        while (state->cursor_line < state->buffer->line_count &&
               state->cursor_col > state->buffer->lines[state->cursor_line].len) {
            if (state->cursor_line + 1 >= state->buffer->line_count) {
                state->cursor_col = state->buffer->lines[state->cursor_line].len;
                break;
            }
            state->cursor_col -= state->buffer->lines[state->cursor_line].len + 1;
            state->cursor_line++;
        }
        clamp_cursor(state);
        state->desired_col = state->cursor_col;
    }
    ensure_cursor_visible(state);
    state->needs_redraw = true;
}

static bool write_all(int fd, const char *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int n = write(fd, data + written, len - written);
        if (n <= 0)
            return false;
        written += (size_t)n;
    }
    return true;
}

static bool save_file(AppState *state)
{
    if (!state || !state->buffer)
        return false;
    if (state->path_focused && !set_buffer_path_from_input(state, false))
        return false;
    if (!state->buffer->path[0]) {
        char default_path[512];
        const char *base =
            state->project_path[0] && strcmp(state->project_path, "/") != 0 ? state->project_path : "/data";
        join_path(base, "untitled.txt", default_path, sizeof(default_path));
        set_buffer_path(state, default_path, false);
    }

    if (!ensure_writable_storage(state, state->buffer->path)) {
        state->needs_redraw = true;
        return false;
    }

    VNodeStat existing = {};
    if (stat(state->buffer->path, &existing) == 0 && existing.is_dir) {
        set_status(state, "Save failed: path is a folder");
        state->needs_redraw = true;
        return false;
    }

    char parent[512];
    if (!parent_directory_is_available(state->buffer->path, parent, sizeof(parent))) {
        set_status(state, "Save failed: folder does not exist");
        state->needs_redraw = true;
        return false;
    }

    int fd = open_file_for_save(state->buffer->path);
    if (fd < 0) {
        if (storage_path_is_read_only(state->buffer->path))
            set_status(state, "Save failed: storage is read-only");
        else
            set_status(state, "Save failed: path is not writable");
        state->needs_redraw = true;
        return false;
    }

    bool ok = true;
    for (int i = 0; i < state->buffer->line_count; i++) {
        TextLine &line = state->buffer->lines[i];
        if (!write_all(fd, line.text, line.len)) {
            ok = false;
            break;
        }
        if (i + 1 < state->buffer->line_count && !write_all(fd, "\n", 1)) {
            ok = false;
            break;
        }
    }
    close(fd);
    if (!ok) {
        set_status(state, "Save failed while writing");
        state->needs_redraw = true;
        return false;
    }

    state->buffer->modified = false;
    copy_cstr(state->buffer->title, sizeof(state->buffer->title), path_basename(state->buffer->path));
    char status[160];
    snprintf(status, sizeof(status), "Saved %s", state->buffer->title);
    set_status(state, status);
    sync_path_input(state);
    char folder[512];
    parent_path(state->buffer->path, folder, sizeof(folder));
    if (strcmp(folder, state->project_path) == 0)
        load_project(state, state->project_path);
    state->needs_redraw = true;
    return true;
}

static void new_file(AppState *state)
{
    if (!state || !state->buffer)
        return;
    char path[512];
    const char *base = state->project_path[0] && strcmp(state->project_path, "/") != 0 ? state->project_path : "/data";
    join_path(base, "untitled.txt", path, sizeof(path));
    reset_buffer(state->buffer, path);
    sync_path_input(state);
    clear_field_focus(state);
    state->cursor_line = 0;
    state->cursor_col = 0;
    state->desired_col = 0;
    state->first_line = 0;
    state->first_col = 0;
    build_outline(state);
    set_status(state, "New file");
    state->needs_redraw = true;
}

static bool append_loaded_char(TextBuffer *buffer, char c)
{
    if (!buffer)
        return false;
    if (buffer->line_count <= 0)
        buffer->line_count = 1;
    TextLine &line = buffer->lines[buffer->line_count - 1];
    if (c == '\r')
        return true;
    if (c == '\n') {
        if (buffer->line_count >= MAX_LINES) {
            buffer->truncated = true;
            return false;
        }
        buffer->line_count++;
        buffer->lines[buffer->line_count - 1].len = 0;
        buffer->lines[buffer->line_count - 1].text[0] = '\0';
        return true;
    }
    if (line.len + 1 >= MAX_LINE_LEN) {
        buffer->truncated = true;
        return true;
    }
    line.text[line.len++] = c;
    line.text[line.len] = '\0';
    return true;
}

static bool byte_is_textish(uint8_t c)
{
    if (c == '\t' || c == '\n' || c == '\r')
        return true;
    if (c >= 32)
        return true;
    return false;
}

static bool sniff_file_is_binary(const char *path, bool *out_binary)
{
    if (out_binary)
        *out_binary = false;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    uint8_t chunk[TEXT_SNIFF_BYTES];
    int n = read(fd, chunk, sizeof(chunk));
    close(fd);
    if (n < 0)
        return false;

    for (int i = 0; i < n; i++) {
        if (chunk[i] == 0 || !byte_is_textish(chunk[i])) {
            if (out_binary)
                *out_binary = true;
            return true;
        }
    }
    return true;
}

static bool load_file(AppState *state, const char *path)
{
    if (!state || !state->buffer || !path || !path[0])
        return false;

    VNodeStat st = {};
    if (stat(path, &st) != 0) {
        set_status(state, "Open failed");
        state->needs_redraw = true;
        return false;
    }
    if (st.is_dir) {
        load_project(state, path);
        return true;
    }

    FileKind kind = classify_file(path, false, st.size);
    if (!file_kind_opens_as_text(kind)) {
        char status[160];
        snprintf(status, sizeof(status), "%s not opened as text", file_kind_label(path, kind));
        set_status(state, status);
        state->needs_redraw = true;
        return false;
    }

    bool binary = false;
    if (!sniff_file_is_binary(path, &binary)) {
        set_status(state, "Open failed");
        state->needs_redraw = true;
        return false;
    }
    if (binary) {
        set_status(state, "Binary file not opened");
        state->needs_redraw = true;
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_status(state, "Open failed");
        state->needs_redraw = true;
        return false;
    }

    reset_buffer(state->buffer, path);
    state->buffer->line_count = 1;
    char chunk[1024];
    int n = 0;
    bool keep_reading = true;
    uint64_t loaded_bytes = 0;
    while (keep_reading && (n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < n; i++) {
            if (loaded_bytes++ >= MAX_OPEN_TEXT_BYTES) {
                state->buffer->truncated = true;
                keep_reading = false;
                break;
            }
            if (!append_loaded_char(state->buffer, chunk[i])) {
                keep_reading = false;
                break;
            }
        }
    }
    close(fd);

    state->buffer->modified = false;
    state->buffer->language = detect_language(path);
    copy_cstr(state->buffer->title, sizeof(state->buffer->title), path_basename(path));
    state->cursor_line = 0;
    state->cursor_col = 0;
    state->desired_col = 0;
    state->first_line = 0;
    state->first_col = 0;
    build_outline(state);

    char status[160];
    snprintf(status, sizeof(status), state->buffer->truncated ? "Opened %s (truncated)" : "Opened %s",
             state->buffer->title);
    set_status(state, status);
    state->focus = FOCUS_EDITOR;
    clear_field_focus(state);
    sync_path_input(state);
    state->needs_redraw = true;
    return true;
}

static int compare_project_rows(const ProjectRow &a, const ProjectRow &b)
{
    if (a.parent != b.parent)
        return a.parent ? -1 : 1;
    if (a.is_dir != b.is_dir)
        return a.is_dir ? -1 : 1;
    return strcmp(a.name, b.name);
}

static void sort_project_rows(AppState *state)
{
    if (!state)
        return;
    int first_sortable = state->project_count > 0 && state->project_rows[0].parent ? 1 : 0;
    for (int i = first_sortable + 1; i < state->project_count; i++) {
        ProjectRow row = state->project_rows[i];
        int j = i - 1;
        while (j >= first_sortable && compare_project_rows(state->project_rows[j], row) > 0) {
            state->project_rows[j + 1] = state->project_rows[j];
            j--;
        }
        state->project_rows[j + 1] = row;
    }
}

static void load_project(AppState *state, const char *path)
{
    if (!state)
        return;
    VNodeStat st = {};
    const char *target = path && path[0] ? path : "/data";
    if (stat(target, &st) != 0 || !st.is_dir)
        target = "/";

    copy_cstr(state->project_path, sizeof(state->project_path), target);
    state->project_count = 0;
    state->project_selected = -1;
    state->project_hovered = -1;
    state->project_first_row = 0;

    if (strcmp(state->project_path, "/") != 0 && state->project_count < MAX_PROJECT_ROWS) {
        ProjectRow &row = state->project_rows[state->project_count++];
        copy_cstr(row.name, sizeof(row.name), "..");
        parent_path(state->project_path, row.path, sizeof(row.path));
        row.kind = FILE_KIND_UNKNOWN;
        row.is_dir = true;
        row.parent = true;
    }

    int fd = open(state->project_path, O_RDONLY);
    if (fd < 0) {
        set_status(state, "Project folder unavailable");
        state->needs_redraw = true;
        return;
    }

    char name[256];
    while (state->project_count < MAX_PROJECT_ROWS && syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        ProjectRow &row = state->project_rows[state->project_count];
        memset(&row, 0, sizeof(row));
        copy_cstr(row.name, sizeof(row.name), name);
        join_path(state->project_path, name, row.path, sizeof(row.path));
        VNodeStat row_stat = {};
        bool has_stat = stat(row.path, &row_stat) == 0;
        row.is_dir = has_stat && row_stat.is_dir;
        row.kind = classify_file(row.path, row.is_dir, has_stat ? row_stat.size : 0);
        row.parent = false;
        state->project_count++;
    }
    close(fd);
    sort_project_rows(state);
    if (state->buffer && state->buffer->path[0]) {
        for (int i = 0; i < state->project_count; i++) {
            if (strcmp(state->project_rows[i].path, state->buffer->path) == 0) {
                state->project_selected = i;
                break;
            }
        }
    }
    state->needs_redraw = true;
}

static void open_project_row(AppState *state, int index)
{
    if (!state || index < 0 || index >= state->project_count)
        return;
    ProjectRow &row = state->project_rows[index];
    state->project_selected = index;
    if (row.is_dir) {
        load_project(state, row.path);
    } else {
        load_file(state, row.path);
    }
}

static bool keyword_match(const char *word, int len, const char *kw)
{
    return (int)strlen(kw) == len && strncmp(word, kw, (size_t)len) == 0;
}

static bool is_keyword(Language language, const char *word, int len)
{
    static const char *cpp_keywords[] = {"auto",     "bool",    "break",  "case",      "class",  "const",   "constexpr",
                                         "continue", "delete",  "do",     "else",      "enum",   "extern",  "false",
                                         "for",      "if",      "inline", "namespace", "new",    "nullptr", "private",
                                         "public",   "return",  "sizeof", "static",    "struct", "switch",  "template",
                                         "true",     "typedef", "using",  "void",      "while"};
    static const char *js_keywords[] = {"async", "await",  "break", "case",     "class", "const",  "else", "export",
                                        "false", "for",    "from",  "function", "if",    "import", "let",  "new",
                                        "null",  "return", "true",  "type",     "var",   "while"};
    static const char *py_keywords[] = {"and",  "as",     "break", "class",  "def",   "elif", "else", "False",
                                        "for",  "from",   "if",    "import", "in",    "None", "not",  "or",
                                        "pass", "return", "self",  "True",   "while", "with", "yield"};
    static const char *rust_keywords[] = {"as",    "async", "await", "break", "const",  "crate", "else",
                                          "enum",  "false", "fn",    "for",   "if",     "impl",  "let",
                                          "match", "mod",   "mut",   "pub",   "return", "self",  "struct",
                                          "trait", "true",  "use",   "where", "while"};
    static const char *css_keywords[] = {"align-items", "background", "border", "color",   "display",  "flex", "font",
                                         "grid",        "height",     "margin", "padding", "position", "width"};
    const char **list = nullptr;
    int count = 0;
    if (language == LANG_CPP) {
        list = cpp_keywords;
        count = (int)(sizeof(cpp_keywords) / sizeof(cpp_keywords[0]));
    } else if (language == LANG_JS) {
        list = js_keywords;
        count = (int)(sizeof(js_keywords) / sizeof(js_keywords[0]));
    } else if (language == LANG_PYTHON) {
        list = py_keywords;
        count = (int)(sizeof(py_keywords) / sizeof(py_keywords[0]));
    } else if (language == LANG_RUST) {
        list = rust_keywords;
        count = (int)(sizeof(rust_keywords) / sizeof(rust_keywords[0]));
    } else if (language == LANG_CSS) {
        list = css_keywords;
        count = (int)(sizeof(css_keywords) / sizeof(css_keywords[0]));
    }
    for (int i = 0; i < count; i++) {
        if (keyword_match(word, len, list[i]))
            return true;
    }
    return false;
}

static uint32_t syntax_text()
{
    return g_gui_style.text;
}
static uint32_t syntax_dim()
{
    return g_gui_style.text_dim;
}
static uint32_t syntax_muted()
{
    return g_gui_style.text_muted;
}
static uint32_t syntax_keyword()
{
    return 0xFF7FB7FFu;
}
static uint32_t syntax_string()
{
    return 0xFF7AD99Au;
}
static uint32_t syntax_number()
{
    return 0xFFFFB86Cu;
}
static uint32_t syntax_comment()
{
    return 0xFF6E8797u;
}
static uint32_t syntax_function()
{
    return 0xFFD7A8FFu;
}
static uint32_t syntax_punct()
{
    return 0xFF99A4B3u;
}
static uint32_t syntax_tag()
{
    return 0xFFFF8A8Au;
}

static bool search_covers_col(const AppState *state, const TextLine &line, int col)
{
    if (!state || !state->search[0] || col < 0 || col >= line.len)
        return false;
    int needle_len = (int)strlen(state->search);
    if (needle_len <= 0)
        return false;
    for (int i = 0; i + needle_len <= line.len; i++) {
        if (strncmp(line.text + i, state->search, (size_t)needle_len) == 0 && col >= i && col < i + needle_len)
            return true;
    }
    return false;
}

static bool is_line_comment_start(Language language, const char *text, int pos)
{
    if (language == LANG_PYTHON || language == LANG_SHELL)
        return text[pos] == '#';
    if (language == LANG_CPP || language == LANG_JS || language == LANG_RUST || language == LANG_CSS)
        return text[pos] == '/' && text[pos + 1] == '/';
    return false;
}

static bool language_has_block_comments(Language language)
{
    return language == LANG_CPP || language == LANG_JS || language == LANG_RUST || language == LANG_CSS;
}

static bool line_starts_inside_block_comment(const TextBuffer *buffer, int line_index)
{
    if (!buffer || !language_has_block_comments(buffer->language))
        return false;

    bool in_block_comment = false;
    for (int line = 0; line < line_index; line++) {
        const TextLine &text_line = buffer->lines[line];
        bool in_string = false;
        char quote = 0;
        for (int col = 0; col < text_line.len; col++) {
            char c = text_line.text[col];
            char next = col + 1 < text_line.len ? text_line.text[col + 1] : 0;
            if (in_block_comment) {
                if (c == '*' && next == '/') {
                    in_block_comment = false;
                    col++;
                }
                continue;
            }
            if (in_string) {
                if (c == quote && (col == 0 || text_line.text[col - 1] != '\\')) {
                    in_string = false;
                    quote = 0;
                }
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') {
                in_string = true;
                quote = c;
                continue;
            }
            if (c == '/' && next == '/')
                break;
            if (c == '/' && next == '*') {
                in_block_comment = true;
                col++;
            }
        }
    }
    return in_block_comment;
}

static void draw_mono_run(Surface *win, const GuiFont *font, const AppState *state, const TextLine &line,
                          int row_start_col, int start_col, int end_col, int x, int y, int cell_w, int cell_h,
                          uint32_t fg, uint32_t row_bg)
{
    if (!win || !font || !state)
        return;
    for (int col = start_col; col < end_col; col++) {
        if (col >= row_start_col) {
            uint32_t bg = search_covers_col(state, line, col) ? g_gui_style.accent_soft : row_bg;
            gui_draw_mono_cell(win, font, x + (col - row_start_col) * cell_w, y, cell_w, cell_h, line.text[col], fg,
                               bg);
        }
    }
}

static bool word_is_json_literal(const char *word, int len)
{
    return keyword_match(word, len, "true") || keyword_match(word, len, "false") || keyword_match(word, len, "null");
}

static void draw_code_line(Surface *win, const AppState *state, int line_index, int x, int y, int max_cols,
                           uint32_t row_bg)
{
    if (!win || !state || !state->buffer || line_index < 0 || line_index >= state->buffer->line_count)
        return;
    const TextLine &line = state->buffer->lines[line_index];
    const GuiFont *font = gui_font_mono();
    int cell_w = gui_font_mono_cell_width(font);
    int cell_h = gui_font_line_height(font);
    if (cell_w <= 0)
        cell_w = 8;
    if (cell_h <= 0)
        cell_h = 16;

    Language language = state->buffer->language;
    bool in_block_comment = line_starts_inside_block_comment(state->buffer, line_index);
    bool html_tag = false;
    bool markdown_heading = language == LANG_MARKDOWN && line.len > 0 && line.text[0] == '#';

    int start = state->first_col;
    int end = min_int(line.len, start + max_cols);
    for (int col = 0; col < end;) {
        char c = line.text[col];
        uint32_t fg = syntax_text();

        if (markdown_heading) {
            fg = (col == 0 || line.text[col - 1] == '#') ? syntax_keyword() : g_gui_style.text;
        } else if (language == LANG_HTML) {
            if (col + 3 < line.len && strncmp(line.text + col, "<!--", 4) == 0) {
                int comment_end = col + 4;
                while (comment_end + 2 < line.len && strncmp(line.text + comment_end, "-->", 3) != 0)
                    comment_end++;
                if (comment_end + 2 < line.len)
                    comment_end += 3;
                else
                    comment_end = line.len;
                int draw_end = min_int(comment_end, end);
                draw_mono_run(win, font, state, line, start, col, draw_end, x, y, cell_w, cell_h, syntax_comment(),
                              row_bg);
                col = draw_end;
                continue;
            }
            if (c == '<')
                html_tag = true;
            if (html_tag) {
                fg = (c == '<' || c == '>' || c == '/' || c == '=') ? syntax_punct() : syntax_tag();
                if (c == '"' || c == '\'') {
                    char q = c;
                    int string_start = col;
                    col++;
                    while (col < end) {
                        if (line.text[col] == q) {
                            col++;
                            break;
                        }
                        col++;
                    }
                    draw_mono_run(win, font, state, line, start, string_start, col, x, y, cell_w, cell_h,
                                  syntax_string(), row_bg);
                    continue;
                }
            }
            if (c == '>')
                html_tag = false;
        } else {
            if (!in_block_comment && is_line_comment_start(language, line.text, col)) {
                draw_mono_run(win, font, state, line, start, col, end, x, y, cell_w, cell_h, syntax_comment(), row_bg);
                return;
            }
            if (!in_block_comment && language_has_block_comments(language) && line.text[col] == '/' &&
                col + 1 < line.len && line.text[col + 1] == '*') {
                in_block_comment = true;
            }
            if (in_block_comment) {
                fg = syntax_comment();
                if (line.text[col] == '*' && col + 1 < line.len && line.text[col + 1] == '/') {
                    draw_mono_run(win, font, state, line, start, col, min_int(col + 2, end), x, y, cell_w, cell_h, fg,
                                  row_bg);
                    col += 2;
                    in_block_comment = false;
                    continue;
                }
            } else if (c == '"' || c == '\'' || c == '`') {
                int string_start = col;
                char quote = c;
                col++;
                while (col < end) {
                    if (line.text[col] == quote && line.text[col - 1] != '\\') {
                        col++;
                        break;
                    }
                    col++;
                }
                draw_mono_run(win, font, state, line, start, string_start, col, x, y, cell_w, cell_h, syntax_string(),
                              row_bg);
                continue;
            } else if (is_digit(c)) {
                int number_start = col;
                int number_end = col + 1;
                while (number_end < line.len &&
                       (is_digit(line.text[number_end]) || line.text[number_end] == '.' ||
                        ascii_lower(line.text[number_end]) == 'x' ||
                        (ascii_lower(line.text[number_end]) >= 'a' && ascii_lower(line.text[number_end]) <= 'f') ||
                        line.text[number_end] == '_'))
                    number_end++;
                int draw_end = min_int(number_end, end);
                draw_mono_run(win, font, state, line, start, number_start, draw_end, x, y, cell_w, cell_h,
                              syntax_number(), row_bg);
                col = draw_end;
                continue;
            } else if (is_alpha(c)) {
                int word_start = col;
                int word_end = col + 1;
                while (word_end < line.len && is_ident(line.text[word_end]))
                    word_end++;
                if (is_keyword(language, line.text + word_start, word_end - word_start) ||
                    (language == LANG_JSON && word_is_json_literal(line.text + word_start, word_end - word_start))) {
                    fg = syntax_keyword();
                } else {
                    int scan = word_end;
                    while (scan < line.len && is_space(line.text[scan]))
                        scan++;
                    fg = (scan < line.len && line.text[scan] == '(') ? syntax_function() : syntax_text();
                }
                int draw_end = min_int(word_end, end);
                draw_mono_run(win, font, state, line, start, word_start, draw_end, x, y, cell_w, cell_h, fg, row_bg);
                col = draw_end;
                continue;
            } else if (c == '#' && col == 0 && language == LANG_CPP) {
                int directive_end = col + 1;
                while (directive_end < line.len && !is_space(line.text[directive_end]))
                    directive_end++;
                int draw_end = min_int(directive_end, end);
                draw_mono_run(win, font, state, line, start, col, draw_end, x, y, cell_w, cell_h, syntax_keyword(),
                              row_bg);
                col = draw_end;
                continue;
            } else if (strchr("{}[]().,:;+-*/%=!<>|&", c) != nullptr) {
                fg = syntax_punct();
            }
        }

        if (col >= start) {
            uint32_t bg = search_covers_col(state, line, col) ? g_gui_style.accent_soft : row_bg;
            gui_draw_mono_cell(win, font, x + (col - start) * cell_w, y, cell_w, cell_h, c, fg, bg);
        }
        col++;
    }
}

static void copy_symbol(char *out, size_t out_size, const char *start, int max_len)
{
    if (!out || out_size == 0)
        return;
    int len = 0;
    while (len < max_len && start[len] && start[len] != '(' && start[len] != ':' && start[len] != '{' &&
           start[len] != '=' && start[len] != ' ' && start[len] != '\t') {
        len++;
    }
    if (len >= (int)out_size)
        len = (int)out_size - 1;
    memcpy(out, start, (size_t)len);
    out[len] = '\0';
}

static void push_outline(AppState *state, const char *badge, const char *label, int line)
{
    if (!state || state->outline_count >= MAX_OUTLINE_ROWS || !label || !label[0])
        return;
    OutlineRow &row = state->outline_rows[state->outline_count++];
    memset(&row, 0, sizeof(row));
    copy_cstr(row.badge, sizeof(row.badge), badge ? badge : "SYM");
    copy_cstr(row.label, sizeof(row.label), label);
    row.line = line;
}

static bool is_control_statement(const char *word)
{
    return strcmp(word, "if") == 0 || strcmp(word, "for") == 0 || strcmp(word, "while") == 0 ||
           strcmp(word, "switch") == 0 || strcmp(word, "catch") == 0;
}

static void build_outline(AppState *state)
{
    if (!state || !state->buffer)
        return;
    state->outline_count = 0;
    state->outline_hovered = -1;
    state->outline_first_row = 0;
    Language language = state->buffer->language;
    for (int i = 0; i < state->buffer->line_count && state->outline_count < MAX_OUTLINE_ROWS; i++) {
        const char *text = state->buffer->lines[i].text;
        while (*text == ' ' || *text == '\t')
            text++;
        if (!*text)
            continue;

        char name[96];
        memset(name, 0, sizeof(name));
        if (language == LANG_MARKDOWN && text[0] == '#') {
            int hashes = 0;
            while (text[hashes] == '#')
                hashes++;
            while (text[hashes] == ' ')
                hashes++;
            copy_symbol(name, sizeof(name), text + hashes, 80);
            if (name[0])
                push_outline(state, "H", name, i);
        } else if (language == LANG_PYTHON) {
            if (starts_with(text, "def ")) {
                copy_symbol(name, sizeof(name), text + 4, 80);
                push_outline(state, "DEF", name, i);
            } else if (starts_with(text, "class ")) {
                copy_symbol(name, sizeof(name), text + 6, 80);
                push_outline(state, "CLS", name, i);
            }
        } else if (language == LANG_RUST) {
            const char *fn = strstr(text, "fn ");
            if (fn) {
                copy_symbol(name, sizeof(name), fn + 3, 80);
                push_outline(state, "FN", name, i);
            } else if (starts_with(text, "struct ")) {
                copy_symbol(name, sizeof(name), text + 7, 80);
                push_outline(state, "ST", name, i);
            } else if (starts_with(text, "enum ")) {
                copy_symbol(name, sizeof(name), text + 5, 80);
                push_outline(state, "EN", name, i);
            }
        } else if (language == LANG_JS) {
            if (starts_with(text, "function ")) {
                copy_symbol(name, sizeof(name), text + 9, 80);
                push_outline(state, "FN", name, i);
            } else if (starts_with(text, "class ")) {
                copy_symbol(name, sizeof(name), text + 6, 80);
                push_outline(state, "CLS", name, i);
            } else {
                const char *arrow = strstr(text, "=>");
                if (arrow) {
                    const char *name_start = text;
                    if (starts_with(text, "const "))
                        name_start = text + 6;
                    else if (starts_with(text, "let "))
                        name_start = text + 4;
                    else if (starts_with(text, "var "))
                        name_start = text + 4;
                    while (*name_start == ' ' || *name_start == '\t')
                        name_start++;
                    copy_symbol(name, sizeof(name), name_start, 80);
                    if (name[0])
                        push_outline(state, "FN", name, i);
                }
            }
        } else if (language == LANG_CPP) {
            const char *paren = strchr(text, '(');
            if (paren && strchr(text, ')')) {
                const char *start_fn = paren;
                while (start_fn > text && (is_ident(*(start_fn - 1)) || *(start_fn - 1) == ':'))
                    start_fn--;
                copy_symbol(name, sizeof(name), start_fn, (int)(paren - start_fn));
                if (name[0] && !is_control_statement(name))
                    push_outline(state, "FN", name, i);
            } else if (starts_with(text, "class ") || starts_with(text, "struct ")) {
                const char *name_start = starts_with(text, "class ") ? text + 6 : text + 7;
                copy_symbol(name, sizeof(name), name_start, 80);
                push_outline(state, "TYPE", name, i);
            }
        }
    }
}

static void find_next(AppState *state)
{
    if (!state || !state->buffer || !state->search[0])
        return;
    int needle_len = (int)strlen(state->search);
    int start_line = state->cursor_line;
    int start_col = state->cursor_col + 1;
    for (int pass = 0; pass < 2; pass++) {
        int from = pass == 0 ? start_line : 0;
        int to = pass == 0 ? state->buffer->line_count : start_line + 1;
        for (int line = from; line < to; line++) {
            int col = (line == start_line && pass == 0) ? start_col : 0;
            TextLine &text_line = state->buffer->lines[line];
            while (col + needle_len <= text_line.len) {
                if (strncmp(text_line.text + col, state->search, (size_t)needle_len) == 0) {
                    state->cursor_line = line;
                    state->cursor_col = col;
                    state->desired_col = col;
                    ensure_cursor_visible(state);
                    set_status(state, "Match found");
                    state->needs_redraw = true;
                    return;
                }
                col++;
            }
        }
    }
    set_status(state, "No matches");
    state->needs_redraw = true;
}

static void draw_button_or_field(Surface *win, Rect rect, const char *label, bool primary, bool active, bool hovered)
{
    gui_app_draw_button(win, rect.x, rect.y, rect.w, rect.h, label, primary, active, hovered);
}

static uint32_t editor_bg()
{
    return g_gui_style.app_surface;
}

static bool ensure_render_surface(Surface *surface, uint32_t width, uint32_t height)
{
    if (!surface || width == 0 || height == 0)
        return false;

    if (surface->buffer && surface->width == width && surface->height == height)
        return true;

    if (surface->owns_buffer && surface->buffer)
        free(surface->buffer);

    *surface = gui_create_surface(width, height);
    return surface->buffer != nullptr;
}

static void present_latitude_frame(Surface *window, Surface *frame)
{
    if (!window || !window->buffer || !frame || !frame->buffer)
        return;

    int w = min_int((int)window->width, (int)frame->width);
    int h = min_int((int)window->height, (int)frame->height);
    if (w <= 0 || h <= 0)
        return;

    // Keep the copy pitch-correct after maximize/resize. The window backing can
    // be wider than the visible client area, so a flat memcpy or old libgui fast
    // blit path shears the image diagonally.
    uint32_t dst_pitch = window->pitch / 4u;
    uint32_t src_pitch = frame->pitch / 4u;
    if (dst_pitch == 0 || src_pitch == 0)
        return;

    size_t row_bytes = (size_t)w * sizeof(uint32_t);
    for (int y = 0; y < h; y++) {
        memcpy(&window->buffer[(size_t)y * dst_pitch], &frame->buffer[(size_t)y * src_pitch], row_bytes);
    }

    asm volatile("sfence" ::: "memory");
    gui_blit_to_screen_rect(window, 0, 0, w, h);
}

static int latitude_view_width(Surface *win)
{
    int w = win ? (int)win->width : 0;
    if (g_my_window && g_my_window->w > 0)
        w = g_my_window->w;
    return max_int(1, w);
}

static int latitude_view_height(Surface *win)
{
    int h = win ? (int)win->height : 0;
    if (g_my_window && g_my_window->h > 0)
        h = g_my_window->h;
    return max_int(1, h);
}

static int draw_status_pill(Surface *win, int x, int y, const char *label, bool active)
{
    if (!win || !label)
        return 0;
    int text_w = gui_measure_text(gui_font_default(), label);
    int pad_x = gui_space_1();
    int h = gui_badge_h();
    int w = text_w + pad_x * 2;
    uint32_t bg = active ? g_gui_style.accent_soft : g_gui_style.chrome_bg;
    uint32_t fg = active ? g_gui_style.text : g_gui_style.text_dim;
    gui_fill_rounded_rect(win, x, y, w, h, h / 2, bg);
    gui_draw_text_clipped(win, gui_font_default(), x + pad_x, gui_align_text_y(gui_font_default(), y, h), text_w, label,
                          fg, bg);
    return w;
}

static void draw_toolbar_hint(Surface *win, int x, int y, int w, const char *hint)
{
    if (!win || !hint || w <= 0)
        return;
    gui_draw_text_clipped(win, gui_font_default(), x, y, w, hint, g_gui_style.text_muted, g_gui_style.chrome_bg);
}

static void draw_minimap(Surface *win, const AppState *state, Rect rect, int first_line, int visible_lines)
{
    if (!win || !state || !state->buffer || rect.w <= 0 || rect.h <= 0)
        return;

    gui_fill_rect(win, rect.x, rect.y, rect.w, rect.h, g_gui_style.app_surface_alt);
    gui_fill_rect(win, rect.x, rect.y, 1, rect.h, g_gui_style.chrome_edge);

    int max_rows = max_int(1, rect.h / 2);
    int count = max_int(max_rows, state->buffer->line_count);
    int step = max_int(1, (count + max_rows - 1) / max_rows);
    int inner_x = rect.x + gui_scaled_metric(4);
    int inner_w = max_int(2, rect.w - gui_scaled_metric(8));

    for (int line = 0; line < state->buffer->line_count; line += step) {
        const TextLine &text = state->buffer->lines[line];
        int y = rect.y + (line * rect.h) / count;
        if (y >= rect.y + rect.h)
            break;
        int used = text.len;
        int mark_w = min_int(inner_w, max_int(2, (used * inner_w) / 96));
        uint32_t color = line == state->cursor_line ? g_gui_style.accent : g_gui_style.text_muted;
        if (text.len == 0)
            color = g_gui_style.chrome_edge;
        gui_fill_rect(win, inner_x, y, mark_w, max_int(1, gui_scaled_metric(1)), color);
    }

    if (visible_lines > 0) {
        int y0 = rect.y + (first_line * rect.h) / count;
        int y1 = rect.y + ((first_line + visible_lines) * rect.h) / count;
        if (y1 <= y0)
            y1 = y0 + gui_scaled_metric(8);
        if (y1 > rect.y + rect.h)
            y1 = rect.y + rect.h;
        gui_draw_rounded_rect(win, rect.x + 2, y0, rect.w - 4, y1 - y0, gui_radius_sm(), g_gui_style.border_focus);
    }
}

static void draw_latitude(Surface *win, AppState *state, LatitudeRects *rects)
{
    if (!win || !state || !state->buffer || !rects)
        return;

    memset(rects, 0, sizeof(*rects));

    int view_w = latitude_view_width(win);
    int view_h = latitude_view_height(win);
    gui_fill_rect(win, 0, 0, view_w, view_h, g_gui_style.app_bg);

    const int margin = view_w >= gui_scaled_metric(1100) ? gui_scaled_metric(12) : gui_scaled_metric(8);
    const int gap = gui_scaled_metric(10);
    const int topbar_h = gui_scaled_metric(44);
    const int min_bottom = gui_scaled_metric(8);

    Rect topbar = gui_rect_make(margin, margin, max_int(0, view_w - margin * 2), topbar_h);
    gui_draw_panel_inset(win, topbar.x, topbar.y, topbar.w, topbar.h, g_gui_style.app_surface_alt, g_gui_style.border,
                         g_gui_style.chrome_bg_alt);

    char title_detail[160];
    snprintf(title_detail, sizeof(title_detail), "%s  Ln %d, Col %d", language_label(state->buffer->language),
             state->cursor_line + 1, state->cursor_col + 1);

    int top_text_y = gui_align_text_y(gui_font_title(), topbar.y, topbar.h);
    int title_text_w = gui_measure_text(gui_font_title(), "Latitude");
    gui_draw_text_clipped(win, gui_font_title(), topbar.x + gui_space_2(), top_text_y, title_text_w, "Latitude",
                          g_gui_style.text, g_gui_style.app_surface_alt);

    const char *path_text = state->buffer->path[0] ? state->buffer->path : "Untitled";
    int path_x = topbar.x + max_int(gui_scaled_metric(118), gui_space_2() + title_text_w + gui_space_3());
    int right_w = gui_measure_text(gui_font_default(), title_detail) + gui_space_3();

    int pills_w = 0;
    pills_w += gui_measure_text(gui_font_default(), "Ctrl+S Save") + gui_space_1() * 3;
    pills_w += gui_measure_text(gui_font_default(), "Ctrl+F Search") + gui_space_1() * 3;
    pills_w += gui_measure_text(gui_font_default(), "Ctrl+L Path") + gui_space_1() * 2;

    int pill_x = topbar.x + topbar.w - right_w - pills_w - gui_space_2();
    int path_w = topbar.w - (path_x - topbar.x) - right_w - gui_space_2();

    if (path_w < gui_scaled_metric(80)) {
        path_w = gui_scaled_metric(80);
    }

    if (pill_x <= path_x + path_w) {
        pill_x = -1;
    } else {
        path_w = pill_x - path_x - gui_space_2();
    }

    gui_draw_text_clipped(win, gui_font_default(), path_x, gui_align_text_y(gui_font_default(), topbar.y, topbar.h),
                          path_w, path_text, g_gui_style.text_dim, g_gui_style.app_surface_alt);
    gui_draw_text_clipped(win, gui_font_default(), topbar.x + topbar.w - right_w,
                          gui_align_text_y(gui_font_default(), topbar.y, topbar.h), right_w - gui_space_2(),
                          state->buffer->modified ? "Modified" : title_detail, g_gui_style.text_dim,
                          g_gui_style.app_surface_alt);

    if (pill_x >= 0) {
        int pill_y = topbar.y + (topbar.h - gui_badge_h()) / 2;
        pill_x += draw_status_pill(win, pill_x, pill_y, "Ctrl+S Save", false) + gui_space_1();
        pill_x += draw_status_pill(win, pill_x, pill_y, "Ctrl+F Search", state->search_focused) + gui_space_1();
        draw_status_pill(win, pill_x, pill_y, "Ctrl+L Path", state->path_focused);
    }

    int body_x = margin;
    int body_y = topbar.y + topbar.h + gap;
    int body_w = max_int(0, view_w - margin * 2);
    int body_h = max_int(0, view_h - body_y - min_bottom);

    int min_sidebar = gui_scaled_metric(188);
    int max_sidebar = gui_scaled_metric(330);
    int sidebar_w = (body_w * 23) / 100;
    sidebar_w = clamp_int(sidebar_w, min_sidebar, max_sidebar);
    if (body_w < gui_scaled_metric(820))
        sidebar_w = clamp_int(body_w / 3, gui_scaled_metric(156), gui_scaled_metric(220));
    if (body_w - sidebar_w - gap < gui_scaled_metric(380))
        sidebar_w = max_int(0, body_w - gap - gui_scaled_metric(380));

    int editor_x = body_x + sidebar_w + gap;
    int editor_w = body_w - sidebar_w - gap;
    if (sidebar_w <= 0 || editor_w < gui_scaled_metric(280)) {
        sidebar_w = 0;
        editor_x = body_x;
        editor_w = body_w;
    }

    rects->sidebar_rect = gui_rect_make(body_x, body_y, sidebar_w, body_h);
    rects->editor_panel = gui_rect_make(editor_x, body_y, editor_w, body_h);

    if (sidebar_w > 0) {
        gui_draw_panel_inset(win, rects->sidebar_rect.x, rects->sidebar_rect.y, rects->sidebar_rect.w,
                             rects->sidebar_rect.h, g_gui_style.app_surface, g_gui_style.border,
                             g_gui_style.chrome_bg_alt);

        int side_header_h = gui_scaled_metric(34);
        gui_fill_rect(win, rects->sidebar_rect.x + 1, rects->sidebar_rect.y + 1, rects->sidebar_rect.w - 2,
                      side_header_h, g_gui_style.chrome_bg);
        gui_draw_text_clipped(win, gui_font_title(), rects->sidebar_rect.x + gui_space_2(),
                              gui_align_text_y(gui_font_title(), rects->sidebar_rect.y, side_header_h),
                              rects->sidebar_rect.w - gui_space_4(), "Explorer", g_gui_style.text,
                              g_gui_style.chrome_bg);
        gui_draw_separator_h(win, rects->sidebar_rect.x + 1, rects->sidebar_rect.y + side_header_h,
                             rects->sidebar_rect.w - 2, g_gui_style.chrome_edge);

        int sy = rects->sidebar_rect.y + side_header_h + gui_space_1();
        gui_draw_text_clipped(win, gui_font_default(), rects->sidebar_rect.x + gui_space_2(), sy,
                              rects->sidebar_rect.w - gui_space_4(), state->project_path, g_gui_style.text_dim,
                              g_gui_style.app_surface);
        sy += gui_line_height() + gui_space_1();

        int outline_header_h = gui_scaled_metric(30);
        int max_project_area = max_int(gui_scaled_metric(150), (rects->sidebar_rect.h * 58) / 100);
        int needed_project_h = (sy - rects->sidebar_rect.y) + state->project_count * (gui_app_row_h() + 2);
        int project_area_bottom = rects->sidebar_rect.y + min_int(max_project_area, needed_project_h);

        if (project_area_bottom > rects->sidebar_rect.y + rects->sidebar_rect.h - gui_scaled_metric(90))
            project_area_bottom = rects->sidebar_rect.y + rects->sidebar_rect.h - gui_scaled_metric(90);

        for (int i = state->project_first_row; i < state->project_count && sy + gui_app_row_h() <= project_area_bottom;
             i++) {
            rects->project_rows[i] =
                gui_rect_make(rects->sidebar_rect.x + 1, sy, rects->sidebar_rect.w - 2, gui_app_row_h());
            FileKind kind = state->project_rows[i].kind;
            const char *badge =
                state->project_rows[i].is_dir ? (state->project_rows[i].parent ? "UP" : "DIR") : file_kind_badge(kind);
            const char *detail_text =
                state->project_rows[i].is_dir ? "Folder" : file_kind_label(state->project_rows[i].path, kind);
            bool muted = !state->project_rows[i].is_dir && !file_kind_opens_as_text(kind);
            gui_app_draw_list_row(win, rects->project_rows[i].x, rects->project_rows[i].y, rects->project_rows[i].w,
                                  rects->project_rows[i].h, badge, state->project_rows[i].name, detail_text,
                                  i == state->project_selected, i == state->project_hovered, muted);
            sy += gui_app_row_h() + 2;
        }

        int outline_y = project_area_bottom + gui_space_1();
        gui_draw_separator_h(win, rects->sidebar_rect.x + gui_space_2(), outline_y - gui_space_1(),
                             rects->sidebar_rect.w - gui_space_4(), g_gui_style.chrome_edge);
        gui_fill_rect(win, rects->sidebar_rect.x + 1, outline_y, rects->sidebar_rect.w - 2, outline_header_h,
                      g_gui_style.chrome_bg);
        gui_draw_text_clipped(win, gui_font_title(), rects->sidebar_rect.x + gui_space_2(),
                              gui_align_text_y(gui_font_title(), outline_y, outline_header_h),
                              rects->sidebar_rect.w - gui_space_4(), "Outline", g_gui_style.text,
                              g_gui_style.chrome_bg);
        gui_draw_separator_h(win, rects->sidebar_rect.x + 1, outline_y + outline_header_h, rects->sidebar_rect.w - 2,
                             g_gui_style.chrome_edge);

        sy = outline_y + outline_header_h + gui_space_1();
        for (int i = state->outline_first_row;
             i < state->outline_count && sy + gui_app_row_h() < rects->sidebar_rect.y + rects->sidebar_rect.h; i++) {
            rects->outline_rows[i] =
                gui_rect_make(rects->sidebar_rect.x + 1, sy, rects->sidebar_rect.w - 2, gui_app_row_h());
            char detail_line[32];
            snprintf(detail_line, sizeof(detail_line), "Line %d", state->outline_rows[i].line + 1);
            gui_app_draw_list_row(win, rects->outline_rows[i].x, rects->outline_rows[i].y, rects->outline_rows[i].w,
                                  rects->outline_rows[i].h, state->outline_rows[i].badge, state->outline_rows[i].label,
                                  detail_line, state->cursor_line == state->outline_rows[i].line,
                                  i == state->outline_hovered, false);
            sy += gui_app_row_h() + 2;
        }
    }

    gui_draw_panel_inset(win, rects->editor_panel.x, rects->editor_panel.y, rects->editor_panel.w,
                         rects->editor_panel.h, editor_bg(), g_gui_style.border, g_gui_style.chrome_bg_alt);

    int panel_x = rects->editor_panel.x;
    int panel_y = rects->editor_panel.y;
    int panel_w = rects->editor_panel.w;
    int panel_h = rects->editor_panel.h;
    int tab_h = gui_scaled_metric(34);
    int toolbar_h = gui_scaled_metric(38);
    int status_h = gui_scaled_metric(24);

    gui_fill_rect(win, panel_x + 1, panel_y + 1, panel_w - 2, tab_h, g_gui_style.chrome_bg);
    gui_draw_separator_h(win, panel_x + 1, panel_y + tab_h, panel_w - 2, g_gui_style.chrome_edge);

    int tab_x = panel_x + gui_space_1();
    int tab_w = min_int(gui_scaled_metric(260), max_int(gui_scaled_metric(130), panel_w / 3));
    int tab_radius = gui_radius_sm();
    gui_fill_rounded_rect(win, tab_x, panel_y + gui_scaled_metric(5), tab_w, tab_h - gui_scaled_metric(5), tab_radius,
                          g_gui_style.app_surface);
    gui_fill_rect(win, tab_x, panel_y + tab_h - tab_radius, tab_w, tab_radius, g_gui_style.app_surface);

    char tab_label[160];
    snprintf(tab_label, sizeof(tab_label), "%s%s", state->buffer->title, state->buffer->modified ? " *" : "");
    gui_draw_text_clipped(win, gui_font_default(), tab_x + gui_space_1(),
                          gui_align_text_y(gui_font_default(), panel_y, tab_h), tab_w - gui_space_2(), tab_label,
                          g_gui_style.text, g_gui_style.app_surface);

    int toolbar_y = panel_y + tab_h;
    gui_fill_rect(win, panel_x + 1, toolbar_y + 1, panel_w - 2, toolbar_h - 1, g_gui_style.app_surface_alt);
    int button_y = toolbar_y + (toolbar_h - gui_app_control_h()) / 2;

    rects->new_button = gui_rect_make(panel_x + gui_space_1(), button_y, gui_scaled_metric(58), gui_app_control_h());
    rects->save_button = gui_rect_make(rects->new_button.x + rects->new_button.w + gui_space_1(), button_y,
                                       gui_scaled_metric(58), gui_app_control_h());
    rects->reload_button = gui_rect_make(rects->save_button.x + rects->save_button.w + gui_space_1(), button_y,
                                         gui_scaled_metric(70), gui_app_control_h());

    draw_button_or_field(win, rects->new_button, "New", false, false, state->hovered == HOVER_NEW_FILE);
    draw_button_or_field(win, rects->save_button, "Save", true, false, state->hovered == HOVER_SAVE);
    draw_button_or_field(win, rects->reload_button, "Reload", false, false, state->hovered == HOVER_RELOAD);

    int search_w = min_int(gui_scaled_metric(210), max_int(gui_scaled_metric(120), panel_w / 5));
    if (state->search_focused) {
        search_w = max_int(search_w, gui_scaled_metric(160));
    } else if (state->path_focused) {
        search_w = gui_scaled_metric(80);
    }

    rects->search_field =
        gui_rect_make(panel_x + panel_w - search_w - gui_space_1(), button_y, search_w, gui_app_control_h());
    int path_field_x = rects->reload_button.x + rects->reload_button.w + gui_space_1();
    int path_field_w = rects->search_field.x - path_field_x - gui_space_1();

    if (path_field_w < gui_scaled_metric(64) && !state->path_focused) {
        search_w = min_int(gui_scaled_metric(160), max_int(gui_scaled_metric(96), panel_w / 6));
        rects->search_field =
            gui_rect_make(panel_x + panel_w - search_w - gui_space_1(), button_y, search_w, gui_app_control_h());
        path_field_w = rects->search_field.x - path_field_x - gui_space_1();
    }

    if (path_field_w > gui_scaled_metric(32)) {
        rects->path_field = gui_rect_make(path_field_x, button_y, path_field_w, gui_app_control_h());
        const char *path_value = (state->path_input[0] || state->path_focused) ? state->path_input : "File path";
        gui_app_draw_text_field(win, rects->path_field.x, rects->path_field.y, rects->path_field.w, rects->path_field.h,
                                path_value, state->path_focused, state->hovered == HOVER_PATH);
    } else {
        rects->path_field = gui_rect_make(0, 0, 0, 0);
    }

    const char *search_value = (state->search[0] || state->search_focused) ? state->search : "Search";
    gui_app_draw_text_field(win, rects->search_field.x, rects->search_field.y, rects->search_field.w,
                            rects->search_field.h, search_value, state->search_focused, state->hovered == HOVER_SEARCH);

    int status_y = panel_y + panel_h - status_h;
    int edit_y = panel_y + tab_h + toolbar_h + 1;
    int edit_h = status_y - edit_y;
    if (edit_h < gui_scaled_metric(24))
        edit_h = gui_scaled_metric(24);

    int minimap_w = panel_w >= gui_scaled_metric(760) ? gui_scaled_metric(76) : 0;
    int gutter_w = gui_scaled_metric(56);
    Rect edit_area = gui_rect_make(panel_x + 1, edit_y, panel_w - 2 - minimap_w, edit_h);
    Rect minimap_rect = gui_rect_make(edit_area.x + edit_area.w, edit_y, minimap_w, edit_h);
    rects->editor_rect = edit_area;

    gui_fill_rect(win, edit_area.x, edit_area.y, edit_area.w, edit_area.h, editor_bg());
    // Separators and gutter fill
    gui_fill_rect(win, edit_area.x, edit_area.y, min_int(gutter_w, edit_area.w), edit_area.h,
                  g_gui_style.app_surface_alt);
    gui_draw_separator_h(win, panel_x + 1, edit_area.y - 1, panel_w - 2, g_gui_style.chrome_edge);
    if (edit_area.w > gutter_w)
        gui_fill_rect(win, edit_area.x + gutter_w - 1, edit_area.y - 1, 1, edit_area.h + 1, g_gui_style.chrome_edge);

    const GuiFont *mono = gui_font_mono();
    int cell_w = gui_font_mono_cell_width(mono);
    int cell_h = gui_font_line_height(mono);
    if (cell_w <= 0)
        cell_w = 8;
    if (cell_h <= 0)
        cell_h = 16;

    state->visible_lines = max_int(1, edit_h / cell_h);
    state->visible_cols = max_int(1, (edit_area.w - gutter_w - gui_space_1()) / cell_w);
    ensure_cursor_visible(state);

    int text_x = edit_area.x + gutter_w + gui_space_1();
    for (int visual = 0; visual < state->visible_lines; visual++) {
        int line_index = state->first_line + visual;
        int row_y = edit_area.y + visual * cell_h;
        if (row_y + cell_h > edit_area.y + edit_area.h)
            break;
        uint32_t row_bg = line_index == state->cursor_line ? g_gui_style.app_surface_alt : editor_bg();
        if (line_index == state->cursor_line && edit_area.w > gutter_w)
            gui_fill_rect(win, edit_area.x + gutter_w, row_y, edit_area.w - gutter_w, cell_h, row_bg);
        if (line_index < state->buffer->line_count) {
            char line_no[16];
            snprintf(line_no, sizeof(line_no), "%d", line_index + 1);
            int number_w = gui_measure_text(gui_font_default(), line_no);
            gui_draw_text_clipped(win, gui_font_default(), edit_area.x + gutter_w - gui_space_1() - number_w,
                                  gui_align_text_y(gui_font_default(), row_y, cell_h), number_w, line_no,
                                  line_index == state->cursor_line ? g_gui_style.text : syntax_muted(),
                                  g_gui_style.app_surface_alt);
            draw_code_line(win, state, line_index, text_x, row_y, state->visible_cols, row_bg);
        }
    }

    if (state->focus == FOCUS_EDITOR && state->cursor_line >= state->first_line &&
        state->cursor_line < state->first_line + state->visible_lines && state->cursor_col >= state->first_col &&
        state->cursor_col <= state->first_col + state->visible_cols) {
        int caret_x = text_x + (state->cursor_col - state->first_col) * cell_w;
        int caret_y = edit_area.y + (state->cursor_line - state->first_line) * cell_h;
        gui_fill_rect(win, caret_x, caret_y + 2, gui_scaled_metric(2), cell_h - 4, g_gui_style.accent);
    }

    if (state->focus == FOCUS_EDITOR) {
        // Draw a sharp focus frame to avoid rounding artifacts over the gutter
        gui_draw_rect(win, panel_x + 1, edit_y, panel_w - 2, edit_h, g_gui_style.border_focus);
        if (panel_w > 4 && edit_h > 4) {
            gui_draw_rect(win, panel_x + 2, edit_y + 1, panel_w - 4, edit_h - 2, g_gui_style.accent_soft);
        }
    }

    if (minimap_w > 0)
        draw_minimap(win, state, minimap_rect, state->first_line, state->visible_lines);

    gui_fill_rect(win, panel_x + 1, status_y, panel_w - 2, status_h - 1, g_gui_style.chrome_bg);
    gui_draw_separator_h(win, panel_x + 1, status_y, panel_w - 2, g_gui_style.chrome_edge);

    char left_status[224];
    snprintf(left_status, sizeof(left_status), "%s%s", state->status, state->buffer->modified ? "  Unsaved" : "");

    char right_status[128];
    snprintf(right_status, sizeof(right_status), "%s  Spaces:%d  %d lines", language_label(state->buffer->language),
             DEFAULT_TAB_SPACES, state->buffer->line_count);

    int rw = gui_measure_text(gui_font_default(), right_status);
    int right_status_x = panel_x + panel_w - gui_space_1() - rw;
    int max_left_w = max_int(40, right_status_x - (panel_x + gui_space_1()) - gui_space_2());

    gui_draw_text_clipped(win, gui_font_default(), panel_x + gui_space_1(),
                          gui_align_text_y(gui_font_default(), status_y, status_h), max_left_w, left_status,
                          g_gui_style.text_dim, g_gui_style.chrome_bg);

    gui_draw_text_clipped(win, gui_font_default(), right_status_x,
                          gui_align_text_y(gui_font_default(), status_y, status_h), rw, right_status,
                          g_gui_style.text_dim, g_gui_style.chrome_bg);
}

static HoverTarget update_hover(AppState *state, LatitudeRects *rects, int x, int y)
{
    if (!state || !rects)
        return HOVER_NONE;
    state->project_hovered = -1;
    state->outline_hovered = -1;
    for (int i = 0; i < state->project_count; i++) {
        if (!gui_rect_is_empty(rects->project_rows[i]) && point_in_rect(rects->project_rows[i], x, y)) {
            state->project_hovered = i;
            return HOVER_PROJECT_ROW;
        }
    }
    for (int i = 0; i < state->outline_count; i++) {
        if (!gui_rect_is_empty(rects->outline_rows[i]) && point_in_rect(rects->outline_rows[i], x, y)) {
            state->outline_hovered = i;
            return HOVER_OUTLINE_ROW;
        }
    }
    if (point_in_rect(rects->new_button, x, y))
        return HOVER_NEW_FILE;
    if (point_in_rect(rects->save_button, x, y))
        return HOVER_SAVE;
    if (point_in_rect(rects->reload_button, x, y))
        return HOVER_RELOAD;
    if (point_in_rect(rects->path_field, x, y))
        return HOVER_PATH;
    if (point_in_rect(rects->search_field, x, y))
        return HOVER_SEARCH;
    if (point_in_rect(rects->editor_rect, x, y))
        return HOVER_EDITOR;
    return HOVER_NONE;
}

static void jump_to_outline(AppState *state, int index)
{
    if (!state || index < 0 || index >= state->outline_count)
        return;
    state->cursor_line = clamp_int(state->outline_rows[index].line, 0, state->buffer->line_count - 1);
    state->cursor_col = 0;
    state->desired_col = 0;
    state->focus = FOCUS_EDITOR;
    ensure_cursor_visible(state);
    state->needs_redraw = true;
}

static void click_editor(AppState *state, LatitudeRects *rects, int x, int y)
{
    if (!state || !rects || !point_in_rect(rects->editor_rect, x, y))
        return;
    const GuiFont *mono = gui_font_mono();
    int cell_w = gui_font_mono_cell_width(mono);
    int cell_h = gui_font_line_height(mono);
    if (cell_w <= 0)
        cell_w = 8;
    if (cell_h <= 0)
        cell_h = 16;
    int gutter_w = gui_scaled_metric(56);
    int text_x = rects->editor_rect.x + gutter_w + gui_space_1();
    int line = state->first_line + (y - rects->editor_rect.y) / cell_h;
    int col = state->first_col + max_int(0, (x - text_x) / cell_w);
    if (line >= state->buffer->line_count)
        line = state->buffer->line_count - 1;
    if (line < 0)
        line = 0;
    col = clamp_int(col, 0, state->buffer->lines[line].len);
    state->cursor_line = line;
    state->cursor_col = col;
    state->desired_col = col;
    state->focus = FOCUS_EDITOR;
    clear_field_focus(state);
    ensure_cursor_visible(state);
    state->needs_redraw = true;
}

static void commit_path_input_for_open(AppState *state)
{
    char resolved[512];
    if (!resolve_input_path(state, state ? state->path_input : nullptr, resolved, sizeof(resolved))) {
        set_status(state, "Enter a file path");
        if (state)
            state->needs_redraw = true;
        return;
    }

    VNodeStat st = {};
    if (stat(resolved, &st) == 0) {
        if (st.is_dir) {
            load_project(state, resolved);
            clear_field_focus(state);
            state->focus = FOCUS_PROJECT;
            set_status(state, "Project folder opened");
            state->needs_redraw = true;
        } else {
            load_file(state, resolved);
        }
        return;
    }

    set_buffer_path(state, resolved, true);
    clear_field_focus(state);
    state->focus = FOCUS_EDITOR;
    set_status(state, "Path set for new file");
    state->needs_redraw = true;
}

static void handle_path_key(AppState *state, uint8_t c)
{
    if (!state)
        return;
    size_t len = strlen(state->path_input);
    if (c == 19) { // Ctrl+S
        if (set_buffer_path_from_input(state, false))
            save_file(state);
        return;
    }
    if (c == '\n' || c == '\r') {
        commit_path_input_for_open(state);
    } else if (c == 27) {
        sync_path_input(state);
        clear_field_focus(state);
        state->focus = FOCUS_EDITOR;
        state->needs_redraw = true;
    } else if ((c == '\b' || c == 127) && len > 0) {
        state->path_input[len - 1] = '\0';
        state->needs_redraw = true;
    } else if (c >= 32 && c <= 126 && len + 1 < sizeof(state->path_input)) {
        state->path_input[len] = (char)c;
        state->path_input[len + 1] = '\0';
        state->needs_redraw = true;
    }
}

static void move_project_selection(AppState *state, int delta)
{
    if (!state || state->project_count <= 0)
        return;

    int selected = state->project_selected;
    if (selected < 0)
        selected = 0;
    else
        selected = clamp_int(selected + delta, 0, state->project_count - 1);

    state->project_selected = selected;
    state->project_hovered = -1;
    state->focus = FOCUS_PROJECT;

    int visible_rows = 1;
    if (state->visible_lines > 0)
        visible_rows = max_int(1, state->visible_lines / 2);
    if (state->project_selected < state->project_first_row)
        state->project_first_row = state->project_selected;
    if (state->project_selected >= state->project_first_row + visible_rows)
        state->project_first_row = state->project_selected - visible_rows + 1;
    state->project_first_row = clamp_int(state->project_first_row, 0, max_int(0, state->project_count - 1));
    state->needs_redraw = true;
}

static void handle_key(AppState *state, uint8_t c)
{
    if (!state || !state->buffer || c == 0)
        return;

    if (state->path_focused) {
        handle_path_key(state, c);
        return;
    }

    if (state->search_focused) {
        size_t len = strlen(state->search);
        if (c == 19) { // Ctrl+S
            save_file(state);
        } else if (c == 12) { // Ctrl+L
            sync_path_input(state);
            state->path_focused = true;
            state->search_focused = false;
            state->focus = FOCUS_PATH;
            state->needs_redraw = true;
        } else if (c == '\n' || c == '\r') {
            find_next(state);
        } else if (c == 27) {
            clear_field_focus(state);
            state->focus = FOCUS_EDITOR;
            state->needs_redraw = true;
        } else if ((c == '\b' || c == 127) && len > 0) {
            state->search[len - 1] = '\0';
            state->needs_redraw = true;
        } else if (c >= 32 && c <= 126 && len + 1 < sizeof(state->search)) {
            state->search[len] = (char)c;
            state->search[len + 1] = '\0';
            state->needs_redraw = true;
        }
        return;
    }

    if (c == 19) { // Ctrl+S
        save_file(state);
        return;
    }
    if (c == 14) { // Ctrl+N
        new_file(state);
        return;
    }
    if (c == 6) { // Ctrl+F
        state->path_focused = false;
        state->search_focused = true;
        state->focus = FOCUS_SEARCH;
        state->needs_redraw = true;
        return;
    }
    if (c == 12) { // Ctrl+L
        sync_path_input(state);
        state->path_focused = true;
        state->search_focused = false;
        state->focus = FOCUS_PATH;
        state->needs_redraw = true;
        return;
    }
    if (c == 15) { // Ctrl+O opens the highlighted project row.
        if (state->project_selected >= 0)
            open_project_row(state, state->project_selected);
        return;
    }

    if (state->focus == FOCUS_PROJECT) {
        if (c == KEY_UP_ARROW) {
            move_project_selection(state, -1);
            return;
        }
        if (c == KEY_DOWN_ARROW) {
            move_project_selection(state, 1);
            return;
        }
        if (c == '\n' || c == '\r') {
            if (state->project_selected >= 0)
                open_project_row(state, state->project_selected);
            return;
        }
        if (c == 27 || c == KEY_RIGHT_ARROW) {
            state->focus = FOCUS_EDITOR;
            state->needs_redraw = true;
            return;
        }
    }

    if (c == KEY_UP_ARROW) {
        move_cursor(state, -1, 0, true);
    } else if (c == KEY_DOWN_ARROW) {
        move_cursor(state, 1, 0, true);
    } else if (c == KEY_LEFT_ARROW || c == KEY_SHIFT_LEFT) {
        move_cursor(state, 0, -1, false);
    } else if (c == KEY_RIGHT_ARROW || c == KEY_SHIFT_RIGHT) {
        move_cursor(state, 0, 1, false);
    } else if (c == KEY_HOME) {
        TextLine *line = current_line(state);
        state->cursor_col = line ? leading_indent(*line) : 0;
        state->desired_col = state->cursor_col;
        ensure_cursor_visible(state);
        state->needs_redraw = true;
    } else if (c == KEY_END) {
        TextLine *line = current_line(state);
        state->cursor_col = line ? line->len : 0;
        state->desired_col = state->cursor_col;
        ensure_cursor_visible(state);
        state->needs_redraw = true;
    } else if (c == KEY_PAGEUP) {
        move_cursor(state, -max_int(1, state->visible_lines - 1), 0, true);
    } else if (c == KEY_PAGEDOWN) {
        move_cursor(state, max_int(1, state->visible_lines - 1), 0, true);
    } else if (c == KEY_DELETE) {
        delete_forward(state);
    } else if (c == '\b' || c == 127) {
        backspace(state);
    } else if (c == '\n' || c == '\r') {
        insert_newline(state);
    } else if (c == '\t') {
        insert_spaces(state, DEFAULT_TAB_SPACES);
    } else if (c >= 32 && c <= 126) {
        insert_text_char(state, (char)c);
    }
}

extern "C" int main()
{
    Surface win = gui_register_window_ex("Latitude", (uint32_t)gui_scaled_metric(980), (uint32_t)gui_scaled_metric(620),
                                         WIN_FLAG_RESIZABLE);
    if (!win.buffer)
        return 1;
    gui_window_set_min_size(gui_scaled_metric(720), gui_scaled_metric(470));
    gui_sync_theme_from_registry();
    gui_request_focus();

    TextBuffer *buffer = static_cast<TextBuffer *>(malloc(sizeof(TextBuffer)));
    AppState *state = static_cast<AppState *>(malloc(sizeof(AppState)));
    LatitudeRects *rects = static_cast<LatitudeRects *>(malloc(sizeof(LatitudeRects)));
    Surface frame = {};
    if (!buffer || !state || !rects) {
        free(buffer);
        free(state);
        free(rects);
        return 1;
    }
    memset(state, 0, sizeof(AppState));
    memset(rects, 0, sizeof(LatitudeRects));
    state->buffer = buffer;
    state->project_selected = -1;
    state->project_hovered = -1;
    state->outline_hovered = -1;
    state->last_project_click_row = -1;
    state->last_editor_click_line = -1;
    state->focus = FOCUS_EDITOR;
    state->needs_redraw = true;
    reset_buffer(buffer, "/data/untitled.txt");
    sync_path_input(state);
    set_status(state, "Ready");
    load_project(state, "/data");
    build_outline(state);

    Registry *registry = gui_registry();
    state->last_settings_generation = registry ? registry->settings_generation : 0;

    while (true) {
        Event ev = {};
        while (poll_event(&ev) > 0) {
            if (ev.type == EVT_WINDOW_CLOSE)
                return 0;
            if (ev.type == EVT_WINDOW_RESIZE && gui_sync_window_size(&win) > 0) {
                state->needs_redraw = true;
                continue;
            }
            if (ev.type == EVT_MOUSE_MOVE) {
                HoverTarget previous = state->hovered;
                int previous_project_hovered = state->project_hovered;
                int previous_outline_hovered = state->outline_hovered;
                HoverTarget hovered = update_hover(state, rects, ev.mouse.x, ev.mouse.y);
                bool hover_changed = hovered != previous || state->project_hovered != previous_project_hovered ||
                                     state->outline_hovered != previous_outline_hovered;
                if (hover_changed) {
                    state->hovered = hovered;
                    state->needs_redraw = true;
                }
                continue;
            }
            if (ev.type == EVT_MOUSE_SCROLL) {
                if (point_in_rect(rects->editor_rect, ev.mouse.x, ev.mouse.y)) {
                    state->first_line += ev.mouse.scroll_y < 0 ? 3 : -3;
                    state->first_line = clamp_int(state->first_line, 0, max_int(0, state->buffer->line_count - 1));
                    state->needs_redraw = true;
                } else if (point_in_rect(rects->sidebar_rect, ev.mouse.x, ev.mouse.y)) {
                    int side_header_h = gui_scaled_metric(34);
                    int sy = rects->sidebar_rect.y + side_header_h + gui_space_1() + gui_line_height() + gui_space_1();
                    int needed_project_h = (sy - rects->sidebar_rect.y) + state->project_count * (gui_app_row_h() + 2);
                    int max_project_area = max_int(gui_scaled_metric(150), (rects->sidebar_rect.h * 58) / 100);
                    int project_area_bottom = rects->sidebar_rect.y + min_int(max_project_area, needed_project_h);
                    if (project_area_bottom > rects->sidebar_rect.y + rects->sidebar_rect.h - gui_scaled_metric(90))
                        project_area_bottom = rects->sidebar_rect.y + rects->sidebar_rect.h - gui_scaled_metric(90);

                    if (ev.mouse.y < project_area_bottom) {
                        state->project_first_row += ev.mouse.scroll_y < 0 ? 3 : -3;
                        state->project_first_row =
                            clamp_int(state->project_first_row, 0, max_int(0, state->project_count - 1));
                    } else {
                        state->outline_first_row += ev.mouse.scroll_y < 0 ? 3 : -3;
                        state->outline_first_row =
                            clamp_int(state->outline_first_row, 0, max_int(0, state->outline_count - 1));
                    }
                    state->needs_redraw = true;
                }
                continue;
            }
            if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 1) {
                HoverTarget target = update_hover(state, rects, ev.mouse.x, ev.mouse.y);
                state->hovered = target;
                if (target == HOVER_NEW_FILE) {
                    clear_field_focus(state);
                    new_file(state);
                } else if (target == HOVER_SAVE) {
                    bool path_ok = !state->path_focused || set_buffer_path_from_input(state, false);
                    clear_field_focus(state);
                    if (path_ok)
                        save_file(state);
                } else if (target == HOVER_RELOAD) {
                    clear_field_focus(state);
                    if (state->buffer->path[0])
                        load_file(state, state->buffer->path);
                } else if (target == HOVER_PATH) {
                    sync_path_input(state);
                    state->path_focused = true;
                    state->search_focused = false;
                    state->focus = FOCUS_PATH;
                    state->needs_redraw = true;
                } else if (target == HOVER_SEARCH) {
                    state->path_focused = false;
                    state->search_focused = true;
                    state->focus = FOCUS_SEARCH;
                    state->needs_redraw = true;
                } else if (target == HOVER_PROJECT_ROW && state->project_hovered >= 0) {
                    uint64_t now = get_ticks();
                    bool double_click = state->last_project_click_row == state->project_hovered &&
                                        now - state->last_project_click_ticks < 450;
                    state->project_selected = state->project_hovered;
                    state->focus = FOCUS_PROJECT;
                    clear_field_focus(state);
                    if (double_click)
                        open_project_row(state, state->project_hovered);
                    state->last_project_click_row = state->project_hovered;
                    state->last_project_click_ticks = now;
                    state->needs_redraw = true;
                } else if (target == HOVER_OUTLINE_ROW && state->outline_hovered >= 0) {
                    jump_to_outline(state, state->outline_hovered);
                } else if (target == HOVER_EDITOR) {
                    click_editor(state, rects, ev.mouse.x, ev.mouse.y);
                } else {
                    clear_field_focus(state);
                    state->focus = FOCUS_EDITOR;
                    state->needs_redraw = true;
                }
                continue;
            }
            if (ev.type == EVT_KEY_DOWN) {
                handle_key(state, (uint8_t)ev.key.c);
            }
        }

        registry = gui_registry();
        if (registry && registry->settings_generation != state->last_settings_generation) {
            state->last_settings_generation = registry->settings_generation;
            if (gui_sync_theme_from_registry())
                state->needs_redraw = true;
        }

        if (state->needs_redraw) {
            if (ensure_render_surface(&frame, win.width, win.height)) {
                draw_latitude(&frame, state, rects);
                present_latitude_frame(&win, &frame);
            } else {
                draw_latitude(&win, state, rects);
                gui_blit_to_screen_rect(&win, 0, 0, (int)win.width, (int)win.height);
            }
            state->needs_redraw = false;
        } else {
            sleep_ms(10);
        }
    }

    return 0;
}
