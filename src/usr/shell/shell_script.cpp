#include "shell_internal.h"

// =============================================================================
// Script Variables
// =============================================================================

void shell_set_var(const char *name, const char *value)
{
    shell_set_var_into(g_current_shell, name, value);
}

void shell_set_var_into(ShellState *s, const char *name, const char *value)
{
    if (!s)
        return;
    // Check for existing variable
    for (int i = 0; i < MAX_VARS; i++) {
        if (s->vars[i].in_use && strcmp(s->vars[i].name, name) == 0) {
            strncpy(s->vars[i].value, value, MAX_VAR_VALUE - 1);
            return;
        }
    }
    // Find empty slot
    for (int i = 0; i < MAX_VARS; i++) {
        if (!s->vars[i].in_use) {
            strncpy(s->vars[i].name, name, MAX_VAR_NAME - 1);
            strncpy(s->vars[i].value, value, MAX_VAR_VALUE - 1);
            s->vars[i].in_use = true;
            return;
        }
    }
}

const char *shell_get_var(const char *name)
{
    return shell_get_var_from(g_current_shell, name);
}

const char *shell_get_var_from(ShellState *s, const char *name)
{
    if (!s)
        return nullptr;
    // Special variable: $?
    static char status_buf[16];
    if (strcmp(name, "?") == 0) {
        itoa(s->last_exit_status, status_buf, 10);
        return status_buf;
    }

    for (int i = 0; i < MAX_VARS; i++) {
        if (s->vars[i].in_use && strcmp(s->vars[i].name, name) == 0) {
            return s->vars[i].value;
        }
    }
    return nullptr;
}

void shell_unset_var(const char *name)
{
    if (!g_current_shell)
        return;
    for (int i = 0; i < MAX_VARS; i++) {
        if (g_current_shell->vars[i].in_use && strcmp(g_current_shell->vars[i].name, name) == 0) {
            g_current_shell->vars[i].in_use = false;
            g_current_shell->vars[i].name[0] = '\0';
            g_current_shell->vars[i].value[0] = '\0';
            return;
        }
    }
}

void expand_variables(const char *input, char *output, int output_size)
{
    int out_idx = 0;
    int in_idx = 0;

    while (input[in_idx] && out_idx < output_size - 1) {
        if (input[in_idx] == '$') {
            in_idx++;
            char var_name[MAX_VAR_NAME];
            int name_idx = 0;

            if (input[in_idx] == '?') {
                var_name[name_idx++] = '?';
                in_idx++;
            } else {
                while (input[in_idx] && name_idx < MAX_VAR_NAME - 1) {
                    char c = input[in_idx];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
                        var_name[name_idx++] = c;
                        in_idx++;
                    } else {
                        break;
                    }
                }
            }
            var_name[name_idx] = '\0';

            if (name_idx > 0) {
                const char *value = shell_get_var(var_name);
                if (value) {
                    while (*value && out_idx < output_size - 1) {
                        output[out_idx++] = *value++;
                    }
                }
            } else {
                output[out_idx++] = '$';
            }
        } else {
            output[out_idx++] = input[in_idx++];
        }
    }
    output[out_idx] = '\0';
}

// =============================================================================
// Script Control Flow
// =============================================================================

static ControlBlock block_stack[MAX_BLOCK_DEPTH];
static int block_depth = 0;

static bool should_skip_execution()
{
    for (int i = 0; i < block_depth; i++) {
        if (block_stack[i].type == BLOCK_IF) {
            bool executing = block_stack[i].in_else ? !block_stack[i].condition_met : block_stack[i].condition_met;
            if (!executing)
                return true;
        } else if (block_stack[i].type == BLOCK_WHILE) {
            if (!block_stack[i].condition_met)
                return true;
        }
    }
    return false;
}

bool evaluate_condition(const char *expr)
{
    char left[128], right[128], op[4];
    int i = 0, j = 0;

    while (expr[i] == ' ')
        i++;
    while (expr[i] && expr[i] != ' ' && expr[i] != '=' && expr[i] != '!' && expr[i] != '<' && expr[i] != '>' &&
           j < 127) {
        left[j++] = expr[i++];
    }
    left[j] = '\0';

    while (expr[i] == ' ')
        i++;
    j = 0;
    while (expr[i] && expr[i] != ' ' && j < 3)
        op[j++] = expr[i++];
    op[j] = '\0';

    while (expr[i] == ' ')
        i++;
    j = 0;
    while (expr[i] && expr[i] != ' ' && j < 127)
        right[j++] = expr[i++];
    right[j] = '\0';

    char left_expanded[128], right_expanded[128];
    expand_variables(left, left_expanded, 128);
    expand_variables(right, right_expanded, 128);

    if (strcmp(op, "==") == 0)
        return strcmp(left_expanded, right_expanded) == 0;
    if (strcmp(op, "!=") == 0)
        return strcmp(left_expanded, right_expanded) != 0;
    if (strcmp(op, "<") == 0)
        return str_to_int(left_expanded) < str_to_int(right_expanded);
    if (strcmp(op, ">") == 0)
        return str_to_int(left_expanded) > str_to_int(right_expanded);
    if (strcmp(op, "<=") == 0)
        return str_to_int(left_expanded) <= str_to_int(right_expanded);
    if (strcmp(op, ">=") == 0)
        return str_to_int(left_expanded) >= str_to_int(right_expanded);

    if (op[0] == '\0' && left_expanded[0] != '\0') {
        return strcmp(left_expanded, "0") != 0 && strcmp(left_expanded, "") != 0;
    }

    return false;
}

static const char *script_lines[MAX_SCRIPT_LINES];
static int script_line_count = 0;
static int script_current_line = 0;

bool execute_script_line(const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0' || *line == '\n' || *line == '\r')
        return true;
    if (*line == '#')
        return true;

    char trimmed[256] = {};
    int len = 0;
    while (line[len] && line[len] != '\n' && line[len] != '\r' && len < 255) {
        trimmed[len] = line[len];
        len++;
    }
    while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t'))
        len--;
    trimmed[len] = '\0';

    if (len == 0)
        return true;

    if (strncmp(trimmed, "if ", 3) == 0) {
        if (block_depth >= MAX_BLOCK_DEPTH) {
            printf("unios: too many nested blocks\n");
            return false;
        }
        block_stack[block_depth].type = BLOCK_IF;
        block_stack[block_depth].in_else = false;
        block_stack[block_depth].start_line = script_current_line;
        block_stack[block_depth].condition_met = !should_skip_execution() && evaluate_condition(trimmed + 3);
        block_depth++;
        return true;
    }

    if (strcmp(trimmed, "else") == 0) {
        if (block_depth == 0 || block_stack[block_depth - 1].type != BLOCK_IF) {
            printf("unios: 'else' without matching 'if'\n");
            return false;
        }
        block_stack[block_depth - 1].in_else = true;
        return true;
    }

    if (strcmp(trimmed, "endif") == 0) {
        if (block_depth == 0 || block_stack[block_depth - 1].type != BLOCK_IF) {
            printf("unios: 'endif' without matching 'if'\n");
            return false;
        }
        block_depth--;
        return true;
    }

    if (strncmp(trimmed, "while ", 6) == 0) {
        if (block_depth >= MAX_BLOCK_DEPTH) {
            printf("unios: too many nested blocks\n");
            return false;
        }
        block_stack[block_depth].type = BLOCK_WHILE;
        block_stack[block_depth].in_else = false;
        block_stack[block_depth].start_line = script_current_line;
        block_stack[block_depth].condition_met = !should_skip_execution() && evaluate_condition(trimmed + 6);
        block_depth++;
        return true;
    }

    if (strcmp(trimmed, "end") == 0) {
        if (block_depth == 0 || block_stack[block_depth - 1].type != BLOCK_WHILE) {
            printf("unios: 'end' without matching 'while'\n");
            return false;
        }
        block_depth--;
        if (!should_skip_execution()) {
            int while_line = block_stack[block_depth].start_line;
            const char *while_cmd = script_lines[while_line];
            while (*while_cmd == ' ' || *while_cmd == '\t')
                while_cmd++;
            if (evaluate_condition(while_cmd + 6)) {
                block_depth++;
                script_current_line = while_line;
                return true;
            }
        }
        return true;
    }

    if (should_skip_execution())
        return true;

    bool is_set_cmd = (strncmp(trimmed, "set ", 4) == 0);
    char expanded[256];
    if (is_set_cmd) {
        strncpy(expanded, trimmed, 255);
    } else {
        expand_variables(trimmed, expanded, sizeof(expanded));
    }

    if (!execute_single_command(expanded, nullptr)) {
        if (g_current_shell)
            g_current_shell->last_exit_status = 1;
    }
    return true;
}

void cmd_run(const char *filename)
{
    while (*filename == ' ')
        filename++;
    char resolved[256];
    shell_resolve_path(filename, resolved);

    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        printf("unios: %s: No such file or directory\n", filename);
        if (g_current_shell)
            g_current_shell->last_exit_status = 1;
        return;
    }

    // Since we don't have stat(), we read in chunks or use a large buffer.
    char *script_data = (char *)malloc(32768);
    if (!script_data) {
        close(fd);
        printf("unios: Out of memory for script\n");
        if (g_current_shell)
            g_current_shell->last_exit_status = 1;
        return;
    }

    int bytes_read = read(fd, script_data, 32767);
    close(fd);

    if (bytes_read < 0) {
        free(script_data);
        printf("unios: Error reading script\n");
        if (g_current_shell)
            g_current_shell->last_exit_status = 1;
        return;
    }
    script_data[bytes_read] = '\0';

    script_line_count = 0;
    const char *line_start = script_data;
    for (int i = 0; i <= bytes_read && script_line_count < MAX_SCRIPT_LINES; i++) {
        if (i == bytes_read || script_data[i] == '\n') {
            script_lines[script_line_count++] = line_start;
            line_start = script_data + i + 1;
        }
    }

    block_depth = 0;
    script_current_line = 0;
    int total_iterations = 0;
    const int MAX_ITERATIONS = 10000;

    while (script_current_line < script_line_count) {
        if (++total_iterations > MAX_ITERATIONS) {
            printf("unios: script exceeded maximum iterations\n");
            block_depth = 0;
            if (g_current_shell)
                g_current_shell->last_exit_status = 1;
            free(script_data);
            return;
        }

        if (!execute_script_line(script_lines[script_current_line])) {
            printf("unios: script error at line %d\n", script_current_line + 1);
            if (g_current_shell)
                g_current_shell->last_exit_status = 1;
            free(script_data);
            return;
        }
        script_current_line++;
    }

    if (block_depth > 0) {
        printf("unios: unclosed control block\n");
        block_depth = 0;
        if (g_current_shell)
            g_current_shell->last_exit_status = 1;
    } else {
        if (g_current_shell)
            g_current_shell->last_exit_status = 0;
    }
    free(script_data);
}
