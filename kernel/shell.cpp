#include "shell.h"
#include "unifs.h"
#include "pmm.h"
#include <stddef.h>

static struct limine_framebuffer* framebuffer = nullptr;
static char cmd_buffer[256];
static int cmd_len = 0;
static uint64_t cursor_x = 68;
static uint64_t cursor_y = 370;

// Forward declarations for drawing (defined in kernel.cpp, we'll extern them)
extern void draw_char(struct limine_framebuffer *fb, uint64_t x, uint64_t y, char c, uint32_t color);
extern void draw_string(struct limine_framebuffer *fb, uint64_t x, uint64_t y, const char *str, uint32_t color);
extern void clear_char(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint32_t bg_color);

// String helpers
static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void new_line() {
    cursor_x = 50;
    cursor_y += 10;
    draw_string(framebuffer, cursor_x, cursor_y, "> ", 0x00FFFF);
    cursor_x = 68;
}

static void clear_screen() {
    for (uint64_t y = 0; y < framebuffer->height; y++) {
        for (uint64_t x = 0; x < framebuffer->width; x++) {
            uint32_t *fb_ptr = (uint32_t*)framebuffer->address;
            fb_ptr[y * (framebuffer->pitch / 4) + x] = 0x000022;
        }
    }
    cursor_x = 50;
    cursor_y = 50;
    draw_string(framebuffer, cursor_x, cursor_y, "uniOS Shell (uniSH)", 0xFFFFFF);
    cursor_y = 70;
    new_line();
}

static void print(const char* str) {
    while (*str) {
        if (*str == '\n') {
            cursor_x = 50;
            cursor_y += 10;
        } else {
            draw_char(framebuffer, cursor_x, cursor_y, *str, 0xFFFFFF);
            cursor_x += 9;
        }
        str++;
    }
}

// Commands
static void cmd_help() {
    print("Available commands:\n");
    print("  help  - Show this message\n");
    print("  ls    - List files\n");
    print("  cat   - Read file (cat <file>)\n");
    print("  mem   - Show memory stats\n");
    print("  clear - Clear screen\n");
}

static void cmd_ls() {
    // Access uniFS entries directly (we need to expose them or iterate)
    // For now, just print a placeholder 
    print("hello.txt\n");
}

static void cmd_cat(const char* filename) {
    const UniFSFile* file = unifs_open(filename);
    if (file) {
        for (uint64_t i = 0; i < file->size; i++) {
            char c = file->data[i];
            if (c == '\n' || c == '\r') {
                cursor_x = 50;
                cursor_y += 10;
            } else {
                draw_char(framebuffer, cursor_x, cursor_y, c, 0xFFFFFF);
                cursor_x += 9;
            }
        }
        cursor_y += 10;
    } else {
        print("File not found.\n");
    }
}

static void cmd_mem() {
    uint64_t free_mem = pmm_get_free_memory() / 1024 / 1024;
    uint64_t total_mem = pmm_get_total_memory() / 1024 / 1024;
    
    char buf[64];
    int i = 0;
    
    // Free memory
    buf[i++] = 'F'; buf[i++] = 'r'; buf[i++] = 'e'; buf[i++] = 'e'; buf[i++] = ':'; buf[i++] = ' ';
    uint64_t n = free_mem;
    if (n == 0) buf[i++] = '0';
    else {
        char tmp[20]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    }
    buf[i++] = 'M'; buf[i++] = 'B'; buf[i++] = '/';
    
    // Total memory
    n = total_mem;
    if (n == 0) buf[i++] = '0';
    else {
        char tmp[20]; int j = 0;
        while (n > 0) { tmp[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
    }
    buf[i++] = 'M'; buf[i++] = 'B'; buf[i++] = '\n'; buf[i] = 0;
    
    print(buf);
}

static void execute_command() {
    cmd_buffer[cmd_len] = 0;
    
    if (cmd_len == 0) {
        new_line();
        return;
    }
    
    cursor_y += 10;
    cursor_x = 50;
    
    if (strcmp(cmd_buffer, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd_buffer, "ls") == 0) {
        cmd_ls();
    } else if (strncmp(cmd_buffer, "cat ", 4) == 0) {
        cmd_cat(cmd_buffer + 4);
    } else if (strcmp(cmd_buffer, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd_buffer, "clear") == 0) {
        clear_screen();
        cmd_len = 0;
        return;
    } else {
        print("Unknown command. Type 'help'.\n");
    }
    
    cmd_len = 0;
    new_line();
}

void shell_init(struct limine_framebuffer* fb) {
    framebuffer = fb;
    cmd_len = 0;
}

void shell_process_char(char c) {
    if (c == '\n') {
        execute_command();
    } else if (c == '\b') {
        if (cmd_len > 0) {
            cmd_len--;
            cursor_x -= 9;
            clear_char(framebuffer, cursor_x, cursor_y, 0x000022);
        }
    } else if (cmd_len < 255) {
        cmd_buffer[cmd_len++] = c;
        draw_char(framebuffer, cursor_x, cursor_y, c, 0xFFFFFF);
        cursor_x += 9;
    }
}
