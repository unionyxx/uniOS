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

constexpr uint64_t USER_SPACE_MAX = 0x0000800000000000ULL;

/**
 * @brief Validate that a user pointer is actually in user space.
 * @param ptr  Pointer to validate
 * @param size Size of the memory region (for overflow checking)
 * @return true if pointer is a valid user space address, false otherwise
 */
[[nodiscard]] static bool validate_user_ptr(const void* ptr, size_t size) {
    uint64_t addr = (uint64_t)ptr;

    if (addr == 0) return false;

    // Must be in the lower canonical half
    if (addr >= USER_SPACE_MAX) return false;

    // Overflow check – end of region must not wrap or enter kernel space
    if (size > 0) {
        uint64_t end = addr + size - 1;
        if (end < addr)            return false;  // integer overflow
        if (end >= USER_SPACE_MAX) return false;
    }

    return true;
}

/**
 * @brief Validate a null-terminated user string.
 *
 * FIXED: The previous implementation called validate_user_ptr() on every
 * single byte, making it O(n) per byte.  We now re-validate only at 4 KB
 * page boundaries; within a single page the address cannot suddenly become
 * invalid, so the per-byte checks were redundant.
 *
 * @param str     String pointer to validate
 * @param max_len Maximum length to scan
 * @return Length of string, or (size_t)-1 if invalid or too long
 */
[[nodiscard]] static size_t validate_user_string(const char* str, size_t max_len) {
    if (!validate_user_ptr(str, 1)) return (size_t)-1;

    for (size_t i = 0; i < max_len; i++) {
        // Re-validate only when we cross into a new page
        if (i > 0 && (((uint64_t)(str + i)) & 0xFFF) == 0) {
            if (!validate_user_ptr(str + i, 1)) return (size_t)-1;
        }
        if (str[i] == '\0') return i;
    }

    return (size_t)-1;  // string too long
}

// TODO: sys_cursor_x / sys_cursor_y should be stored per-process (in the
//       Process struct) once a proper per-process terminal state is added.
static uint64_t sys_cursor_x = 50;
static uint64_t sys_cursor_y = 480;

// TODO: fd_table is a single kernel-global table.  This means all processes
//       share one FD namespace, so a child opening/closing a file silently
//       corrupts the parent's state.  Move fd_table into the Process struct
//       (or key it by PID) when proper multi-process support is needed.

// File descriptor table (simple, single-process for now)
static FileDescriptor fd_table[MAX_OPEN_FILES];
static bool           fd_initialized = false;

static void init_fd_table() {
    if (fd_initialized) return;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        fd_table[i].in_use = false;
    }
    // Reserve stdin / stdout / stderr
    fd_table[0].in_use = true;
    fd_table[1].in_use = true;
    fd_table[2].in_use = true;
    fd_initialized = true;
}

static int find_free_fd() {
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].in_use) return i;
    }
    return -1;
}

// Check if a file is currently open in fd_table.
// Used by the filesystem to prevent deletion of open files.
bool is_file_open(const char* filename) {
    if (!filename || !fd_initialized) return false;

    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i].in_use && fd_table[i].filename) {
            const char* a     = fd_table[i].filename;
            const char* b     = filename;
            bool        match = true;
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
    if (validate_user_string(filename, 4096) == (size_t)-1) {
        return (uint64_t)-1;
    }

    init_fd_table();

    UniFSFile file;
    if (!unifs_open_into(filename, &file)) {
        return (uint64_t)-1;
    }

    int fd = find_free_fd();
    if (fd < 0) return (uint64_t)-1;

    fd_table[fd].in_use   = true;
    fd_table[fd].filename = file.name;
    fd_table[fd].position = 0;
    fd_table[fd].size     = file.size;
    fd_table[fd].data     = file.data;

    return fd;
}

// SYS_READ: read(fd, buf, count) -> bytes_read
static uint64_t sys_read(int fd, char* buf, uint64_t count) {
    if (count > 0 && !validate_user_ptr(buf, count)) {
        return (uint64_t)-1;
    }

    init_fd_table();

    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table[fd].in_use) {
        return (uint64_t)-1;
    }

    if (fd == STDIN_FD) {
        // TODO: Read from keyboard input buffer
        return 0;
    }

    FileDescriptor* f         = &fd_table[fd];
    uint64_t        remaining = f->size - f->position;
    uint64_t        to_read   = (count < remaining) ? count : remaining;

    for (uint64_t i = 0; i < to_read; i++) {
        buf[i] = f->data[f->position + i];
    }
    f->position += to_read;

    return to_read;
}

// SYS_WRITE: write(fd, buf, count) -> bytes_written
//
// FIXED: The original loop condition was `i < count && buf[i]`, which stopped
// writing on the first '\0' byte and silently truncated binary data.  The
// `count` argument alone must control the loop, matching POSIX semantics.
static uint64_t sys_write(int fd, const char* buf, uint64_t count) {
    if (count > 0 && !validate_user_ptr(buf, count)) {
        return (uint64_t)-1;
    }

    if (fd == STDOUT_FD || fd == STDERR_FD) {
        // FIXED: removed `&& buf[i]` – count controls the loop, not null termination
        for (uint64_t i = 0; i < count; i++) {
            if (buf[i] == '\n') {
                sys_cursor_x  = 50;
                sys_cursor_y += 10;
            } else {
                gfx_draw_char(sys_cursor_x, sys_cursor_y, buf[i], COLOR_GREEN);
                sys_cursor_x += 9;
            }
        }
        return count;
    }

    // File descriptor write – simple overwrite semantics (not append)
    init_fd_table();
    if (fd >= 3 && fd < MAX_OPEN_FILES && fd_table[fd].in_use) {
        int res = unifs_write(fd_table[fd].filename, buf, count);
        if (res == 0) {
            fd_table[fd].size     = count;
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
    if (!fd_table[fd].in_use)           return (uint64_t)-1;

    fd_table[fd].in_use = false;
    return 0;
}

// Process ID (simple, single PID for now)
static uint64_t current_pid = 1;

// User stack location (must match elf_load_user)
constexpr uint64_t USER_STACK_TOP = 0x7FFFF000ULL;

// TODO: g_user_entry and g_user_task_done are global state shared by all
//       processes.  If SYS_EXEC is called from two processes concurrently
//       (possible after process_fork), they will race on these variables.
//       Move them into the Process struct as exec_entry / exec_done.
static uint64_t         g_user_entry      = 0;
static volatile bool    g_user_task_done  = false;
static int32_t          g_user_exit_status = 0;

// External scheduler functions
extern void    scheduler_create_task(void (*entry)(), const char* name);
extern void    process_exit(int32_t status);
extern void    scheduler_yield();
extern Process* process_get_current();

/**
 * @brief Wrapper function that runs in a kernel task, enters Ring 3,
 *        and when user code calls SYS_EXIT, properly terminates the task.
 */
static void user_task_wrapper() {
    if (g_user_entry != 0) {
        DEBUG_LOG("user_task: entering Ring 3 at 0x%llx\n", g_user_entry);
        enter_user_mode(g_user_entry, USER_STACK_TOP);
        // Should never reach here – SYS_EXIT calls process_exit()
    }
    process_exit(-1);
}

/**
 * @brief Execute an ELF binary in Ring 3 (userspace).
 * @param path Path to the ELF file in the filesystem
 * @return Exit status of the user program, or -1 on error
 */
[[nodiscard]] static int64_t do_exec(const char* path) {
    UniFSFile file;
    if (!unifs_open_into(path, &file)) {
        DEBUG_WARN("exec: file not found: %s\n", path);
        return -1;
    }

    if (!elf_validate(file.data, file.size)) {
        DEBUG_WARN("exec: not a valid ELF: %s\n", path);
        return -1;
    }

    DEBUG_LOG("exec: loading ELF '%s' (%llu bytes)\n", path, file.size);

    uint64_t entry = elf_load_user(file.data, file.size);
    if (entry == 0) {
        DEBUG_WARN("exec: failed to load ELF\n");
        return -1;
    }

    DEBUG_LOG("exec: entry point = 0x%llx\n", entry);

    g_user_entry      = entry;
    g_user_task_done  = false;
    g_user_exit_status = 0;

    scheduler_create_task(user_task_wrapper, "user");

    // Wait for the user task to signal completion via SYS_EXIT
    while (!g_user_task_done) {
        scheduler_yield();
    }

    DEBUG_LOG("exec: user program exited with status %d\n", g_user_exit_status);
    return g_user_exit_status;
}

// Kernel-mode exec wrapper (called by the shell)
[[nodiscard]] int64_t kernel_exec(const char* path) {
    return do_exec(path);
}

extern "C" uint64_t syscall_handler(uint64_t syscall_num,
                                     uint64_t arg1,
                                     uint64_t arg2,
                                     uint64_t arg3) {
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
            g_user_exit_status = (int32_t)arg1;
            g_user_task_done   = true;

            Process* p = process_get_current();
            if (p) p->state = PROCESS_ZOMBIE;

            // Enable interrupts so the timer can fire and reschedule
            asm volatile("sti; hlt" ::: "memory");
            for (;;) { asm volatile("hlt"); }
        }

        case SYS_EXEC: {
            if (validate_user_string((const char*)arg1, 256) == (size_t)-1) {
                return (uint64_t)-1;
            }
            return do_exec((const char*)arg1);
        }

        case SYS_WAIT4: {
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
