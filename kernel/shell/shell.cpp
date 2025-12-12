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
#include <stddef.h>

// Use shared string utilities
using kstring::strcmp;
using kstring::strncmp;
using kstring::strlen;
using kstring::strcpy;

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

// Helper to redraw entire command line without ANY cursor glitches
// Uses ONLY direct drawing methods - never put_char
static void redraw_line_at(int row, int new_cursor_pos) {
    // 1. Hide cursor completely - sync position first so it clears at right spot
    g_terminal.set_cursor_pos(2 + cursor_pos, row);
    g_terminal.set_cursor_visible(false);
    
    // 2. Calculate how much to clear (max of current and previous length + extra margin)
    int clear_count = last_displayed_len + 2;
    if (cmd_len + 2 > clear_count) clear_count = cmd_len + 2;
    
    // 3. Clear entire line area using direct method
    g_terminal.clear_chars(2, row, clear_count);
    
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
            g_terminal.write_char_at_color(2 + i, row, cmd_buffer[i], 0xFF000000, 0xFFFFFFFF);
        } else {
            g_terminal.write_char_at(2 + i, row, cmd_buffer[i]);
        }
    }
    
    // 5. Update tracking variables
    last_displayed_len = cmd_len;
    cursor_pos = new_cursor_pos;
    
    // 6. Position and show cursor at new location
    g_terminal.set_cursor_pos(2 + cursor_pos, row);
    g_terminal.set_cursor_visible(true);
}

// Clear line - does NOT show cursor at end (called before display_line)
static void clear_line() {
    int col, row;
    g_terminal.get_cursor_pos(&col, &row);
    
    g_terminal.set_cursor_visible(false);
    
    // Clear entire area with direct method
    int clear_count = last_displayed_len + 2;
    if (cmd_len + 2 > clear_count) clear_count = cmd_len + 2;
    g_terminal.clear_chars(2, row, clear_count);
    
    cmd_len = 0;
    cursor_pos = 0;
    last_displayed_len = 0;
    
    g_terminal.set_cursor_pos(2, row);
    // NOTE: Do NOT show cursor here - display_line will show it at the right position
}

// Display line for history - shows cursor at end
static void display_line() {
    int col, row;
    g_terminal.get_cursor_pos(&col, &row);
    
    // Cursor should already be hidden from clear_line
    g_terminal.set_cursor_visible(false);
    
    for (int i = 0; i < cmd_len; i++) {
        g_terminal.write_char_at(2 + i, row, cmd_buffer[i]);
    }
    
    cursor_pos = cmd_len;
    last_displayed_len = cmd_len;
    
    g_terminal.set_cursor_pos(2 + cursor_pos, row);
    g_terminal.set_cursor_visible(true);
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
    g_terminal.write_line("Other:");
    g_terminal.write_line("  clear     - Clear screen");
    g_terminal.write_line("  gui       - Start GUI mode");
    g_terminal.write_line("  help      - Show this help");
    g_terminal.write_line("  reboot    - Reboot system");
    g_terminal.write_line("  poweroff  - Shutdown system");
    g_terminal.write_line("");
    g_terminal.write_line("Shortcuts:");
    g_terminal.write_line("  Tab       - Command completion");
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
        g_terminal.write_line("File not found.");
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
        g_terminal.write_line("File not found.");
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
        g_terminal.write_line("File not found.");
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
            g_terminal.write_line("File not found.");
            break;
        case UNIFS_ERR_READONLY:
            g_terminal.write_line("Cannot delete boot file (read-only).");
            break;
        default:
            g_terminal.write_line("Error deleting file.");
    }
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
    uint64_t text_len = strlen(text);
    
    // Add newline
    char* text_with_newline = (char*)malloc(text_len + 2);
    if (!text_with_newline) {
        g_terminal.write_line("Out of memory.");
        return;
    }
    strcpy(text_with_newline, text);
    text_with_newline[text_len] = '\n';
    text_with_newline[text_len + 1] = 0;
    
    int result = unifs_write(filename, text_with_newline, text_len + 1);
    free(text_with_newline);
    
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
    uint64_t text_len = strlen(text);
    
    // Add newline
    char* text_with_newline = (char*)malloc(text_len + 2);
    if (!text_with_newline) {
        g_terminal.write_line("Out of memory.");
        return;
    }
    strcpy(text_with_newline, text);
    text_with_newline[text_len] = '\n';
    text_with_newline[text_len + 1] = 0;
    
    int result = unifs_append(filename, text_with_newline, text_len + 1);
    free(text_with_newline);
    
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
    
    // Calculate boot vs RAM files
    uint64_t ram_file_count = file_count > 3 ? file_count - 3 : 0;  // Boot files are first 3
    uint64_t boot_file_count = file_count - ram_file_count;
    
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

static void execute_command() {
    cmd_buffer[cmd_len] = 0;
    
    // Trim trailing spaces (from tab completion)
    while (cmd_len > 0 && cmd_buffer[cmd_len - 1] == ' ') {
        cmd_len--;
        cmd_buffer[cmd_len] = 0;
    }
    
    // Add to history before execution
    add_to_history(cmd_buffer);
    history_index = -1;  // Reset history browsing
    
    if (cmd_len == 0) {
        g_terminal.write("\n> ");
        return;
    }
    
    g_terminal.write("\n"); // Move to next line after command input
    
    if (strcmp(cmd_buffer, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd_buffer, "ls") == 0) {
        cmd_ls();
    } else if (strncmp(cmd_buffer, "cat ", 4) == 0) {
        cmd_cat(cmd_buffer + 4);
    } else if (strncmp(cmd_buffer, "stat ", 5) == 0) {
        cmd_stat(cmd_buffer + 5);
    } else if (strncmp(cmd_buffer, "hexdump ", 8) == 0) {
        cmd_hexdump(cmd_buffer + 8);
    } else if (strncmp(cmd_buffer, "touch ", 6) == 0) {
        cmd_touch(cmd_buffer + 6);
    } else if (strncmp(cmd_buffer, "rm ", 3) == 0) {
        cmd_rm(cmd_buffer + 3);
    } else if (strncmp(cmd_buffer, "write ", 6) == 0) {
        cmd_write(cmd_buffer + 6);
    } else if (strncmp(cmd_buffer, "append ", 7) == 0) {
        cmd_append(cmd_buffer + 7);
    } else if (strcmp(cmd_buffer, "df") == 0) {
        cmd_df();
    } else if (strcmp(cmd_buffer, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd_buffer, "date") == 0) {
        cmd_date();
    } else if (strcmp(cmd_buffer, "uptime") == 0) {
        cmd_uptime();
    } else if (strncmp(cmd_buffer, "echo ", 5) == 0) {
        cmd_echo(cmd_buffer + 5);
    } else if (strcmp(cmd_buffer, "echo") == 0) {
        g_terminal.write("\n");  // Just echo newline
    } else if (strcmp(cmd_buffer, "version") == 0) {
        cmd_version();
    } else if (strcmp(cmd_buffer, "uname") == 0) {
        cmd_uname();
    } else if (strcmp(cmd_buffer, "cpuinfo") == 0) {
        cmd_cpuinfo();
    } else if (strcmp(cmd_buffer, "lspci") == 0) {
        cmd_lspci();
    } else if (strcmp(cmd_buffer, "ifconfig") == 0) {
        cmd_ifconfig();
    } else if (strcmp(cmd_buffer, "dhcp") == 0) {
        cmd_dhcp_request();
    } else if (strncmp(cmd_buffer, "ping ", 5) == 0) {
        cmd_ping(cmd_buffer + 5);
    } else if (strcmp(cmd_buffer, "clear") == 0) {
        g_terminal.clear();
        g_terminal.write("uniOS Shell (uniSH)\n\n");
        cmd_len = 0;
        cursor_pos = 0;
        g_terminal.write("> ");
        return;
    } else if (strcmp(cmd_buffer, "gui") == 0) {
        extern void gui_start();
        gui_start();
        // After GUI, clear and reset
        g_terminal.clear();
        g_terminal.write("uniOS Shell (uniSH)\n\n");
    } else if (strcmp(cmd_buffer, "reboot") == 0) {
        g_terminal.write_line("Rebooting...");
        
        // 1. Keyboard Controller Reset (0x64 = 0xFE)
        outb(0x64, 0xFE);
        for (volatile int i = 0; i < 1000000; i++);
        
        // 2. PCI Reset (0xCF9 = 0x06) - Reset + System Reset
        outb(0xCF9, 0x06);
        for (volatile int i = 0; i < 1000000; i++);
        
        // 3. Triple Fault (load invalid IDT)
        struct {
            uint16_t limit;
            uint64_t base;
        } __attribute__((packed)) invalid_idt = { 0, 0 };
        asm volatile("lidt %0; int3" :: "m"(invalid_idt));
        
        asm volatile("cli; hlt");
    } else if (strcmp(cmd_buffer, "poweroff") == 0) {
        if (acpi_is_available()) {
            g_terminal.write_line("ACPI available, attempting shutdown...");
        } else {
            g_terminal.write_line("ACPI not available.");
        }
        acpi_poweroff();
        g_terminal.write_line("Shutdown failed.");
    } else {
        g_terminal.write("Unknown command: ");
        g_terminal.write_line(cmd_buffer);
    }
    
    cmd_len = 0;
    cursor_pos = 0;
    g_terminal.write("> ");
}

void shell_init(struct limine_framebuffer* fb) {
    (void)fb; // Not used directly anymore
    g_terminal.init(COLOR_WHITE, COLOR_BLACK);
    g_terminal.write("uniOS Shell (uniSH)\n");
    g_terminal.write("Type 'help' for commands.\n\n");
    g_terminal.write("> ");
    
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
        // Move cursor left within command
        if (cursor_pos > 0) {
            cursor_pos--;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            g_terminal.set_cursor_pos(col - 1, row);
        }
    } else if (uc == KEY_RIGHT_ARROW) {
        // Move cursor right within command
        if (cursor_pos < cmd_len) {
            cursor_pos++;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            g_terminal.set_cursor_pos(col + 1, row);
        }
    } else if (c == 1) {  // Ctrl+A - move to start
        if (cursor_pos > 0) {
            cursor_pos = 0;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            g_terminal.set_cursor_pos(2, row);  // After "> "
        }
    } else if (c == 5) {  // Ctrl+E - move to end
        if (cursor_pos < cmd_len) {
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            g_terminal.set_cursor_pos(2 + cmd_len, row);  // After "> " + cmd_len
            cursor_pos = cmd_len;
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
            // Don't cancel line - just copied
        } else {
            // No selection - cancel line
            g_terminal.write("^C\n");
            cmd_len = 0;
            cursor_pos = 0;
            g_terminal.write("> ");
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
    } else if (c == 12) {  // Ctrl+L - clear screen
        g_terminal.clear();
        g_terminal.write("uniOS Shell (uniSH)\n\n> ");
        for (int i = 0; i < cmd_len; i++) {
            g_terminal.put_char(cmd_buffer[i]);
        }
    } else if (uc == KEY_HOME) {  // Home key - move to start
        if (cursor_pos > 0) {
            cursor_pos = 0;
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            g_terminal.set_cursor_pos(2, row);  // After "> "
        }
    } else if (uc == KEY_END) {  // End key - move to end
        if (cursor_pos < cmd_len) {
            int col, row;
            g_terminal.get_cursor_pos(&col, &row);
            g_terminal.set_cursor_pos(2 + cmd_len, row);
            cursor_pos = cmd_len;
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
    } else if (c == '\t') {  // Tab - command completion
        // Find matching commands
        static const char* commands[] = {
            "help", "ls", "cat", "stat", "hexdump", "touch", "rm", "write", "append", "df",
            "mem", "date", "uptime", "version", "uname", "cpuinfo", "lspci",
            "ifconfig", "dhcp", "ping", "clear", "gui", "reboot", "poweroff", "echo", nullptr
        };
        
        if (cmd_len > 0) {
            cmd_buffer[cmd_len] = 0;
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
                g_terminal.write("\n> ");
                for (int i = 0; i < cmd_len; i++) {
                    g_terminal.put_char(cmd_buffer[i]);
                }
            }
        }
    } else if (c >= 32 && cmd_len < 255) {
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

