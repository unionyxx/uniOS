#include "shell_internal.h"

void add_to_history(const char *cmd)
{
    if (!g_current_shell || cmd[0] == '\0')
        return;
    if (g_current_shell->history_count > 0 &&
        strcmp(g_current_shell->history[(g_current_shell->history_count - 1) % HISTORY_SIZE], cmd) == 0)
        return;

    char *dest = g_current_shell->history[g_current_shell->history_count % HISTORY_SIZE];
    strncpy(dest, cmd, 255);
    g_current_shell->history_count++;
}

void redraw_line_at(int row, int new_cursor_pos)
{
    (void)row;
    // Simplistic redraw for userspace: clear line and reprint prompt + buffer
    printf("\r\x1b[K"); // CR then Clear to end of line
    print_prompt();
    printf("%s", cmd_buffer);

    // Move cursor back to new_cursor_pos
    int prompt_len = get_prompt_len();
    printf("\r\x1b[%dC", prompt_len + new_cursor_pos);

    cursor_pos = new_cursor_pos;
}

void clear_line()
{
    printf("\r\x1b[K");
}

void display_line()
{
    printf("%s", cmd_buffer);
}

void read_input_hidden(char *buf, int max_len)
{
    // Basic implementation: we don't have termios yet to disable echo
    // For now, it's not hidden.
    int n = read(0, buf, (size_t)max_len - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (buf[n - 1] == '\n')
            buf[n - 1] = '\0';
    }
}

void read_input_visible(char *buf, int max_len)
{
    int n = read(0, buf, (size_t)max_len - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (buf[n - 1] == '\n')
            buf[n - 1] = '\0';
    }
}
