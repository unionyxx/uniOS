#include <kernel/syscall.h>
#include <boot/limine.h>
#include <kernel/fs/unifs.h>
#include <kernel/fs/pipe.h>
#include <kernel/process.h>
#include <kernel/debug.h>
#include <drivers/video/framebuffer.h>
#include <kernel/elf.h>
#include <stddef.h>

// External assembly function for Ring 3 transition
extern "C" void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

// ============================================================================
// User Pointer Validation
// ============================================================================
// User space addresses are in the lower half of the address space (< 0x0000800000000000)
// Kernel space (HHDM) starts at the higher half (>= 0xFFFF800000000000)

#define USER_SPACE_MAX 0x0000800000000000ULL

/**
 * @brief Validate that a user pointer is actually in user space
 * @param ptr Pointer to validate
 * @param size Size of the memory region (for overflow checking)
 * @return true if pointer is valid user space address, false otherwise
 */
static bool validate_user_ptr(const void* ptr, size_t size) {
    uint64_t addr = (uint64_t)ptr;
    
    // Null pointer check
    if (addr == 0) return false;
    
    // User space check - must be in lower half
    if (addr >= USER_SPACE_MAX) return false;
    
    // Overflow check - end of region must not wrap or enter kernel space
    if (size > 0) {
        uint64_t end = addr + size - 1;
        if (end < addr) return false;  // Overflow
        if (end >= USER_SPACE_MAX) return false;
    }
    
    return true;
}

/**
 * @brief Validate a user string pointer (null-terminated)
 * @param str String pointer to validate
 * @param max_len Maximum length to check
 * @return Length of string, or (size_t)-1 if invalid
 */
static size_t validate_user_string(const char* str, size_t max_len) {
    if (!validate_user_ptr(str, 1)) return (size_t)-1;
    
    // Walk the string, checking each byte
    for (size_t i = 0; i < max_len; i++) {
        if (!validate_user_ptr(str + i, 1)) return (size_t)-1;
        if (str[i] == '\0') return i;
    }
    
    // String too long
    return (size_t)-1;
}

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

// Check if a file is currently open in fd_table
// Used by filesystem to prevent deletion of open files
bool is_file_open(const char* filename) {
    if (!filename || !fd_initialized) return false;
    
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i].in_use && fd_table[i].filename) {
            // Compare filenames
            const char* a = fd_table[i].filename;
            const char* b = filename;
            bool match = true;
            while (*a && *b) {
                if (*a++ != *b++) { match = false; break; }
            }
            if (match && *a == *b) return true;
        }
    }
    return false;
}

// SYS_OPEN: open(filename, flags, mode) -> fd
static uint64_t sys_open(const char* filename) {
    // Validate user pointer
    if (validate_user_string(filename, 4096) == (size_t)-1) {
        return (uint64_t)-1;
    }
    
    init_fd_table();
    
    // Use thread-safe version with local buffer to avoid race condition
    UniFSFile file;
    if (!unifs_open_into(filename, &file)) {
        return (uint64_t)-1;
    }
    
    int fd = find_free_fd();
    if (fd < 0) return (uint64_t)-1;
    
    // Copy data from local buffer to fd_table (safe, won't be overwritten)
    fd_table[fd].in_use = true;
    fd_table[fd].filename = file.name;
    fd_table[fd].position = 0;
    fd_table[fd].size = file.size;
    fd_table[fd].data = file.data;
    
    return fd;
}

// SYS_READ: read(fd, buf, count) -> bytes_read
static uint64_t sys_read(int fd, char* buf, uint64_t count) {
    // Validate user buffer
    if (count > 0 && !validate_user_ptr(buf, count)) {
        return (uint64_t)-1;
    }
    
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
    // Validate user buffer
    if (count > 0 && !validate_user_ptr(buf, count)) {
        return (uint64_t)-1;
    }
    
    if (fd == STDOUT_FD || fd == STDERR_FD) {
        for (uint64_t i = 0; i < count && buf[i]; i++) {
            if (buf[i] == '\n') {
                sys_cursor_x = 50;
                sys_cursor_y += 10;
            } else {
                gfx_draw_char(sys_cursor_x, sys_cursor_y, buf[i], COLOR_GREEN);
                sys_cursor_x += 9;
            }
        }
        return count;
    }
    
    // File descriptor write support
    init_fd_table();
    if (fd >= 3 && fd < MAX_OPEN_FILES && fd_table[fd].in_use) {
        // Use unifs_write to overwrite file contents
        // Note: This implements simple overwrite semantics, not append
        int res = unifs_write(fd_table[fd].filename, buf, count);
        if (res == 0) {
            // Update cached size in fd_table
            fd_table[fd].size = count;
            fd_table[fd].position = count;
            return count;
        }
        return (uint64_t)-1;
    }
    
    return (uint64_t)-1;
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

// User stack location (must match elf_load_user)
#define USER_STACK_TOP 0x7FFFF000ULL  // Stack grows down from here

// State for the user task wrapper
static uint64_t g_user_entry = 0;
static volatile bool g_user_task_done = false;
static int32_t g_user_exit_status = 0;

// External scheduler functions
extern void scheduler_create_task(void (*entry)(), const char* name);
extern void process_exit(int32_t status);
extern void scheduler_yield();
extern Process* process_get_current();

/**
 * @brief Wrapper function that runs in a kernel task, enters Ring 3, 
 *        and when user code calls SYS_EXIT, properly terminates the task.
 */
static void user_task_wrapper() {
    if (g_user_entry != 0) {
        DEBUG_LOG("user_task: entering Ring 3 at 0x%llx\n", g_user_entry);
        
        // Transition to Ring 3 - when user calls SYS_EXIT, we handle it there
        enter_user_mode(g_user_entry, USER_STACK_TOP);
        
        // Should never reach here - SYS_EXIT will call process_exit
    }
    
    // Fallback - if enter_user_mode somehow returns
    process_exit(-1);
}

/**
 * @brief Execute an ELF binary in Ring 3 (userspace)
 * @param path Path to the ELF file
 * @return exit status of user program, or -1 on error
 */
static int64_t do_exec(const char* path) {
    // Open the file
    UniFSFile file;
    if (!unifs_open_into(path, &file)) {
        DEBUG_WARN("exec: file not found: %s\n", path);
        return -1;
    }
    
    // Validate it's an ELF
    if (!elf_validate(file.data, file.size)) {
        DEBUG_WARN("exec: not a valid ELF: %s\n", path);
        return -1;
    }
    
    DEBUG_LOG("exec: loading ELF '%s' (%llu bytes)\n", path, file.size);
    
    // Load the ELF into user memory space
    uint64_t entry = elf_load_user(file.data, file.size);
    if (entry == 0) {
        DEBUG_WARN("exec: failed to load ELF\n");
        return -1;
    }
    
    DEBUG_LOG("exec: entry point = 0x%llx\n", entry);
    
    // Set up globals for the wrapper task
    g_user_entry = entry;
    g_user_task_done = false;
    g_user_exit_status = 0;
    
    // Create a new task that will run the user program
    scheduler_create_task(user_task_wrapper, "user");
    
    // Wait for the user task to complete
    // The user task will set g_user_task_done when it exits
    while (!g_user_task_done) {
        scheduler_yield();
    }
    
    DEBUG_LOG("exec: user program exited with status %d\n", g_user_exit_status);
    return g_user_exit_status;
}

// Kernel-mode exec wrapper (for shell to call)
int64_t kernel_exec(const char* path) {
    return do_exec(path);
}

extern "C" uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    // DEBUG_LOG("Syscall: %d\n", syscall_num); // Uncomment for verbose logging
    
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
            // Signal to the waiting shell that the user program has exited
            g_user_exit_status = (int32_t)arg1;
            g_user_task_done = true;
            
            // Mark this process as zombie
            Process* p = process_get_current();
            if (p) {
                p->state = PROCESS_ZOMBIE;
            }
            
            // Enable interrupts so timer can fire, then halt
            asm volatile("sti; hlt" ::: "memory");
            
            // Loop forever
            for(;;) { asm volatile("hlt"); }
        }
        case SYS_EXEC: {
            // Validate path pointer
            if (validate_user_string((const char*)arg1, 256) == (size_t)-1) {
                return (uint64_t)-1;
            }
            return do_exec((const char*)arg1);
        }
        case SYS_WAIT4: {
            // Validate status pointer if provided
            if (arg2 != 0 && !validate_user_ptr((void*)arg2, sizeof(int32_t))) {
                return (uint64_t)-1;
            }
            extern int64_t process_waitpid(int64_t pid, int32_t* status);
            return process_waitpid((int64_t)arg1, (int32_t*)arg2);
        }
        default:
            DEBUG_WARN("Unknown syscall: %d\n", syscall_num);
            return (uint64_t)-1;
    }
}
