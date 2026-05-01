#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>
#include <unistd.h>

#define PIPE_BUFFER_SIZE 4096
#define MAX_BLOCK_DEPTH 16
#define MAX_SCRIPT_LINES 256
#define HISTORY_SIZE 32
#define MAX_VARS 32
#define MAX_VAR_NAME 32
#define MAX_VAR_VALUE 128

enum CmdType
{
    CMD_NONE, // No arguments
    CMD_ARGS, // Takes rest of line as args
    CMD_PIPED // Supports piped input
};

typedef void (*CmdHandlerNone)();
typedef void (*CmdHandlerArgs)(const char *);
typedef void (*CmdHandlerPiped)(const char *, const char *);

struct CommandEntry
{
    const char *name;
    const char *usage;
    const char *description;
    CmdType type;
    CmdHandlerNone handler_none;
    CmdHandlerArgs handler_args;
    CmdHandlerPiped handler_piped;
};

struct Variable
{
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
    bool in_use;
};

struct ShellState
{
    char cwd[256];
    char history[HISTORY_SIZE][256];
    int history_count;
    int history_index;
    Variable vars[MAX_VARS];
    int last_exit_status;
};

enum BlockType
{
    BLOCK_IF,
    BLOCK_WHILE
};

struct ControlBlock
{
    BlockType type;
    bool condition_met;
    bool in_else;
    int start_line;
};

#define KEY_UP_ARROW 0x80
#define KEY_DOWN_ARROW 0x81
#define KEY_LEFT_ARROW 0x82
#define KEY_RIGHT_ARROW 0x83
#define KEY_HOME 0x84
#define KEY_END 0x85
#define KEY_DELETE 0x86
#define KEY_SHIFT_LEFT 0x90
#define KEY_SHIFT_RIGHT 0x91

extern char cmd_buffer[256];
extern int cmd_len;
extern int cursor_pos;
extern char clipboard[256];
extern int clipboard_len;
extern int selection_start;
extern int last_displayed_len;
extern int last_displayed_prompt_len;

extern ShellState *g_current_shell;

void shell_write(const char *str);
void shell_write_line(const char *str);
void shell_put_char(char c);
void print_prompt();
int get_prompt_len();

void shell_resolve_path(const char *path, char *out);
char *read_file_to_buf(const char *path, uint64_t *out_size);

void shell_set_var(const char *name, const char *value);
void shell_set_var_into(ShellState *s, const char *name, const char *value);
const char *shell_get_var(const char *name);
const char *shell_get_var_from(ShellState *s, const char *name);
void shell_unset_var(const char *name);

const CommandEntry *shell_get_commands();
int shell_get_command_count();
const CommandEntry *shell_find_command(const char *name);
bool shell_path_exists(const char *path, bool *is_dir);

void expand_variables(const char *input, char *output, int output_size);
bool evaluate_condition(const char *expr);
bool execute_script_line(const char *line);
bool execute_single_command(const char *cmd, const char *piped_input);
void execute_command();
int str_to_int(const char *s);

void cmd_help();
void cmd_ls(const char *path);
void cmd_cd(const char *path);
void cmd_pwd();
void cmd_mount();
void cmd_cat(const char *filename);
void cmd_cat_piped(const char *args, const char *piped_input);
void cmd_stat(const char *filename);
void cmd_hexdump(const char *filename);
void cmd_touch(const char *filename);
void cmd_rm(const char *filename);
void cmd_mkdir(const char *dirname);
void cmd_rmdir(const char *dirname);
void cmd_cp(const char *args);
void cmd_mv(const char *args);
void cmd_tree(const char *path);
void cmd_find(const char *args);
void cmd_du(const char *path);
void cmd_which(const char *name);
void cmd_type(const char *name);
void cmd_write(const char *args);
void cmd_append(const char *args);
void cmd_run(const char *filename);
void cmd_set(const char *args);
void cmd_unset(const char *name);
void cmd_ping(const char *target);
void cmd_sleep(const char *args);
void cmd_read(const char *varname);
void cmd_test(const char *args);
void cmd_expr(const char *args);
void cmd_source(const char *filename);
void cmd_time(const char *cmd);
void cmd_echo(const char *text);
void cmd_debug(const char *args);
void cmd_exec(const char *args);
void cmd_wc(const char *filename, const char *piped_input);
void cmd_head(const char *args, const char *piped_input);
void cmd_tail(const char *args, const char *piped_input);
void cmd_grep(const char *args, const char *piped_input);
void cmd_sort(const char *filename, const char *piped_input);
void cmd_uniq(const char *filename, const char *piped_input);
void cmd_rev(const char *filename, const char *piped_input);
void cmd_tac(const char *filename, const char *piped_input);
void cmd_nl(const char *filename, const char *piped_input);
void cmd_tr(const char *args, const char *piped_input);
void cmd_df();
void cmd_mem();
void cmd_storage(const char *args);
void cmd_kill(const char *args);
void cmd_reboot();
void cmd_poweroff();
void cmd_quiet(const char *args);
void cmd_random(const char *args);
void cmd_sysinfo();
void cmd_resolve(const char *hostname);

void cmd_sound(const char *filename);
void cmd_play(const char *filename);
void cmd_date();
void cmd_uptime();
void cmd_history();
void cmd_ps();
void cmd_useradd(const char *username);
void cmd_passwd(const char *args);
void cmd_kheap();
void cmd_env();
void cmd_true();
void cmd_false();
void cmd_dmesg();
void cmd_login();
void cmd_version();
void cmd_uname();
void cmd_cpuinfo();
void cmd_lspci();
void cmd_ifconfig();
void cmd_dhcp_request();

// Auth & Input
bool shell_login();
void read_input_hidden(char *buf, int max_len);
void read_input_visible(char *buf, int max_len);
bool constant_time_equals(const char *a, const char *b, size_t len);

// Editor
void redraw_line_at(int row, int new_cursor_pos);
void clear_line();
void display_line();
void add_to_history(const char *cmd);
