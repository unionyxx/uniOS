#include "shell.h"
#include "terminal.h"
#include "graphics.h"
#include "unifs.h"
#include "pmm.h"
#include "io.h"
#include "acpi.h"
#include "timer.h"
#include "input.h"
#include "rtc.h"
#include "pci.h"
#include "net.h"
#include "e1000.h"
#include "ipv4.h"
#include "icmp.h"
#include "dhcp.h"
#include "dns.h"
#include "kstring.h"
#include "heap.h"
#include "version.h"
#include "scheduler.h"
#include <stddef.h>

// Use shared string utilities
using kstring::strcmp;
using kstring::strncmp;
using kstring::strlen;
using kstring::strcpy;

// =============================================================================
// Command Dispatch Table Types
// =============================================================================
enum CmdType {
    CMD_NONE,      // No arguments (e.g., "help", "ls")
    CMD_ARGS,      // Takes rest of line as args (e.g., "cat file.txt")
    CMD_PIPED      // Supports piped input (e.g., "wc", "grep")
};

typedef void (*CmdHandlerNone)();
typedef void (*CmdHandlerArgs)(const char*);
typedef void (*CmdHandlerPiped)(const char*, const char*);

struct CommandEntry {
    const char* name;
    CmdType type;
    CmdHandlerNone handler_none;
    CmdHandlerArgs handler_args;
    CmdHandlerPiped handler_piped;
};

// Piping support
#define PIPE_BUFFER_SIZE 4096
static char pipe_buffer_a[PIPE_BUFFER_SIZE];
static char pipe_buffer_b[PIPE_BUFFER_SIZE];

// Forward declaration for piped command execution
static bool execute_single_command(const char* cmd, const char* piped_input);


static char cmd_buffer[256];
static int cmd_len = 0;
static int cursor_pos = 0; // Position within cmd_buffer

// Command history
#define HISTORY_SIZE 10
static char history[HISTORY_SIZE][256];
static int history_count = 0;
static int history_index = -1;  // Current browsing position (-1 = not browsing)

// Clipboard for cut/copy/paste
static char clipboard[256];
static int clipboard_len = 0;

// Text selection state (-1 = no selection)
static int selection_start = -1;  // Position where selection began

// Special key codes (sent by input layer via escape sequences)
#define KEY_UP_ARROW    0x80
#define KEY_DOWN_ARROW  0x81
#define KEY_LEFT_ARROW  0x82
#define KEY_RIGHT_ARROW 0x83
#define KEY_HOME        0x84
#define KEY_END         0x85
#define KEY_DELETE      0x86
// Shift+Arrow for text selection
#define KEY_SHIFT_LEFT  0x90
#define KEY_SHIFT_RIGHT 0x91

// Bootloader info (set by kmain.cpp from Limine request)
extern const char* g_bootloader_name;
extern const char* g_bootloader_version;

// =============================================================================
// Script Variables
// =============================================================================
#define MAX_VARS 32
#define MAX_VAR_NAME 32
#define MAX_VAR_VALUE 256

struct ShellVariable {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
    bool in_use;
};

static ShellVariable shell_vars[MAX_VARS];
static int last_exit_status = 0;  // $? - last command exit status

// Set a shell variable
static void shell_set_var(const char* name, const char* value) {
    // Check for existing variable
    for (int i = 0; i < MAX_VARS; i++) {
        if (shell_vars[i].in_use && strcmp(shell_vars[i].name, name) == 0) {
            // Update existing
            int j = 0;
            while (value[j] && j < MAX_VAR_VALUE - 1) {
                shell_vars[i].value[j] = value[j];
                j++;
            }
            shell_vars[i].value[j] = '\0';
            return;
        }
    }
    // Find empty slot
    for (int i = 0; i < MAX_VARS; i++) {
        if (!shell_vars[i].in_use) {
            int j = 0;
            while (name[j] && j < MAX_VAR_NAME - 1) {
                shell_vars[i].name[j] = name[j];
                j++;
            }
            shell_vars[i].name[j] = '\0';
            
            j = 0;
            while (value[j] && j < MAX_VAR_VALUE - 1) {
                shell_vars[i].value[j] = value[j];
                j++;
            }
            shell_vars[i].value[j] = '\0';
            shell_vars[i].in_use = true;
            return;
        }
    }
    // No space - silently fail (could add error message)
}

// Get a shell variable (returns nullptr if not found)
static const char* shell_get_var(const char* name) {
    // Special variable: $?
    static char status_buf[16];
    if (strcmp(name, "?") == 0) {
        int val = last_exit_status;
        int i = 0;
        if (val == 0) {
            status_buf[i++] = '0';
        } else {
            if (val < 0) { status_buf[i++] = '-'; val = -val; }
            char tmp[16]; int j = 0;
            while (val > 0) { tmp[j++] = '0' + (val % 10); val /= 10; }
            while (j > 0) status_buf[i++] = tmp[--j];
        }
        status_buf[i] = '\0';
        return status_buf;
    }
    
    for (int i = 0; i < MAX_VARS; i++) {
        if (shell_vars[i].in_use && strcmp(shell_vars[i].name, name) == 0) {
            return shell_vars[i].value;
        }
    }
    return nullptr;
}

// Unset a shell variable
static void shell_unset_var(const char* name) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (shell_vars[i].in_use && strcmp(shell_vars[i].name, name) == 0) {
            shell_vars[i].in_use = false;
            shell_vars[i].name[0] = '\0';
            shell_vars[i].value[0] = '\0';
            return;
        }
    }
}

// Expand variables in a string ($NAME -> value)
static void expand_variables(const char* input, char* output, int output_size) {
    int out_idx = 0;
    int in_idx = 0;
    
    while (input[in_idx] && out_idx < output_size - 1) {
        if (input[in_idx] == '$') {
            // Extract variable name
            in_idx++;
            char var_name[MAX_VAR_NAME];
            int name_idx = 0;
            
            // Variable name: alphanumeric and underscore, or just '?'
            if (input[in_idx] == '?') {
                var_name[name_idx++] = '?';
                in_idx++;
            } else {
                while (input[in_idx] && name_idx < MAX_VAR_NAME - 1) {
                    char c = input[in_idx];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_') {
                        var_name[name_idx++] = c;
                        in_idx++;
                    } else {
                        break;
                    }
                }
            }
            var_name[name_idx] = '\0';
            
            if (name_idx > 0) {
                const char* value = shell_get_var(var_name);
                if (value) {
                    while (*value && out_idx < output_size - 1) {
                        output[out_idx++] = *value++;
                    }
                }
                // If variable not found, expand to empty string
            } else {
                // Lone $ - output as-is
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
#define MAX_BLOCK_DEPTH 16
#define MAX_SCRIPT_LINES 256

enum BlockType { BLOCK_IF, BLOCK_WHILE };

struct ControlBlock {
    BlockType type;
    bool condition_met;   // Was condition true?
    bool in_else;         // Currently in else branch?
    int start_line;       // For while: line number to loop back
};

static ControlBlock block_stack[MAX_BLOCK_DEPTH];
static int block_depth = 0;

// Check if we should skip execution based on control flow
static bool should_skip_execution() {
    for (int i = 0; i < block_depth; i++) {
        if (block_stack[i].type == BLOCK_IF) {
            bool executing = block_stack[i].in_else ? !block_stack[i].condition_met 
                                                     : block_stack[i].condition_met;
            if (!executing) return true;
        } else if (block_stack[i].type == BLOCK_WHILE) {
            if (!block_stack[i].condition_met) return true;
        }
    }
    return false;
}

// Simple string to integer conversion
static int str_to_int(const char* s) {
    int result = 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}

// Evaluate a condition expression
// Supports: ==, !=, <, >, <=, >=
static bool evaluate_condition(const char* expr) {
    char left[128], right[128], op[4];
    int i = 0, j = 0;
    
    // Skip leading spaces
    while (expr[i] == ' ') i++;
    
    // Extract left operand
    while (expr[i] && expr[i] != ' ' && expr[i] != '=' && expr[i] != '!' &&
           expr[i] != '<' && expr[i] != '>' && j < 127) {
        left[j++] = expr[i++];
    }
    left[j] = '\0';
    
    // Skip spaces
    while (expr[i] == ' ') i++;
    
    // Extract operator
    j = 0;
    while (expr[i] && expr[i] != ' ' && j < 3) {
        op[j++] = expr[i++];
    }
    op[j] = '\0';
    
    // Skip spaces
    while (expr[i] == ' ') i++;
    
    // Extract right operand
    j = 0;
    while (expr[i] && expr[i] != ' ' && j < 127) {
        right[j++] = expr[i++];
    }
    right[j] = '\0';
    
    // Expand variables in operands
    char left_expanded[128], right_expanded[128];
    expand_variables(left, left_expanded, 128);
    expand_variables(right, right_expanded, 128);
    
    // Compare
    if (strcmp(op, "==") == 0) {
        return strcmp(left_expanded, right_expanded) == 0;
    } else if (strcmp(op, "!=") == 0) {
        return strcmp(left_expanded, right_expanded) != 0;
    } else if (strcmp(op, "<") == 0) {
        return str_to_int(left_expanded) < str_to_int(right_expanded);
    } else if (strcmp(op, ">") == 0) {
        return str_to_int(left_expanded) > str_to_int(right_expanded);
    } else if (strcmp(op, "<=") == 0) {
        return str_to_int(left_expanded) <= str_to_int(right_expanded);
    } else if (strcmp(op, ">=") == 0) {
        return str_to_int(left_expanded) >= str_to_int(right_expanded);
    }
    
    // If just a value with no operator, check if non-empty and not "0"
    if (op[0] == '\0' && left_expanded[0] != '\0') {
        return strcmp(left_expanded, "0") != 0 && strcmp(left_expanded, "") != 0;
    }
    
    return false;
}

extern "C" void jump_to_user_mode(uint64_t code_sel, uint64_t stack, uint64_t entry);

// String functions now provided by kstring.h

static void add_to_history(const char* cmd) {
    if (cmd[0] == '\0') return;  // Don't add empty commands
    
    // Don't add if same as last command
    if (history_count > 0 && strcmp(history[(history_count - 1) % HISTORY_SIZE], cmd) == 0) {
        return;
    }
    
    strcpy(history[history_count % HISTORY_SIZE], cmd);
    history_count++;
}

static int last_displayed_len = 0;  // Track last displayed line length for proper clearing

// =============================================================================
// Rich Prompt: user@unios:~$
// =============================================================================
// Prompt segments:
//   "user"   (4 chars) - blue
//   "@unios" (6 chars) - green  
//   ":~$ "   (4 chars) - gray
// Total: 14 characters
#define PROMPT_LEN 14

static void print_prompt() {
    int col, row;
    g_terminal.get_cursor_pos(&col, &row);
    
    // Draw colored prompt segments
    // "user" in blue
    g_terminal.write_char_at_color(0, row, 'u', COLOR_PROMPT_USER, COLOR_BG);
    g_terminal.write_char_at_color(1, row, 's', COLOR_PROMPT_USER, COLOR_BG);
    g_terminal.write_char_at_color(2, row, 'e', COLOR_PROMPT_USER, COLOR_BG);
    g_terminal.write_char_at_color(3, row, 'r', COLOR_PROMPT_USER, COLOR_BG);
    
    // "@unios" in green
    g_terminal.write_char_at_color(4, row, '@', COLOR_PROMPT_HOST, COLOR_BG);
    g_terminal.write_char_at_color(5, row, 'u', COLOR_PROMPT_HOST, COLOR_BG);
    g_terminal.write_char_at_color(6, row, 'n', COLOR_PROMPT_HOST, COLOR_BG);
    g_terminal.write_char_at_color(7, row, 'i', COLOR_PROMPT_HOST, COLOR_BG);
    g_terminal.write_char_at_color(8, row, 'o', COLOR_PROMPT_HOST, COLOR_BG);
    g_terminal.write_char_at_color(9, row, 's', COLOR_PROMPT_HOST, COLOR_BG);
    
    // ":~$ " in gray
    g_terminal.write_char_at_color(10, row, ':', COLOR_PROMPT_PATH, COLOR_BG);
    g_terminal.write_char_at_color(11, row, '~', COLOR_PROMPT_PATH, COLOR_BG);
    g_terminal.write_char_at_color(12, row, '$', COLOR_PROMPT_PATH, COLOR_BG);
    g_terminal.write_char_at_color(13, row, ' ', COLOR_TEXT, COLOR_BG);
    
    g_terminal.set_cursor_pos(PROMPT_LEN, row);
}

// Helper to redraw entire command line without ANY cursor glitches
// Uses ONLY direct drawing methods - never put_char
static void redraw_line_at(int row, int new_cursor_pos) {
    // 1. Hide cursor completely - sync position first so it clears at right spot
    g_terminal.set_cursor_pos(PROMPT_LEN + cursor_pos, row);
    g_terminal.set_cursor_visible(false);
    
    // 2. Calculate how much to clear (max of current and previous length + extra margin)
    int clear_count = last_displayed_len + PROMPT_LEN;
    if (cmd_len + PROMPT_LEN > clear_count) clear_count = cmd_len + PROMPT_LEN;
    
    // 3. Clear entire line area using direct method (after prompt)
    g_terminal.clear_chars(PROMPT_LEN, row, clear_count);
    
    // 4. Draw new content - highlight selected text if selection active
    int sel_min = -1, sel_max = -1;
    if (selection_start >= 0) {
        sel_min = (selection_start < cursor_pos) ? selection_start : cursor_pos;
        sel_max = (selection_start > cursor_pos) ? selection_start : cursor_pos;
    }
    
    for (int i = 0; i < cmd_len; i++) {
        bool is_selected = (sel_min >= 0 && i >= sel_min && i < sel_max);
        if (is_selected) {
            // Draw with inverted colors for selection
            g_terminal.write_char_at_color(PROMPT_LEN + i, row, cmd_buffer[i], 0xFF000000, 0xFFFFFFFF);
        } else {
            g_terminal.write_char_at(PROMPT_LEN + i, row, cmd_buffer[i]);
        }
    }
    
    // 5. Update tracking variables
    last_displayed_len = cmd_len;
    cursor_pos = new_cursor_pos;
    
    // 6. Position and show cursor at new location
    g_terminal.set_cursor_pos(PROMPT_LEN + cursor_pos, row);
    g_terminal.set_cursor_visible(true);
}

// Clear line - does NOT show cursor at end (called before display_line)
static void clear_line() {
    int col, row;
    g_terminal.get_cursor_pos(&col, &row);
    
    g_terminal.set_cursor_visible(false);
    
    // Clear entire area with direct method
    int clear_count = last_displayed_len + PROMPT_LEN;
    if (cmd_len + PROMPT_LEN > clear_count) clear_count = cmd_len + PROMPT_LEN;
    g_terminal.clear_chars(PROMPT_LEN, row, clear_count);
    
    cmd_len = 0;
    cursor_pos = 0;
    last_displayed_len = 0;
    
    g_terminal.set_cursor_pos(PROMPT_LEN, row);
    // NOTE: Do NOT show cursor here - display_line will show it at the right position
}

// Display line for history - shows cursor at end
static void display_line() {
    int col, row;
    g_terminal.get_cursor_pos(&col, &row);
    
    // Cursor should already be hidden from clear_line
    g_terminal.set_cursor_visible(false);
    
    for (int i = 0; i < cmd_len; i++) {
        g_terminal.write_char_at(PROMPT_LEN + i, row, cmd_buffer[i]);
    }
    
    cursor_pos = cmd_len;
    last_displayed_len = cmd_len;
    
    g_terminal.set_cursor_pos(PROMPT_LEN + cursor_pos, row);
    g_terminal.set_cursor_visible(true);
}

// =============================================================================
// Error message helpers for better UX
// =============================================================================

static void error_file_not_found(const char* filename) {
    g_terminal.write("Error: '");
    g_terminal.write(filename);
    g_terminal.write_line("' not found");
}

static void error_usage(const char* usage) {
    g_terminal.write("Usage: ");
    g_terminal.write_line(usage);
}

// =============================================================================
// Script Commands
// =============================================================================

// set NAME=value - set a variable
static void cmd_set(const char* args) {
    // Find '=' separator
    const char* eq = args;
    while (*eq && *eq != '=') eq++;
    
    if (*eq != '=') {
        // No '=' found - list all variables
        bool any = false;
        for (int i = 0; i < MAX_VARS; i++) {
            if (shell_vars[i].in_use) {
                if (!any) { g_terminal.write_line("Variables:"); any = true; }
                g_terminal.write("  ");
                g_terminal.write(shell_vars[i].name);
                g_terminal.write("=");
                g_terminal.write_line(shell_vars[i].value);
            }
        }
        if (!any) g_terminal.write_line("No variables set.");
        return;
    }
    
    // Extract name and value
    char name[MAX_VAR_NAME];
    int name_len = eq - args;
    if (name_len >= MAX_VAR_NAME) name_len = MAX_VAR_NAME - 1;
    for (int i = 0; i < name_len; i++) name[i] = args[i];
    name[name_len] = '\0';
    
    // Skip spaces in name
    while (name_len > 0 && name[name_len - 1] == ' ') {
        name[--name_len] = '\0';
    }
    
    // Get value (after '=')
    const char* value = eq + 1;
    while (*value == ' ') value++;  // Skip leading spaces
    
    // Handle simple arithmetic: VAR+N or VAR-N
    char final_value[MAX_VAR_VALUE];
    if (value[0] == '$') {
        // Check for arithmetic
        char var_ref[MAX_VAR_NAME];
        int vi = 0;
        const char* p = value + 1;
        while (*p && *p != '+' && *p != '-' && vi < MAX_VAR_NAME - 1) {
            var_ref[vi++] = *p++;
        }
        var_ref[vi] = '\0';
        
        if (*p == '+' || *p == '-') {
            char op = *p++;
            int operand = str_to_int(p);
            const char* current = shell_get_var(var_ref);
            int current_val = current ? str_to_int(current) : 0;
            int result = (op == '+') ? current_val + operand : current_val - operand;
            
            // Convert result to string
            int fi = 0;
            if (result < 0) { final_value[fi++] = '-'; result = -result; }
            if (result == 0) {
                final_value[fi++] = '0';
            } else {
                char tmp[16]; int ti = 0;
                while (result > 0) { tmp[ti++] = '0' + (result % 10); result /= 10; }
                while (ti > 0) final_value[fi++] = tmp[--ti];
            }
            final_value[fi] = '\0';
            value = final_value;
        } else {
            // Just variable expansion
            expand_variables(value, final_value, MAX_VAR_VALUE);
            value = final_value;
        }
    }
    
    shell_set_var(name, value);
}

// unset NAME - remove a variable
static void cmd_unset(const char* name) {
    // Skip leading spaces
    while (*name == ' ') name++;
    shell_unset_var(name);
}

// Script execution state
static const char* script_lines[MAX_SCRIPT_LINES];
static int script_line_count = 0;
static int script_current_line = 0;

// Execute a single script line (with control flow handling)
static bool execute_script_line(const char* line) {
    // Skip leading whitespace
    while (*line == ' ' || *line == '\t') line++;
    
    // Skip empty lines
    if (*line == '\0' || *line == '\n' || *line == '\r') return true;
    
    // Skip comments
    if (*line == '#') return true;
    
    // Trim trailing whitespace
    char trimmed[256];
    int len = 0;
    while (line[len] && line[len] != '\n' && line[len] != '\r' && len < 255) {
        trimmed[len] = line[len];
        len++;
    }
    while (len > 0 && (trimmed[len-1] == ' ' || trimmed[len-1] == '\t')) len--;
    trimmed[len] = '\0';
    
    if (len == 0) return true;
    
    // Handle control flow keywords
    if (strncmp(trimmed, "if ", 3) == 0) {
        // Push if block
        if (block_depth >= MAX_BLOCK_DEPTH) {
            g_terminal.write_line("Error: Too many nested blocks");
            return false;
        }
        block_stack[block_depth].type = BLOCK_IF;
        block_stack[block_depth].in_else = false;
        block_stack[block_depth].start_line = script_current_line;
        
        if (!should_skip_execution()) {
            block_stack[block_depth].condition_met = evaluate_condition(trimmed + 3);
        } else {
            block_stack[block_depth].condition_met = false;
        }
        block_depth++;
        return true;
    }
    
    if (strcmp(trimmed, "else") == 0) {
        if (block_depth == 0 || block_stack[block_depth - 1].type != BLOCK_IF) {
            g_terminal.write_line("Error: 'else' without matching 'if'");
            return false;
        }
        block_stack[block_depth - 1].in_else = true;
        return true;
    }
    
    if (strcmp(trimmed, "endif") == 0) {
        if (block_depth == 0 || block_stack[block_depth - 1].type != BLOCK_IF) {
            g_terminal.write_line("Error: 'endif' without matching 'if'");
            return false;
        }
        block_depth--;
        return true;
    }
    
    if (strncmp(trimmed, "while ", 6) == 0) {
        // Push while block
        if (block_depth >= MAX_BLOCK_DEPTH) {
            g_terminal.write_line("Error: Too many nested blocks");
            return false;
        }
        block_stack[block_depth].type = BLOCK_WHILE;
        block_stack[block_depth].in_else = false;
        block_stack[block_depth].start_line = script_current_line;
        
        if (!should_skip_execution()) {
            block_stack[block_depth].condition_met = evaluate_condition(trimmed + 6);
        } else {
            block_stack[block_depth].condition_met = false;
        }
        block_depth++;
        return true;
    }
    
    if (strcmp(trimmed, "end") == 0) {
        if (block_depth == 0 || block_stack[block_depth - 1].type != BLOCK_WHILE) {
            g_terminal.write_line("Error: 'end' without matching 'while'");
            return false;
        }
        
        // Pop the while block first to check parent execution state
        block_depth--;
        
        // Only loop if parent blocks allow execution
        if (!should_skip_execution()) {
            // Re-evaluate condition for loop continuation
            int while_line = block_stack[block_depth].start_line;
            const char* while_cmd = script_lines[while_line];
            while (*while_cmd == ' ' || *while_cmd == '\t') while_cmd++;
            
            if (evaluate_condition(while_cmd + 6)) {
                // Push the while block back and loop
                block_depth++;
                script_current_line = while_line;
                return true;
            }
        }
        
        // Loop done (condition false or parent skipping)
        return true;
    }
    
    // Normal command - check if we should execute
    if (should_skip_execution()) return true;
    
    // Don't expand variables for 'set' command - it handles its own expansion
    // This is needed for arithmetic like set I=$I+1 to work correctly
    bool is_set_cmd = (strncmp(trimmed, "set ", 4) == 0);
    
    char expanded[256];
    if (is_set_cmd) {
        // Copy without expansion
        int i = 0;
        while (trimmed[i] && i < 255) {
            expanded[i] = trimmed[i];
            i++;
        }
        expanded[i] = '\0';
    } else {
        expand_variables(trimmed, expanded, sizeof(expanded));
    }
    
    bool result = execute_single_command(expanded, nullptr);
    // Only set exit status to 1 if command wasn't recognized
    // Commands like true, false, test set their own exit status
    if (!result) {
        last_exit_status = 1;
    }
    return true;
}

// run <script> - execute a script file
static void cmd_run(const char* filename) {
    // Skip leading spaces
    while (*filename == ' ') filename++;
    
    const UniFSFile* file = unifs_open(filename);
    if (!file) {
        error_file_not_found(filename);
        last_exit_status = 1;
        return;
    }
    
    // Parse file into lines
    const char* data = (const char*)file->data;
    uint64_t size = file->size;
    
    script_line_count = 0;
    const char* line_start = data;
    
    for (uint64_t i = 0; i <= size && script_line_count < MAX_SCRIPT_LINES; i++) {
        if (i == size || data[i] == '\n') {
            script_lines[script_line_count++] = line_start;
            line_start = data + i + 1;
        }
    }
    
    // Reset control flow
    block_depth = 0;
    
    // Execute lines (with infinite loop protection)
    script_current_line = 0;
    int total_iterations = 0;
    const int MAX_ITERATIONS = 10000;  // Prevent infinite loops
    
    while (script_current_line < script_line_count) {
        if (++total_iterations > MAX_ITERATIONS) {
            g_terminal.write_line("Error: Script exceeded maximum iterations (infinite loop?)");
            block_depth = 0;
            last_exit_status = 1;
            return;
        }
        
        if (!execute_script_line(script_lines[script_current_line])) {
            g_terminal.write("Script error at line ");
            char num[16];
            int n = script_current_line + 1;
            int i = 0;
            if (n == 0) num[i++] = '0';
            else {
                char tmp[16]; int j = 0;
                while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
                while (j > 0) num[i++] = tmp[--j];
            }
            num[i] = '\0';
            g_terminal.write_line(num);
            last_exit_status = 1;
            return;
        }
        script_current_line++;
    }
    
    // Check for unclosed blocks
    if (block_depth > 0) {
        g_terminal.write_line("Error: Unclosed control block at end of script");
        block_depth = 0;
        last_exit_status = 1;
        return;
    }
    
    last_exit_status = 0;
}

static void cmd_help() {
    g_terminal.write_line("File Commands:");
    g_terminal.write_line("  ls        - List files with sizes");
    g_terminal.write_line("  cat <f>   - Show file contents");
    g_terminal.write_line("  stat <f>  - Show file information");
    g_terminal.write_line("  hexdump <f> - Hex dump of file");
    g_terminal.write_line("  touch <f> - Create empty file");
    g_terminal.write_line("  rm <f>    - Delete file");
    g_terminal.write_line("  write <f> <text> - Write text to file");
    g_terminal.write_line("  append <f> <text> - Append text to file");
    g_terminal.write_line("  df        - Show filesystem stats");
    g_terminal.write_line("");
    g_terminal.write_line("System Commands:");
    g_terminal.write_line("  mem       - Show memory usage");
    g_terminal.write_line("  date      - Show current date/time");
    g_terminal.write_line("  uptime    - Show system uptime");
    g_terminal.write_line("  version   - Show kernel version");
    g_terminal.write_line("  uname     - System information");
    g_terminal.write_line("  cpuinfo   - CPU information");
    g_terminal.write_line("  lspci     - List PCI devices");
    g_terminal.write_line("");
    g_terminal.write_line("Network Commands:");
    g_terminal.write_line("  ifconfig  - Show network config");
    g_terminal.write_line("  dhcp      - Request IP via DHCP");
    g_terminal.write_line("  ping <ip> - Ping an IP address");
    g_terminal.write_line("");
    g_terminal.write_line("Scripting:");
    g_terminal.write_line("  run <f>   - Execute script file");
    g_terminal.write_line("  source <f> - Run script (same as run)");
    g_terminal.write_line("  set N=V   - Set variable (or list all)");
    g_terminal.write_line("  unset N   - Remove variable");
    g_terminal.write_line("  env       - List all variables");
    g_terminal.write_line("  $NAME     - Variable expansion");
    g_terminal.write_line("  # comment - Script comments");
    g_terminal.write_line("  if/else/endif - Conditionals");
    g_terminal.write_line("  while/end - Loops");
    g_terminal.write_line("  test <expr> - Evaluate condition");
    g_terminal.write_line("  expr <n> <op> <n> - Arithmetic");
    g_terminal.write_line("  read <var> - Read input to variable");
    g_terminal.write_line("");
    g_terminal.write_line("Utilities:");
    g_terminal.write_line("  true/false - Exit with 0/1");
    g_terminal.write_line("  sleep <ms> - Wait milliseconds");
    g_terminal.write_line("  time <cmd> - Measure execution time");
    g_terminal.write_line("  exit      - Shutdown (alias for poweroff)");
    g_terminal.write_line("");
    g_terminal.write_line("Text Processing (pipe-friendly):");
    g_terminal.write_line("  wc [f]    - Count lines/words/chars");
    g_terminal.write_line("  head [n] [f] - First n lines (default 10)");
    g_terminal.write_line("  tail [n] [f] - Last n lines (default 10)");
    g_terminal.write_line("  grep <p> [f] - Search for pattern");
    g_terminal.write_line("  sort [f]  - Sort lines alphabetically");
    g_terminal.write_line("  uniq [f]  - Remove duplicate lines");
    g_terminal.write_line("  rev [f]   - Reverse characters in each line");
    g_terminal.write_line("  tac [f]   - Print lines in reverse order");
    g_terminal.write_line("  nl [f]    - Number lines");
    g_terminal.write_line("  tr <a> <b> - Translate char a to b (pipe)");
    g_terminal.write_line("  echo <text> - Print text");
    g_terminal.write_line("");
    g_terminal.write_line("Other:");
    g_terminal.write_line("  clear     - Clear screen");
    g_terminal.write_line("  gui       - Start GUI mode");
    g_terminal.write_line("  help      - Show this help");
    g_terminal.write_line("  reboot    - Reboot system");
    g_terminal.write_line("  poweroff  - Shutdown system");
    g_terminal.write_line("");
    g_terminal.write_line("Piping: cmd1 | cmd2 - Pass output as input");
    g_terminal.write_line("Shortcuts:");
    g_terminal.write_line("  Tab       - Command/filename completion");
    g_terminal.write_line("  Ctrl+A/E  - Move to start/end");
    g_terminal.write_line("  Ctrl+U/K  - Cut before/after cursor");
    g_terminal.write_line("  Ctrl+W    - Delete word");
    g_terminal.write_line("  Ctrl+Y    - Paste");
    g_terminal.write_line("  Ctrl+C    - Copy selection / cancel");
    g_terminal.write_line("  Ctrl+L    - Clear screen");
    g_terminal.write_line("  Shift+Arrows - Select text");
}

static void cmd_ls() {
    uint64_t count = unifs_get_file_count();
    
    if (count == 0) {
        g_terminal.write_line("No files.");
        return;
    }
    
    for (uint64_t i = 0; i < count; i++) {
        const char* name = unifs_get_file_name(i);
        uint64_t size = unifs_get_file_size_by_index(i);
        int type = unifs_get_file_type(name);
        
        if (name) {
            // Format size (right-aligned in 8 chars)
            char size_str[16];
            int si = 0;
            if (size >= 1024) {
                uint64_t kb = size / 1024;
                if (kb >= 1000) size_str[si++] = '0' + (kb / 1000) % 10;
                if (kb >= 100) size_str[si++] = '0' + (kb / 100) % 10;
                if (kb >= 10) size_str[si++] = '0' + (kb / 10) % 10;
                size_str[si++] = '0' + kb % 10;
                size_str[si++] = 'K';
            } else {
                if (size >= 1000) size_str[si++] = '0' + (size / 1000) % 10;
                if (size >= 100) size_str[si++] = '0' + (size / 100) % 10;
                if (size >= 10) size_str[si++] = '0' + (size / 10) % 10;
                size_str[si++] = '0' + size % 10;
                size_str[si++] = 'B';
            }
            size_str[si] = 0;
            
            // Type indicator
            const char* type_str;
            switch (type) {
                case UNIFS_TYPE_TEXT: type_str = "[TXT]"; break;
                case UNIFS_TYPE_ELF:  type_str = "[ELF]"; break;
                case UNIFS_TYPE_BINARY: type_str = "[BIN]"; break;
                default: type_str = "[???]"; break;
            }
            
            g_terminal.write("  ");
            g_terminal.write(type_str);
            g_terminal.write(" ");
            
            // Pad size to 6 chars
            for (int p = si; p < 6; p++) g_terminal.write(" ");
            g_terminal.write(size_str);
            g_terminal.write("  ");
            g_terminal.write_line(name);
        }
    }
}

static void cmd_stat(const char* filename) {
    if (!unifs_file_exists(filename)) {
        error_file_not_found(filename);
        return;
    }
    
    uint64_t size = unifs_get_file_size(filename);
    int type = unifs_get_file_type(filename);
    
    g_terminal.write("  File: ");
    g_terminal.write_line(filename);
    
    g_terminal.write("  Size: ");
    char size_str[32];
    int si = 0;
    if (size >= 10000) size_str[si++] = '0' + (size / 10000) % 10;
    if (size >= 1000) size_str[si++] = '0' + (size / 1000) % 10;
    if (size >= 100) size_str[si++] = '0' + (size / 100) % 10;
    if (size >= 10) size_str[si++] = '0' + (size / 10) % 10;
    size_str[si++] = '0' + size % 10;
    size_str[si++] = ' ';
    size_str[si++] = 'b';
    size_str[si++] = 'y';
    size_str[si++] = 't';
    size_str[si++] = 'e';
    size_str[si++] = 's';
    size_str[si] = 0;
    g_terminal.write_line(size_str);
    
    g_terminal.write("  Type: ");
    switch (type) {
        case UNIFS_TYPE_TEXT: g_terminal.write_line("Text file"); break;
        case UNIFS_TYPE_ELF: g_terminal.write_line("ELF executable"); break;
        case UNIFS_TYPE_BINARY: g_terminal.write_line("Binary file"); break;
        default: g_terminal.write_line("Unknown"); break;
    }
}

static void cmd_hexdump(const char* filename) {
    const UniFSFile* file = unifs_open(filename);
    if (!file) {
        error_file_not_found(filename);
        return;
    }
    
    // Limit output to 256 bytes for readability
    uint64_t display_size = (file->size < 256) ? file->size : 256;
    const char* hex = "0123456789abcdef";
    
    for (uint64_t offset = 0; offset < display_size; offset += 16) {
        char line[80];
        int li = 0;
        
        // Offset
        line[li++] = hex[(offset >> 12) & 0xF];
        line[li++] = hex[(offset >> 8) & 0xF];
        line[li++] = hex[(offset >> 4) & 0xF];
        line[li++] = hex[offset & 0xF];
        line[li++] = ':';
        line[li++] = ' ';
        
        // Hex bytes
        for (int i = 0; i < 16; i++) {
            if (offset + i < file->size) {
                uint8_t b = file->data[offset + i];
                line[li++] = hex[b >> 4];
                line[li++] = hex[b & 0xF];
            } else {
                line[li++] = ' ';
                line[li++] = ' ';
            }
            line[li++] = ' ';
            if (i == 7) line[li++] = ' ';  // Extra space in middle
        }
        
        line[li++] = ' ';
        line[li++] = '|';
        
        // ASCII representation
        for (int i = 0; i < 16 && offset + i < file->size; i++) {
            uint8_t b = file->data[offset + i];
            line[li++] = (b >= 32 && b < 127) ? b : '.';
        }
        
        line[li++] = '|';
        line[li] = 0;
        g_terminal.write_line(line);
    }
    
    if (file->size > 256) {
        g_terminal.write_line("... (truncated, showing first 256 bytes)");
    }
}

static void cmd_cat(const char* filename) {
    const UniFSFile* file = unifs_open(filename);
    if (file) {
        // Check if it's a text file
        if (unifs_get_file_type(filename) != UNIFS_TYPE_TEXT) {
            g_terminal.write_line("Binary file, use 'hexdump' instead.");
            return;
        }
        for (uint64_t i = 0; i < file->size; i++) {
            g_terminal.put_char(file->data[i]);
        }
        g_terminal.write("\n");
    } else {
        error_file_not_found(filename);
    }
}

static void cmd_touch(const char* filename) {
    int result = unifs_create(filename);
    switch (result) {
        case UNIFS_OK:
            g_terminal.write("Created: ");
            g_terminal.write_line(filename);
            break;
        case UNIFS_ERR_EXISTS:
            g_terminal.write_line("File already exists.");
            break;
        case UNIFS_ERR_FULL:
            g_terminal.write_line("Filesystem full (max 64 files).");
            break;
        case UNIFS_ERR_NAME_TOO_LONG:
            g_terminal.write_line("Filename too long (max 63 chars).");
            break;
        default:
            g_terminal.write_line("Error creating file.");
    }
}

static void cmd_rm(const char* filename) {
    int result = unifs_delete(filename);
    switch (result) {
        case UNIFS_OK:
            g_terminal.write("Deleted: ");
            g_terminal.write_line(filename);
            break;
        case UNIFS_ERR_NOT_FOUND:
            error_file_not_found(filename);
            break;
        case UNIFS_ERR_READONLY:
            g_terminal.write_line("Cannot delete boot file (read-only).");
            break;
        case UNIFS_ERR_IN_USE:
            g_terminal.write_line("Cannot delete: file is currently open.");
            break;
        default:
            g_terminal.write_line("Error deleting file.");
    }
}

// Process escape sequences in a string (\n -> newline, \t -> tab, \\ -> backslash)
// Returns allocated string that must be freed
static char* process_escapes(const char* input) {
    uint64_t input_len = strlen(input);
    char* output = (char*)malloc(input_len + 1);
    if (!output) return nullptr;
    
    int out_idx = 0;
    for (uint64_t i = 0; i < input_len; i++) {
        if (input[i] == '\\' && i + 1 < input_len) {
            char next = input[i + 1];
            if (next == 'n') {
                output[out_idx++] = '\n';
                i++;  // Skip the 'n'
            } else if (next == 't') {
                output[out_idx++] = '\t';
                i++;
            } else if (next == '\\') {
                output[out_idx++] = '\\';
                i++;
            } else {
                // Unknown escape, keep as-is
                output[out_idx++] = input[i];
            }
        } else {
            output[out_idx++] = input[i];
        }
    }
    output[out_idx] = '\0';
    return output;
}

static void cmd_write(const char* args) {
    // Parse "filename text to write"
    const char* space = args;
    while (*space && *space != ' ') space++;
    
    if (!*space) {
        g_terminal.write_line("Usage: write <filename> <text>");
        return;
    }
    
    // Extract filename
    char filename[64];
    int len = space - args;
    if (len > 63) len = 63;
    for (int i = 0; i < len; i++) filename[i] = args[i];
    filename[len] = 0;
    
    // Skip space to get text
    const char* text = space + 1;
    
    // Process escape sequences (\n, \t, \\)
    char* processed = process_escapes(text);
    if (!processed) {
        g_terminal.write_line("Out of memory.");
        return;
    }
    
    uint64_t processed_len = strlen(processed);
    
    // Add trailing newline if not already present
    char* final_text;
    if (processed_len == 0 || processed[processed_len - 1] != '\n') {
        final_text = (char*)malloc(processed_len + 2);
        if (!final_text) {
            free(processed);
            g_terminal.write_line("Out of memory.");
            return;
        }
        strcpy(final_text, processed);
        final_text[processed_len] = '\n';
        final_text[processed_len + 1] = '\0';
        processed_len++;
    } else {
        final_text = processed;
        processed = nullptr;  // Don't double-free
    }
    
    int result = unifs_write(filename, final_text, processed_len);
    free(final_text);
    if (processed) free(processed);
    
    switch (result) {
        case UNIFS_OK:
            g_terminal.write("Written: ");
            g_terminal.write_line(filename);
            break;
        case UNIFS_ERR_READONLY:
            g_terminal.write_line("Cannot write to boot file (read-only).");
            break;
        case UNIFS_ERR_NO_MEMORY:
            g_terminal.write_line("Out of memory or file too large.");
            break;
        case UNIFS_ERR_FULL:
            g_terminal.write_line("Filesystem full.");
            break;
        default:
            g_terminal.write_line("Error writing file.");
    }
}

static void cmd_append(const char* args) {
    // Parse "filename text to append"
    const char* space = args;
    while (*space && *space != ' ') space++;
    
    if (!*space) {
        g_terminal.write_line("Usage: append <filename> <text>");
        return;
    }
    
    // Extract filename
    char filename[64];
    int len = space - args;
    if (len > 63) len = 63;
    for (int i = 0; i < len; i++) filename[i] = args[i];
    filename[len] = 0;
    
    // Skip space to get text
    const char* text = space + 1;
    
    // Process escape sequences (\n, \t, \\)
    char* processed = process_escapes(text);
    if (!processed) {
        g_terminal.write_line("Out of memory.");
        return;
    }
    
    uint64_t processed_len = strlen(processed);
    
    // Add trailing newline if not already present
    char* final_text;
    if (processed_len == 0 || processed[processed_len - 1] != '\n') {
        final_text = (char*)malloc(processed_len + 2);
        if (!final_text) {
            free(processed);
            g_terminal.write_line("Out of memory.");
            return;
        }
        strcpy(final_text, processed);
        final_text[processed_len] = '\n';
        final_text[processed_len + 1] = '\0';
        processed_len++;
    } else {
        final_text = processed;
        processed = nullptr;  // Don't double-free
    }
    
    int result = unifs_append(filename, final_text, processed_len);
    free(final_text);
    if (processed) free(processed);
    
    switch (result) {
        case UNIFS_OK:
            g_terminal.write("Appended to: ");
            g_terminal.write_line(filename);
            break;
        case UNIFS_ERR_READONLY:
            g_terminal.write_line("Cannot append to boot file (read-only).");
            break;
        case UNIFS_ERR_NO_MEMORY:
            g_terminal.write_line("Out of memory or file too large.");
            break;
        default:
            g_terminal.write_line("Error appending to file.");
    }
}

static void cmd_df() {
    uint64_t total = unifs_get_total_size();
    uint64_t free_slots = unifs_get_free_slots();
    uint64_t file_count = unifs_get_file_count();
    
    char buf[128];
    int i = 0;
    
    auto append_str = [&](const char* s) { while (*s) buf[i++] = *s++; };
    auto append_num = [&](uint64_t n) {
        if (n == 0) { buf[i++] = '0'; return; }
        char tmp[20]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j-- > 0) buf[i++] = tmp[j];
    };
    
    // Calculate boot vs RAM files (boot files have is_boot flag in uniFS)
    uint64_t boot_file_count = unifs_get_boot_file_count();
    uint64_t ram_file_count = file_count > boot_file_count ? file_count - boot_file_count : 0;
    
    // Filesystem summary
    g_terminal.write_line("uniFS Status:");
    
    i = 0;
    append_str("  Boot:  ");
    append_num(boot_file_count);
    append_str(" files (read-only)");
    buf[i] = 0;
    g_terminal.write_line(buf);
    
    i = 0;
    append_str("  RAM:   ");
    append_num(ram_file_count);
    append_str(" / ");
    append_num(64);  // UNIFS_MAX_FILES
    append_str(" files");
    buf[i] = 0;
    g_terminal.write_line(buf);
    
    i = 0;
    append_str("  Used:  ");
    if (total >= 1024) {
        append_num(total / 1024);
        append_str(" KB");
    } else {
        append_num(total);
        append_str(" B");
    }
    buf[i] = 0;
    g_terminal.write_line(buf);
}

static void cmd_mem() {
    uint64_t free_bytes = pmm_get_free_memory();
    uint64_t total_bytes = pmm_get_total_memory();
    uint64_t used_bytes = total_bytes - free_bytes;
    
    uint64_t free_kb = free_bytes / 1024;
    uint64_t total_kb = total_bytes / 1024;
    uint64_t used_kb = used_bytes / 1024;
    
    char buf[128];
    int i = 0;
    
    auto append_str = [&](const char* s) {
        while (*s) buf[i++] = *s++;
    };
    
    auto append_num = [&](uint64_t n) {
        if (n == 0) { buf[i++] = '0'; return; }
        char tmp[20]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    };
    
    append_str("Memory Status:\n");
    
    append_str("  Total: "); append_num(total_kb); append_str(" KB ("); 
    append_num(total_kb / 1024); append_str(" MB)\n");
    
    append_str("  Used:  "); append_num(used_kb); append_str(" KB\n");
    
    append_str("  Free:  "); append_num(free_kb); append_str(" KB\n");
    
    buf[i] = 0;
    g_terminal.write(buf);
}

static void cmd_date() {
    RTCTime time;
    rtc_get_time(&time);
    
    char buf[64];
    int i = 0;
    
    auto append_num2 = [&](int n) {
        buf[i++] = '0' + (n / 10);
        buf[i++] = '0' + (n % 10);
    };
    
    auto append_num4 = [&](int n) {
        buf[i++] = '0' + (n / 1000);
        buf[i++] = '0' + ((n / 100) % 10);
        buf[i++] = '0' + ((n / 10) % 10);
        buf[i++] = '0' + (n % 10);
    };
    
    // Format: YYYY-MM-DD HH:MM:SS
    append_num4(time.year);
    buf[i++] = '-';
    append_num2(time.month);
    buf[i++] = '-';
    append_num2(time.day);
    buf[i++] = ' ';
    append_num2(time.hour);
    buf[i++] = ':';
    append_num2(time.minute);
    buf[i++] = ':';
    append_num2(time.second);
    buf[i] = 0;
    
    g_terminal.write_line(buf);
}

static void cmd_uptime() {
    uint64_t seconds = rtc_get_uptime_seconds();
    uint64_t minutes = seconds / 60;
    uint64_t hours = minutes / 60;
    uint64_t days = hours / 24;
    
    char buf[64];
    int i = 0;
    
    auto append_str = [&](const char* s) {
        while (*s) buf[i++] = *s++;
    };
    
    auto append_num = [&](uint64_t n) {
        if (n == 0) { buf[i++] = '0'; return; }
        char tmp[20]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    };
    
    append_str("up ");
    if (days > 0) { append_num(days); append_str(" day(s), "); }
    append_num(hours % 24); append_str(":");
    if ((minutes % 60) < 10) buf[i++] = '0';
    append_num(minutes % 60); append_str(":");
    if ((seconds % 60) < 10) buf[i++] = '0';
    append_num(seconds % 60);
    buf[i] = 0;
    
    g_terminal.write_line(buf);
}

static void cmd_echo(const char* text) {
    g_terminal.write_line(text);
}

// =============================================================================
// Shell Polish Commands (v0.5.1-v0.5.9)
// =============================================================================

// env - List all variables in a nice format
static void cmd_env() {
    bool any = false;
    for (int i = 0; i < MAX_VARS; i++) {
        if (shell_vars[i].in_use) {
            any = true;
            g_terminal.write(shell_vars[i].name);
            g_terminal.write("=");
            g_terminal.write_line(shell_vars[i].value);
        }
    }
    if (!any) g_terminal.write_line("No environment variables set.");
}

// true - Always succeeds (exit status 0)
static void cmd_true() {
    last_exit_status = 0;
}

// false - Always fails (exit status 1)
static void cmd_false() {
    last_exit_status = 1;
}

// sleep <ms> - Sleep for specified milliseconds (non-blocking)
static void cmd_sleep(const char* args) {
    while (*args == ' ') args++;
    int ms = str_to_int(args);
    if (ms <= 0) {
        g_terminal.write_line("Usage: sleep <milliseconds>");
        return;
    }
    if (ms > 60000) ms = 60000;  // Cap at 60 seconds
    
    // Use proper scheduler sleep (non-busy-waiting)
    scheduler_sleep_ms(ms);
}

// time <cmd> - Measure command execution time
static void cmd_time(const char* cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') {
        g_terminal.write_line("Usage: time <command>");
        return;
    }
    
    uint64_t start = timer_get_ticks();
    execute_single_command(cmd, nullptr);
    uint64_t end = timer_get_ticks();
    
    uint64_t elapsed_ticks = end - start;
    uint64_t elapsed_ms = (elapsed_ticks * 1000) / timer_get_frequency();
    
    char buf[64];
    int i = 0;
    const char* prefix = "\nTime: ";
    while (*prefix) buf[i++] = *prefix++;
    
    if (elapsed_ms == 0) {
        buf[i++] = '<'; buf[i++] = '1';
    } else {
        char tmp[16]; int j = 0;
        uint64_t n = elapsed_ms;
        if (n == 0) tmp[j++] = '0';
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    }
    buf[i++] = 'm'; buf[i++] = 's'; buf[i++] = '\0';
    g_terminal.write_line(buf);
}

// expr - Evaluate arithmetic expression
// Usage: expr 5 + 3, expr $X * 2
static void cmd_expr(const char* args) {
    while (*args == ' ') args++;
    
    // Parse: operand1 operator operand2
    char op1_str[64], op2_str[64];
    char op = 0;
    int i = 0, j = 0;
    
    // First operand
    while (args[i] && args[i] != ' ' && j < 63) op1_str[j++] = args[i++];
    op1_str[j] = '\0';
    
    // Skip space
    while (args[i] == ' ') i++;
    
    // Operator
    if (args[i] == '+' || args[i] == '-' || args[i] == '*' || args[i] == '/' || args[i] == '%') {
        op = args[i++];
    }
    
    // Skip space
    while (args[i] == ' ') i++;
    
    // Second operand
    j = 0;
    while (args[i] && args[i] != ' ' && j < 63) op2_str[j++] = args[i++];
    op2_str[j] = '\0';
    
    if (op == 0 || op2_str[0] == '\0') {
        g_terminal.write_line("Usage: expr <n1> <op> <n2>  (op: + - * / %)");
        return;
    }
    
    // Expand variables
    char exp1[64], exp2[64];
    expand_variables(op1_str, exp1, 64);
    expand_variables(op2_str, exp2, 64);
    
    int a = str_to_int(exp1);
    int b = str_to_int(exp2);
    int result = 0;
    
    switch (op) {
        case '+': result = a + b; break;
        case '-': result = a - b; break;
        case '*': result = a * b; break;
        case '/': result = (b != 0) ? a / b : 0; break;
        case '%': result = (b != 0) ? a % b : 0; break;
    }
    
    // Print result
    char buf[32];
    i = 0;
    if (result < 0) { buf[i++] = '-'; result = -result; }
    if (result == 0) {
        buf[i++] = '0';
    } else {
        char tmp[16]; j = 0;
        while (result > 0) { tmp[j++] = '0' + (result % 10); result /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    }
    buf[i] = '\0';
    g_terminal.write_line(buf);
}

// test - Evaluate test expression (for scripting)
// Usage: test -f file, test $X == hello, test $N -gt 5
static void cmd_test(const char* args) {
    while (*args == ' ') args++;
    
    // test -f <file> - file exists
    if (strncmp(args, "-f ", 3) == 0) {
        const char* fname = args + 3;
        while (*fname == ' ') fname++;
        char expanded[64];
        expand_variables(fname, expanded, 64);
        last_exit_status = unifs_file_exists(expanded) ? 0 : 1;
        return;
    }
    
    // test -z <str> - string is empty
    if (strncmp(args, "-z ", 3) == 0) {
        const char* str = args + 3;
        while (*str == ' ') str++;
        char expanded[256];
        expand_variables(str, expanded, 256);
        last_exit_status = (expanded[0] == '\0') ? 0 : 1;
        return;
    }
    
    // test -n <str> - string is not empty
    if (strncmp(args, "-n ", 3) == 0) {
        const char* str = args + 3;
        while (*str == ' ') str++;
        char expanded[256];
        expand_variables(str, expanded, 256);
        last_exit_status = (expanded[0] != '\0') ? 0 : 1;
        return;
    }
    
    // test <val1> <op> <val2> - comparison
    last_exit_status = evaluate_condition(args) ? 0 : 1;
}

// read VAR - Read user input into a variable
static void cmd_read(const char* varname) {
    while (*varname == ' ') varname++;
    
    if (*varname == '\0') {
        g_terminal.write_line("Usage: read <varname>");
        return;
    }
    
    // Extract variable name (first word only)
    char name[MAX_VAR_NAME];
    int i = 0;
    while (varname[i] && varname[i] != ' ' && i < MAX_VAR_NAME - 1) {
        name[i] = varname[i];
        i++;
    }
    name[i] = '\0';
    
    // Show prompt
    g_terminal.write(name);
    g_terminal.write("? ");
    
    // Read input character by character
    char input_buf[MAX_VAR_VALUE];
    int input_len = 0;
    
    while (input_len < MAX_VAR_VALUE - 1) {
        char c = input_keyboard_get_char();
        if (c == 0) {
            // No input yet, poll
            for (volatile int j = 0; j < 1000; j++);
            continue;
        }
        
        if (c == '\n' || c == '\r') {
            g_terminal.write("\n");
            break;
        }
        
        if (c == '\b' && input_len > 0) {
            input_len--;
            g_terminal.write("\b \b");
            continue;
        }
        
        if (c >= 32 && c < 127) {  // Printable
            input_buf[input_len++] = c;
            g_terminal.put_char(c);
        }
    }
    input_buf[input_len] = '\0';
    
    shell_set_var(name, input_buf);
}

// source <file> - Execute script in current context (variables persist)
// Same as run but shares variable space
static void cmd_source(const char* filename) {
    // source is essentially the same as run since we share global vars
    cmd_run(filename);
}

// =============================================================================
// Enhanced Text Processing Commands
// Better than Unix: cleaner output, smarter defaults, works with pipes
// =============================================================================

// wc - Word/line/character count with formatted output
// Usage: wc [file] or pipe: ls | wc
static void cmd_wc(const char* filename, const char* piped_input) {
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    // Get data from file or piped input
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: wc <file> or pipe input");
        return;
    }
    
    // Count statistics
    uint64_t lines = 0, words = 0, chars = 0;
    bool in_word = false;
    
    for (uint64_t i = 0; i < data_len; i++) {
        char c = data[i];
        chars++;
        
        if (c == '\n') lines++;
        
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    
    // Add 1 to lines if data doesn't end with newline
    if (data_len > 0 && data[data_len - 1] != '\n') lines++;
    
    // Output in clean format
    char buf[128];
    int i = 0;
    auto append_str = [&](const char* s) { while (*s) buf[i++] = *s++; };
    auto append_num = [&](uint64_t n) {
        if (n == 0) { buf[i++] = '0'; return; }
        char tmp[20]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    };
    
    append_str("  Lines: "); append_num(lines); append_str("\n");
    append_str("  Words: "); append_num(words); append_str("\n");
    append_str("  Chars: "); append_num(chars);
    buf[i] = 0;
    g_terminal.write_line(buf);
}

// head - Show first N lines (default 10)
// Usage: head [n] [file] or pipe: ls | head 5
static void cmd_head(const char* args, const char* piped_input) {
    int n = 10;  // Smart default
    const char* filename = nullptr;
    
    // Parse args: could be "5", "5 file.txt", "file.txt", or empty
    if (args && args[0]) {
        if (args[0] >= '0' && args[0] <= '9') {
            n = 0;
            const char* p = args;
            while (*p >= '0' && *p <= '9') {
                n = n * 10 + (*p - '0');
                p++;
            }
            while (*p == ' ') p++;
            if (*p) filename = p;
        } else {
            filename = args;
        }
    }
    
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: head [n] <file> or pipe input");
        return;
    }
    
    // Output first n lines
    int line_count = 0;
    for (uint64_t i = 0; i < data_len && line_count < n; i++) {
        g_terminal.put_char(data[i]);
        if (data[i] == '\n') line_count++;
    }
    
    // Add newline if last line didn't have one
    if (data_len > 0 && data[data_len - 1] != '\n' && line_count < n) {
        g_terminal.put_char('\n');
    }
}

// tail - Show last N lines (default 10)
// Usage: tail [n] [file] or pipe: ls | tail 3
static void cmd_tail(const char* args, const char* piped_input) {
    int n = 10;  // Smart default
    const char* filename = nullptr;
    
    // Parse args: could be "5", "5 file.txt", "file.txt", or empty
    if (args && args[0]) {
        if (args[0] >= '0' && args[0] <= '9') {
            n = 0;
            const char* p = args;
            while (*p >= '0' && *p <= '9') {
                n = n * 10 + (*p - '0');
                p++;
            }
            while (*p == ' ') p++;
            if (*p) filename = p;
        } else {
            filename = args;
        }
    }
    
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: tail [n] <file> or pipe input");
        return;
    }
    
    // Count total lines
    int total_lines = 0;
    for (uint64_t i = 0; i < data_len; i++) {
        if (data[i] == '\n') total_lines++;
    }
    if (data_len > 0 && data[data_len - 1] != '\n') total_lines++;
    
    // Find start position for last n lines
    int skip_lines = (total_lines > n) ? (total_lines - n) : 0;
    int line_count = 0;
    uint64_t start = 0;
    
    for (uint64_t i = 0; i < data_len && line_count < skip_lines; i++) {
        if (data[i] == '\n') {
            line_count++;
            start = i + 1;
        }
    }
    
    // Output from start position
    for (uint64_t i = start; i < data_len; i++) {
        g_terminal.put_char(data[i]);
    }
    
    // Add newline if needed
    if (data_len > 0 && data[data_len - 1] != '\n') {
        g_terminal.put_char('\n');
    }
}

// Helper: case-insensitive character comparison
static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

// grep - Search for pattern in text
// Usage: grep pattern [file] or pipe: ls | grep elf
// Case-insensitive by default (modern approach)
static void cmd_grep(const char* args, const char* piped_input) {
    if (!args || !args[0]) {
        g_terminal.write_line("Usage: grep <pattern> [file]");
        return;
    }
    
    // Parse pattern and optional filename
    char pattern[64];
    const char* filename = nullptr;
    int pi = 0;
    const char* p = args;
    
    // Get pattern (first word)
    while (*p && *p != ' ' && pi < 63) {
        pattern[pi++] = *p++;
    }
    pattern[pi] = '\0';
    
    // Skip space and get filename if present
    while (*p == ' ') p++;
    if (*p) filename = p;
    
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: grep <pattern> <file> or pipe input");
        return;
    }
    
    int pattern_len = strlen(pattern);
    if (pattern_len == 0) return;
    
    // Process line by line
    uint64_t line_start = 0;
    int matches = 0;
    
    for (uint64_t i = 0; i <= data_len; i++) {
        bool is_end = (i == data_len) || (data[i] == '\n');
        
        if (is_end) {
            // Check if this line contains the pattern (case-insensitive)
            bool found = false;
            
            for (uint64_t j = line_start; j + pattern_len <= i && !found; j++) {
                bool match = true;
                for (int k = 0; k < pattern_len && match; k++) {
                    if (to_lower(data[j + k]) != to_lower(pattern[k])) {
                        match = false;
                    }
                }
                if (match) found = true;
            }
            
            // Output matching line
            if (found) {
                matches++;
                for (uint64_t j = line_start; j < i; j++) {
                    g_terminal.put_char(data[j]);
                }
                g_terminal.put_char('\n');
            }
            
            line_start = i + 1;
        }
    }
    
    if (matches == 0) {
        g_terminal.write_line("No matches found.");
    }
}

// sort - Sort lines alphabetically
// Usage: sort [file] or pipe: ls | sort
static void cmd_sort(const char* filename, const char* piped_input) {
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: sort <file> or pipe input");
        return;
    }
    
    if (data_len == 0) return;
    
    // Count lines and store pointers
    const int MAX_LINES = 256;
    const char* lines[MAX_LINES];
    int line_lens[MAX_LINES];
    int line_count = 0;
    
    uint64_t line_start = 0;
    for (uint64_t i = 0; i <= data_len && line_count < MAX_LINES; i++) {
        if (i == data_len || data[i] == '\n') {
            if (i > line_start) {  // Skip empty lines
                lines[line_count] = data + line_start;
                line_lens[line_count] = i - line_start;
                line_count++;
            }
            line_start = i + 1;
        }
    }
    
    // Bubble sort (simple, works for small datasets)
    for (int i = 0; i < line_count - 1; i++) {
        for (int j = 0; j < line_count - i - 1; j++) {
            // Compare lines
            int min_len = (line_lens[j] < line_lens[j+1]) ? line_lens[j] : line_lens[j+1];
            bool swap = false;
            for (int k = 0; k < min_len; k++) {
                if (lines[j][k] > lines[j+1][k]) { swap = true; break; }
                if (lines[j][k] < lines[j+1][k]) break;
            }
            if (!swap && line_lens[j] > line_lens[j+1]) swap = true;
            
            if (swap) {
                const char* tmp = lines[j];
                lines[j] = lines[j+1];
                lines[j+1] = tmp;
                int tmp_len = line_lens[j];
                line_lens[j] = line_lens[j+1];
                line_lens[j+1] = tmp_len;
            }
        }
    }
    
    // Output sorted lines
    for (int i = 0; i < line_count; i++) {
        for (int j = 0; j < line_lens[i]; j++) {
            g_terminal.put_char(lines[i][j]);
        }
        g_terminal.put_char('\n');
    }
}

// uniq - Remove consecutive duplicate lines
// Usage: uniq [file] or pipe: sort data.txt | uniq
static void cmd_uniq(const char* filename, const char* piped_input) {
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: uniq <file> or pipe input");
        return;
    }
    
    if (data_len == 0) return;
    
    const char* prev_line = nullptr;
    int prev_len = 0;
    uint64_t line_start = 0;
    
    for (uint64_t i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            const char* curr_line = data + line_start;
            int curr_len = i - line_start;
            
            // Check if different from previous
            bool is_dup = false;
            if (prev_line && curr_len == prev_len) {
                is_dup = true;
                for (int j = 0; j < curr_len; j++) {
                    if (curr_line[j] != prev_line[j]) {
                        is_dup = false;
                        break;
                    }
                }
            }
            
            if (!is_dup && curr_len > 0) {
                for (int j = 0; j < curr_len; j++) {
                    g_terminal.put_char(curr_line[j]);
                }
                g_terminal.put_char('\n');
            }
            
            prev_line = curr_line;
            prev_len = curr_len;
            line_start = i + 1;
        }
    }
}

// rev - Reverse characters in each line
// Usage: rev [file] or pipe: echo hello | rev → olleh
static void cmd_rev(const char* filename, const char* piped_input) {
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: rev <file> or pipe input");
        return;
    }
    
    if (data_len == 0) return;
    
    uint64_t line_start = 0;
    for (uint64_t i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            // Reverse this line
            for (int64_t j = i - 1; j >= (int64_t)line_start; j--) {
                g_terminal.put_char(data[j]);
            }
            g_terminal.put_char('\n');
            line_start = i + 1;
        }
    }
}

// tac - Print lines in reverse order (opposite of cat)
// Usage: tac [file] or pipe: ls | tac
static void cmd_tac(const char* filename, const char* piped_input) {
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: tac <file> or pipe input");
        return;
    }
    
    if (data_len == 0) return;
    
    // Store line positions
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
    
    // Print lines in reverse
    for (int i = line_count - 1; i >= 0; i--) {
        for (uint64_t j = line_starts[i]; j < line_ends[i]; j++) {
            g_terminal.put_char(data[j]);
        }
        g_terminal.put_char('\n');
    }
}

// nl - Number lines
// Usage: nl [file] or pipe: cat file.txt | nl
static void cmd_nl(const char* filename, const char* piped_input) {
    const char* data = nullptr;
    uint64_t data_len = 0;
    
    if (filename && filename[0]) {
        const UniFSFile* file = unifs_open(filename);
        if (!file) {
            error_file_not_found(filename);
            return;
        }
        data = (const char*)file->data;
        data_len = file->size;
    } else if (piped_input) {
        data = piped_input;
        data_len = strlen(piped_input);
    } else {
        g_terminal.write_line("Usage: nl <file> or pipe input");
        return;
    }
    
    if (data_len == 0) return;
    
    int line_num = 1;
    uint64_t line_start = 0;
    
    for (uint64_t i = 0; i <= data_len; i++) {
        if (i == data_len || data[i] == '\n') {
            // Print line number (right-aligned in 6 chars)
            char num_buf[8];
            int n = line_num;
            int pos = 5;
            num_buf[6] = ' ';
            num_buf[7] = '\0';
            while (pos >= 0) {
                if (n > 0) {
                    num_buf[pos--] = '0' + (n % 10);
                    n /= 10;
                } else {
                    num_buf[pos--] = ' ';
                }
            }
            g_terminal.write(num_buf);
            
            // Print line content
            for (uint64_t j = line_start; j < i; j++) {
                g_terminal.put_char(data[j]);
            }
            g_terminal.put_char('\n');
            
            line_num++;
            line_start = i + 1;
        }
    }
}

// tr - Translate characters (simple version: tr <from> <to>)
// Usage: echo hello | tr e a → hallo
static void cmd_tr(const char* args, const char* piped_input) {
    if (!args || !args[0]) {
        g_terminal.write_line("Usage: tr <from_char> <to_char>");
        return;
    }
    
    char from_char = args[0];
    char to_char = ' ';
    
    // Parse: "e a" or "ea"
    const char* p = args + 1;
    while (*p == ' ') p++;
    if (*p) to_char = *p;
    
    if (!piped_input) {
        g_terminal.write_line("tr requires piped input");
        return;
    }
    
    // Translate and output
    for (const char* s = piped_input; *s; s++) {
        g_terminal.put_char(*s == from_char ? to_char : *s);
    }
}


static void cmd_version() {
    g_terminal.write("uniOS Kernel v");
    g_terminal.write_line(UNIOS_VERSION_STRING);
    g_terminal.write_line("Built with GCC for x86_64-elf");
    
    // Display actual bootloader info if available
    if (g_bootloader_name && g_bootloader_version) {
        g_terminal.write("Bootloader: ");
        g_terminal.write(g_bootloader_name);
        g_terminal.write(" ");
        g_terminal.write_line(g_bootloader_version);
    } else {
        g_terminal.write_line("Bootloader: Limine (version unknown)");
    }
}

static void cmd_uname() {
    g_terminal.write("uniOS ");
    g_terminal.write(UNIOS_VERSION_STRING);
    g_terminal.write_line(" x86_64");
}

static void cmd_cpuinfo() {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    
    // Get vendor string
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = 0;
    
    g_terminal.write("Vendor: ");
    g_terminal.write_line(vendor);
    
    // Get processor info
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    
    uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    uint32_t model = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
    uint32_t stepping = eax & 0xF;
    
    char buf[64];
    int i = 0;
    
    auto append_str = [&](const char* s) {
        while (*s) buf[i++] = *s++;
    };
    
    auto append_num = [&](uint32_t n) {
        if (n == 0) { buf[i++] = '0'; return; }
        char tmp[20]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    };
    
    append_str("Family: "); append_num(family);
    append_str(", Model: "); append_num(model);
    append_str(", Stepping: "); append_num(stepping);
    buf[i] = 0;
    
    g_terminal.write_line(buf);
    
    // Features
    g_terminal.write("Features: ");
    if (edx & (1 << 0)) g_terminal.write("FPU ");
    if (edx & (1 << 4)) g_terminal.write("TSC ");
    if (edx & (1 << 5)) g_terminal.write("MSR ");
    if (edx & (1 << 6)) g_terminal.write("PAE ");
    if (edx & (1 << 9)) g_terminal.write("APIC ");
    if (edx & (1 << 23)) g_terminal.write("MMX ");
    if (edx & (1 << 25)) g_terminal.write("SSE ");
    if (edx & (1 << 26)) g_terminal.write("SSE2 ");
    if (ecx & (1 << 0)) g_terminal.write("SSE3 ");
    if (ecx & (1 << 9)) g_terminal.write("SSSE3 ");
    if (ecx & (1 << 28)) g_terminal.write("AVX ");
    g_terminal.write("\n");
}

static void cmd_lspci() {
    g_terminal.write_line("PCI Devices:");
    
    // Use PCI bus scan from pci.cpp
    for (uint8_t bus = 0; bus < 8; bus++) {  // Limit to first 8 buses for speed
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint16_t vendor = pci_config_read16(bus, device, function, 0x00);
                if (vendor == 0xFFFF) continue;
                
                uint16_t device_id = pci_config_read16(bus, device, function, 0x02);
                uint8_t class_code = pci_config_read8(bus, device, function, 0x0B);
                uint8_t subclass = pci_config_read8(bus, device, function, 0x0A);
                
                char buf[64];
                int i = 0;
                
                auto append_hex4 = [&](uint16_t v) {
                    const char* hex = "0123456789ABCDEF";
                    buf[i++] = hex[(v >> 12) & 0xF];
                    buf[i++] = hex[(v >> 8) & 0xF];
                    buf[i++] = hex[(v >> 4) & 0xF];
                    buf[i++] = hex[v & 0xF];
                };
                
                auto append_hex2 = [&](uint8_t v) {
                    const char* hex = "0123456789ABCDEF";
                    buf[i++] = hex[(v >> 4) & 0xF];
                    buf[i++] = hex[v & 0xF];
                };
                
                auto append_num = [&](uint8_t n) {
                    if (n >= 100) buf[i++] = '0' + (n / 100);
                    if (n >= 10) buf[i++] = '0' + ((n / 10) % 10);
                    buf[i++] = '0' + (n % 10);
                };
                
                // Format: Bus:Dev.Func Vendor:Device [Class:Subclass]
                append_num(bus); buf[i++] = ':';
                append_num(device); buf[i++] = '.';
                append_num(function); buf[i++] = ' ';
                append_hex4(vendor); buf[i++] = ':';
                append_hex4(device_id); buf[i++] = ' ';
                buf[i++] = '['; append_hex2(class_code); buf[i++] = ':';
                append_hex2(subclass); buf[i++] = ']';
                buf[i] = 0;
                
                g_terminal.write("  ");
                g_terminal.write_line(buf);
                
                // Only check function 0 if not multifunction
                if (function == 0) {
                    uint8_t header = pci_config_read8(bus, device, 0, 0x0E);
                    if (!(header & 0x80)) break;
                }
            }
        }
    }
}

// Parse IP address from string (e.g., "10.0.2.2")
static uint32_t parse_ip(const char* str) {
    uint32_t ip = 0;
    uint8_t octets[4] = {0, 0, 0, 0};
    int octet_idx = 0;
    
    while (*str && octet_idx < 4) {
        if (*str >= '0' && *str <= '9') {
            octets[octet_idx] = octets[octet_idx] * 10 + (*str - '0');
        } else if (*str == '.') {
            octet_idx++;
        }
        str++;
    }
    
    ip = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return ip;
}

static void cmd_ifconfig() {
    g_terminal.write_line("Network Interface Configuration:");
    
    // Check if NIC is present
    if (!net_link_up() && net_get_ip() == 0) {
        g_terminal.write_line("  No network interface found.");
        return;
    }
    
    // Get MAC address
    uint8_t mac[6];
    net_get_mac(mac);
    
    char buf[80];
    int i = 0;
    
    auto append_str = [&](const char* s) {
        while (*s) buf[i++] = *s++;
    };
    
    auto append_hex2 = [&](uint8_t v) {
        const char* hex = "0123456789abcdef";
        buf[i++] = hex[(v >> 4) & 0xF];
        buf[i++] = hex[v & 0xF];
    };
    
    append_str("  MAC: ");
    for (int j = 0; j < 6; j++) {
        append_hex2(mac[j]);
        if (j < 5) buf[i++] = ':';
    }
    buf[i] = 0;
    g_terminal.write_line(buf);
    
    // IP address
    i = 0;
    uint32_t ip = net_get_ip();
    if (ip == 0) {
        g_terminal.write_line("  IP: Not configured (run 'dhcp')");
    } else {
        char ip_str[20];
        ip_format(ip, ip_str);
        append_str("  IP: ");
        append_str(ip_str);
        buf[i] = 0;
        g_terminal.write_line(buf);
        
        // Netmask
        i = 0;
        uint32_t mask = net_get_netmask();
        ip_format(mask, ip_str);
        append_str("  Netmask: ");
        append_str(ip_str);
        buf[i] = 0;
        g_terminal.write_line(buf);
        
        // Gateway
        i = 0;
        uint32_t gw = net_get_gateway();
        ip_format(gw, ip_str);
        append_str("  Gateway: ");
        append_str(ip_str);
        buf[i] = 0;
        g_terminal.write_line(buf);
    }
    
    // Link status
    g_terminal.write(net_link_up() ? "  Link: UP\n" : "  Link: DOWN\n");
}

static void cmd_dhcp_request() {
    if (!net_link_up()) {
        g_terminal.write_line("No network link detected.");
        return;
    }
    
    g_terminal.write_line("Requesting IP via DHCP...");
    
    if (dhcp_request()) {
        uint32_t ip = net_get_ip();
        char ip_str[20];
        ip_format(ip, ip_str);
        g_terminal.write("IP acquired: ");
        g_terminal.write_line(ip_str);
    } else {
        g_terminal.write_line("DHCP failed. No response from server.");
    }
}

// Ping state
static volatile bool ping_received = false;
static volatile uint16_t ping_rtt = 0;

static void ping_callback(uint32_t src_ip, uint16_t seq, uint16_t rtt_ms, bool success) {
    (void)src_ip;
    (void)seq;
    if (success) {
        ping_received = true;
        ping_rtt = rtt_ms;
    }
}

static void cmd_ping(const char* target) {
    if (net_get_ip() == 0) {
        g_terminal.write_line("Not configured. Run 'dhcp' first.");
        return;
    }
    
    // Resolve target (can be IP or hostname)
    g_terminal.write("Resolving ");
    g_terminal.write(target);
    g_terminal.write_line("...");
    
    uint32_t target_ip = dns_resolve(target);
    if (target_ip == 0) {
        g_terminal.write_line("Could not resolve hostname.");
        return;
    }
    
    char target_str[20];
    ip_format(target_ip, target_str);
    g_terminal.write("Pinging ");
    g_terminal.write(target_str);
    g_terminal.write_line("...");
    
    icmp_set_ping_callback(ping_callback);
    
    // Send 4 pings
    int sent = 0, received = 0;
    for (int seq = 1; seq <= 4; seq++) {
        ping_received = false;
        
        if (!icmp_send_echo_request(target_ip, 1234, seq)) {
            g_terminal.write_line("Failed to send ping.");
            continue;
        }
        sent++;
        
        // Wait for reply (up to 2 seconds)
        uint64_t start = timer_get_ticks();
        uint64_t timeout = (2000 * timer_get_frequency()) / 1000;
        
        while (!ping_received && (timer_get_ticks() - start) < timeout) {
            net_poll();
            for (volatile int j = 0; j < 1000; j++);
        }
        
        char buf[64];
        int i = 0;
        auto append_str = [&](const char* s) { while (*s) buf[i++] = *s++; };
        auto append_num = [&](int n) {
            if (n >= 100) buf[i++] = '0' + (n / 100);
            if (n >= 10) buf[i++] = '0' + ((n / 10) % 10);
            buf[i++] = '0' + (n % 10);
        };
        
        if (ping_received) {
            received++;
            append_str("Reply from ");
            append_str(target_str);
            append_str(": seq=");
            append_num(seq);
            append_str(" time=");
            append_num(ping_rtt);
            append_str("ms");
            buf[i] = 0;
            g_terminal.write_line(buf);
        } else {
            append_str("Request timeout for seq=");
            append_num(seq);
            buf[i] = 0;
            g_terminal.write_line(buf);
        }
    }
    
    icmp_set_ping_callback(nullptr);
    
    char summary[64];
    int i = 0;
    auto append_str = [&](const char* s) { while (*s) summary[i++] = *s++; };
    auto append_num = [&](int n) {
        if (n >= 10) summary[i++] = '0' + (n / 10);
        summary[i++] = '0' + (n % 10);
    };
    append_str("--- ");
    append_num(sent);
    append_str(" sent, ");
    append_num(received);
    append_str(" received ---");
    summary[i] = 0;
    g_terminal.write_line(summary);
}

// Helper: output piped input (used by cat when no file argument given)
static void cmd_cat_piped(const char* input) {
    if (input) {
        g_terminal.write(input);
    }
}

// =============================================================================
// Command Dispatch Table
// =============================================================================
static const CommandEntry commands[] = {
    // No-arg commands (exact match, no arguments)
    {"help",     CMD_NONE, cmd_help, nullptr, nullptr},
    {"ls",       CMD_NONE, cmd_ls, nullptr, nullptr},
    {"df",       CMD_NONE, cmd_df, nullptr, nullptr},
    {"mem",      CMD_NONE, cmd_mem, nullptr, nullptr},
    {"date",     CMD_NONE, cmd_date, nullptr, nullptr},
    {"uptime",   CMD_NONE, cmd_uptime, nullptr, nullptr},
    {"version",  CMD_NONE, cmd_version, nullptr, nullptr},
    {"uname",    CMD_NONE, cmd_uname, nullptr, nullptr},
    {"cpuinfo",  CMD_NONE, cmd_cpuinfo, nullptr, nullptr},
    {"lspci",    CMD_NONE, cmd_lspci, nullptr, nullptr},
    {"ifconfig", CMD_NONE, cmd_ifconfig, nullptr, nullptr},
    {"dhcp",     CMD_NONE, cmd_dhcp_request, nullptr, nullptr},
    {"env",      CMD_NONE, cmd_env, nullptr, nullptr},
    {"true",     CMD_NONE, cmd_true, nullptr, nullptr},
    {"false",    CMD_NONE, cmd_false, nullptr, nullptr},
    
    // Arg commands (command + space + args)
    {"cat",      CMD_ARGS, nullptr, cmd_cat, nullptr},
    {"stat",     CMD_ARGS, nullptr, cmd_stat, nullptr},
    {"hexdump",  CMD_ARGS, nullptr, cmd_hexdump, nullptr},
    {"touch",    CMD_ARGS, nullptr, cmd_touch, nullptr},
    {"rm",       CMD_ARGS, nullptr, cmd_rm, nullptr},
    {"write",    CMD_ARGS, nullptr, cmd_write, nullptr},
    {"append",   CMD_ARGS, nullptr, cmd_append, nullptr},
    {"run",      CMD_ARGS, nullptr, cmd_run, nullptr},
    {"set",      CMD_ARGS, nullptr, cmd_set, nullptr},
    {"unset",    CMD_ARGS, nullptr, cmd_unset, nullptr},
    {"ping",     CMD_ARGS, nullptr, cmd_ping, nullptr},
    {"sleep",    CMD_ARGS, nullptr, cmd_sleep, nullptr},
    {"read",     CMD_ARGS, nullptr, cmd_read, nullptr},
    {"test",     CMD_ARGS, nullptr, cmd_test, nullptr},
    {"expr",     CMD_ARGS, nullptr, cmd_expr, nullptr},
    {"source",   CMD_ARGS, nullptr, cmd_source, nullptr},
    {"time",     CMD_ARGS, nullptr, cmd_time, nullptr},
    {"echo",     CMD_ARGS, nullptr, cmd_echo, nullptr},
    
    // Piped commands (support file arg or piped input)
    {"wc",       CMD_PIPED, nullptr, nullptr, cmd_wc},
    {"head",     CMD_PIPED, nullptr, nullptr, cmd_head},
    {"tail",     CMD_PIPED, nullptr, nullptr, cmd_tail},
    {"grep",     CMD_PIPED, nullptr, nullptr, cmd_grep},
    {"sort",     CMD_PIPED, nullptr, nullptr, cmd_sort},
    {"uniq",     CMD_PIPED, nullptr, nullptr, cmd_uniq},
    {"rev",      CMD_PIPED, nullptr, nullptr, cmd_rev},
    {"tac",      CMD_PIPED, nullptr, nullptr, cmd_tac},
    {"nl",       CMD_PIPED, nullptr, nullptr, cmd_nl},
    {"tr",       CMD_PIPED, nullptr, nullptr, cmd_tr},
};

static const int NUM_COMMANDS = sizeof(commands) / sizeof(commands[0]);

// Execute a single command, optionally with piped input
// Returns true if command was recognized, false otherwise
static bool execute_single_command(const char* cmd, const char* piped_input) {
    // Skip leading whitespace
    while (*cmd == ' ') cmd++;
    
    // Get command length (excluding trailing spaces)
    int len = strlen(cmd);
    while (len > 0 && cmd[len - 1] == ' ') len--;
    
    if (len == 0) return true;  // Empty command is OK
    
    // Make a local copy for parsing
    char local_cmd[256];
    if (len > 255) len = 255;
    for (int i = 0; i < len; i++) local_cmd[i] = cmd[i];
    local_cmd[len] = '\0';
    
    // Expand variables in command (except for 'set' which handles its own)
    // This is needed for arithmetic like set I=$I+1 to work correctly
    bool is_set_command = (strncmp(local_cmd, "set ", 4) == 0 || strcmp(local_cmd, "set") == 0);
    
    if (!is_set_command) {
        char expanded_cmd[256];
        expand_variables(local_cmd, expanded_cmd, sizeof(expanded_cmd));
        // Use expanded_cmd from here on
        for (int i = 0; expanded_cmd[i] && i < 255; i++) {
            local_cmd[i] = expanded_cmd[i];
            local_cmd[i + 1] = '\0';
        }
    }
    // ==========================================================================
    // Command Dispatch via Table Lookup
    // ==========================================================================
    for (int i = 0; i < NUM_COMMANDS; i++) {
        const CommandEntry& c = commands[i];
        int name_len = strlen(c.name);
        
        if (c.type == CMD_NONE) {
            // No-arg command: exact match only
            if (strcmp(local_cmd, c.name) == 0) {
                c.handler_none();
                return true;
            }
        }
        else if (c.type == CMD_ARGS) {
            // Arg command: "cmd" with no args, or "cmd args"
            if (strcmp(local_cmd, c.name) == 0) {
                c.handler_args("");  // No args case
                return true;
            }
            if (strncmp(local_cmd, c.name, name_len) == 0 && local_cmd[name_len] == ' ') {
                c.handler_args(local_cmd + name_len + 1);
                return true;
            }
        }
        else if (c.type == CMD_PIPED) {
            // Piped command: supports file arg or piped input
            if (strcmp(local_cmd, c.name) == 0) {
                c.handler_piped(nullptr, piped_input);
                return true;
            }
            if (strncmp(local_cmd, c.name, name_len) == 0 && local_cmd[name_len] == ' ') {
                c.handler_piped(local_cmd + name_len + 1, piped_input);
                return true;
            }
        }
    }
    
    // ==========================================================================
    // Special Commands (require inline logic, not table-dispatchable)
    // ==========================================================================
    
    // "." is alias for "source"
    if (strncmp(local_cmd, ". ", 2) == 0) {
        cmd_source(local_cmd + 2);
        return true;
    }
    
    // "cat" with no args outputs piped input
    if (strcmp(local_cmd, "cat") == 0) {
        cmd_cat_piped(piped_input);
        return true;
    }
    
    // "echo" with no args outputs piped input or newline
    if (strcmp(local_cmd, "echo") == 0) {
        if (piped_input && piped_input[0]) {
            g_terminal.write(piped_input);
        } else {
            g_terminal.write("\n");
        }
        return true;
    }
    
    // "exit" is alias for poweroff
    if (strcmp(local_cmd, "exit") == 0) {
        if (acpi_is_available()) {
            g_terminal.write_line("Shutting down...");
        }
        acpi_poweroff();
        g_terminal.write_line("Shutdown failed.");
        return true;
    }
    
    // "clear" - clear screen
    if (strcmp(local_cmd, "clear") == 0) {
        g_terminal.clear();
        g_terminal.write("uniOS Shell\n\n");
        return true;
    }
    
    // "gui" - start graphical interface
    if (strcmp(local_cmd, "gui") == 0) {
        extern void gui_start();
        gui_start();
        g_terminal.clear();
        g_terminal.write("uniOS Shell\n\n");
        return true;
    }
    
    // "reboot" - restart system
    if (strcmp(local_cmd, "reboot") == 0) {
        g_terminal.write_line("Rebooting...");
        outb(0x64, 0xFE);
        for (volatile int i = 0; i < 1000000; i++);
        outb(0xCF9, 0x06);
        for (volatile int i = 0; i < 1000000; i++);
        struct {
            uint16_t limit;
            uint64_t base;
        } __attribute__((packed)) invalid_idt = { 0, 0 };
        asm volatile("lidt %0; int3" :: "m"(invalid_idt));
        asm volatile("cli; hlt");
        return true;
    }
    
    // "poweroff" - shutdown system
    if (strcmp(local_cmd, "poweroff") == 0) {
        if (acpi_is_available()) {
            g_terminal.write_line("ACPI available, attempting shutdown...");
        } else {
            g_terminal.write_line("ACPI not available.");
        }
        acpi_poweroff();
        g_terminal.write_line("Shutdown failed.");
        return true;
    }
    
    // Unknown command
    g_terminal.write("Unknown command: ");
    g_terminal.write_line(local_cmd);
    return false;
}

static void execute_command() {
    cmd_buffer[cmd_len] = 0;
    selection_start = -1;  // Clear any text selection
    
    // Trim trailing spaces (from tab completion)
    while (cmd_len > 0 && cmd_buffer[cmd_len - 1] == ' ') {
        cmd_len--;
        cmd_buffer[cmd_len] = 0;
    }
    
    // Add to history before execution
    add_to_history(cmd_buffer);
    history_index = -1;  // Reset history browsing
    
    if (cmd_len == 0) {
        g_terminal.write("\n");
        print_prompt();
        return;
    }
    
    // Clear selection highlighting by redrawing line with normal colors
    int col, row;
    g_terminal.get_cursor_pos(&col, &row);
    redraw_line_at(row, cursor_pos);
    
    g_terminal.write("\n"); // Move to next line after command input
    
    // Check for pipes
    bool has_pipe = false;
    for (int i = 0; i < cmd_len; i++) {
        if (cmd_buffer[i] == '|') {
            has_pipe = true;
            break;
        }
    }
    
    if (!has_pipe) {
        // Simple case: no pipes, execute directly
        execute_single_command(cmd_buffer, nullptr);
    } else {
        // Parse and execute pipeline
        // Commands: cmd1 | cmd2 | cmd3 ...
        // We alternate between pipe_buffer_a and pipe_buffer_b
        
        // Clear pipe buffers to prevent stale data
        pipe_buffer_a[0] = '\0';
        pipe_buffer_b[0] = '\0';
        
        char* current_input = nullptr;
        char* current_output = pipe_buffer_a;
        char* commands[16];  // Max 16 commands in pipeline
        int cmd_count = 0;
        
        // Split by pipe character
        char* start = cmd_buffer;
        for (int i = 0; i <= cmd_len && cmd_count < 16; i++) {
            if (cmd_buffer[i] == '|' || cmd_buffer[i] == '\0') {
                cmd_buffer[i] = '\0';
                commands[cmd_count++] = start;
                start = cmd_buffer + i + 1;
            }
        }
        
        // Execute each command in the pipeline
        for (int i = 0; i < cmd_count; i++) {
            bool is_last = (i == cmd_count - 1);
            
            if (is_last) {
                // Last command: output to screen normally
                execute_single_command(commands[i], current_input);
            } else {
                // Not last: capture output to buffer
                current_output[0] = '\0';  // Clear destination buffer
                g_terminal.start_capture(current_output, PIPE_BUFFER_SIZE - 1);
                execute_single_command(commands[i], current_input);
                g_terminal.stop_capture();
                
                // Swap buffers for next iteration
                current_input = current_output;
                current_output = (current_output == pipe_buffer_a) ? pipe_buffer_b : pipe_buffer_a;
            }
        }
    }
    
    cmd_len = 0;
    cursor_pos = 0;
    
    // Only add newline if not at start of line (e.g., after clear command)
    g_terminal.get_cursor_pos(&col, &row);
    if (col > 0) {
        g_terminal.write("\n");
    }
    print_prompt();
}

void shell_init(struct limine_framebuffer* fb) {
    (void)fb; // Not used directly anymore
    g_terminal.init(COLOR_TEXT, COLOR_BG);
    g_terminal.write("uniOS Shell\n");
    g_terminal.write("Type 'help' for commands.\n\n");
    print_prompt();
    
    cmd_len = 0;
    cursor_pos = 0;
    
    // Ensure cursor is visible and blinking
    g_terminal.set_cursor_visible(true);
}

void shell_process_char(char c) {
    uint8_t uc = (uint8_t)c;  // Cast to unsigned for special key comparison
    
    if (c == '\n') {
        execute_command();
    } else if (c == '\b') {
        if (cursor_pos > 0) {
            // Remove char at cursor position - 1
            for (int i = cursor_pos - 1; i < cmd_len - 1; i++) {
                cmd_buffer[i] = cmd_buffer[i + 1];
            }
            cmd_len--;
            
            // Get current row and redraw
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos - 1);
        }
    } else if (uc == KEY_UP_ARROW) {
        // Navigate history up
        if (history_count > 0) {
            int max_idx = (history_count < HISTORY_SIZE) ? history_count : HISTORY_SIZE;
            if (history_index < max_idx - 1) {
                history_index++;
                clear_line();
                int idx = (history_count - 1 - history_index) % HISTORY_SIZE;
                if (idx < 0) idx += HISTORY_SIZE;
                strcpy(cmd_buffer, history[idx]);
                cmd_len = strlen(cmd_buffer);
                display_line();
            }
        }
    } else if (uc == KEY_DOWN_ARROW) {
        // Navigate history down
        if (history_index > 0) {
            history_index--;
            clear_line();
            int idx = (history_count - 1 - history_index) % HISTORY_SIZE;
            if (idx < 0) idx += HISTORY_SIZE;
            strcpy(cmd_buffer, history[idx]);
            cmd_len = strlen(cmd_buffer);
            display_line();
        } else if (history_index == 0) {
            history_index = -1;
            clear_line();
        }
    } else if (uc == KEY_LEFT_ARROW) {
        // Move cursor left within command - clear selection
        if (cursor_pos > 0) {
            bool had_selection = (selection_start >= 0);
            selection_start = -1;
            cursor_pos--;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            if (had_selection) {
                redraw_line_at(row, cursor_pos);  // Clear selection visual
            } else {
                g_terminal.set_cursor_pos(col - 1, row);
            }
        }
    } else if (uc == KEY_RIGHT_ARROW) {
        // Move cursor right within command - clear selection
        if (cursor_pos < cmd_len) {
            bool had_selection = (selection_start >= 0);
            selection_start = -1;
            cursor_pos++;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            if (had_selection) {
                redraw_line_at(row, cursor_pos);  // Clear selection visual
            } else {
                g_terminal.set_cursor_pos(col + 1, row);
            }
        }
    } else if (c == 1) {  // Ctrl+A - move to start - clear selection
        bool had_selection = (selection_start >= 0);
        selection_start = -1;
        if (cursor_pos > 0 || had_selection) {
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            cursor_pos = 0;
            if (had_selection) {
                redraw_line_at(row, cursor_pos);
            } else {
                g_terminal.set_cursor_pos(PROMPT_LEN, row);
            }
        }
    } else if (c == 5) {  // Ctrl+E - move to end - clear selection
        bool had_selection = (selection_start >= 0);
        selection_start = -1;
        if (cursor_pos < cmd_len || had_selection) {
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            cursor_pos = cmd_len;
            if (had_selection) {
                redraw_line_at(row, cursor_pos);
            } else {
                g_terminal.set_cursor_pos(PROMPT_LEN + cmd_len, row);
            }
        }
    } else if (c == 3) {  // Ctrl+C - copy selection OR cancel line
        if (selection_start >= 0) {
            // Copy selected text to clipboard
            int sel_min = (selection_start < cursor_pos) ? selection_start : cursor_pos;
            int sel_max = (selection_start > cursor_pos) ? selection_start : cursor_pos;
            clipboard_len = sel_max - sel_min;
            for (int i = 0; i < clipboard_len; i++) {
                clipboard[i] = cmd_buffer[sel_min + i];
            }
            clipboard[clipboard_len] = 0;
            selection_start = -1;  // Clear selection
            // Redraw to remove highlighting
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos);
        } else {
            // No selection - cancel line
            g_terminal.write("^C\n");
            cmd_len = 0;
            cursor_pos = 0;
            print_prompt();
        }
    } else if (c == 21) {  // Ctrl+U - clear line before cursor (cut to clipboard)
        if (cursor_pos > 0) {
            // Copy to clipboard
            clipboard_len = cursor_pos;
            for (int i = 0; i < cursor_pos; i++) clipboard[i] = cmd_buffer[i];
            clipboard[clipboard_len] = 0;
            
            // Shift remaining text to start
            for (int i = cursor_pos; i < cmd_len; i++) {
                cmd_buffer[i - cursor_pos] = cmd_buffer[i];
            }
            cmd_len -= cursor_pos;
            cursor_pos = 0;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, 0);
        }
    } else if (c == 11) {  // Ctrl+K - kill to end of line (cut to clipboard)
        if (cursor_pos < cmd_len) {
            // Copy to clipboard
            clipboard_len = cmd_len - cursor_pos;
            for (int i = 0; i < clipboard_len; i++) {
                clipboard[i] = cmd_buffer[cursor_pos + i];
            }
            clipboard[clipboard_len] = 0;
            
            // Truncate line at cursor
            cmd_len = cursor_pos;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos);
        }
    } else if (c == 25) {  // Ctrl+Y - yank (paste from clipboard)
        if (clipboard_len > 0 && cmd_len + clipboard_len < 255) {
            // Shift text after cursor to make room
            for (int i = cmd_len - 1; i >= cursor_pos; i--) {
                cmd_buffer[i + clipboard_len] = cmd_buffer[i];
            }
            // Insert clipboard
            for (int i = 0; i < clipboard_len; i++) {
                cmd_buffer[cursor_pos + i] = clipboard[i];
            }
            cmd_len += clipboard_len;
            cursor_pos += clipboard_len;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos);
        }
    } else if (c == 23) {  // Ctrl+W - delete word before cursor
        if (cursor_pos > 0) {
            int word_start = cursor_pos - 1;
            // Skip trailing spaces
            while (word_start > 0 && cmd_buffer[word_start] == ' ') word_start--;
            // Skip word
            while (word_start > 0 && cmd_buffer[word_start - 1] != ' ') word_start--;
            
            // Copy deleted text to clipboard
            clipboard_len = cursor_pos - word_start;
            for (int i = 0; i < clipboard_len; i++) {
                clipboard[i] = cmd_buffer[word_start + i];
            }
            clipboard[clipboard_len] = 0;
            
            // Shift remaining text
            for (int i = cursor_pos; i < cmd_len; i++) {
                cmd_buffer[word_start + (i - cursor_pos)] = cmd_buffer[i];
            }
            cmd_len -= (cursor_pos - word_start);
            cursor_pos = word_start;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos);
        }
    } else if (c == 12) {  // Ctrl+L - clear screen (preserves current input)
        g_terminal.clear();
        g_terminal.write("uniOS Shell\n\n");
        print_prompt();
        // Redraw current command buffer
        for (int i = 0; i < cmd_len; i++) {
            g_terminal.put_char(cmd_buffer[i]);
        }
        // Position cursor correctly
        int col, row;
        g_terminal.get_cursor_pos(&col, &row);
        g_terminal.set_cursor_pos(PROMPT_LEN + cursor_pos, row);
    } else if (uc == KEY_HOME) {  // Home key - move to start - clear selection
        bool had_selection = (selection_start >= 0);
        selection_start = -1;
        if (cursor_pos > 0 || had_selection) {
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            cursor_pos = 0;
            if (had_selection) {
                redraw_line_at(row, cursor_pos);
            } else {
                g_terminal.set_cursor_pos(PROMPT_LEN, row);
            }
        }
    } else if (uc == KEY_END) {  // End key - move to end - clear selection
        bool had_selection = (selection_start >= 0);
        selection_start = -1;
        if (cursor_pos < cmd_len || had_selection) {
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            cursor_pos = cmd_len;
            if (had_selection) {
                redraw_line_at(row, cursor_pos);
            } else {
                g_terminal.set_cursor_pos(PROMPT_LEN + cmd_len, row);
            }
        }
    } else if (uc == KEY_DELETE) {  // Delete key - delete char at cursor
        if (cursor_pos < cmd_len) {
            // Shift text left
            for (int i = cursor_pos; i < cmd_len - 1; i++) {
                cmd_buffer[i] = cmd_buffer[i + 1];
            }
            cmd_len--;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos);
        }
    } else if (uc == KEY_SHIFT_LEFT) {  // Shift+Left - extend selection left
        if (cursor_pos > 0) {
            if (selection_start < 0) {
                selection_start = cursor_pos;  // Start new selection
            }
            cursor_pos--;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos);  // Redraw with selection highlighting
        }
    } else if (uc == KEY_SHIFT_RIGHT) {  // Shift+Right - extend selection right
        if (cursor_pos < cmd_len) {
            if (selection_start < 0) {
                selection_start = cursor_pos;  // Start new selection
            }
            cursor_pos++;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            redraw_line_at(row, cursor_pos);  // Redraw with selection highlighting
        }
    } else if (c == '\t') {  // Tab - command or filename completion
        cmd_buffer[cmd_len] = 0;
        
        // Check if there's a space - if so, complete filename, otherwise command
        int space_pos = -1;
        for (int i = 0; i < cmd_len; i++) {
            if (cmd_buffer[i] == ' ') {
                space_pos = i;
                break;
            }
        }
        
        if (space_pos >= 0) {
            // Filename completion - get partial filename after last space
            int last_space = space_pos;
            for (int i = cmd_len - 1; i > space_pos; i--) {
                if (cmd_buffer[i] == ' ') {
                    last_space = i;
                    break;
                }
            }
            
            const char* partial = cmd_buffer + last_space + 1;
            int partial_len = cmd_len - last_space - 1;
            
            // Search uniFS for matching files
            int matches = 0;
            const char* last_match = nullptr;
            uint64_t file_count = unifs_get_file_count();
            
            for (uint64_t i = 0; i < file_count; i++) {
                const char* fname = unifs_get_file_name(i);
                if (fname && strncmp(partial, fname, partial_len) == 0) {
                    matches++;
                    last_match = fname;
                }
            }
            
            if (matches == 1 && last_match) {
                // Complete the filename
                int fname_len = strlen(last_match);
                // Replace partial with full filename
                for (int i = 0; i < fname_len; i++) {
                    cmd_buffer[last_space + 1 + i] = last_match[i];
                }
                cmd_len = last_space + 1 + fname_len;
                cursor_pos = cmd_len;
                int col, row;
                g_terminal.get_cursor_pos(&col, &row);
                redraw_line_at(row, cursor_pos);
            } else if (matches > 1) {
                // Show matching files
                g_terminal.write("\n");
                for (uint64_t i = 0; i < file_count; i++) {
                    const char* fname = unifs_get_file_name(i);
                    if (fname && strncmp(partial, fname, partial_len) == 0) {
                        g_terminal.write(fname);
                        g_terminal.write("  ");
                    }
                }
                g_terminal.write("\n");
                print_prompt();
                for (int i = 0; i < cmd_len; i++) {
                    g_terminal.put_char(cmd_buffer[i]);
                }
            }
        } else if (cmd_len > 0) {
            // Command completion
            static const char* commands[] = {
                "help", "ls", "cat", "stat", "hexdump", "touch", "rm", "write", "append", "df",
                "mem", "date", "uptime", "version", "uname", "cpuinfo", "lspci",
                "ifconfig", "dhcp", "ping", "clear", "gui", "reboot", "poweroff", "echo",
                "wc", "head", "tail", "grep", "sort", "uniq", "rev", "tac", "nl", "tr",
                // Scripting commands (v0.5.0+)
                "run", "set", "unset", "env",
                // v0.5.x additions
                "exit", "time", "true", "false", "sleep", "read", "test", "expr", "source",
                nullptr
            };
            
            int matches = 0;
            const char* last_match = nullptr;
            
            for (int i = 0; commands[i]; i++) {
                if (strncmp(cmd_buffer, commands[i], cmd_len) == 0) {
                    matches++;
                    last_match = commands[i];
                }
            }
            
            if (matches == 1 && last_match) {
                // Complete the command
                strcpy(cmd_buffer, last_match);
                cmd_len = strlen(cmd_buffer);
                cursor_pos = cmd_len;
                cmd_buffer[cmd_len++] = ' ';  // Add space after
                cursor_pos++;
                int col, row;
                g_terminal.get_cursor_pos(&col, &row);
                redraw_line_at(row, cursor_pos);
            } else if (matches > 1) {
                // Show options
                g_terminal.write("\n");
                for (int i = 0; commands[i]; i++) {
                    if (strncmp(cmd_buffer, commands[i], cmd_len) == 0) {
                        g_terminal.write(commands[i]);
                        g_terminal.write("  ");
                    }
                }
                g_terminal.write("\n");
                print_prompt();
                for (int i = 0; i < cmd_len; i++) {
                    g_terminal.put_char(cmd_buffer[i]);
                }
            }
        }
    } else if (c >= 32 && cmd_len < 255) {
        // Clear selection when typing
        selection_start = -1;
        // Insert character at cursor position
        if (cursor_pos < cmd_len) {
            // Shift text right to make room
            for (int i = cmd_len; i > cursor_pos; i--) {
                cmd_buffer[i] = cmd_buffer[i - 1];
            }
        }
        cmd_buffer[cursor_pos] = c;
        cmd_len++;
        cursor_pos++;
        
        // Redraw from cursor to end
        int col, row;
        g_terminal.get_cursor_pos(&col, &row);
        redraw_line_at(row, cursor_pos);
    }
}

void shell_tick() {
    g_terminal.update_cursor();
}

