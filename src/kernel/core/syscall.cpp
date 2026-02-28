#include <kernel/syscall.h>
#include <boot/limine.h>
#include <kernel/fs/unifs.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/pipe.h>
#include <kernel/mm/heap.h>
#include <kernel/process.h>
#include <kernel/debug.h>
#include <drivers/video/framebuffer.h>
#include <kernel/elf.h>
#include <stddef.h>

extern "C" void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

constexpr uint64_t USER_SPACE_MAX = 0x0000800000000000ULL;
constexpr uint64_t USER_STACK_TOP = 0x7FFFF000ULL;

[[nodiscard]] static bool validate_user_ptr(const void* ptr, size_t size) {
    uint64_t addr = (uint64_t)ptr;

    if (addr == 0) return false;

    if (addr >= USER_SPACE_MAX) return false;

    if (size > 0) {
        uint64_t end = addr + size - 1;
        if (end < addr)            return false;
        if (end >= USER_SPACE_MAX) return false;
    }

    return true;
}

[[nodiscard]] static size_t validate_user_string(const char* str, size_t max_len) {
    if (!validate_user_ptr(str, 1)) return (size_t)-1;

    for (size_t i = 0; i < max_len; i++) {
        if (i > 0 && (((uint64_t)(str + i)) & 0xFFF) == 0) {
            if (!validate_user_ptr(str + i, 1)) return (size_t)-1;
        }
        if (str[i] == '\0') return i;
    }

    return (size_t)-1;
}

static int find_free_fd(Process* p) {
    if (!p) return -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!p->fd_table[i].used) return i;
    }
    return -1;
}

bool is_file_open(const char* filename) {
    // This is now harder with VNodes if we only have the filename.
    // For now, we can just return false or implement a VNode lookup comparison.
    // But since we are moving to VFS, the FS drivers should handle locking/sharing.
    return false;
}

static uint64_t sys_open(const char* filename) {
    if (validate_user_string(filename, 4096) == (size_t)-1) {
        return (uint64_t)-1;
    }

    Process* p = process_get_current();
    if (!p) return (uint64_t)-1;

    int fd = find_free_fd(p);
    if (fd < 0) return (uint64_t)-1;

    char resolved[512];
    vfs_resolve_relative_path(p->cwd, filename, resolved);
    
    VNode* node = vfs_lookup_vnode(resolved);
    
    if (!node) return (uint64_t)-1;

    p->fd_table[fd].used   = true;
    p->fd_table[fd].vnode  = node;
    p->fd_table[fd].offset = 0;

    return fd;
}

static uint64_t sys_read(int fd, char* buf, uint64_t count) {
    if (count > 0 && !validate_user_ptr(buf, count)) {
        return (uint64_t)-1;
    }

    Process* p = process_get_current();
    if (!p) return (uint64_t)-1;

    if (fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used) {
        return (uint64_t)-1;
    }

    if (fd == STDIN_FD) {
        return 0;
    }

    FileDescriptor* f = &p->fd_table[fd];
    if (!f->vnode->ops->read) return (uint64_t)-1;

    int64_t bytes_read = f->vnode->ops->read(f->vnode, buf, count, f->offset, f);
    if (bytes_read > 0) {
        f->offset += bytes_read;
    }

    return (uint64_t)bytes_read;
}

static uint64_t sys_write(int fd, const char* buf, uint64_t count) {
    if (count > 0 && !validate_user_ptr(buf, count)) {
        return (uint64_t)-1;
    }

    Process* p = process_get_current();
    if (!p) return (uint64_t)-1;

    if (fd == STDOUT_FD || fd == STDERR_FD) {
        for (uint64_t i = 0; i < count; i++) {
            if (buf[i] == '\n') {
                p->cursor_x  = 50;
                p->cursor_y += 18;
            } else {
                gfx_draw_char(p->cursor_x, p->cursor_y, buf[i], COLOR_WHITE);
                p->cursor_x += 9;
            }
        }
        return count;
    }

    if (fd >= 3 && fd < MAX_OPEN_FILES && p->fd_table[fd].used) {
        FileDescriptor* f = &p->fd_table[fd];
        if (!f->vnode->ops->write) return (uint64_t)-1;

        int64_t bytes_written = f->vnode->ops->write(f->vnode, buf, count, f->offset, f);
        if (bytes_written > 0) {
            f->offset += bytes_written;
        }
        return (uint64_t)bytes_written;
    }

    return (uint64_t)-1;
}

// SYS_CLOSE: close(fd) -> 0 on success
static uint64_t sys_close(int fd) {
    Process* p = process_get_current();
    if (!p) return (uint64_t)-1;

    if (fd < 3 || fd >= MAX_OPEN_FILES) return (uint64_t)-1;
    if (!p->fd_table[fd].used)           return (uint64_t)-1;

    FileDescriptor* f = &p->fd_table[fd];
    if (f->vnode->ops->close) {
        f->vnode->ops->close(f->vnode);
    }
    
    f->vnode->ref_count--;
    // NOTE: In a real system we would free if ref_count == 0, 
    // but VNodes might be cached by FS.
    
    f->used = false;
    f->vnode = nullptr;
    return 0;
}

// External scheduler and memory functions
extern void    scheduler_create_task(void (*entry)(), const char* name);
extern void    process_exit(int32_t status);
extern void    scheduler_yield();
extern Process* process_get_current();
extern uint64_t* vmm_create_address_space();

static void user_task_wrapper() {
    Process* p = process_get_current();
    if (p->exec_entry != 0) {
        enter_user_mode(p->exec_entry, USER_STACK_TOP);
    }
    process_exit(-1);
}

[[nodiscard]] static int64_t do_exec(const char* path) {
    Process* p = process_get_current();
    if (!p) return -1;

    char resolved[256];
    vfs_resolve_relative_path(p->cwd, path, resolved);

    VNode* node = vfs_lookup_vnode(resolved);
    if (!node) {
        DEBUG_WARN("exec: file not found: %s", resolved);
        return -1;
    }

    if (node->is_dir) {
        node->ref_count--; // TODO: better VNode management
        return -1;
    }

    // Read the file into memory
    uint8_t* buffer = (uint8_t*)malloc(node->size);
    if (!buffer) {
        node->ref_count--;
        return -1;
    }

    if (!node->ops->read) {
        free(buffer);
        node->ref_count--;
        return -1;
    }

    int64_t bytes_read = node->ops->read(node, buffer, node->size, 0, nullptr);
    if (bytes_read != (int64_t)node->size) {
        free(buffer);
        node->ref_count--;
        return -1;
    }

    if (!elf_validate(buffer, node->size)) {
        DEBUG_WARN("exec: not a valid ELF: %s", path);
        free(buffer);
        node->ref_count--;
        return -1;
    }

    Process* current = process_get_current();
    if (!current) {
        free(buffer);
        node->ref_count--;
        return -1;
    }

    uint64_t* new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        free(buffer);
        node->ref_count--;
        return -1;
    }

    scheduler_create_task(user_task_wrapper, "user");
    
    extern Process* scheduler_get_process_list();
    Process* child = scheduler_get_process_list();
    while (child->next != scheduler_get_process_list()) {
        child = child->next;
    }
    
    child->page_table = new_pml4;

    uint64_t entry = elf_load_user(buffer, node->size, child);
    
    free(buffer);
    node->ref_count--;

    if (entry == 0) {
        DEBUG_WARN("exec: failed to load ELF");
        return -1;
    }

    child->exec_entry = entry;
    current->exec_done = false;

    while (!current->exec_done) {
        scheduler_yield();
    }

    return current->exec_exit_status;
}

// Kernel-mode exec wrapper (called by the shell)
[[nodiscard]] int64_t kernel_exec(const char* path) {
    return do_exec(path);
}

static uint64_t sys_readdir(int fd, uint64_t index, char* name_out) {
    if (!validate_user_ptr(name_out, 256)) return (uint64_t)-1;
    
    Process* p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used) return (uint64_t)-1;
    
    FileDescriptor* f = &p->fd_table[fd];
    if (!f->vnode->ops->readdir) return (uint64_t)-1;
    
    return (uint64_t)f->vnode->ops->readdir(f->vnode, index, name_out);
}

extern "C" uint64_t syscall_handler(uint64_t syscall_num,
                                     uint64_t arg1,
                                     uint64_t arg2,
                                     uint64_t arg3,
                                     SyscallFrame* frame) {
    switch (syscall_num) {
        case SYS_READ:
            return sys_read((int)arg1, (char*)arg2, arg3);

        case SYS_WRITE:
            return sys_write((int)arg1, (const char*)arg2, arg3);

        case SYS_OPEN:
            return sys_open((const char*)arg1);

        case SYS_CLOSE:
            return sys_close((int)arg1);

        case SYS_PIPE: {
            if (!validate_user_ptr((void*)arg1, sizeof(int) * 2)) {
                return (uint64_t)-1;
            }
            int pipe_id = pipe_create();
            if (pipe_id < 0) return (uint64_t)-1;
            
            Process* p = process_get_current();
            int fd1 = find_free_fd(p);
            if (fd1 < 0) return (uint64_t)-1;
            p->fd_table[fd1].used = true;
            p->fd_table[fd1].vnode = pipe_get_vnode(pipe_id, false); // Read end
            p->fd_table[fd1].offset = 0;
            
            int fd2 = find_free_fd(p);
            if (fd2 < 0) {
                // TODO: cleanup fd1
                return (uint64_t)-1;
            }
            p->fd_table[fd2].used = true;
            p->fd_table[fd2].vnode = pipe_get_vnode(pipe_id, true); // Write end
            p->fd_table[fd2].offset = 0;
            
            int* fds = (int*)arg1;
            fds[0] = fd1;
            fds[1] = fd2;
            return 0;
        }

        case SYS_GETDENTS:
            return sys_readdir((int)arg1, arg2, (char*)arg3);

        case SYS_GETPID: {
            Process* p = process_get_current();
            return p ? p->pid : 1;
        }

        case SYS_FORK: {
            return process_fork(frame);
        }

        case SYS_EXIT: {
            Process* p = process_get_current();
            if (p) {
                p->state = PROCESS_ZOMBIE;
                p->exit_status = (int32_t)arg1;
                
                Process* parent = process_find_by_pid(p->parent_pid);
                if (parent) {
                    parent->exec_done = true;
                    parent->exec_exit_status = (int32_t)arg1;
                }
            }

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
            return process_waitpid((int64_t)arg1, (int32_t*)arg2);
        }

        default:
            DEBUG_WARN("Unknown syscall: %d\n", syscall_num);
            return (uint64_t)-1;
    }
}
