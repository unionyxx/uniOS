#include <socket.h>
#include <stdlib.h>
#include <uapi/sysinfo.h>

#include "shell_internal.h"
#include "wav.h"

void cmd_help()
{
    const CommandEntry *cmds = shell_get_commands();
    int count = shell_get_command_count();
    printf("uniOS shell commands (%d)\n", count);
    printf("\x1b[90m  %-24s %s\x1b[0m\n", "command", "description");
    for (int i = 0; i < count; i++)
        printf("  %-24s %s\n", cmds[i].usage, cmds[i].description);
}

static void set_status(int status)
{
    if (g_current_shell)
        g_current_shell->last_exit_status = status;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *skip_spaces(const char *s)
{
    while (s && is_space(*s))
        s++;
    return s ? s : "";
}

static const char *parse_token(const char *s, char *out, int out_size)
{
    s = skip_spaces(s);
    int i = 0;
    char quote = 0;
    if (*s == '\'' || *s == '"')
        quote = *s++;
    while (*s && i < out_size - 1) {
        if (quote) {
            if (*s == quote) {
                s++;
                break;
            }
        } else if (is_space(*s)) {
            break;
        }
        out[i++] = *s++;
    }
    out[i] = '\0';
    return skip_spaces(s);
}

static bool read_arg2(const char *args, char *a, int a_size, char *b, int b_size, const char **rest)
{
    const char *p = parse_token(args, a, a_size);
    if (a[0] == '\0')
        return false;
    p = parse_token(p, b, b_size);
    if (b[0] == '\0')
        return false;
    if (rest)
        *rest = p;
    return true;
}

static void join_path(const char *dir, const char *name, char *out, int out_size)
{
    if (!dir || dir[0] == '\0')
        dir = "/";
    strncpy(out, dir, (size_t)out_size - 1);
    out[out_size - 1] = '\0';
    size_t len = strlen(out);
    if (len == 0 || out[len - 1] != '/')
        strncat(out, "/", (size_t)out_size - strlen(out) - 1);
    strncat(out, name, (size_t)out_size - strlen(out) - 1);
}

static void print_size(uint64_t bytes)
{
    if (bytes >= 1024ull * 1024ull)
        printf("%lluM", (unsigned long long)(bytes / (1024ull * 1024ull)));
    else if (bytes >= 1024ull)
        printf("%lluK", (unsigned long long)(bytes / 1024ull));
    else
        printf("%lluB", (unsigned long long)bytes);
}

void cmd_ls(const char *path)
{
    bool long_format = false;
    bool show_all = false;
    bool human = false;
    const char *p = skip_spaces(path);
    char target[256] = {};
    while (*p == '-') {
        char flag[32];
        p = parse_token(p, flag, sizeof(flag));
        for (int i = 1; flag[i]; i++) {
            if (flag[i] == 'l')
                long_format = true;
            else if (flag[i] == 'a')
                show_all = true;
            else if (flag[i] == 'h')
                human = true;
        }
    }
    if (*p)
        strncpy(target, p, sizeof(target) - 1);

    char resolved[256];
    if (target[0] == '\0') {
        strncpy(resolved, g_current_shell->cwd, 255);
    } else {
        shell_resolve_path(target, resolved);
    }
    resolved[255] = '\0';

    struct VNodeStat target_stat;
    if (stat(resolved, &target_stat) < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", target[0] ? target : resolved);
        set_status(1);
        return;
    }

    if (!target_stat.is_dir) {
        if (long_format) {
            printf("- ");
            if (human)
                print_size(target_stat.size);
            else
                printf("%llu", (unsigned long long)target_stat.size);
            printf(" %s\n", target[0] ? target : resolved);
        } else {
            printf("%s\n", target[0] ? target : resolved);
        }
        return;
    }

    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", resolved);
        set_status(1);
        return;
    }

    char name[256];
    while (syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (!show_all && name[0] == '.')
            continue;

        char full_path[512];
        join_path(resolved, name, full_path, sizeof(full_path));

        struct VNodeStat st;
        if (stat(full_path, &st) == 0) {
            if (long_format) {
                printf("%c ", st.is_dir ? 'd' : '-');
                if (human)
                    print_size(st.size);
                else
                    printf("%llu", (unsigned long long)st.size);
                printf(" %s%s\n", name, st.is_dir ? "/" : "");
            } else if (st.is_dir) {
                printf("\x1b[34m%s/\x1b[0m  ", name);
            } else {
                printf("%s  ", name);
            }
        } else {
            printf("%s  ", name);
        }
    }

    if (!long_format)
        printf("\n");
    close(fd);
}

void cmd_cd(const char *path)
{
    if (!path || strlen(path) == 0) {
        path = "/";
    }
    char resolved[256];
    shell_resolve_path(path, resolved);

    struct VNodeStat st;
    if (stat(resolved, &st) < 0 || !st.is_dir) {
        printf("cd: no such directory: %s\n", path);
        return;
    }

    strncpy(g_current_shell->cwd, resolved, 255);
    g_current_shell->cwd[255] = '\0';
}

void cmd_pwd()
{
    printf("%s\n", g_current_shell->cwd);
}

void cmd_cat(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        char buffer[512];
        int bytes_read;
        while ((bytes_read = read(0, buffer, sizeof(buffer))) > 0)
            write(1, buffer, (size_t)bytes_read);
        return;
    }

    char resolved[256];
    shell_resolve_path(filename, resolved);
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        printf("cat: %s: No such file or directory\n", filename);
        set_status(1);
        return;
    }

    char buffer[1024];
    int bytes_read;
    while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        printf("%s", buffer);
    }
    close(fd);
}

void cmd_cat_piped(const char *args, const char *piped_input)
{
    if (args && args[0]) {
        cmd_cat(args);
        return;
    }
    if (piped_input) {
        printf("%s", piped_input);
        return;
    }
    cmd_cat("");
}

void cmd_stat(const char *filename)
{
    filename = skip_spaces(filename);
    if (!filename || filename[0] == '\0') {
        printf("stat: missing operand\n");
        set_status(1);
        return;
    }
    char resolved[256];
    shell_resolve_path(filename, resolved);
    struct VNodeStat st;
    if (stat(resolved, &st) < 0) {
        printf("stat: cannot stat '%s': No such file or directory\n", filename);
        set_status(1);
        return;
    }
    printf("  File: %s\n", filename);
    printf("  Size: %llu bytes\n", (unsigned long long)st.size);
    printf("  Type: %s\n", st.is_dir ? "Directory" : "Regular File");
    printf(" Inode: %llu\n", (unsigned long long)st.inode);
}

void cmd_touch(const char *filename)
{
    filename = skip_spaces(filename);
    if (!filename || filename[0] == '\0') {
        printf("touch: missing file operand\n");
        set_status(1);
        return;
    }
    char resolved[256];
    shell_resolve_path(filename, resolved);
    int fd = open(resolved, O_WRONLY | O_CREAT);
    if (fd >= 0) {
        close(fd);
    } else {
        printf("touch: failed to create '%s'\n", filename);
        set_status(1);
    }
}

void cmd_rm(const char *filename)
{
    filename = skip_spaces(filename);
    if (!filename || filename[0] == '\0') {
        printf("rm: missing file operand\n");
        set_status(1);
        return;
    }
    char resolved[256];
    shell_resolve_path(filename, resolved);
    if (unlink(resolved) != 0) {
        printf("rm: failed to delete '%s'\n", filename);
        set_status(1);
    }
}

void cmd_mkdir(const char *dirname)
{
    dirname = skip_spaces(dirname);
    if (!dirname || dirname[0] == '\0') {
        printf("mkdir: missing directory operand\n");
        set_status(1);
        return;
    }
    char resolved[256];
    shell_resolve_path(dirname, resolved);
    if (mkdir(resolved) != 0) {
        printf("mkdir: failed to create directory '%s'\n", dirname);
        set_status(1);
    }
}

void cmd_rmdir(const char *dirname)
{
    dirname = skip_spaces(dirname);
    if (!dirname || dirname[0] == '\0') {
        printf("rmdir: missing directory operand\n");
        set_status(1);
        return;
    }
    char resolved[256];
    shell_resolve_path(dirname, resolved);
    if (rmdir(resolved) != 0) {
        printf("rmdir: failed to remove '%s'\n", dirname);
        set_status(1);
    }
}

void cmd_cp(const char *args)
{
    char src[256], dst[256];
    if (!read_arg2(args, src, sizeof(src), dst, sizeof(dst), nullptr)) {
        printf("Usage: cp <src> <dst>\n");
        set_status(1);
        return;
    }

    char resolved_src[256], resolved_dst[256];
    shell_resolve_path(src, resolved_src);
    shell_resolve_path(dst, resolved_dst);

    struct VNodeStat st;
    if (stat(resolved_src, &st) < 0 || st.is_dir) {
        printf("cp: cannot stat '%s'\n", src);
        set_status(1);
        return;
    }

    int in = open(resolved_src, O_RDONLY);
    if (in < 0) {
        printf("cp: cannot open '%s'\n", src);
        set_status(1);
        return;
    }
    int out = open(resolved_dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        printf("cp: cannot create '%s'\n", dst);
        set_status(1);
        return;
    }

    char buffer[1024];
    int n;
    while ((n = read(in, buffer, sizeof(buffer))) > 0) {
        if (write(out, buffer, (size_t)n) != n) {
            printf("cp: write failed\n");
            set_status(1);
            break;
        }
    }
    close(in);
    close(out);
}

void cmd_mv(const char *args)
{
    char src[256], dst[256];
    if (!read_arg2(args, src, sizeof(src), dst, sizeof(dst), nullptr)) {
        printf("Usage: mv <src> <dst>\n");
        set_status(1);
        return;
    }

    char resolved_src[256], resolved_dst[256];
    shell_resolve_path(src, resolved_src);
    shell_resolve_path(dst, resolved_dst);
    if (rename(resolved_src, resolved_dst) != 0) {
        printf("mv: failed to move '%s' to '%s'\n", src, dst);
        set_status(1);
    }
}

void cmd_df()
{
    VolumeInfo vols[16];
    int count = get_volumes(vols, 16);
    if (count < 0) {
        printf("df: failed to get volumes\n");
        set_status(1);
        return;
    }
    printf("SOURCE        FLAGS       MOUNT  NAME\n");
    for (int i = 0; i < count; i++) {
        char flags[5];
        flags[0] = (vols[i].flags & VOLUME_FLAG_MOUNTED) ? 'm' : '-';
        flags[1] = (vols[i].flags & VOLUME_FLAG_WRITABLE) ? 'w' : 'r';
        flags[2] = (vols[i].flags & VOLUME_FLAG_SYSTEM_DATA) ? 's' : '-';
        flags[3] = (vols[i].flags & VOLUME_FLAG_STORAGE_DEVICE) ? 'd' : '-';
        flags[4] = '\0';
        printf("%-13s %-10s %-6s %s\n", vols[i].source_device, flags, vols[i].mount_path, vols[i].display_name);
    }
}

void cmd_mount()
{
    cmd_df();
}

void cmd_storage(const char *args)
{
    const char *mode = skip_spaces(args);
    if (!mode || mode[0] == '\0') {
        int current = get_storage_mode();
        const char *name = current == STORAGE_MODE_OFF ? "off" : current == STORAGE_MODE_WRITABLE ? "rw" : "ro";
        printf("storage: %s\n", name);
        return;
    }

    int next = -1;
    if (strcmp(mode, "off") == 0)
        next = STORAGE_MODE_OFF;
    else if (strcmp(mode, "ro") == 0 || strcmp(mode, "read-only") == 0)
        next = STORAGE_MODE_READ_ONLY;
    else if (strcmp(mode, "rw") == 0 || strcmp(mode, "writable") == 0)
        next = STORAGE_MODE_WRITABLE;

    if (next < 0) {
        printf("Usage: storage [off|ro|rw]\n");
        set_status(1);
        return;
    }

    if (set_storage_mode(next) != 0) {
        printf("storage: mode changes are owned by the desktop storage guard\n");
        set_status(1);
    }
}

void cmd_echo(const char *text)
{
    printf("%s\n", text);
}

void cmd_version()
{
    printf("uniOS @ %s\n", GIT_COMMIT);
}

void cmd_uname()
{
    printf("uniOS %s x86_64\n", GIT_COMMIT);
}

void cmd_exec(const char *args)
{
    while (*args == ' ')
        args++;
    if (args[0] == '\0')
        return;

    char res[256];
    shell_resolve_path(args, res);

    int pid = fork();
    if (pid == 0) {
        exec(res);
        printf("exec: failed to run %s\n", res);
        exit(-1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status);
    } else {
        printf("fork failed\n");
    }
}

static uint64_t du_path(const char *path, int depth, bool print_each)
{
    struct VNodeStat st;
    if (stat(path, &st) < 0)
        return 0;
    if (!st.is_dir) {
        if (print_each) {
            printf("%llu\t%s\n", (unsigned long long)st.size, path);
        }
        return st.size;
    }

    uint64_t total = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char name[256];
    while (depth < 16 && syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[512];
        join_path(path, name, child, sizeof(child));
        total += du_path(child, depth + 1, print_each);
    }
    close(fd);
    if (print_each)
        printf("%llu\t%s\n", (unsigned long long)total, path);
    return total;
}

static void tree_path(const char *path, int depth)
{
    if (depth > 8)
        return;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;

    char name[256];
    while (syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        char child[512];
        join_path(path, name, child, sizeof(child));
        struct VNodeStat st;
        bool is_dir = stat(child, &st) == 0 && st.is_dir;
        for (int i = 0; i < depth; i++)
            printf("  ");
        printf("%s%s\n", name, is_dir ? "/" : "");
        if (is_dir)
            tree_path(child, depth + 1);
    }
    close(fd);
}

void cmd_tree(const char *path)
{
    const char *target = skip_spaces(path);
    char resolved[256];
    shell_resolve_path((target && target[0]) ? target : ".", resolved);
    printf("%s\n", resolved);
    tree_path(resolved, 1);
}

static void find_path(const char *path, const char *pattern, int depth)
{
    if (depth > 12)
        return;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;

    char name[256];
    while (syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[512];
        join_path(path, name, child, sizeof(child));
        if (!pattern || pattern[0] == '\0' || strstr(name, pattern))
            printf("%s\n", child);
        struct VNodeStat st;
        if (stat(child, &st) == 0 && st.is_dir)
            find_path(child, pattern, depth + 1);
    }
    close(fd);
}

void cmd_find(const char *args)
{
    char dir[256] = ".";
    char pattern[128] = "";
    const char *p = skip_spaces(args);
    if (*p) {
        p = parse_token(p, dir, sizeof(dir));
        if (*p)
            parse_token(p, pattern, sizeof(pattern));
    }
    char resolved[256];
    shell_resolve_path(dir, resolved);
    find_path(resolved, pattern, 0);
}

void cmd_du(const char *path)
{
    const char *target = skip_spaces(path);
    char resolved[256];
    shell_resolve_path((target && target[0]) ? target : ".", resolved);
    uint64_t total = du_path(resolved, 0, false);
    printf("%llu\t%s\n", (unsigned long long)total, resolved);
}

void cmd_which(const char *name)
{
    name = skip_spaces(name);
    if (!name || name[0] == '\0') {
        printf("which: missing command name\n");
        set_status(1);
        return;
    }
    if (shell_find_command(name)) {
        printf("%s: shell builtin\n", name);
        return;
    }

    char candidate[256];
    snprintf(candidate, sizeof(candidate), "/bin/%s.elf", name);
    if (shell_path_exists(candidate, nullptr)) {
        printf("%s\n", candidate);
        return;
    }
    snprintf(candidate, sizeof(candidate), "/bin/%s", name);
    if (shell_path_exists(candidate, nullptr)) {
        printf("%s\n", candidate);
        return;
    }
    set_status(1);
}

void cmd_type(const char *name)
{
    name = skip_spaces(name);
    if (shell_find_command(name)) {
        printf("%s is a shell builtin\n", name);
        return;
    }
    cmd_which(name);
}

static void write_text_to_file(const char *args, bool append)
{
    char path[256];
    const char *text = parse_token(args, path, sizeof(path));
    if (path[0] == '\0') {
        printf("Usage: %s <file> <text>\n", append ? "append" : "write");
        set_status(1);
        return;
    }

    char resolved[256];
    shell_resolve_path(path, resolved);
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(resolved, flags);
    if (fd < 0) {
        printf("%s: cannot open '%s'\n", append ? "append" : "write", path);
        set_status(1);
        return;
    }
    write(fd, text, strlen(text));
    close(fd);
}

void cmd_write(const char *args)
{
    write_text_to_file(args, false);
}

void cmd_append(const char *args)
{
    write_text_to_file(args, true);
}

void cmd_hexdump(const char *filename)
{
    filename = skip_spaces(filename);
    if (!filename || filename[0] == '\0') {
        printf("Usage: hexdump <file>\n");
        set_status(1);
        return;
    }

    char resolved[256];
    shell_resolve_path(filename, resolved);
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        printf("hexdump: cannot open '%s'\n", filename);
        set_status(1);
        return;
    }

    uint8_t bytes[16];
    uint64_t offset = 0;
    int n;
    while ((n = read(fd, bytes, sizeof(bytes))) > 0) {
        printf("%08llx  ", (unsigned long long)offset);
        for (int i = 0; i < 16; i++) {
            if (i < n)
                printf("%02x ", bytes[i]);
            else
                printf("   ");
            if (i == 7)
                putchar(' ');
        }
        printf(" |");
        for (int i = 0; i < n; i++)
            putchar(bytes[i] >= 32 && bytes[i] <= 126 ? (char)bytes[i] : '.');
        printf("|\n");
        offset += (uint64_t)n;
    }
    close(fd);
}

void cmd_mem()
{
    struct MemInfo info;
    if (get_meminfo(&info) == 0) {
        printf("Memory Status:\n");
        printf("  Total: %llu KB\n", (unsigned long long)info.total_kb);
        printf("  Used:  %llu KB\n", (unsigned long long)info.used_kb);
        printf("  Free:  %llu KB\n", (unsigned long long)info.free_kb);
    } else {
        printf("mem: failed to get memory info\n");
    }
}

void cmd_ps()
{
    ProcessInfo procs[32];
    int count = get_procs(procs, 32);
    if (count < 0) {
        printf("ps: error retrieving process list\n");
        return;
    }

    printf("\x1b[90m  PID  PARENT  STATE     PRI  UID   NAME\x1b[0m\n");
    for (int i = 0; i < count; i++) {
        const char *state_str = "READY";
        switch ((int)procs[i].state) {
            case ProcessState_Ready:
                state_str = "READY ";
                break;
            case ProcessState_Running:
                state_str = "RUN   ";
                break;
            case ProcessState_Blocked:
                state_str = "BLOCK ";
                break;
            case ProcessState_Sleeping:
                state_str = "SLEEP ";
                break;
            case ProcessState_Zombie:
                state_str = "ZOMBIE";
                break;
            case ProcessState_Waiting:
                state_str = "WAIT  ";
                break;
        }
        printf("  %-3d  %-6d  %s  %-3d  %-3d   %s\n", (int)procs[i].pid, (int)procs[i].parent_pid, state_str,
               (int)procs[i].priority, (int)procs[i].uid, procs[i].name);
    }
}

void cmd_date()
{
    struct SysTime t;
    if (get_time(&t) == 0) {
        printf("%04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    } else {
        printf("date: failed to get time\n");
    }
}

void cmd_uptime()
{
    uint64_t up = get_uptime();
    uint64_t s = up % 60;
    uint64_t m = (up / 60) % 60;
    uint64_t h = (up / 3600);
    printf("up %02llu:%02llu:%02llu\n", (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
}

char *get_file_data(const char *filename, const char *piped_input, uint64_t *out_len)
{
    if (filename && filename[0]) {
        char resolved[256];
        shell_resolve_path(filename, resolved);
        struct VNodeStat st;
        if (stat(resolved, &st) < 0)
            return nullptr;
        int fd = open(resolved, O_RDONLY);
        if (fd < 0)
            return nullptr;
        char *data = (char *)malloc((size_t)st.size + 1);
        if (!data) {
            close(fd);
            return nullptr;
        }
        int64_t bytes_read = read(fd, data, (size_t)st.size);
        close(fd);
        if (bytes_read < 0) {
            free(data);
            return nullptr;
        }
        data[bytes_read] = '\0';
        *out_len = (uint64_t)bytes_read;
        return data;
    } else if (piped_input) {
        size_t len = strlen(piped_input);
        char *data = (char *)malloc(len + 1);
        if (!data)
            return nullptr;
        strncpy(data, piped_input, len);
        data[len] = '\0';
        *out_len = len;
        return data;
    } else {
        // Read from stdin (fd 0) until EOF - useful for pipes
        uint32_t max_size = 65536;
        char *data = (char *)malloc(max_size + 1);
        if (!data)
            return nullptr;

        uint32_t total_read = 0;
        while (total_read < max_size) {
            int64_t n = read(0, data + total_read, max_size - total_read);
            if (n <= 0)
                break;
            total_read += (uint32_t)n;
        }
        data[total_read] = '\0';
        *out_len = (uint64_t)total_read;
        return data;
    }
    return nullptr;
}

void cmd_wc(const char *filename, const char *piped_input)
{
    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;
    uint64_t lines = 0, words = 0, chars = 0;
    bool in_word = false;
    for (uint64_t i = 0; i < data_len; i++) {
        char c = data[i];
        chars++;
        if (c == '\n')
            lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            in_word = false;
        else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    if (data_len > 0 && data[data_len - 1] != '\n')
        lines++;
    printf("  Lines: %llu\n  Words: %llu\n  Chars: %llu\n", (unsigned long long)lines, (unsigned long long)words, (unsigned long long)chars);
    free(data);
}

static const char *parse_line_count_arg(const char *args, int *line_count)
{
    *line_count = 10;
    const char *p = skip_spaces(args);
    if (strncmp(p, "-n", 2) == 0) {
        p += 2;
        p = skip_spaces(p);
        *line_count = atoi(p);
        while (*p && !is_space(*p))
            p++;
        return skip_spaces(p);
    }
    if (*p == '-' && p[1] >= '0' && p[1] <= '9') {
        *line_count = atoi(p + 1);
        while (*p && !is_space(*p))
            p++;
        return skip_spaces(p);
    }
    return p;
}

void cmd_head(const char *args, const char *piped_input)
{
    int wanted = 10;
    const char *filename = parse_line_count_arg(args, &wanted);
    if (wanted < 0)
        wanted = 0;

    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;

    int lines = 0;
    for (uint64_t i = 0; i < data_len; i++) {
        putchar(data[i]);
        if (data[i] == '\n' && ++lines >= wanted)
            break;
    }
    free(data);
}

void cmd_tail(const char *args, const char *piped_input)
{
    int wanted = 10;
    const char *filename = parse_line_count_arg(args, &wanted);
    if (wanted < 0)
        wanted = 0;

    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;

    uint64_t start = 0;
    int lines = 0;
    for (int64_t i = (int64_t)data_len - 1; i >= 0; i--) {
        if (data[i] == '\n' && i != (int64_t)data_len - 1) {
            lines++;
            if (lines >= wanted) {
                start = (uint64_t)i + 1;
                break;
            }
        }
    }
    for (uint64_t i = start; i < data_len; i++)
        putchar(data[i]);
    free(data);
}

void cmd_grep(const char *args, const char *piped_input)
{
    if (!args || !args[0])
        return;
    char pattern[64];
    const char *p = args;
    int pi = 0;
    while (*p && *p != ' ' && pi < 63) {
        pattern[pi++] = *p++;
    }
    pattern[pi] = '\0';
    while (*p == ' ')
        p++;
    const char *filename = (*p) ? p : nullptr;
    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;
    char *line = data;
    while (line && *line) {
        char *next_line = line;
        while (*next_line && *next_line != '\n')
            next_line++;
        char saved = *next_line;
        *next_line = '\0';
        bool found = false;
        for (int i = 0; line[i]; i++) {
            bool match = true;
            for (int j = 0; pattern[j]; j++) {
                if (line[i + j] != pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                found = true;
                break;
            }
        }
        if (found)
            printf("%s\n", line);
        *next_line = saved;
        if (saved == '\n')
            line = next_line + 1;
        else
            line = nullptr;
    }
    free(data);
}

void cmd_sort(const char *filename, const char *piped_input)
{
    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;
    const int MAX_LINES = 256;
    const char *lines[MAX_LINES];
    int line_lens[MAX_LINES];
    int line_count = 0;
    uint64_t line_start = 0;
    for (uint64_t i = 0; i <= data_len && line_count < MAX_LINES; i++) {
        if (i == data_len || data[i] == '\n') {
            if (i > line_start) {
                lines[line_count] = data + line_start;
                line_lens[line_count] = (int)(i - line_start);
                line_count++;
            }
            line_start = i + 1;
        }
    }
    for (int i = 0; i < line_count - 1; i++) {
        for (int j = 0; j < line_count - i - 1; j++) {
            int min_len = (line_lens[j] < line_lens[j + 1]) ? line_lens[j] : line_lens[j + 1];
            bool swap = false;
            for (int k = 0; k < min_len; k++) {
                if (lines[j][k] > lines[j + 1][k]) {
                    swap = true;
                    break;
                }
                if (lines[j][k] < lines[j + 1][k])
                    break;
            }
            if (!swap && line_lens[j] > line_lens[j + 1])
                swap = true;
            if (swap) {
                const char *tmp = lines[j];
                lines[j] = lines[j + 1];
                lines[j + 1] = tmp;
                int tmp_len = line_lens[j];
                line_lens[j] = line_lens[j + 1];
                line_lens[j + 1] = tmp_len;
            }
        }
    }
    for (int i = 0; i < line_count; i++) {
        for (int j = 0; j < line_lens[i]; j++)
            putchar(lines[i][j]);
        putchar('\n');
    }
    free(data);
}

void cmd_uniq(const char *filename, const char *piped_input)
{
    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;
    const char *prev_line = nullptr;
    int prev_len = 0;
    uint64_t line_start = 0;
    for (uint64_t i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            const char *curr_line = data + line_start;
            int curr_len = (int)(i - line_start);
            bool is_dup = (prev_line && curr_len == prev_len);
            if (is_dup)
                for (int j = 0; j < curr_len; j++)
                    if (curr_line[j] != prev_line[j]) {
                        is_dup = false;
                        break;
                    }
            if (!is_dup && curr_len > 0) {
                for (int j = 0; j < curr_len; j++)
                    putchar(curr_line[j]);
                putchar('\n');
            }
            prev_line = curr_line;
            prev_len = curr_len;
            line_start = i + 1;
        }
    }
    free(data);
}

void cmd_rev(const char *filename, const char *piped_input)
{
    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;
    uint64_t line_start = 0;
    for (uint64_t i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            for (int64_t j = (int64_t)i - 1; j >= (int64_t)line_start; j--)
                putchar(data[j]);
            putchar('\n');
            line_start = i + 1;
        }
    }
    free(data);
}

void cmd_tac(const char *filename, const char *piped_input)
{
    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;
    const int MAX_LINES = 256;
    uint64_t line_starts[MAX_LINES];
    uint64_t line_ends[MAX_LINES];
    int line_count = 0;
    uint64_t line_start = 0;
    for (uint64_t i = 0; i <= data_len && line_count < MAX_LINES; i++) {
        if (i == data_len || data[i] == '\n') {
            if (i > line_start) {
                line_starts[line_count] = line_start;
                line_ends[line_count] = i;
                line_count++;
            }
            line_start = i + 1;
        }
    }
    for (int i = line_count - 1; i >= 0; i--) {
        for (uint64_t j = line_starts[i]; j < line_ends[i]; j++)
            putchar(data[j]);
        putchar('\n');
    }
    free(data);
}

void cmd_nl(const char *filename, const char *piped_input)
{
    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;
    int line_num = 1;
    uint64_t line_start = 0;
    for (uint64_t i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            printf("%6d  ", line_num++);
            for (uint64_t j = line_start; j < i; j++) {
                putchar(data[j]);
            }
            putchar('\n');
            line_start = i + 1;
        }
    }
    free(data);
}

void cmd_tr(const char *args, const char *piped_input)
{
    char from[128], to[128];
    const char *rest = nullptr;
    if (!read_arg2(args, from, sizeof(from), to, sizeof(to), &rest)) {
        printf("Usage: tr <set1> <set2> [file]\n");
        set_status(1);
        return;
    }

    char filename[256] = {};
    if (rest && *rest)
        parse_token(rest, filename, sizeof(filename));

    uint64_t data_len = 0;
    char *data = get_file_data(filename, piped_input, &data_len);
    if (!data)
        return;

    for (uint64_t i = 0; i < data_len; i++) {
        char c = data[i];
        for (int j = 0; from[j]; j++) {
            if (c == from[j]) {
                c = to[j] ? to[j] : to[strlen(to) - 1];
                break;
            }
        }
        putchar(c);
    }
    free(data);
}

void cmd_cpuinfo()
{
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char vendor[13];
    *reinterpret_cast<uint32_t *>(&vendor[0]) = ebx;
    *reinterpret_cast<uint32_t *>(&vendor[4]) = edx;
    *reinterpret_cast<uint32_t *>(&vendor[8]) = ecx;
    vendor[12] = 0;
    printf("Vendor: %s\n", vendor);
}

void cmd_env()
{
    if (!g_current_shell)
        return;
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_current_shell->vars[i].in_use)
            printf("%s=%s\n", g_current_shell->vars[i].name, g_current_shell->vars[i].value);
    }
}

void cmd_history()
{
    if (!g_current_shell)
        return;
    int first = g_current_shell->history_count > HISTORY_SIZE ? g_current_shell->history_count - HISTORY_SIZE : 0;
    for (int i = first; i < g_current_shell->history_count; i++)
        printf("%4d  %s\n", i + 1, g_current_shell->history[i % HISTORY_SIZE]);
}

void cmd_set(const char *args)
{
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
        cmd_env();
        return;
    }

    const char *eq = strchr(args, '=');
    if (!eq) {
        const char *value = shell_get_var(args);
        if (value)
            printf("%s\n", value);
        else
            set_status(1);
        return;
    }

    char name[MAX_VAR_NAME];
    int len = (int)(eq - args);
    if (len <= 0 || len >= MAX_VAR_NAME) {
        printf("set: invalid variable name\n");
        set_status(1);
        return;
    }
    strncpy(name, args, (size_t)len);
    name[len] = '\0';
    shell_set_var(name, eq + 1);
}

void cmd_unset(const char *name)
{
    name = skip_spaces(name);
    if (!name || name[0] == '\0') {
        printf("unset: missing variable name\n");
        set_status(1);
        return;
    }
    shell_unset_var(name);
}

void cmd_read(const char *varname)
{
    varname = skip_spaces(varname);
    if (!varname || varname[0] == '\0') {
        printf("read: missing variable name\n");
        set_status(1);
        return;
    }
    char value[MAX_VAR_VALUE];
    read_input_visible(value, sizeof(value));
    shell_set_var(varname, value);
}

void cmd_test(const char *args)
{
    set_status(evaluate_condition(args) ? 0 : 1);
}

void cmd_expr(const char *args)
{
    char left[32], op[8], right[32];
    const char *p = parse_token(args, left, sizeof(left));
    p = parse_token(p, op, sizeof(op));
    parse_token(p, right, sizeof(right));
    if (left[0] == '\0' || op[0] == '\0' || right[0] == '\0') {
        printf("Usage: expr <a> <op> <b>\n");
        set_status(1);
        return;
    }

    int a = atoi(left);
    int b = atoi(right);
    if (strcmp(op, "+") == 0)
        printf("%d\n", a + b);
    else if (strcmp(op, "-") == 0)
        printf("%d\n", a - b);
    else if (strcmp(op, "*") == 0)
        printf("%d\n", a * b);
    else if (strcmp(op, "/") == 0)
        printf("%d\n", b ? a / b : 0);
    else if (strcmp(op, "%") == 0)
        printf("%d\n", b ? a % b : 0);
    else if (strcmp(op, "==") == 0)
        printf("%d\n", a == b);
    else if (strcmp(op, "!=") == 0)
        printf("%d\n", a != b);
    else if (strcmp(op, "<") == 0)
        printf("%d\n", a < b);
    else if (strcmp(op, ">") == 0)
        printf("%d\n", a > b);
    else {
        printf("expr: unknown operator '%s'\n", op);
        set_status(1);
    }
}

void cmd_source(const char *filename)
{
    cmd_run(filename);
}

void cmd_time(const char *cmd)
{
    cmd = skip_spaces(cmd);
    if (!cmd || cmd[0] == '\0') {
        printf("Usage: time <command>\n");
        set_status(1);
        return;
    }
    uint64_t start = get_ticks();
    char copy[256];
    strncpy(copy, cmd, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    execute_single_command(copy, nullptr);
    uint64_t elapsed = get_ticks() - start;
    printf("time: %llu ms\n", (unsigned long long)elapsed);
}

void cmd_sleep(const char *args)
{
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
        printf("Usage: sleep <ms|Ns>\n");
        set_status(1);
        return;
    }
    int n = atoi(args);
    size_t len = strlen(args);
    if (len > 0 && args[len - 1] == 's')
        n *= 1000;
    if (n > 0)
        sleep_ms((uint32_t)n);
}

void cmd_true()
{
    set_status(0);
}

void cmd_false()
{
    set_status(1);
}

void cmd_kheap()
{
    MemInfo info;
    if (get_meminfo(&info) != 0) {
        printf("kheap: failed to get memory info\n");
        set_status(1);
        return;
    }
    printf("Kernel heap: %llu KB used / %llu KB total\n", (unsigned long long)info.heap_used_kb, (unsigned long long)info.heap_total_kb);
}

void cmd_sysinfo()
{
    SystemProfile info;
    if (get_sysinfo(&info) != 0) {
        printf("sysinfo: failed to get system profile\n");
        set_status(1);
        return;
    }
    printf("kernel:     %s\n", info.kernel_commit);
    printf("bootloader: %s %s\n", info.bootloader_name, info.bootloader_version);
    printf("timer_hz:   %u\n", info.timer_hz);
    printf("debug:      %s\n", info.kernel_build_debug ? "yes" : "no");
}

void cmd_random(const char *args)
{
    int count = atoi(skip_spaces(args));
    if (count <= 0)
        count = 16;
    if (count > 256)
        count = 256;
    uint8_t bytes[256];
    if (getrandom(bytes, (size_t)count) != count) {
        printf("random: getrandom failed\n");
        set_status(1);
        return;
    }
    for (int i = 0; i < count; i++)
        printf("%02x", bytes[i]);
    putchar('\n');
}

void cmd_kill(const char *args)
{
    char pid_arg[32], sig_arg[32];
    const char *p = parse_token(args, pid_arg, sizeof(pid_arg));
    parse_token(p, sig_arg, sizeof(sig_arg));
    if (pid_arg[0] == '\0') {
        printf("Usage: kill <pid> [sig]\n");
        set_status(1);
        return;
    }
    int sig = sig_arg[0] ? atoi(sig_arg) : SIGTERM;
    if ((int)syscall2(SYS_KILL, (uint64_t)atoi(pid_arg), (uint64_t)sig) != 0) {
        printf("kill: failed\n");
        set_status(1);
    }
}

void cmd_reboot()
{
    syscall0(SYS_REBOOT);
}

void cmd_poweroff()
{
    syscall0(SYS_POWEROFF);
}

void cmd_quiet(const char *args)
{
    args = skip_spaces(args);
    if (strcmp(args, "on") == 0 || strcmp(args, "1") == 0)
        syscall1(SYS_SET_QUIET, 1);
    else if (strcmp(args, "off") == 0 || strcmp(args, "0") == 0)
        syscall1(SYS_SET_QUIET, 0);
    else {
        printf("Usage: quiet <on|off>\n");
        set_status(1);
    }
}

void cmd_dmesg()
{
    printf("dmesg: kernel log export is not exposed to userland yet\n");
    set_status(1);
}

void cmd_resolve(const char *hostname)
{
    hostname = skip_spaces(hostname);
    if (!hostname || hostname[0] == '\0') {
        printf("Usage: resolve <host>\n");
        set_status(1);
        return;
    }
    in_addr addr;
    if (resolve_host(hostname, &addr) != 0) {
        printf("resolve: failed for %s\n", hostname);
        set_status(1);
        return;
    }
    uint32_t ip = addr.s_addr;
    printf("%u.%u.%u.%u\n", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

void cmd_ping(const char *target)
{
    printf("ping: ICMP echo is not exposed to userland; resolving target instead\n");
    cmd_resolve(target);
}

void cmd_lspci()
{
    printf("lspci: PCI enumeration is not exposed to userland yet\n");
    set_status(1);
}

void cmd_ifconfig()
{
    printf("ifconfig: network interface status is not exposed to userland yet\n");
    set_status(1);
}

void cmd_dhcp_request()
{
    printf("dhcp: DHCP is managed by the kernel network stack\n");
}

void cmd_login()
{
    printf("login: interactive user sessions are not wired into this shell yet\n");
    set_status(1);
}

void cmd_sound(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        printf("Usage: sound <filename>\n");
        return;
    }
    char resolved[256];
    shell_resolve_path(filename, resolved);
    sound_play(resolved);
}

void cmd_play(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        printf("Usage: play <filename>\n");
        return;
    }
    char resolved[256];
    shell_resolve_path(filename, resolved);

    uint8_t *data = NULL;
    uint32_t data_size = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint8_t *buffer = NULL;

    if (wav_open(resolved, &data, &data_size, &sample_rate, &channels, &buffer)) {
        printf("Playing %s: %u Hz, %u channels, %u bytes\n", resolved, sample_rate, channels, data_size);
        sound_config(sample_rate, (uint8_t)channels, 16);
        sound_write(data, data_size);
        // We can't free buffer yet if sound_write is async, but currently it's sync in the kernel call.
        // Actually sound_play in kernel starts DMA.
        // For now, let's just leave it allocated to be safe.
    }
}

void cmd_alias(const char *args)
{
    args = skip_spaces(args);
    if (!args || args[0] == '\0') {
        for (int i = 0; i < 32; i++) {
            if (g_current_shell->aliases[i].in_use) {
                printf("alias %s='%s'\n", g_current_shell->aliases[i].name, g_current_shell->aliases[i].value);
            }
        }
        set_status(0);
        return;
    }

    const char *eq = strchr(args, '=');
    if (!eq) {
        char name[32];
        size_t len = strlen(args);
        if (len >= 32) len = 31;
        strncpy(name, args, len);
        name[len] = '\0';
        while (len > 0 && name[len - 1] == ' ') {
            name[len - 1] = '\0';
            len--;
        }
        
        bool found = false;
        for (int i = 0; i < 32; i++) {
            if (g_current_shell->aliases[i].in_use && strcmp(g_current_shell->aliases[i].name, name) == 0) {
                printf("alias %s='%s'\n", g_current_shell->aliases[i].name, g_current_shell->aliases[i].value);
                found = true;
                break;
            }
        }
        if (!found) {
            printf("alias: %s: not found\n", name);
            set_status(1);
        } else {
            set_status(0);
        }
        return;
    }

    char name[32];
    int name_len = (int)(eq - args);
    while (name_len > 0 && args[name_len - 1] == ' ') {
        name_len--;
    }
    if (name_len <= 0) {
        printf("alias: invalid alias name\n");
        set_status(1);
        return;
    }
    if (name_len >= 32) {
        name_len = 31;
    }
    strncpy(name, args, (size_t)name_len);
    name[name_len] = '\0';

    const char *val_ptr = eq + 1;
    while (*val_ptr == ' ') {
        val_ptr++;
    }

    char val[128];
    int val_len = 0;
    if (*val_ptr == '\'' || *val_ptr == '"') {
        char quote = *val_ptr;
        val_ptr++;
        while (*val_ptr && *val_ptr != quote && val_len < 127) {
            val[val_len++] = *val_ptr++;
        }
    } else {
        while (*val_ptr && val_len < 127) {
            val[val_len++] = *val_ptr++;
        }
        while (val_len > 0 && val[val_len - 1] == ' ') {
            val_len--;
        }
    }
    val[val_len] = '\0';

    int slot = -1;
    for (int i = 0; i < 32; i++) {
        if (g_current_shell->aliases[i].in_use && strcmp(g_current_shell->aliases[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        for (int i = 0; i < 32; i++) {
            if (!g_current_shell->aliases[i].in_use) {
                slot = i;
                break;
            }
        }
    }

    if (slot == -1) {
        printf("alias: too many aliases (limit is 32)\n");
        set_status(1);
        return;
    }

    strncpy(g_current_shell->aliases[slot].name, name, 31);
    g_current_shell->aliases[slot].name[31] = '\0';
    strncpy(g_current_shell->aliases[slot].value, val, 127);
    g_current_shell->aliases[slot].value[127] = '\0';
    g_current_shell->aliases[slot].in_use = true;
    set_status(0);
}

void cmd_unalias(const char *name)
{
    name = skip_spaces(name);
    if (!name || name[0] == '\0') {
        printf("Usage: unalias <name>\n");
        set_status(1);
        return;
    }

    char target[32];
    size_t len = strlen(name);
    if (len >= 32) len = 31;
    strncpy(target, name, len);
    target[len] = '\0';
    while (len > 0 && target[len - 1] == ' ') {
        target[len - 1] = '\0';
        len--;
    }

    for (int i = 0; i < 32; i++) {
        if (g_current_shell->aliases[i].in_use && strcmp(g_current_shell->aliases[i].name, target) == 0) {
            g_current_shell->aliases[i].in_use = false;
            g_current_shell->aliases[i].name[0] = '\0';
            g_current_shell->aliases[i].value[0] = '\0';
            set_status(0);
            return;
        }
    }
    printf("unalias: %s: not found\n", target);
    set_status(1);
}

