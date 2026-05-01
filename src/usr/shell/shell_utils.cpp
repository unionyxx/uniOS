#include "shell_internal.h"

void shell_resolve_path(const char *path, char *out)
{
    char temp_path[256];

    if (path[0] == '/') {
        strncpy(temp_path, path, 255);
        temp_path[255] = '\0';
    } else {
        strncpy(temp_path, g_current_shell->cwd, 255);
        temp_path[255] = '\0';
        if (temp_path[strlen(temp_path) - 1] != '/') {
            strncat(temp_path, "/", 255 - strlen(temp_path));
        }
        strncat(temp_path, path, 255 - strlen(temp_path));
    }

    // Normalize: resolve . and .. components
    char *segments[32];
    int depth = 0;
    char copy[256];
    strncpy(copy, temp_path, 255);
    copy[255] = '\0';

    char *tok = copy;
    if (*tok == '/')
        tok++;

    char *p2 = tok;
    while (true) {
        if (*p2 == '/' || *p2 == '\0') {
            char saved = *p2;
            *p2 = '\0';

            if (strcmp(tok, "..") == 0) {
                if (depth > 0)
                    depth--;
            } else if (strcmp(tok, ".") != 0 && tok[0] != '\0') {
                if (depth < 32)
                    segments[depth++] = tok;
            }

            if (saved == '\0')
                break;
            tok = p2 + 1;
            p2 = tok;
            continue;
        }
        p2++;
    }

    // Rebuild path correctly
    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strncat(out, "/", 255 - strlen(out));
        strncat(out, segments[i], 255 - strlen(out));
    }
    if (out[0] == '\0') {
        strncpy(out, "/", 255);
    }
    out[255] = '\0';
}

char *read_file_to_buf(const char *path, uint64_t *out_size)
{
    struct VNodeStat st;
    if (stat(path, &st) < 0)
        return nullptr;
    if (st.is_dir)
        return nullptr;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return nullptr;

    char *buf = (char *)malloc((size_t)st.size + 1);
    if (!buf) {
        close(fd);
        return nullptr;
    }

    int64_t bytes_read = read(fd, buf, (size_t)st.size);
    close(fd);

    if (bytes_read < 0) {
        free(buf);
        return nullptr;
    }

    buf[bytes_read] = '\0';
    if (out_size)
        *out_size = (uint64_t)bytes_read;
    return buf;
}

int str_to_int(const char *s)
{
    int result = 0;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}

bool constant_time_equals(const char *a, const char *b, size_t len)
{
    uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= (uint8_t)(a[i] ^ b[i]);
    }
    return result == 0;
}
