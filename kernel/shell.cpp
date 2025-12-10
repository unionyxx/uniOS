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
#include <stddef.h>

static char cmd_buffer[256];
static int cmd_len = 0;
static int cursor_pos = 0; // Position within cmd_buffer

// Command history
#define HISTORY_SIZE 10
static char history[HISTORY_SIZE][256];
static int history_count = 0;
static int history_index = -1;  // Current browsing position (-1 = not browsing)

// Special key codes (sent by input layer via escape sequences)
#define KEY_UP_ARROW    0x80
#define KEY_DOWN_ARROW  0x81
#define KEY_LEFT_ARROW  0x82
#define KEY_RIGHT_ARROW 0x83

// Bootloader info (set by kmain.cpp from Limine request)
extern const char* g_bootloader_name;
extern const char* g_bootloader_version;

extern "C" void jump_to_user_mode(uint64_t code_sel, uint64_t stack, uint64_t entry);

static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static size_t strlen(const char* s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

static void strcpy(char* dst, const char* src) {
    while ((*dst++ = *src++));
}

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
    
    // 4. Draw new content using direct method
    for (int i = 0; i < cmd_len; i++) {
        g_terminal.write_char_at(2 + i, row, cmd_buffer[i]);
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
    g_terminal.write_line("Commands:");
    g_terminal.write_line("  help      - Show this help");
    g_terminal.write_line("  ls        - List files");
    g_terminal.write_line("  cat <f>   - Show file contents");
    g_terminal.write_line("  echo <t>  - Print text");
    g_terminal.write_line("  mem       - Show memory usage");
    g_terminal.write_line("  date      - Show current date/time");
    g_terminal.write_line("  uptime    - Show system uptime");
    g_terminal.write_line("  version   - Show kernel version");
    g_terminal.write_line("  uname     - System information");
    g_terminal.write_line("  cpuinfo   - CPU information");
    g_terminal.write_line("  lspci     - List PCI devices");
    g_terminal.write_line("  clear     - Clear screen");
    g_terminal.write_line("  gui       - Start GUI mode");
    g_terminal.write_line("  reboot    - Reboot system");
    g_terminal.write_line("  poweroff  - Shutdown system");
}

static void cmd_ls() {
    extern uint64_t unifs_get_file_count();
    extern const char* unifs_get_file_name(uint64_t index);
    
    uint64_t count = unifs_get_file_count();
    for (uint64_t i = 0; i < count; i++) {
        const char* name = unifs_get_file_name(i);
        if (name) {
            g_terminal.write(name);
            g_terminal.write("  ");
        }
    }
    g_terminal.write("\n");
}

static void cmd_cat(const char* filename) {
    const UniFSFile* file = unifs_open(filename);
    if (file) {
        for (uint64_t i = 0; i < file->size; i++) {
            g_terminal.put_char(file->data[i]);
        }
        g_terminal.write("\n");
    } else {
        g_terminal.write_line("File not found.");
    }
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
    g_terminal.write_line("uniOS Kernel v0.2.4");
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
    g_terminal.write_line("uniOS 0.2.2 x86_64");
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

static void execute_command() {
    cmd_buffer[cmd_len] = 0;
    
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
    } else if (c >= 32 && cmd_len < 255) {
        cmd_buffer[cmd_len++] = c;
        cursor_pos++;
        g_terminal.put_char(c);
    }
}

void shell_tick() {
    g_terminal.update_cursor();
}

