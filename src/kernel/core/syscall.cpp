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
#include <libk/kstring.h>
#include <libk/kstd.h>

using kstring::string_view;
using kstd::unique_ptr;

extern "C" void enter_user_mode(uint64_t entry_point, uint64_t user_stack);

static constexpr uint64_t USER_SPACE_MAX = 0x0000800000000000ULL;
static constexpr uint64_t USER_STACK_TOP = 0x7FFFF000ULL;

[[nodiscard]] static bool validate_user_ptr(const void* ptr, size_t size) {
    const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    if (addr == 0 || addr >= USER_SPACE_MAX) return false;
    if (size > 0) {
        const uint64_t end = addr + size - 1;
        if (end < addr || end >= USER_SPACE_MAX) return false;
    }
    return true;
}

[[nodiscard]] static size_t validate_user_string(const char* str, size_t max_len) {
    if (!validate_user_ptr(str, 1)) return static_cast<size_t>(-1);
    for (size_t i = 0; i < max_len; i++) {
        if (i > 0 && ((reinterpret_cast<uint64_t>(str + i)) & 0xFFF) == 0) {
            if (!validate_user_ptr(str + i, 1)) return static_cast<size_t>(-1);
        }
        if (str[i] == '\0') return i;
    }
    return static_cast<size_t>(-1);
}

[[nodiscard]] static int find_free_fd(Process* p) {
    if (!p) return -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!p->fd_table[i].used) return i;
    }
    return -1;
}

bool is_file_open(const char*) { return false; }

[[nodiscard]] static uint64_t sys_open(const char* filename) {
    if (validate_user_string(filename, 4096) == static_cast<size_t>(-1)) return static_cast<uint64_t>(-1);
    Process* p = process_get_current();
    if (!p) return static_cast<uint64_t>(-1);
    int fd = find_free_fd(p);
    if (fd < 0) return static_cast<uint64_t>(-1);

    char resolved[512];
    vfs_resolve_relative_path(p->cwd, filename, resolved);
    VNode* node = vfs_lookup_vnode(resolved);
    if (!node) return static_cast<uint64_t>(-1);

    p->fd_table[fd].used = true;
    p->fd_table[fd].vnode = node;
    p->fd_table[fd].offset = 0;
    return static_cast<uint64_t>(fd);
}

[[nodiscard]] static uint64_t sys_read(int fd, char* buf, uint64_t count) {
    if (count > 0 && !validate_user_ptr(buf, count)) return static_cast<uint64_t>(-1);
    Process* p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used) return static_cast<uint64_t>(-1);
    if (fd == STDIN_FD) return 0;

    FileDescriptor* f = &p->fd_table[fd];
    if (!f->vnode->ops->read) return static_cast<uint64_t>(-1);
    int64_t bytes_read = f->vnode->ops->read(f->vnode, buf, count, f->offset, f);
    if (bytes_read > 0) f->offset += bytes_read;
    return static_cast<uint64_t>(bytes_read);
}

[[nodiscard]] static uint64_t sys_write(int fd, const char* buf, uint64_t count) {
    if (count > 0 && !validate_user_ptr(buf, count)) return static_cast<uint64_t>(-1);
    Process* p = process_get_current();
    if (!p) return static_cast<uint64_t>(-1);

    if (fd == STDOUT_FD || fd == STDERR_FD) {
        for (uint64_t i = 0; i < count; i++) {
            if (buf[i] == '\n') {
                p->cursor_x = 50;
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
        if (!f->vnode->ops->write) return static_cast<uint64_t>(-1);
        int64_t bytes_written = f->vnode->ops->write(f->vnode, buf, count, f->offset, f);
        if (bytes_written > 0) f->offset += bytes_written;
        return static_cast<uint64_t>(bytes_written);
    }
    return static_cast<uint64_t>(-1);
}

[[nodiscard]] static uint64_t sys_close(int fd) {
    Process* p = process_get_current();
    if (!p || fd < 3 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used) return static_cast<uint64_t>(-1);

    FileDescriptor* f = &p->fd_table[fd];
    if (f->vnode->ops->close) f->vnode->ops->close(f->vnode);
    f->vnode->ref_count--;
    f->used = false;
    f->vnode = nullptr;
    return 0;
}

extern void scheduler_create_task(void (*entry)(), const char* name);
extern void process_exit(int32_t status);
extern void scheduler_yield();
extern Process* process_get_current();
extern uint64_t* vmm_create_address_space();

static void user_task_wrapper() {
    Process* p = process_get_current();
    if (p->exec_entry != 0) enter_user_mode(p->exec_entry, USER_STACK_TOP);
    process_exit(-1);
}

[[nodiscard]] static int64_t do_exec(const char* path) {
    Process* p = process_get_current();
    if (!p) return -1;

    char resolved[256];
    vfs_resolve_relative_path(p->cwd, path, resolved);
    VNode* node = vfs_lookup_vnode(resolved);
    if (!node) return -1;
    if (node->is_dir) { node->ref_count--; return -1; }

    kstd::unique_ptr<uint8_t[]> buffer(static_cast<uint8_t*>(malloc(node->size)));
    if (!buffer || !node->ops->read) { node->ref_count--; return -1; }

    if (node->ops->read(node, buffer.get(), node->size, 0, nullptr) != static_cast<int64_t>(node->size)) {
        node->ref_count--; return -1;
    }

    if (!elf_validate(buffer.get(), node->size)) { node->ref_count--; return -1; }

    uint64_t* new_pml4 = vmm_create_address_space();
    if (!new_pml4) { node->ref_count--; return -1; }

    scheduler_create_task(user_task_wrapper, "user");
    extern Process* scheduler_get_process_list();
    Process* child = scheduler_get_process_list();
    while (child->next != scheduler_get_process_list()) child = child->next;
    
    child->page_table = new_pml4;
    uint64_t entry = elf_load_user(buffer.get(), node->size, child);
    node->ref_count--;

    if (entry == 0) return -1;
    child->exec_entry = entry;
    p->exec_done = false;
    while (!p->exec_done) scheduler_yield();
    return p->exec_exit_status;
}

[[nodiscard]] int64_t kernel_exec(const char* path) { return do_exec(path); }

static uint64_t sys_readdir(int fd, uint64_t index, char* name_out) {
    if (!validate_user_ptr(name_out, 256)) return static_cast<uint64_t>(-1);
    Process* p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used) return static_cast<uint64_t>(-1);
    FileDescriptor* f = &p->fd_table[fd];
    if (!f->vnode->ops->readdir) return static_cast<uint64_t>(-1);
    return static_cast<uint64_t>(f->vnode->ops->readdir(f->vnode, index, name_out));
}

extern "C" uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, SyscallFrame* frame) {
    switch (syscall_num) {
        case SYS_READ: return sys_read(static_cast<int>(arg1), reinterpret_cast<char*>(arg2), arg3);
        case SYS_WRITE: return sys_write(static_cast<int>(arg1), reinterpret_cast<const char*>(arg2), arg3);
        case SYS_OPEN: return sys_open(reinterpret_cast<const char*>(arg1));
        case SYS_CLOSE: return sys_close(static_cast<int>(arg1));
        case SYS_PIPE: {
            if (!validate_user_ptr(reinterpret_cast<void*>(arg1), sizeof(int) * 2)) return static_cast<uint64_t>(-1);
            int pipe_id = pipe_create();
            if (pipe_id < 0) return static_cast<uint64_t>(-1);
            Process* p = process_get_current();
            int fd1 = find_free_fd(p);
            if (fd1 < 0) return static_cast<uint64_t>(-1);
            p->fd_table[fd1].used = true;
            p->fd_table[fd1].vnode = pipe_get_vnode(pipe_id, false);
            p->fd_table[fd1].offset = 0;
            int fd2 = find_free_fd(p);
            if (fd2 < 0) return static_cast<uint64_t>(-1);
            p->fd_table[fd2].used = true;
            p->fd_table[fd2].vnode = pipe_get_vnode(pipe_id, true);
            p->fd_table[fd2].offset = 0;
            reinterpret_cast<int*>(arg1)[0] = fd1;
            reinterpret_cast<int*>(arg1)[1] = fd2;
            return 0;
        }
        case SYS_GETDENTS: return sys_readdir(static_cast<int>(arg1), arg2, reinterpret_cast<char*>(arg3));
        case SYS_GETPID: { Process* p = process_get_current(); return p ? p->pid : 1; }
        case SYS_FORK: return process_fork(frame);
        case SYS_EXIT: {
            Process* p = process_get_current();
            if (p) {
                p->state = PROCESS_ZOMBIE;
                p->exit_status = static_cast<int32_t>(arg1);
                if (Process* parent = process_find_by_pid(p->parent_pid)) {
                    parent->exec_done = true;
                    parent->exec_exit_status = static_cast<int32_t>(arg1);
                }
            }
            asm volatile("sti; hlt" ::: "memory");
            for (;;) asm volatile("hlt");
        }
        case SYS_EXEC:
            if (validate_user_string(reinterpret_cast<const char*>(arg1), 256) == static_cast<size_t>(-1)) return static_cast<uint64_t>(-1);
            return static_cast<uint64_t>(do_exec(reinterpret_cast<const char*>(arg1)));
        case SYS_WAIT4:
            if (arg2 != 0 && !validate_user_ptr(reinterpret_cast<void*>(arg2), sizeof(int32_t))) return static_cast<uint64_t>(-1);
            return process_waitpid(static_cast<int64_t>(arg1), reinterpret_cast<int32_t*>(arg2));
        default: DEBUG_WARN("Unknown syscall: %d", syscall_num); return static_cast<uint64_t>(-1);
    }
}
