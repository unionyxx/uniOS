#include "shell_internal.h"

void add_to_history(const char *cmd)
{
    if (!g_current_shell || !cmd || cmd[0] == '\0')
        return;
    if (g_current_shell->history_count > 0) {
        const char *last = g_current_shell->history[(g_current_shell->history_count - 1) % HISTORY_SIZE];
        if (strcmp(last, cmd) == 0)
            return;
    }

    char *dest = g_current_shell->history[g_current_shell->history_count % HISTORY_SIZE];
    strncpy(dest, cmd, sizeof(g_current_shell->history[0]) - 1);
    dest[sizeof(g_current_shell->history[0]) - 1] = '\0';
    g_current_shell->history_count++;
}

void read_input_hidden(char *buf, int max_len)
{
    if (!buf || max_len <= 0)
        return;
    int len = 0;
    for (;;) {
        char raw;
        int n = read(0, &raw, 1);
        if (n == 0)
            break;
        if (n < 0)
            continue;
        unsigned char c = (unsigned char)raw;
        if (c == '\n' || c == '\r')
            break;
        if ((c == '\b' || c == 127) && len > 0) {
            len--;
            printf("\b \b");
            continue;
        }
        if (c >= 32 && c <= 126 && len < max_len - 1) {
            buf[len++] = (char)c;
            putchar('*');
        }
    }
    buf[len] = '\0';
    putchar('\n');
}

void read_input_visible(char *buf, int max_len)
{
    if (!buf || max_len <= 0)
        return;
    int len = 0;
    for (;;) {
        char raw;
        int n = read(0, &raw, 1);
        if (n == 0)
            break;
        if (n < 0)
            continue;
        unsigned char c = (unsigned char)raw;
        if (c == '\n' || c == '\r')
            break;
        if ((c == '\b' || c == 127) && len > 0) {
            len--;
            printf("\b \b");
            continue;
        }
        if (c >= 32 && c <= 126 && len < max_len - 1) {
            buf[len++] = (char)c;
            putchar((char)c);
        }
    }
    buf[len] = '\0';
    putchar('\n');
}
