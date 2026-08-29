#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/fs.h>

#include "unistd.h"

static inline bool cfg_read_text_file(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0)
        return false;
    out[0] = '\0';

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    size_t limit = out_size - 1;
    size_t total = 0;
    while (total < limit) {
        int n = read(fd, out + total, limit - total);
        if (n < 0) {
            close(fd);
            out[0] = '\0';
            return false;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    if (total == 0)
        return false;

    out[total] = '\0';
    return true;
}

static inline bool cfg_read_first_line(const char *path, char *out, size_t out_size)
{
    if (!cfg_read_text_file(path, out, out_size))
        return false;

    for (size_t i = 0; out[i]; i++) {
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = '\0';
            break;
        }
    }
    return out[0] != '\0';
}

static inline bool cfg_write_text_file(const char *path, const char *contents)
{
    if (!path || !contents)
        return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    size_t len = strlen(contents);
    bool ok = write(fd, contents, len) == (int)len;
    close(fd);
    return ok;
}

// Atomic-ish replacement: write <path>.tmp, unlink the destination, then rename.
// FAT32 rename refuses to overwrite an existing destination, so the unlink is
// required. A crash mid-write leaves <path>.tmp (or nothing) and the original
// intact; a crash between unlink and rename drops <path>, which the bootstrap
// config (/etc/...) covers. This avoids the truncate-then-write window where a
// crashed write of <path> itself would corrupt the boot config.
static inline bool cfg_write_text_file_atomic(const char *path, const char *contents)
{
    if (!path || !contents)
        return false;
    char tmp[260];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return cfg_write_text_file(path, contents); // path too long: best effort
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    size_t len = strlen(contents);
    bool ok = write(fd, contents, len) == (int)len;
    close(fd);
    if (!ok)
        return false;
    unlink(path); // best-effort; ignore "no such file"
    return rename(tmp, path) == 0;
}

static inline bool cfg_line_value(const char *config, const char *key, char *out, size_t out_size)
{
    if (!config || !key || !out || out_size == 0)
        return false;
    out[0] = '\0';

    size_t key_len = strlen(key);
    const char *line = config;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
            len--;
        if (len > key_len + 1 && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            size_t value_len = len - key_len - 1;
            if (value_len >= out_size)
                value_len = out_size - 1;
            memcpy(out, line + key_len + 1, value_len);
            out[value_len] = '\0';
            return true;
        }
        if (!next)
            break;
        line = next + 1;
    }
    return false;
}

static inline bool cfg_read_text_from_candidates(const char *const *candidates, size_t count, char *out,
                                                 size_t out_size)
{
    if (!candidates)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (cfg_read_text_file(candidates[i], out, out_size))
            return true;
    }
    return false;
}

static inline bool cfg_read_first_line_from_candidates(const char *const *candidates, size_t count, char *out,
                                                       size_t out_size)
{
    if (!candidates)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (cfg_read_first_line(candidates[i], out, out_size))
            return true;
    }
    return false;
}

// Persistent store for per-app view toggles (files_sidebar, latitude_wrap,
// terminal_zoom, ...). Kept separate from SYSTEM.CFG so app writes never race
// the WM/Preferences persistence of system settings.
#define APP_SETTINGS_CONFIG_PATH "/data/APPS.CFG"

static inline int cfg_load_int(const char *path, const char *key, int fallback)
{
    char text[1024];
    char value[32];
    if (!cfg_read_text_file(path, text, sizeof(text)))
        return fallback;
    if (!cfg_line_value(text, key, value, sizeof(value)))
        return fallback;
    int sign = 1;
    const char *p = value;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (*p < '0' || *p > '9')
        return fallback;
    int result = 0;
    while (*p >= '0' && *p <= '9') {
        result = result * 10 + (*p - '0');
        p++;
    }
    return sign * result;
}

// Read-modify-write: preserve every other key, replace (or append) `key=value`.
static inline bool cfg_save_int(const char *path, const char *key, int value)
{
    char text[1024];
    char out[1024];
    if (!cfg_read_text_file(path, text, sizeof(text)))
        text[0] = '\0';

    size_t pos = 0;
    size_t key_len = strlen(key);
    const char *line = text;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        size_t line_total = len + (next ? 1 : 0);
        bool is_target = (len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=');
        if (!is_target) {
            if (pos + line_total + 1 >= sizeof(out))
                return false;
            memcpy(out + pos, line, line_total);
            pos += line_total;
        }
        if (!next)
            break;
        line = next + 1;
    }
    if (pos > 0 && out[pos - 1] != '\n') {
        if (pos + 1 >= sizeof(out))
            return false;
        out[pos++] = '\n';
    }
    int written = snprintf(out + pos, sizeof(out) - pos, "%s=%d\n", key, value);
    if (written < 0 || (size_t)written >= sizeof(out) - pos)
        return false;
    out[pos + (size_t)written] = '\0';
    return cfg_write_text_file(path, out);
}
