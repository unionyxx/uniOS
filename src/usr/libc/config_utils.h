#pragma once

#include <stddef.h>
#include <stdint.h>
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
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    size_t len = strlen(contents);
    bool ok = write(fd, contents, len) == (int)len;
    close(fd);
    return ok;
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
