#include "shell_internal.h"

char pipe_buffer_a[PIPE_BUFFER_SIZE];
char pipe_buffer_b[PIPE_BUFFER_SIZE];

ShellState g_shell_state;
ShellState *g_current_shell = &g_shell_state;

char cmd_buffer[256];
int cmd_len = 0;
int cursor_pos = 0;
char clipboard[256];
int clipboard_len = 0;
int selection_start = -1;
int last_displayed_len = 0;
int last_displayed_prompt_len = 0;

void shell_write(const char *str)
{
    printf("%s", str);
}

void shell_write_line(const char *str)
{
    printf("%s\n", str);
}

void shell_put_char(char c)
{
    putchar(c);
}

int get_prompt_len()
{
    int len = 14; // "root@unios " + " $ "
    if (g_current_shell)
        len += (int)strlen(g_current_shell->cwd);
    return len;
}

void print_prompt()
{
    const char *cwd = g_current_shell ? g_current_shell->cwd : "/";
    const char *marker = (g_current_shell && g_current_shell->last_exit_status != 0) ? "!" : "$";
    printf("\x1b[90mroot@unios\x1b[0m \x1b[34m%s\x1b[0m %s ", cwd, marker);
}

void shell_init_state(ShellState *s)
{
    memset(s, 0, sizeof(ShellState));
    struct VNodeStat st = {};
    const char *initial_cwd = (stat("/data", &st) == 0 && st.is_dir) ? "/data" : "/";
    strncpy(s->cwd, initial_cwd, 255);
    s->cwd[255] = '\0';
}

static const CommandEntry commands[] = {
    {"help", "help", "show command reference", CMD_NONE, cmd_help, nullptr, nullptr},
    {"clear", "clear", "clear the terminal", CMD_NONE, []() { printf("\x1b[2J\x1b[H"); }, nullptr, nullptr},
    {"exit", "exit", "exit the shell", CMD_NONE, []() { exit(0); }, nullptr, nullptr},
    {"true", "true", "return success", CMD_NONE, cmd_true, nullptr, nullptr},
    {"false", "false", "return failure", CMD_NONE, cmd_false, nullptr, nullptr},

    {"pwd", "pwd", "print working directory", CMD_NONE, cmd_pwd, nullptr, nullptr},
    {"ls", "ls [-lah] [path]", "list directory entries", CMD_ARGS, nullptr, cmd_ls, nullptr},
    {"cd", "cd [dir]", "change directory", CMD_ARGS, nullptr, cmd_cd, nullptr},
    {"cat", "cat [file]", "print file or stdin", CMD_PIPED, nullptr, nullptr, cmd_cat_piped},
    {"stat", "stat <path>", "show file metadata", CMD_ARGS, nullptr, cmd_stat, nullptr},
    {"touch", "touch <file>", "create or update a file", CMD_ARGS, nullptr, cmd_touch, nullptr},
    {"rm", "rm <file>", "remove a file", CMD_ARGS, nullptr, cmd_rm, nullptr},
    {"mkdir", "mkdir <dir>", "create a directory", CMD_ARGS, nullptr, cmd_mkdir, nullptr},
    {"rmdir", "rmdir <dir>", "remove an empty directory", CMD_ARGS, nullptr, cmd_rmdir, nullptr},
    {"cp", "cp <src> <dst>", "copy a file", CMD_ARGS, nullptr, cmd_cp, nullptr},
    {"mv", "mv <src> <dst>", "move or rename a path", CMD_ARGS, nullptr, cmd_mv, nullptr},
    {"tree", "tree [dir]", "print a directory tree", CMD_ARGS, nullptr, cmd_tree, nullptr},
    {"find", "find [dir] [pattern]", "find files by name", CMD_ARGS, nullptr, cmd_find, nullptr},
    {"du", "du [path]", "summarize file sizes", CMD_ARGS, nullptr, cmd_du, nullptr},
    {"df", "df", "show mounted volumes", CMD_NONE, cmd_df, nullptr, nullptr},
    {"mount", "mount", "show mount table", CMD_NONE, cmd_mount, nullptr, nullptr},
    {"storage", "storage [off|ro|rw]", "show or request storage guard mode", CMD_ARGS, nullptr, cmd_storage, nullptr},
    {"write", "write <file> <text>", "replace file contents", CMD_ARGS, nullptr, cmd_write, nullptr},
    {"append", "append <file> <text>", "append text to a file", CMD_ARGS, nullptr, cmd_append, nullptr},
    {"hexdump", "hexdump <file>", "show bytes in hex", CMD_ARGS, nullptr, cmd_hexdump, nullptr},
    {"which", "which <cmd>", "locate a builtin or executable", CMD_ARGS, nullptr, cmd_which, nullptr},
    {"type", "type <cmd>", "describe command resolution", CMD_ARGS, nullptr, cmd_type, nullptr},

    {"echo", "echo <text>", "print text", CMD_ARGS, nullptr, cmd_echo, nullptr},
    {"wc", "wc [file]", "count lines, words, and bytes", CMD_PIPED, nullptr, nullptr, cmd_wc},
    {"head", "head [-n N] [file]", "print first lines", CMD_PIPED, nullptr, nullptr, cmd_head},
    {"tail", "tail [-n N] [file]", "print last lines", CMD_PIPED, nullptr, nullptr, cmd_tail},
    {"grep", "grep <pattern> [file]", "search for a pattern", CMD_PIPED, nullptr, nullptr, cmd_grep},
    {"sort", "sort [file]", "sort lines", CMD_PIPED, nullptr, nullptr, cmd_sort},
    {"uniq", "uniq [file]", "collapse duplicate lines", CMD_PIPED, nullptr, nullptr, cmd_uniq},
    {"rev", "rev [file]", "reverse each line", CMD_PIPED, nullptr, nullptr, cmd_rev},
    {"tac", "tac [file]", "print lines in reverse order", CMD_PIPED, nullptr, nullptr, cmd_tac},
    {"nl", "nl [file]", "number lines", CMD_PIPED, nullptr, nullptr, cmd_nl},
    {"tr", "tr <set1> <set2> [file]", "translate characters", CMD_PIPED, nullptr, nullptr, cmd_tr},

    {"run", "run <script>", "run a uniOS shell script", CMD_ARGS, nullptr, cmd_run, nullptr},
    {"source", "source <script>", "run a script in this shell", CMD_ARGS, nullptr, cmd_source, nullptr},
    {"set", "set [name=value]", "show or set shell variables", CMD_ARGS, nullptr, cmd_set, nullptr},
    {"unset", "unset <name>", "unset a shell variable", CMD_ARGS, nullptr, cmd_unset, nullptr},
    {"alias", "alias [name=value]", "define or list aliases", CMD_ARGS, nullptr, cmd_alias, nullptr},
    {"unalias", "unalias <name>", "remove an alias definition", CMD_ARGS, nullptr, cmd_unalias, nullptr},
    {"read", "read <name>", "read stdin into a variable", CMD_ARGS, nullptr, cmd_read, nullptr},
    {"test", "test <expr>", "evaluate a shell condition", CMD_ARGS, nullptr, cmd_test, nullptr},
    {"expr", "expr <a> <op> <b>", "evaluate integer expression", CMD_ARGS, nullptr, cmd_expr, nullptr},
    {"time", "time <command>", "time a command", CMD_ARGS, nullptr, cmd_time, nullptr},
    {"sleep", "sleep <ms|Ns>", "sleep for a duration", CMD_ARGS, nullptr, cmd_sleep, nullptr},
    {"history", "history", "show command history", CMD_NONE, cmd_history, nullptr, nullptr},
    {"env", "env", "show shell variables", CMD_NONE, cmd_env, nullptr, nullptr},

    {"mem", "mem", "show memory usage", CMD_NONE, cmd_mem, nullptr, nullptr},
    {"kheap", "kheap", "show kernel heap usage", CMD_NONE, cmd_kheap, nullptr, nullptr},
    {"ps", "ps", "list processes", CMD_NONE, cmd_ps, nullptr, nullptr},
    {"kill", "kill <pid> [sig]", "send a signal", CMD_ARGS, nullptr, cmd_kill, nullptr},
    {"date", "date", "show RTC time", CMD_NONE, cmd_date, nullptr, nullptr},
    {"uptime", "uptime", "show uptime", CMD_NONE, cmd_uptime, nullptr, nullptr},
    {"version", "version", "show kernel build", CMD_NONE, cmd_version, nullptr, nullptr},
    {"uname", "uname", "show system name", CMD_NONE, cmd_uname, nullptr, nullptr},
    {"sysinfo", "sysinfo", "show boot and kernel profile", CMD_NONE, cmd_sysinfo, nullptr, nullptr},
    {"cpuinfo", "cpuinfo", "show CPU vendor", CMD_NONE, cmd_cpuinfo, nullptr, nullptr},
    {"random", "random [bytes]", "read kernel random bytes", CMD_ARGS, nullptr, cmd_random, nullptr},
    {"reboot", "reboot", "reboot the system", CMD_NONE, cmd_reboot, nullptr, nullptr},
    {"poweroff", "poweroff", "power off the system", CMD_NONE, cmd_poweroff, nullptr, nullptr},
    {"quiet", "quiet <on|off>", "toggle framebuffer boot logging", CMD_ARGS, nullptr, cmd_quiet, nullptr},
    {"dmesg", "dmesg", "show kernel log status", CMD_NONE, cmd_dmesg, nullptr, nullptr},

    {"resolve", "resolve <host>", "resolve a DNS name", CMD_ARGS, nullptr, cmd_resolve, nullptr},
    {"ping", "ping <host>", "resolve or probe a host", CMD_ARGS, nullptr, cmd_ping, nullptr},
    {"sound", "sound <file>", "play audio through kernel parser", CMD_ARGS, nullptr, cmd_sound, nullptr},
    {"play", "play <file>", "play WAV audio through user parser", CMD_ARGS, nullptr, cmd_play, nullptr},
};

static const int NUM_COMMANDS = sizeof(commands) / sizeof(commands[0]);

const CommandEntry *shell_get_commands()
{
    return commands;
}

int shell_get_command_count()
{
    return NUM_COMMANDS;
}

const CommandEntry *shell_find_command(const char *name)
{
    if (!name || !name[0])
        return nullptr;
    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(commands[i].name, name) == 0)
            return &commands[i];
    }
    return nullptr;
}

bool shell_path_exists(const char *path, bool *is_dir)
{
    struct VNodeStat st;
    if (!path || stat(path, &st) < 0)
        return false;
    if (is_dir)
        *is_dir = st.is_dir;
    return true;
}

static void shell_set_status(int status)
{
    if (g_current_shell)
        g_current_shell->last_exit_status = status;
}

static bool split_exec_name(const char *cmd, char *name, int name_size)
{
    while (*cmd == ' ')
        cmd++;
    int i = 0;
    while (*cmd && *cmd != ' ' && *cmd != '\t' && i < name_size - 1)
        name[i++] = *cmd++;
    name[i] = '\0';
    return i > 0;
}

static bool try_exec_external(const char *trimmed_cmd)
{
    char name[128];
    if (!split_exec_name(trimmed_cmd, name, sizeof(name)))
        return false;

    char resolved[256];
    if (strchr(name, '/')) {
        shell_resolve_path(name, resolved);
    } else {
        snprintf(resolved, sizeof(resolved), "/bin/%s.elf", name);
        if (!shell_path_exists(resolved, nullptr)) {
            snprintf(resolved, sizeof(resolved), "/bin/%s", name);
        }
    }

    bool is_dir = false;
    if (!shell_path_exists(resolved, &is_dir) || is_dir)
        return false;

    int pid = fork();
    if (pid == 0) {
        exec(resolved);
        printf("%s: exec failed\n", resolved);
        exit(126);
    }
    if (pid < 0) {
        printf("shell: fork failed\n");
        shell_set_status(1);
        return true;
    }

    int status = 0;
    waitpid(pid, &status);
    shell_set_status(status);
    return true;
}

void expand_aliases(char *cmd, int max_size)
{
    int iterations = 0;
    bool expanded_any = true;
    while (expanded_any && iterations < 5) {
        expanded_any = false;
        
        char *start = cmd;
        while (*start == ' ') {
            start++;
        }
        if (*start == '\0')
            break;

        int word_len = 0;
        while (start[word_len] && start[word_len] != ' ' && start[word_len] != '\t' && start[word_len] != '=') {
            word_len++;
        }

        if (word_len == 0 || word_len >= 32)
            break;

        char word[32];
        strncpy(word, start, (size_t)word_len);
        word[word_len] = '\0';

        int slot = -1;
        if (g_current_shell) {
            for (int i = 0; i < 32; i++) {
                if (g_current_shell->aliases[i].in_use && strcmp(g_current_shell->aliases[i].name, word) == 0) {
                    slot = i;
                    break;
                }
            }
        }

        if (slot != -1) {
            const char *val = g_current_shell->aliases[slot].value;
            const char *rest = start + word_len;
            while (*rest == ' ') {
                rest++;
            }

            char expanded[256];
            if (*rest != '\0') {
                snprintf(expanded, sizeof(expanded), "%s %s", val, rest);
            } else {
                snprintf(expanded, sizeof(expanded), "%s", val);
            }

            strncpy(cmd, expanded, (size_t)max_size - 1);
            cmd[max_size - 1] = '\0';
            expanded_any = true;
            iterations++;
        }
    }
}

static char *extract_redir_filename(char *start)
{
    while (*start == ' ') {
        start++;
    }
    char *end = start;
    while (*end && *end != '<' && *end != '>') {
        end++;
    }
    char *trailing = end;
    while (trailing > start && (*(trailing - 1) == ' ' || *(trailing - 1) == '\t')) {
        trailing--;
    }
    *trailing = '\0';
    return start;
}

bool execute_single_command(const char *cmd, const char *piped_input)
{
    while (*cmd == ' ')
        cmd++;
    int total_len = (int)strlen(cmd);
    while (total_len > 0 && cmd[total_len - 1] == ' ')
        total_len--;
    if (total_len == 0)
        return true;

    char local_cmd[256];
    int copy_len = (total_len > 255) ? 255 : total_len;
    strncpy(local_cmd, cmd, (size_t)copy_len);
    local_cmd[copy_len] = '\0';

    // Expand aliases
    expand_aliases(local_cmd, sizeof(local_cmd));

    int redirect_fd = -1;
    char *redirect_file = nullptr;
    bool append = false;

    int redirect_in_fd = -1;
    char *redirect_in_file = nullptr;

    char *ptr_in = strchr(local_cmd, '<');
    char *ptr_out_double = strstr(local_cmd, ">>");
    char *ptr_out_single = strchr(local_cmd, '>');
    if (ptr_out_double) {
        ptr_out_single = nullptr;
    }

    char *first_redir = nullptr;
    if (ptr_in) {
        first_redir = ptr_in;
    }
    if (ptr_out_double) {
        if (!first_redir || ptr_out_double < first_redir) {
            first_redir = ptr_out_double;
        }
    }
    if (ptr_out_single) {
        if (!first_redir || ptr_out_single < first_redir) {
            first_redir = ptr_out_single;
        }
    }

    if (ptr_in) {
        redirect_in_file = extract_redir_filename(ptr_in + 1);
    }
    if (ptr_out_double) {
        append = true;
        redirect_file = extract_redir_filename(ptr_out_double + 2);
    } else if (ptr_out_single) {
        redirect_file = extract_redir_filename(ptr_out_single + 1);
    }

    if (first_redir) {
        *first_redir = '\0';
    }

    if (redirect_file && redirect_file[0] != '\0') {
        char resolved[256];
        shell_resolve_path(redirect_file, resolved);
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        redirect_fd = open(resolved, flags);
        if (redirect_fd < 0) {
            printf("shell: failed to open '%s' for redirection\n", redirect_file);
            shell_set_status(1);
            return false;
        }
    }

    if (redirect_in_file && redirect_in_file[0] != '\0') {
        char resolved[256];
        shell_resolve_path(redirect_in_file, resolved);
        redirect_in_fd = open(resolved, O_RDONLY);
        if (redirect_in_fd < 0) {
            printf("shell: failed to open '%s' for input redirection\n", redirect_in_file);
            if (redirect_fd >= 0) {
                close(redirect_fd);
            }
            shell_set_status(1);
            return false;
        }
    }

    int stdout_backup = -1;
    if (redirect_fd >= 0) {
        stdout_backup = 100;
        dup2(1, stdout_backup);
        dup2(redirect_fd, 1);
        close(redirect_fd);
    }

    int stdin_backup = -1;
    if (redirect_in_fd >= 0) {
        stdin_backup = 101;
        dup2(0, stdin_backup);
        dup2(redirect_in_fd, 0);
        close(redirect_in_fd);
    }

    char *trimmed_cmd = local_cmd;
    while (*trimmed_cmd == ' ')
        trimmed_cmd++;
    int len = (int)strlen(trimmed_cmd);
    while (len > 0 && trimmed_cmd[len - 1] == ' ') {
        trimmed_cmd[len - 1] = '\0';
        len--;
    }

    bool cmd_found = false;
    for (int i = 0; i < NUM_COMMANDS; i++) {
        const CommandEntry &c = commands[i];
        int name_len = (int)strlen(c.name);
        if (c.type == CMD_NONE) {
            if (strcmp(trimmed_cmd, c.name) == 0) {
                shell_set_status(0);
                c.handler_none();
                cmd_found = true;
                break;
            }
        } else if (c.type == CMD_ARGS) {
            if (strcmp(trimmed_cmd, c.name) == 0) {
                shell_set_status(0);
                c.handler_args("");
                cmd_found = true;
                break;
            }
            if (strncmp(trimmed_cmd, c.name, (size_t)name_len) == 0 && trimmed_cmd[name_len] == ' ') {
                shell_set_status(0);
                c.handler_args(trimmed_cmd + name_len + 1);
                cmd_found = true;
                break;
            }
        } else if (c.type == CMD_PIPED) {
            if (strcmp(trimmed_cmd, c.name) == 0) {
                shell_set_status(0);
                c.handler_piped("", piped_input);
                cmd_found = true;
                break;
            }
            if (strncmp(trimmed_cmd, c.name, (size_t)name_len) == 0 && trimmed_cmd[name_len] == ' ') {
                shell_set_status(0);
                c.handler_piped(trimmed_cmd + name_len + 1, piped_input);
                cmd_found = true;
                break;
            }
        }
    }

    if (!cmd_found && len > 0) {
        cmd_found = try_exec_external(trimmed_cmd);
        if (!cmd_found) {
            printf("Unknown command: %s\n", trimmed_cmd);
            shell_set_status(127);
        }
    }

    if (stdout_backup >= 0) {
        dup2(stdout_backup, 1);
        close(stdout_backup);
    }
    if (stdin_backup >= 0) {
        dup2(stdin_backup, 0);
        close(stdin_backup);
    }

    return cmd_found;
}

static bool starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}

static void redraw_input_line()
{
    printf("\r\x1b[K");
    print_prompt();
    printf("%s", cmd_buffer);
    int move_left = cmd_len - cursor_pos;
    if (move_left > 0)
        printf("\x1b[%dD", move_left);
}

static void set_input_buffer(const char *text)
{
    strncpy(cmd_buffer, text ? text : "", sizeof(cmd_buffer) - 1);
    cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
    cmd_len = (int)strlen(cmd_buffer);
    cursor_pos = cmd_len;
    redraw_input_line();
}

static void insert_input_text(const char *text)
{
    if (!text)
        return;
    int add_len = (int)strlen(text);
    if (add_len <= 0 || cmd_len + add_len >= (int)sizeof(cmd_buffer))
        return;
    memmove(cmd_buffer + cursor_pos + add_len, cmd_buffer + cursor_pos, (size_t)(cmd_len - cursor_pos + 1));
    memcpy(cmd_buffer + cursor_pos, text, (size_t)add_len);
    cmd_len += add_len;
    cursor_pos += add_len;
    redraw_input_line();
}

static void insert_input_char(char c)
{
    char text[2] = {c, '\0'};
    insert_input_text(text);
}

static void backspace_input()
{
    if (cursor_pos <= 0)
        return;
    memmove(cmd_buffer + cursor_pos - 1, cmd_buffer + cursor_pos, (size_t)(cmd_len - cursor_pos + 1));
    cursor_pos--;
    cmd_len--;
    redraw_input_line();
}

static void delete_input_char()
{
    if (cursor_pos >= cmd_len)
        return;
    memmove(cmd_buffer + cursor_pos, cmd_buffer + cursor_pos + 1, (size_t)(cmd_len - cursor_pos));
    cmd_len--;
    redraw_input_line();
}

static void delete_previous_word()
{
    int start = cursor_pos;
    while (start > 0 && cmd_buffer[start - 1] == ' ')
        start--;
    while (start > 0 && cmd_buffer[start - 1] != ' ')
        start--;
    if (start == cursor_pos)
        return;
    memmove(cmd_buffer + start, cmd_buffer + cursor_pos, (size_t)(cmd_len - cursor_pos + 1));
    cmd_len -= cursor_pos - start;
    cursor_pos = start;
    redraw_input_line();
}

static bool token_is_command_position(int token_start)
{
    for (int i = token_start - 1; i >= 0; i--) {
        if (cmd_buffer[i] == '|')
            return true;
        if (cmd_buffer[i] != ' ' && cmd_buffer[i] != '\t')
            return false;
    }
    return true;
}

struct CompletionMatch
{
    char text[256];
    bool is_dir;
};

static int common_prefix_len(const CompletionMatch *matches, int count)
{
    if (count <= 0)
        return 0;
    int prefix_len = (int)strlen(matches[0].text);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < prefix_len && matches[i].text[j] && matches[i].text[j] == matches[0].text[j])
            j++;
        prefix_len = j;
    }
    return prefix_len;
}

static void print_matches(const CompletionMatch *matches, int count)
{
    putchar('\n');
    for (int i = 0; i < count; i++) {
        if (matches[i].is_dir)
            printf("\x1b[34m%s/\x1b[0m  ", matches[i].text);
        else
            printf("%s  ", matches[i].text);
    }
    putchar('\n');
    redraw_input_line();
}

static int collect_command_matches(const char *prefix, CompletionMatch *matches, int max_matches)
{
    int count = 0;
    for (int i = 0; i < NUM_COMMANDS && count < max_matches; i++) {
        if (!starts_with(commands[i].name, prefix))
            continue;
        strncpy(matches[count].text, commands[i].name, sizeof(matches[count].text) - 1);
        matches[count].text[sizeof(matches[count].text) - 1] = '\0';
        matches[count].is_dir = false;
        count++;
    }
    return count;
}

static void split_completion_path(const char *token, char *dir_token, int dir_size, char *base, int base_size)
{
    const char *slash = strrchr(token, '/');
    if (!slash) {
        strncpy(dir_token, ".", (size_t)dir_size - 1);
        dir_token[dir_size - 1] = '\0';
        strncpy(base, token, (size_t)base_size - 1);
        base[base_size - 1] = '\0';
        return;
    }

    int dir_len = (int)(slash - token);
    if (dir_len == 0)
        dir_len = 1;
    if (dir_len >= dir_size)
        dir_len = dir_size - 1;
    strncpy(dir_token, token, (size_t)dir_len);
    dir_token[dir_len] = '\0';
    strncpy(base, slash + 1, (size_t)base_size - 1);
    base[base_size - 1] = '\0';
}

static int collect_file_matches(const char *token, CompletionMatch *matches, int max_matches)
{
    char dir_token[256];
    char base[128];
    split_completion_path(token, dir_token, sizeof(dir_token), base, sizeof(base));

    char resolved_dir[256];
    if (strcmp(dir_token, ".") == 0)
        strncpy(resolved_dir, g_current_shell ? g_current_shell->cwd : "/", sizeof(resolved_dir) - 1);
    else
        shell_resolve_path(dir_token, resolved_dir);
    resolved_dir[sizeof(resolved_dir) - 1] = '\0';

    int fd = open(resolved_dir, O_RDONLY);
    if (fd < 0)
        return 0;

    int count = 0;
    char name[256] = {};
    while (count < max_matches && syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (!starts_with(name, base))
            continue;

        char full_path[512];
        strncpy(full_path, resolved_dir, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
        if (full_path[strlen(full_path) - 1] != '/')
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        strncat(full_path, name, sizeof(full_path) - strlen(full_path) - 1);

        bool is_dir = false;
        shell_path_exists(full_path, &is_dir);
        strncpy(matches[count].text, name, sizeof(matches[count].text) - 1);
        matches[count].text[sizeof(matches[count].text) - 1] = '\0';
        matches[count].is_dir = is_dir;
        count++;
    }
    close(fd);
    return count;
}

static void complete_input()
{
    int token_start = cursor_pos;
    while (token_start > 0 && cmd_buffer[token_start - 1] != ' ' && cmd_buffer[token_start - 1] != '\t' &&
           cmd_buffer[token_start - 1] != '|')
        token_start--;

    char token[128];
    int token_len = cursor_pos - token_start;
    if (token_len >= (int)sizeof(token))
        token_len = (int)sizeof(token) - 1;
    strncpy(token, cmd_buffer + token_start, (size_t)token_len);
    token[token_len] = '\0';

    CompletionMatch matches[48];
    int count = token_is_command_position(token_start) ? collect_command_matches(token, matches, 48)
                                                       : collect_file_matches(token, matches, 48);
    if (count == 0)
        return;

    int base_len = token_len;
    if (!token_is_command_position(token_start)) {
        const char *slash = strrchr(token, '/');
        if (slash)
            base_len = (int)strlen(slash + 1);
    }

    if (count == 1) {
        const char *completion = matches[0].text;
        const char *suffix = completion + base_len;
        insert_input_text(suffix);
        if (matches[0].is_dir)
            insert_input_text("/");
        else if (token_is_command_position(token_start))
            insert_input_text(" ");
        return;
    }

    int common_len = common_prefix_len(matches, count);
    if (common_len > base_len) {
        char suffix[128];
        int suffix_len = common_len - base_len;
        if (suffix_len >= (int)sizeof(suffix))
            suffix_len = (int)sizeof(suffix) - 1;
        strncpy(suffix, matches[0].text + base_len, (size_t)suffix_len);
        suffix[suffix_len] = '\0';
        insert_input_text(suffix);
        return;
    }

    print_matches(matches, count);
}

void shell_get_line(char *buf, int max_len)
{
    cmd_buffer[0] = '\0';
    cmd_len = 0;
    cursor_pos = 0;
    selection_start = -1;
    int history_cursor = g_current_shell ? g_current_shell->history_count : 0;

    while (cmd_len < max_len - 1) {
        char raw;
        if (read(0, &raw, 1) <= 0)
            continue;
        unsigned char c = (unsigned char)raw;

        if (c == '\n' || c == '\r') {
            putchar('\n');
            break;
        }

        if (c == '\t') {
            complete_input();
            continue;
        }

        if (c == '\b' || c == 127) {
            backspace_input();
            continue;
        }

        if (c == KEY_DELETE) {
            delete_input_char();
            continue;
        }

        if (c == KEY_LEFT_ARROW) {
            if (cursor_pos > 0) {
                cursor_pos--;
                printf("\x1b[1D");
            }
            continue;
        }

        if (c == KEY_RIGHT_ARROW) {
            if (cursor_pos < cmd_len) {
                cursor_pos++;
                printf("\x1b[1C");
            }
            continue;
        }

        if (c == KEY_HOME || c == 1) {
            cursor_pos = 0;
            redraw_input_line();
            continue;
        }

        if (c == KEY_END || c == 5) {
            cursor_pos = cmd_len;
            redraw_input_line();
            continue;
        }

        if (c == KEY_UP_ARROW) {
            if (g_current_shell && g_current_shell->history_count > 0) {
                int first =
                    g_current_shell->history_count > HISTORY_SIZE ? g_current_shell->history_count - HISTORY_SIZE : 0;
                if (history_cursor > first)
                    history_cursor--;
                set_input_buffer(g_current_shell->history[history_cursor % HISTORY_SIZE]);
            }
            continue;
        }

        if (c == KEY_DOWN_ARROW) {
            if (g_current_shell && history_cursor < g_current_shell->history_count - 1) {
                history_cursor++;
                set_input_buffer(g_current_shell->history[history_cursor % HISTORY_SIZE]);
            } else {
                history_cursor = g_current_shell ? g_current_shell->history_count : 0;
                set_input_buffer("");
            }
            continue;
        }

        if (c == 4) {
            if (cmd_len == 0)
                exit(0);
            delete_input_char();
            continue;
        }

        if (c == 11) {
            cmd_buffer[cursor_pos] = '\0';
            cmd_len = cursor_pos;
            redraw_input_line();
            continue;
        }

        if (c == 12) {
            printf("\x1b[2J\x1b[H");
            redraw_input_line();
            continue;
        }

        if (c == 21) {
            cmd_buffer[0] = '\0';
            cmd_len = 0;
            cursor_pos = 0;
            redraw_input_line();
            continue;
        }

        if (c == 23) {
            delete_previous_word();
            continue;
        }

        if (c >= 32 && c <= 126) {
            insert_input_char((char)c);
        }
    }
    strncpy(buf, cmd_buffer, (size_t)max_len - 1);
    buf[max_len - 1] = '\0';
    if (buf[0] != '\0')
        add_to_history(buf);
}

bool execute_pipeline(char *line)
{
    char *cmds[10];
    int num_cmds = 0;

    char *token = strtok(line, "|");
    while (token && num_cmds < 10) {
        while (*token == ' ')
            token++;
        int len = (int)strlen(token);
        while (len > 0 && token[len - 1] == ' ')
            len--;
        if (len > 0) {
            token[len] = '\0';
            cmds[num_cmds++] = token;
        }
        token = strtok(NULL, "|");
    }

    if (num_cmds == 0)
        return true;
    if (num_cmds == 1)
        return execute_single_command(cmds[0], nullptr);

    int pipefds[18]; // 2 * (10 - 1)
    for (int i = 0; i < num_cmds - 1; i++) {
        if (pipe(pipefds + i * 2) < 0) {
            printf("pipe failed\n");
            return false;
        }
    }

    int pids[10];
    for (int i = 0; i < num_cmds; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            if (i > 0)
                dup2(pipefds[(i - 1) * 2], 0);
            if (i < num_cmds - 1)
                dup2(pipefds[i * 2 + 1], 1);

            for (int j = 0; j < 2 * (num_cmds - 1); j++)
                close(pipefds[j]);

            execute_single_command(cmds[i], nullptr);
            exit(0);
        }
    }

    for (int i = 0; i < 2 * (num_cmds - 1); i++)
        close(pipefds[i]);
    for (int i = 0; i < num_cmds; i++) {
        int status;
        waitpid(pids[i], &status);
    }

    return true;
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_init_state(&g_shell_state);

    bool is_dir = false;
    if (shell_path_exists("/etc/shell.rc", &is_dir) && !is_dir) {
        cmd_source("/etc/shell.rc");
    } else if (shell_path_exists("/data/shell.rc", &is_dir) && !is_dir) {
        cmd_source("/data/shell.rc");
    }

    while (true) {
        print_prompt();

        char line[256];
        shell_get_line(line, 256);

        if (line[0] != '\0') {
            execute_pipeline(line);
        }
    }

    return 0;
}
