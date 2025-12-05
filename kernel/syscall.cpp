#include "syscall.h"
#include "limine.h"
#include "unifs.h"
#include "pipe.h"
#include "process.h"
#include <stddef.h>

extern struct limine_framebuffer* g_framebuffer;
extern void draw_char(struct limine_framebuffer *fb, uint64_t x, uint64_t y, char c, uint32_t color);

static uint64_t sys_cursor_x = 50;
static uint64_t sys_cursor_y = 480;

// File descriptor table (simple, single-process for now)
static FileDescriptor fd_table[MAX_OPEN_FILES];
static bool fd_initialized = false;

static void init_fd_table() {
    if (fd_initialized) return;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        fd_table[i].in_use = false;
    }
    // Reserve stdin/stdout/stderr
    fd_table[0].in_use = true; // stdin
    fd_table[1].in_use = true; // stdout
    fd_table[2].in_use = true; // stderr
    fd_initialized = true;
}

static int find_free_fd() {
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].in_use) return i;
    }
    return -1;
}

// SYS_OPEN: open(filename, flags, mode) -> fd
static uint64_t sys_open(const char* filename) {
    init_fd_table();
    
    const UniFSFile* file = unifs_open(filename);
    if (!file) return (uint64_t)-1;
    
    int fd = find_free_fd();
    if (fd < 0) return (uint64_t)-1;
    
    fd_table[fd].in_use = true;
    fd_table[fd].filename = file->name;
    fd_table[fd].position = 0;
    fd_table[fd].size = file->size;
    fd_table[fd].data = file->data;
    
    return fd;
}

// SYS_READ: read(fd, buf, count) -> bytes_read
static uint64_t sys_read(int fd, char* buf, uint64_t count) {
    init_fd_table();
    
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table[fd].in_use) {
        return (uint64_t)-1;
    }
    
    if (fd == STDIN_FD) {
        // TODO: Read from keyboard
        return 0;
    }
    
    FileDescriptor* f = &fd_table[fd];
    uint64_t remaining = f->size - f->position;
    uint64_t to_read = (count < remaining) ? count : remaining;
    
    for (uint64_t i = 0; i < to_read; i++) {
        buf[i] = f->data[f->position + i];
    }
    f->position += to_read;
    
    return to_read;
}

// SYS_WRITE: write(fd, buf, count) -> bytes_written
static uint64_t sys_write(int fd, const char* buf, uint64_t count) {
    if (fd == STDOUT_FD || fd == STDERR_FD) {
        for (uint64_t i = 0; i < count && buf[i]; i++) {
            if (buf[i] == '\n') {
                sys_cursor_x = 50;
                sys_cursor_y += 10;
            } else {
                draw_char(g_framebuffer, sys_cursor_x, sys_cursor_y, buf[i], 0x00FF00);
                sys_cursor_x += 9;
            }
        }
        return count;
    }
    return (uint64_t)-1; // Can't write to files (read-only FS)
}

// SYS_CLOSE: close(fd) -> 0 on success
static uint64_t sys_close(int fd) {
    init_fd_table();
    
    if (fd < 3 || fd >= MAX_OPEN_FILES) return (uint64_t)-1;
    if (!fd_table[fd].in_use) return (uint64_t)-1;
    
    fd_table[fd].in_use = false;
    return 0;
}

// Process ID (simple, single PID for now)
static uint64_t current_pid = 1;

extern "C" uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (syscall_num) {
        case SYS_READ:
            return sys_read((int)arg1, (char*)arg2, arg3);
        case SYS_WRITE:
            return sys_write((int)arg1, (const char*)arg2, arg3);
        case SYS_OPEN:
            return sys_open((const char*)arg1);
        case SYS_CLOSE:
            return sys_close((int)arg1);
        case SYS_PIPE:
            return pipe_create();
        case SYS_GETPID: {
            extern Process* process_get_current();
            Process* p = process_get_current();
            return p ? p->pid : 1;
        }
        case SYS_FORK: {
            extern uint64_t process_fork();
            return process_fork();
        }
        case SYS_EXIT: {
            extern void process_exit(int32_t status);
            process_exit((int32_t)arg1);
            return 0; // Never reached
        }
        case SYS_WAIT4: {
            extern int64_t process_waitpid(int64_t pid, int32_t* status);
            return process_waitpid((int64_t)arg1, (int32_t*)arg2);
        }
        default:
            return (uint64_t)-1;
    }
}
