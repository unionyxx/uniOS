#include <boot/boot_info.h>
#include <drivers/class/hid/input.h>
#include <drivers/rtc/rtc.h>
#include <drivers/sound/sound.h>
#include <drivers/video/display.h>
#include <drivers/video/framebuffer.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/elf.h>
#include <kernel/event.h>
#include <kernel/fs/pipe.h>
#include <kernel/fs/storage_guard.h>
#include <kernel/fs/unifs.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/volume.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/net/dns.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time/timer.h>
#include <libk/kstd.h>
#include <libk/kstring.h>
#include <stddef.h>
#include <uapi/signal.h>

using kstd::unique_ptr;
using kstring::string_view;

extern "C" void enter_user_mode(uint64_t entry_point, uint64_t user_stack);
extern bool display_buffer_set_wm_access(DisplayBufferHandle handle, bool allow);
extern bool display_import_surface(uint64_t owner_pid, const DisplaySurfaceImport &request, DisplaySurface *out_surface);

static constexpr uint64_t USER_SPACE_MAX = 0x0000800000000000ULL;
static constexpr uint64_t USER_STACK_TOP = 0x0000700000000000ULL;

struct ShmBlock
{
    uint64_t phys_addr;
    size_t size;
    uint64_t owner_pid;
    bool used;
};
static ShmBlock g_shm_blocks[64] = {};
static Spinlock g_shm_lock = SPINLOCK_INIT;
static constexpr uint64_t SHM_BASE = 0x300000000ULL;
static constexpr uint64_t SHM_SLOT_SIZE = 0x1000000ULL;

#define STDIN_FD 0
#define STDOUT_FD 1
#define STDERR_FD 2

static constexpr int SOCKET_KIND_UDP = 1;
static constexpr int SOCKET_KIND_TCP = 2;
static constexpr int SOCKET_KIND_SHIFT = 12;
static constexpr int SOCKET_INDEX_MASK = (1 << SOCKET_KIND_SHIFT) - 1;

[[nodiscard]] static int socket_make_handle(int kind, int index)
{
    return (kind << SOCKET_KIND_SHIFT) | index;
}

[[nodiscard]] static bool socket_decode_handle(int handle, int *kind, int *index)
{
    if (!kind || !index || handle < 0)
        return false;

    if (handle < UDP_MAX_SOCKETS) {
        *kind = SOCKET_KIND_UDP;
        *index = handle;
        return true;
    }

    const int decoded_kind = handle >> SOCKET_KIND_SHIFT;
    const int decoded_index = handle & SOCKET_INDEX_MASK;
    if (decoded_kind == SOCKET_KIND_UDP && decoded_index < UDP_MAX_SOCKETS) {
        *kind = SOCKET_KIND_UDP;
        *index = decoded_index;
        return true;
    }
    if (decoded_kind == SOCKET_KIND_TCP && decoded_index < TCP_MAX_SOCKETS) {
        *kind = SOCKET_KIND_TCP;
        *index = decoded_index;
        return true;
    }
    return false;
}

static uint64_t g_random_state = 0x7F4A7C15D39E2B41ULL;

[[nodiscard]] static uint64_t read_tsc_counter()
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

[[nodiscard]] static bool rdrand64(uint64_t *out)
{
    if (!out || !g_cpu_features.has_rdrand)
        return false;
    for (int i = 0; i < 10; i++) {
        uint64_t value = 0;
        unsigned char ok = 0;
        asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) {
            *out = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] static uint64_t random_next_u64()
{
    uint64_t hw = 0;
    const bool have_hw = rdrand64(&hw);
    uint64_t x = g_random_state ^ read_tsc_counter() ^ timer_get_ticks() ^
                 reinterpret_cast<uint64_t>(&g_random_state) ^ 0x9E3779B97F4A7C15ULL;
    if (have_hw)
        x ^= hw;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_random_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

#define STAC()                                                                                                         \
    if (g_cpu_features.has_smap)                                                                                       \
    asm volatile("stac" ::: "memory")
#define CLAC()                                                                                                         \
    if (g_cpu_features.has_smap)                                                                                       \
    asm volatile("clac" ::: "memory")

[[nodiscard]] static bool checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (!out)
        return false;
    if (a != 0 && b > SIZE_MAX / a)
        return false;
    *out = a * b;
    return true;
}

[[nodiscard]] static bool checked_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    return !__builtin_add_overflow(a, b, out);
}

[[nodiscard]] static bool checked_add_size(size_t a, size_t b, size_t *out)
{
    if (!out || a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

[[nodiscard]] static bool round_up_page_size(uint64_t value, size_t *out)
{
    if (!out || value == 0 || value > (uint64_t)SIZE_MAX - 0xFFFULL)
        return false;
    *out = (size_t)((value + 0xFFFULL) & ~0xFFFULL);
    return *out != 0;
}

[[nodiscard]] static bool validate_user_ptr(const void *ptr, size_t size, bool write = false)
{
    const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    if (addr == 0 || addr >= USER_SPACE_MAX)
        return false;
    if (size == 0)
        return true;

    const uint64_t end = addr + size;
    if (end < addr || end > USER_SPACE_MAX)
        return false;

    Process *p = process_get_current();
    if (!p)
        return false;

    uint64_t current = addr;
    while (current < end) {
        VMA *vma = vma_find(p->vma_list, current);
        if (!vma)
            return false;
        if (write && !(vma->flags & PTE_WRITABLE))
            return false;
        current = vma->end;
    }
    return true;
}

[[nodiscard]] static size_t validate_user_string(const char *str, size_t max_len)
{
    if (!validate_user_ptr(str, 1, false))
        return static_cast<size_t>(-1);
    STAC();
    for (size_t i = 0; i < max_len; i++) {
        if (i > 0 && ((reinterpret_cast<uint64_t>(str + i)) & 0xFFF) == 0) {
            CLAC();
            if (!validate_user_ptr(str + i, 1, false))
                return static_cast<size_t>(-1);
            STAC();
        }
        if (str[i] == '\0') {
            CLAC();
            return i;
        }
    }
    CLAC();
    return static_cast<size_t>(-1);
}

static bool clip_display_rect(Rect &rect, uint32_t width, uint32_t height)
{
    int64_t x = rect.x;
    int64_t y = rect.y;
    int64_t w = rect.w;
    int64_t h = rect.h;

    if (w <= 0 || h <= 0)
        return false;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    int64_t max_width = (int64_t)width;
    int64_t max_height = (int64_t)height;
    if (x >= max_width || y >= max_height)
        return false;
    if (x + w > max_width)
        w = max_width - x;
    if (y + h > max_height)
        h = max_height - y;
    if (w <= 0 || h <= 0)
        return false;

    rect.x = (int32_t)x;
    rect.y = (int32_t)y;
    rect.w = (int32_t)w;
    rect.h = (int32_t)h;
    return true;
}

static bool validate_display_buffer_access(const DisplayPresentRequest &request, uint32_t width, uint32_t height)
{
    if (!request.buffer || request.stride == 0)
        return false;

    uint64_t max_words = 0;
    for (uint32_t i = 0; i < request.rect_count; i++) {
        Rect rect = request.rects[i];
        if (!clip_display_rect(rect, width, height))
            continue;

        int64_t src_x = (int64_t)rect.x - request.source_origin_x;
        int64_t src_y = (int64_t)rect.y - request.source_origin_y;
        if (src_x < 0 || src_y < 0)
            return false;

        uint64_t src_right = (uint64_t)src_x + (uint64_t)rect.w;
        uint64_t src_bottom = (uint64_t)src_y + (uint64_t)rect.h;
        if (src_right > request.stride)
            return false;

        uint64_t row_offset = 0;
        uint64_t row_end = 0;
        if (__builtin_mul_overflow(src_bottom - 1u, (uint64_t)request.stride, &row_offset) ||
            __builtin_add_overflow(row_offset, src_right, &row_end)) {
            return false;
        }
        if (row_end > max_words)
            max_words = row_end;
    }

    if (max_words == 0)
        return true;
    if (max_words > (UINT64_MAX / sizeof(uint32_t)))
        return false;
    return validate_user_ptr(request.buffer, (size_t)(max_words * sizeof(uint32_t)), false);
}

[[nodiscard]] static bool copy_string_from_user(const char *user_str, char *kernel_buf, size_t max_len)
{
    size_t len = validate_user_string(user_str, max_len);
    if (len == static_cast<size_t>(-1))
        return false;

    STAC();
    for (size_t i = 0; i < len; i++) {
        kernel_buf[i] = user_str[i];
    }
    kernel_buf[len] = '\0';
    CLAC();
    return true;
}

static bool shm_unmap_from_process(Process *p, int id)
{
    if (!p || id < 0 || id >= 64 || !p->page_table)
        return false;

    const uint64_t virt_start = SHM_BASE + (uint64_t)((uint32_t)id) * SHM_SLOT_SIZE;

    spinlock_acquire(&p->vma_lock);
    VMA *mapping = vma_find(p->vma_list, virt_start);
    if (!mapping || mapping->start != virt_start || mapping->type != VMAType::Shared) {
        spinlock_release(&p->vma_lock);
        return false;
    }

    const uint64_t mapping_start = mapping->start;
    const uint64_t mapping_end = mapping->end;
    vma_remove(&p->vma_list, mapping_start, mapping_end);
    spinlock_release(&p->vma_lock);

    for (uint64_t virt = mapping_start; virt < mapping_end; virt += 4096) {
        uint64_t phys = vmm_virt_to_phys_in(p->page_table, virt);
        if (!phys)
            continue;
        vmm_unmap_page_in(p->page_table, virt);
        pmm_refcount_dec(reinterpret_cast<void *>(phys));
    }

    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    return true;
}

static bool munmap_process_range(Process *p, uint64_t addr, size_t length)
{
    if (!p || !p->page_table || addr == 0 || length == 0 || (addr & 0xFFFULL) != 0)
        return false;

    if (length > SIZE_MAX - 0xFFFULL)
        return false;
    length = (length + 0xFFFULL) & ~0xFFFULL;
    if (length == 0 || addr > UINT64_MAX - length)
        return false;

    spinlock_acquire(&p->vma_lock);
    VMA *mapping = vma_find(p->vma_list, addr);
    if (!mapping || mapping->start != addr || mapping->end != addr + length || mapping->type == VMAType::Text ||
        mapping->type == VMAType::Stack || mapping->type == VMAType::Shared) {
        spinlock_release(&p->vma_lock);
        return false;
    }

    const uint64_t mapping_start = mapping->start;
    const uint64_t mapping_end = mapping->end;
    vma_remove(&p->vma_list, mapping_start, mapping_end);
    spinlock_release(&p->vma_lock);

    for (uint64_t virt = mapping_start; virt < mapping_end; virt += 4096) {
        uint64_t phys = vmm_virt_to_phys_in(p->page_table, virt);
        if (!phys)
            continue;
        vmm_unmap_page_in(p->page_table, virt);
        pmm_free_frame(reinterpret_cast<void *>(phys));
    }

    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    return true;
}

static bool shm_release_owner_reference(Process *p, int id)
{
    if (id < 0 || id >= 64)
        return false;

    spinlock_acquire(&g_shm_lock);
    ShmBlock *block = &g_shm_blocks[id];
    if (!block->used) {
        spinlock_release(&g_shm_lock);
        return false;
    }

    const uint64_t caller_pid = p ? p->pid : 0;
    const uint32_t caller_uid = p ? p->uid : 0;
    if (caller_uid != 0 && block->owner_pid != caller_pid) {
        spinlock_release(&g_shm_lock);
        return false;
    }

    const uint64_t phys_start = block->phys_addr;
    const size_t size = block->size;
    block->used = false;
    block->phys_addr = 0;
    block->size = 0;
    block->owner_pid = 0;
    spinlock_release(&g_shm_lock);

    for (uint64_t i = 0; i < size; i += 4096) {
        pmm_refcount_dec(reinterpret_cast<void *>(phys_start + i));
    }
    return true;
}

void shm_cleanup_process(Process *proc)
{
    if (!proc)
        return;

    for (int id = 0; id < 64; id++) {
        shm_unmap_from_process(proc, id);
    }

    for (int id = 0; id < 64; id++) {
        spinlock_acquire(&g_shm_lock);
        bool owned = g_shm_blocks[id].used && g_shm_blocks[id].owner_pid == proc->pid;
        spinlock_release(&g_shm_lock);
        if (owned) {
            shm_release_owner_reference(proc, id);
        }
    }
}

[[nodiscard]] static int find_free_fd(Process *p)
{
    if (!p)
        return -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!p->fd_table[i].used)
            return i;
    }
    return -1;
}

[[nodiscard]] static uint64_t sys_open(const char *filename, int flags, uint64_t mode)
{
    char k_path[512];
    if (!copy_string_from_user(filename, k_path, 511))
        return static_cast<uint64_t>(-1);

    // Preserve compatibility with older two-argument open() shims while also
    // honoring the POSIX-style mode argument when O_CREAT is used. The VFS
    // sanitizes zero or out-of-range modes to a safe default.
    return static_cast<uint64_t>(vfs_open(k_path, flags, static_cast<uint16_t>(mode)));
}

void signal_send(Process *p, int sig)
{
    if (!p || sig <= 0 || sig > 31)
        return;
    p->signals.pending |= (1ULL << sig);
    if (p->state == ProcessState_Waiting || p->state == ProcessState_Blocked) {
        scheduler_wake_process(p);
    }
}

extern "C" void signal_send_current(int sig)
{
    signal_send(process_get_current(), sig);
}

extern "C" void signal_check(SyscallFrame *frame)
{
    Process *p = process_get_current();
    if (!p || p->signals.pending == 0)
        return;

    for (int i = 1; i < 32; i++) {
        if (p->signals.pending & (1ULL << i)) {
            p->signals.pending &= ~(1ULL << i); // Clear pending bit

            if (p->signals.handlers[i] == SIG_DFL) {
                // Default actions
                if (i == SIGINT || i == SIGTERM || i == SIGQUIT || i == SIGKILL || i == SIGSEGV) {
                    process_exit(-i); // Use negative signum as exit status
                }
            } else if (p->signals.handlers[i] == SIG_IGN) {
                continue;
            } else {
                // Basic signal delivery: Hijack RIP to the handler
                // PUSH a return trampoline address onto the user stack
                uint64_t *user_stack = reinterpret_cast<uint64_t *>(p->sp); // Default or current?
                // We need to use valid_user_ptr and copy_to_user logic
                uint64_t trampoline = p->signals.restorer;
                if (trampoline == 0) {
                    // Fallback to a known location or just fail?
                    // For now, assume it's set via a new syscall or fixed in crt0
                    // Let's use a hardcoded address if we can't find it
                }

                frame->rsp -= 8;
                uint64_t phys_stack = vmm_virt_to_phys(frame->rsp);
                if (phys_stack == 0) {
                    // Stack is not mapped or invalid? Segfault!
                    CLAC();
                    process_exit(-SIGSEGV);
                    return;
                }
                uint64_t *stack_ptr = reinterpret_cast<uint64_t *>(vmm_phys_to_virt(phys_stack));
                *stack_ptr = trampoline;

                frame->rip = (uint64_t)p->signals.handlers[i];
                return;
            }
        }
    }
}

[[nodiscard]] static uint64_t sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    if (sig <= 0 || sig > 31)
        return static_cast<uint64_t>(-1);
    Process *p = process_get_current();
    if (!p)
        return static_cast<uint64_t>(-1);

    if (oldact && validate_user_ptr(oldact, sizeof(struct sigaction), true)) {
        STAC();
        oldact->sa_handler = p->signals.handlers[sig];
        oldact->sa_flags = 0; // Not fully implemented yet
        CLAC();
    }

    if (act && validate_user_ptr(act, sizeof(struct sigaction), false)) {
        STAC();
        p->signals.handlers[sig] = act->sa_handler;
        p->signals.restorer = (uint64_t)act->sa_restorer;
        CLAC();
    }

    return 0;
}

[[nodiscard]] static uint64_t sys_kill(uint64_t pid, int sig)
{
    if (sig <= 0 || sig > 31)
        return static_cast<uint64_t>(-1);

    Process *current = process_get_current();
    Process *target = process_find_by_pid(pid);
    if (!current || !target)
        return static_cast<uint64_t>(-1);

    if (current->uid != 0 && current->pid != target->pid && current->uid != target->uid) {
        return static_cast<uint64_t>(-1);
    }

    signal_send(target, sig);
    return 0;
}

#include <kernel/terminal.h>

[[nodiscard]] static uint64_t sys_read(int fd, char *buf, uint64_t count)
{
    if (count > 0 && !validate_user_ptr(buf, count, true))
        return static_cast<uint64_t>(-1);
    Process *p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES)
        return static_cast<uint64_t>(-1);

    // Respect redirected FDs (e.g. pipes) even for stdin
    if (p->fd_table[fd].used && p->fd_table[fd].vnode) {
        return static_cast<uint64_t>(vfs_read(fd, buf, count));
    }

    if (fd == STDIN_FD) {
        if (count == 0)
            return 0;

        uint64_t read_count = 0;
        while (read_count == 0) {
            input_poll();
            while (!input_keyboard_has_char() && !serial_has_char()) {
                scheduler_sleep_ms(1);
                input_poll();
            }

            STAC();
            while (read_count < count) {
                if (input_keyboard_has_char()) {
                    buf[read_count++] = input_keyboard_get_char();
                } else if (serial_has_char()) {
                    buf[read_count++] = serial_getc();
                } else {
                    break;
                }
            }
            CLAC();
        }
        return read_count;
    }

    return static_cast<uint64_t>(-1);
}

[[nodiscard]] static uint64_t sys_write(int fd, const char *buf, uint64_t count)
{
    if (count > 0 && !validate_user_ptr(buf, count, false))
        return static_cast<uint64_t>(-1);
    Process *p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES)
        return static_cast<uint64_t>(-1);

    // Respect redirected FDs (e.g. pipes) even for stdout/stderr
    if (p->fd_table[fd].used && p->fd_table[fd].vnode) {
        return static_cast<uint64_t>(vfs_write(fd, buf, count));
    }

    if (fd == STDOUT_FD || fd == STDERR_FD) {
        if (g_boot_user_stdout_to_framebuffer) {
            // Buffer-based logging to achieve unified style for user-space boot messages
            char k_log_buf[512];
            uint64_t to_copy = (count < 511) ? count : 511;
            STAC();
            for (uint64_t i = 0; i < to_copy; i++)
                k_log_buf[i] = buf[i];
            CLAC();
            k_log_buf[to_copy] = '\0';

            if (to_copy > 0 && k_log_buf[0] == '[') {
                kprintf("%s", k_log_buf);
                return count;
            }

            // Remove trailing newline if present, as klog handles it
            if (to_copy > 0 && k_log_buf[to_copy - 1] == '\n')
                k_log_buf[to_copy - 1] = '\0';

            if (to_copy > 0) {
                klog(LogModule::Boot, LogLevel::Info, nullptr, "%s", k_log_buf);
            }
            return count;
        }

        // GUI mode: only write to serial, never to the framebuffer terminal.
        // The Window Manager owns the framebuffer now.
        STAC();
        for (uint64_t i = 0; i < count; i++) {
            serial_putc(buf[i]);
        }
        CLAC();
        return count;
    }

    return static_cast<uint64_t>(vfs_write(fd, buf, count));
}

[[nodiscard]] static uint64_t sys_close(int fd)
{
    return static_cast<uint64_t>(vfs_close(fd));
}

[[nodiscard]] static uint64_t sys_dup2(int oldfd, int newfd)
{
    Process *p = process_get_current();
    if (!p)
        return static_cast<uint64_t>(-1);

    if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || !p->fd_table[oldfd].used)
        return static_cast<uint64_t>(-1);
    if (newfd < 0 || newfd >= MAX_OPEN_FILES)
        return static_cast<uint64_t>(-1);

    if (oldfd == newfd)
        return static_cast<uint64_t>(newfd);

    if (p->fd_table[newfd].used) {
        vfs_close(newfd);
    }

    p->fd_table[newfd] = p->fd_table[oldfd];
    if (p->fd_table[newfd].vnode) {
        p->fd_table[newfd].vnode->ref_count++;
    }

    return static_cast<uint64_t>(newfd);
}

extern void process_exit(int32_t status);
extern void scheduler_yield();
extern Process *process_get_current();
extern uint64_t *vmm_create_address_space();

static void user_task_wrapper()
{
    Process *p = process_get_current();
    DEBUG_INFO("Launching process: pid=%lu entry=0x%lx cr3=%p", p->pid, p->exec_entry, (void *)p->page_table);

    if (p->exec_entry != 0) {
        DEBUG_INFO("Entering user mode: rip=0x%lx rsp=0x%lx", p->exec_entry, USER_STACK_TOP);
        enter_user_mode(p->exec_entry, USER_STACK_TOP);
    }

    DEBUG_ERROR("Process %lu fell through to exit(-1)", p->pid);
    process_exit(-1);
}

[[nodiscard]] int64_t do_exec(const char *path, SyscallFrame *frame)
{
    Process *p = process_get_current();
    if (!p)
        return -1;

    char k_path[512];
    if (!copy_string_from_user(path, k_path, 511))
        return -1;

    char resolved[512];
    vfs_resolve_relative_path(p->cwd, k_path, resolved);
    VNode *node = vfs_lookup_vnode(resolved);
    if (!node)
        return -1;
    if (node->is_dir) {
        vfs_close_vnode(node);
        return -1;
    }

    kstd::unique_ptr<uint8_t[]> buffer(static_cast<uint8_t *>(malloc(node->size)));
    if (!buffer || !node->ops || !node->ops->read) {
        vfs_close_vnode(node);
        return -1;
    }

    if (node->ops->read(node, buffer.get(), node->size, 0, nullptr) != static_cast<int64_t>(node->size)) {
        vfs_close_vnode(node);
        return -1;
    }

    if (!elf_validate(buffer.get(), node->size)) {
        vfs_close_vnode(node);
        return -1;
    }

    uint64_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        vfs_close_vnode(node);
        return -1;
    }

    Process *loader_proc = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!loader_proc) {
        vmm_free_address_space(new_pml4);
        vfs_close_vnode(node);
        return -1;
    }
    kstring::zero_memory(loader_proc, sizeof(Process));

    kstring::memcpy(loader_proc, p, sizeof(Process));
    loader_proc->page_table = new_pml4;
    loader_proc->vma_list = nullptr;

    uint64_t entry = elf_load_user(buffer.get(), node->size, loader_proc);
    vfs_close_vnode(node);

    if (entry == 0) {
        if (loader_proc->vma_list)
            vma_free_all(loader_proc->vma_list);
        vmm_free_address_space(new_pml4);
        aligned_free(loader_proc);
        return -1;
    }

    uint64_t *old_pml4 = p->page_table;
    VMA *old_vma_list = p->vma_list;

    spinlock_acquire(&p->vma_lock);
    p->page_table = new_pml4;
    p->vma_list = loader_proc->vma_list;
    p->exec_entry = entry;
    spinlock_release(&p->vma_lock);

    loader_proc->vma_list = nullptr;
    aligned_free(loader_proc);

    if (old_vma_list)
        vma_free_all(old_vma_list);

    p->exec_entry = entry;
    kstring::strncpy(p->name, kstring::strrchr(k_path, '/') ? kstring::strrchr(k_path, '/') + 1 : k_path, 31);

    if (frame) {
        frame->rip = entry;
        frame->rsp = USER_STACK_TOP;
        frame->rbx = frame->rbp = frame->r12 = frame->r13 = frame->r14 = frame->r15 = 0;
    }

    uint64_t new_pml4_phys = reinterpret_cast<uint64_t>(new_pml4) - vmm_get_hhdm_offset();
    vmm_switch_address_space(reinterpret_cast<uint64_t *>(new_pml4_phys));

    if (old_pml4)
        vmm_free_address_space(old_pml4);

    return 0;
}

[[nodiscard]] int64_t kernel_exec(const char *path)
{
    Process *p = process_get_current();
    if (!p) {
        DEBUG_ERROR("kernel_exec: no current process for %s", path ? path : "(null)");
        return -1;
    }

    char resolved[512];
    vfs_resolve_relative_path(p->cwd, path, resolved);
    VNode *node = vfs_lookup_vnode(resolved);
    if (!node) {
        DEBUG_ERROR("kernel_exec: executable not found: %s", resolved);
        return -1;
    }

    size_t file_size = node->size;
    kstd::unique_ptr<uint8_t[]> buffer(static_cast<uint8_t *>(malloc(file_size)));
    if (!buffer || !node->ops || !node->ops->read) {
        vfs_close_vnode(node);
        DEBUG_ERROR("kernel_exec: allocation failed for %s (%lu bytes)", resolved, (uint64_t)file_size);
        return -1;
    }
    if (node->ops->read(node, buffer.get(), file_size, 0, nullptr) != (int64_t)file_size) {
        vfs_close_vnode(node);
        DEBUG_ERROR("kernel_exec: read failed for %s", resolved);
        return -1;
    }
    vfs_close_vnode(node);

    uint64_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        DEBUG_ERROR("kernel_exec: address space allocation failed for %s", resolved);
        return -1;
    }

    Process *loader_proc = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!loader_proc) {
        vmm_free_address_space(new_pml4);
        DEBUG_ERROR("kernel_exec: loader process allocation failed for %s", resolved);
        return -1;
    }
    kstring::zero_memory(loader_proc, sizeof(Process));
    loader_proc->page_table = new_pml4;
    loader_proc->vma_list = nullptr;
    loader_proc->uid = p->uid;
    loader_proc->parent_pid = p->pid;
    spinlock_init(&loader_proc->vma_lock);

    uint64_t entry = elf_load_user(buffer.get(), file_size, loader_proc);
    if (entry == 0) {
        if (loader_proc->vma_list)
            vma_free_all(loader_proc->vma_list);
        vmm_free_address_space(new_pml4);
        aligned_free(loader_proc);
        DEBUG_ERROR("kernel_exec: elf_load_user failed for %s", resolved);
        return -1;
    }

    const uint64_t flags = interrupts_save_disable();

    Process *child = scheduler_create_task(user_task_wrapper, path);
    if (!child) {
        interrupts_restore(flags);
        if (loader_proc->vma_list)
            vma_free_all(loader_proc->vma_list);
        vmm_free_address_space(new_pml4);
        aligned_free(loader_proc);
        DEBUG_ERROR("kernel_exec: scheduler_create_task failed for %s", resolved);
        return -1;
    }

    child->page_table = new_pml4;
    child->vma_list = loader_proc->vma_list;
    loader_proc->vma_list = nullptr;
    child->exec_entry = entry;

    interrupts_restore(flags);
    aligned_free(loader_proc);
    DEBUG_SUCCESS("kernel_exec: launched %s as pid %lu", resolved, child->pid);
    return static_cast<int64_t>(child->pid);
}

static uint64_t sys_getprocs(ProcessInfo *out, uint64_t max_count)
{
    if (max_count == 0)
        return 0;
    if (max_count > (uint64_t)SIZE_MAX / sizeof(ProcessInfo))
        return static_cast<uint64_t>(-1);
    const size_t out_bytes = sizeof(ProcessInfo) * (size_t)max_count;
    if (!validate_user_ptr(out, out_bytes, true))
        return static_cast<uint64_t>(-1);

    extern Process *scheduler_get_process_list();
    Process *list = scheduler_get_process_list();
    if (!list)
        return 0;

    uint64_t count = 0;
    Process *curr = list;

    // Zero out the entire user buffer first to prevent leaks via alignment padding
    STAC();
    kstring::zero_memory(out, out_bytes);

    do {
        out[count].pid = curr->pid;
        out[count].parent_pid = curr->parent_pid;
        out[count].uid = curr->uid;
        out[count].state = curr->state;
        out[count].priority = curr->priority;
        kstring::strncpy(out[count].name, curr->name, 31);
        out[count].name[31] = '\0';

        count++;
        curr = curr->next;
    } while (curr != list && count < max_count);
    CLAC();

    return count;
}

static uint64_t sys_getsysinfo(SystemProfile *out)
{
    if (!validate_user_ptr(out, sizeof(SystemProfile), true))
        return static_cast<uint64_t>(-1);

    extern const char *g_bootloader_name;
    extern const char *g_bootloader_version;

    SystemProfile profile = {};
    kstring::strncpy(profile.kernel_commit, GIT_COMMIT, sizeof(profile.kernel_commit) - 1);
    if (g_bootloader_name) {
        kstring::strncpy(profile.bootloader_name, g_bootloader_name, sizeof(profile.bootloader_name) - 1);
    }
    if (g_bootloader_version) {
        kstring::strncpy(profile.bootloader_version, g_bootloader_version, sizeof(profile.bootloader_version) - 1);
    }
    profile.timer_hz = timer_get_frequency();
#ifdef DEBUG
    profile.kernel_build_debug = 1;
#else
    profile.kernel_build_debug = 0;
#endif

    STAC();
    *out = profile;
    CLAC();
    return 0;
}

[[nodiscard]] static uint64_t sys_set_quiet(bool quiet)
{
    extern bool g_boot_quiet;
    extern bool g_boot_framebuffer_logging;
    extern bool g_boot_user_stdout_to_framebuffer;
    g_boot_quiet = quiet;
#ifdef DEBUG
    bool enable_framebuffer_output = !quiet;
#else
    bool enable_framebuffer_output = false;
#endif
    g_boot_framebuffer_logging = enable_framebuffer_output;
    g_boot_user_stdout_to_framebuffer = enable_framebuffer_output;
    return 0;
}

extern "C" uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                    SyscallFrame *frame)
{
    // Log syscalls using DEBUG_TRACE
    // DEBUG_TRACE("Syscall PID %d: num=%d, args=(0x%llx, 0x%llx, 0x%llx)",
    //            process_get_current()->pid, (int)syscall_num, arg1, arg2, arg3);

    switch (syscall_num) {
        case SYS_READ:
            return sys_read(static_cast<int>(arg1), reinterpret_cast<char *>(arg2), arg3);
        case SYS_WRITE:
            return sys_write(static_cast<int>(arg1), reinterpret_cast<const char *>(arg2), arg3);
        case SYS_OPEN:
            return sys_open(reinterpret_cast<const char *>(arg1), (int)arg2, arg3);
        case SYS_SIGACTION:
            return sys_sigaction((int)arg1, reinterpret_cast<const struct sigaction *>(arg2),
                                 reinterpret_cast<struct sigaction *>(arg3));
        case SYS_SIGRETURN:
            // Minimal sigreturn: just yield and let the next signal or normal execution continue
            // In a real OS, this would restore the registers from the stack
            return 0;

        case SYS_CLOSE:
            return sys_close(static_cast<int>(arg1));
        case SYS_PIPE: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(int) * 2, true))
                return static_cast<uint64_t>(-1);
            Process *p = process_get_current();
            int fd1 = find_free_fd(p);
            if (fd1 < 0)
                return static_cast<uint64_t>(-1);
            p->fd_table[fd1].used = true;
            int fd2 = find_free_fd(p);
            if (fd2 < 0) {
                p->fd_table[fd1].used = false;
                return static_cast<uint64_t>(-1);
            }
            p->fd_table[fd2].used = true;
            int pipe_id = pipe_create();
            if (pipe_id < 0) {
                p->fd_table[fd1].used = false;
                p->fd_table[fd2].used = false;
                return static_cast<uint64_t>(-1);
            }
            p->fd_table[fd1].vnode = pipe_get_vnode(pipe_id, false);
            p->fd_table[fd1].flags = 0;
            p->fd_table[fd1].offset = 0;
            p->fd_table[fd1].dir_pos = 0;
            p->fd_table[fd2].vnode = pipe_get_vnode(pipe_id, true);
            p->fd_table[fd2].flags = 0;
            p->fd_table[fd2].offset = 0;
            p->fd_table[fd2].dir_pos = 0;
            STAC();
            reinterpret_cast<int *>(arg1)[0] = fd1;
            reinterpret_cast<int *>(arg1)[1] = fd2;
            CLAC();
            return 0;
        }
        case SYS_DUP2:
            return sys_dup2(static_cast<int>(arg1), static_cast<int>(arg2));
        case SYS_GETDENTS: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg2), 256, true))
                return static_cast<uint64_t>(-1);
            STAC();
            uint64_t ret = static_cast<uint64_t>(vfs_readdir(static_cast<int>(arg1), reinterpret_cast<char *>(arg2)));
            CLAC();
            return ret;
        }
        case SYS_GETPROCS:
            return sys_getprocs(reinterpret_cast<ProcessInfo *>(arg1), arg2);
        case SYS_GETPID: {
            Process *p = process_get_current();
            return p ? p->pid : 1;
        }
        case SYS_GETUID: {
            Process *p = process_get_current();
            return p ? p->uid : 0;
        }
        case SYS_YIELD:
            scheduler_yield();
            return 0;
        case SYS_SETUID: {
            Process *p = process_get_current();
            if (!p || p->uid != 0)
                return static_cast<uint64_t>(-1);
            p->uid = static_cast<uint32_t>(arg1);
            return 0;
        }
        case SYS_KILL:
            return sys_kill(arg1, (int)arg2);
        case SYS_FORK:
            return process_fork(frame);
        case SYS_EXIT:
            process_exit(static_cast<int32_t>(arg1));
            return 0;
        case SYS_REBOOT:
            system_reboot();
            return 0;
        case SYS_POWEROFF:
            system_poweroff();
            return 0;
        case SYS_SET_QUIET:
            return sys_set_quiet(arg1 != 0);
        case SYS_GETSYSINFO:
            return sys_getsysinfo(reinterpret_cast<SystemProfile *>(arg1));
        case SYS_MMAP: {
            size_t length = 0;
            if (!round_up_page_size(arg2, &length))
                return static_cast<uint64_t>(-1);

            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);
            size_t num_pages = length / 4096;

            uint64_t virt_start = 0x100000000ULL;

            // FIX: Restart loop if overlap is found to handle unsorted VMA linked list
            bool overlap_found;
            do {
                overlap_found = false;
                for (VMA *curr = p->vma_list; curr; curr = curr->next) {
                    uint64_t probe_end = 0;
                    if (!checked_add_u64(virt_start, length, &probe_end))
                        return static_cast<uint64_t>(-1);
                    // Check if [virt_start, virt_start + length) overlaps with curr
                    if (virt_start < curr->end && probe_end > curr->start) {
                        if (curr->end > UINT64_MAX - 0xFFFULL)
                            return static_cast<uint64_t>(-1);
                        virt_start = (curr->end + 0xFFFULL) & ~0xFFFULL;
                        overlap_found = true;
                        break; // Break and restart check from the new virt_start
                    }
                }
            } while (overlap_found);

            uint64_t virt_end = 0;
            if (!checked_add_u64(virt_start, length, &virt_end) || virt_end >= USER_STACK_TOP)
                return static_cast<uint64_t>(-1);

            uint64_t flags = PTE_PRESENT | PTE_USER;
            if (arg3 & 2)
                flags |= PTE_WRITABLE;

            uint64_t *target_pml4 = p->page_table ? p->page_table : vmm_get_kernel_pml4();
            for (size_t i = 0; i < num_pages; i++) {
                void *new_frame = pmm_alloc_frame();
                if (!new_frame) {
                    // Fail-forward: cleanup previously allocated pages in this call
                    for (size_t j = 0; j < i; j++) {
                        uint64_t vaddr = virt_start + (j * 4096);
                        uint64_t phys = vmm_virt_to_phys_in(target_pml4, vaddr);
                        vmm_unmap_page_in(target_pml4, vaddr);
                        if (phys)
                            pmm_free_frame(reinterpret_cast<void *>(phys));
                    }
                    return static_cast<uint64_t>(-1);
                }
                if (!vmm_map_page_in(target_pml4, virt_start + (i * 4096), reinterpret_cast<uint64_t>(new_frame), flags)
                         .ok()) {
                    pmm_free_frame(new_frame);
                    // Rollback as before
                    for (size_t j = 0; j < i; j++) {
                        uint64_t vaddr = virt_start + (j * 4096);
                        uint64_t phys = vmm_virt_to_phys_in(target_pml4, vaddr);
                        vmm_unmap_page_in(target_pml4, vaddr);
                        if (phys)
                            pmm_free_frame(reinterpret_cast<void *>(phys));
                    }
                    return static_cast<uint64_t>(-1);
                }
                kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(new_frame))),
                                     4096);
            }

            if (!vma_add(&p->vma_list, virt_start, virt_end, flags, VMAType::Data)) {
                // Cleanup on VMA failure
                for (size_t i = 0; i < num_pages; i++) {
                    uint64_t vaddr = virt_start + (i * 4096);
                    uint64_t phys = vmm_virt_to_phys_in(target_pml4, vaddr);
                    vmm_unmap_page_in(target_pml4, vaddr);
                    if (phys)
                        pmm_free_frame(reinterpret_cast<void *>(phys));
                }
                return static_cast<uint64_t>(-1);
            }

            asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
            return virt_start;
        }
        case SYS_MUNMAP: {
            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);
            return munmap_process_range(p, arg1, static_cast<size_t>(arg2)) ? 0 : static_cast<uint64_t>(-1);
        }
        case SYS_DISPLAY_GET_CAPS: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayCaps), true))
                return static_cast<uint64_t>(-1);
            DisplayCaps caps = {};
            if (!display_get_caps(&caps))
                return static_cast<uint64_t>(-1);
            STAC();
            *reinterpret_cast<DisplayCaps *>(arg1) = caps;
            CLAC();
            return 0;
        }
        case SYS_DISPLAY_GET_STATUS: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayStatus), true))
                return static_cast<uint64_t>(-1);
            DisplayStatus status = {};
            if (!display_get_status(&status))
                return static_cast<uint64_t>(-1);
            STAC();
            *reinterpret_cast<DisplayStatus *>(arg1) = status;
            CLAC();
            return 0;
        }
        case SYS_DISPLAY_QUERY_CONNECTORS: {
            size_t max_count = (size_t)arg2;
            size_t bytes = 0;
            if (!checked_mul_size(max_count, sizeof(DisplayConnectorInfo), &bytes))
                return static_cast<uint64_t>(-1);
            if (max_count != 0 && !validate_user_ptr(reinterpret_cast<void *>(arg1), bytes, true))
                return static_cast<uint64_t>(-1);

            DisplayConnectorInfo infos[4] = {};
            uint32_t copy_count = max_count > 4u ? 4u : (uint32_t)max_count;
            int count = display_query_connectors(infos, copy_count);
            if (count < 0)
                return static_cast<uint64_t>(-1);
            if (copy_count != 0) {
                uint32_t written = (uint32_t)count;
                if (written > copy_count)
                    written = copy_count;
                STAC();
                kstring::memcpy(reinterpret_cast<void *>(arg1), infos, (size_t)written * sizeof(DisplayConnectorInfo));
                CLAC();
            }
            return (uint64_t)count;
        }
        case SYS_DISPLAY_GET_MODES: {
            uint32_t connector_id = (uint32_t)arg1;
            size_t max_count = (size_t)arg3;
            size_t bytes = 0;
            if (!checked_mul_size(max_count, sizeof(DisplayMode), &bytes))
                return static_cast<uint64_t>(-1);
            if (max_count != 0 && !validate_user_ptr(reinterpret_cast<void *>(arg2), bytes, true))
                return static_cast<uint64_t>(-1);

            DisplayMode modes[32] = {};
            uint32_t copy_count = max_count > 32u ? 32u : (uint32_t)max_count;
            int count = display_get_modes(connector_id, modes, copy_count);
            if (count < 0)
                return static_cast<uint64_t>(-1);
            if (copy_count != 0) {
                uint32_t written = (uint32_t)count;
                if (written > copy_count)
                    written = copy_count;
                STAC();
                kstring::memcpy(reinterpret_cast<void *>(arg2), modes, (size_t)written * sizeof(DisplayMode));
                CLAC();
            }
            return (uint64_t)count;
        }
        case SYS_DISPLAY_SET_MODE: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayMode), false))
                return static_cast<uint64_t>(-1);
            DisplayMode mode = {};
            STAC();
            mode = *reinterpret_cast<const DisplayMode *>(arg1);
            CLAC();
            return display_set_mode(mode) ? 0 : static_cast<uint64_t>(-1);
        }
        case SYS_DISPLAY_ATOMIC_COMMIT: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayAtomicRequest), false))
                return static_cast<uint64_t>(-1);
            DisplayAtomicRequest request = {};
            STAC();
            request = *reinterpret_cast<const DisplayAtomicRequest *>(arg1);
            CLAC();
            return display_atomic_commit(request) ? 0 : static_cast<uint64_t>(-1);
        }
        case SYS_DISPLAY_BUFFER_CREATE: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayBufferCreate), true))
                return static_cast<uint64_t>(-1);

            DisplayBufferCreate request = {};
            STAC();
            request = *reinterpret_cast<const DisplayBufferCreate *>(arg1);
            CLAC();

            if (display_buffer_create(&request) != 0)
                return static_cast<uint64_t>(-1);

            STAC();
            *reinterpret_cast<DisplayBufferCreate *>(arg1) = request;
            CLAC();
            return 0;
        }
        case SYS_DISPLAY_BUFFER_MAP: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayBufferMap), true))
                return static_cast<uint64_t>(-1);

            DisplayBufferMap request = {};
            STAC();
            request = *reinterpret_cast<const DisplayBufferMap *>(arg1);
            CLAC();

            if (display_buffer_map(&request) != 0)
                return static_cast<uint64_t>(-1);

            STAC();
            *reinterpret_cast<DisplayBufferMap *>(arg1) = request;
            CLAC();
            return 0;
        }
        case SYS_DISPLAY_BUFFER_DESTROY:
            return display_buffer_destroy((DisplayBufferHandle)arg1) ? 0 : static_cast<uint64_t>(-1);
        case SYS_DISPLAY_BUFFER_SET_WM_ACCESS:
            return display_buffer_set_wm_access((DisplayBufferHandle)arg1, arg2 != 0) ? 0 : static_cast<uint64_t>(-1);
        case SYS_DISPLAY_SURFACE_IMPORT: {
            if (arg1 == 0 || arg2 == 0)
                return static_cast<uint64_t>(-1);
            if (!validate_user_ptr(reinterpret_cast<const void *>(arg1), sizeof(DisplaySurfaceImport), false) ||
                !validate_user_ptr(reinterpret_cast<void *>(arg2), sizeof(DisplaySurface), true)) {
                return static_cast<uint64_t>(-1);
            }
            Process *process = process_get_current();
            if (!process)
                return static_cast<uint64_t>(-1);

            DisplaySurfaceImport request = {};
            DisplaySurface surface = {};
            STAC();
            request = *reinterpret_cast<const DisplaySurfaceImport *>(arg1);
            CLAC();
            if (!display_import_surface(process->pid, request, &surface))
                return static_cast<uint64_t>(-1);
            STAC();
            *reinterpret_cast<DisplaySurface *>(arg2) = surface;
            CLAC();
            return 0;
        }
        case SYS_DISPLAY_PRESENT: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayPresentRequest), false))
                return static_cast<uint64_t>(-1);

            DisplayPresentRequest request = {};
            STAC();
            request = *reinterpret_cast<const DisplayPresentRequest *>(arg1);
            CLAC();

            if (request.rect_count > 128)
                return static_cast<uint64_t>(-1);

            DisplayCaps caps = {};
            if (!display_get_caps(&caps))
                return static_cast<uint64_t>(-1);

            Rect rects[128] = {};
            if (request.rect_count > 0) {
                size_t rect_bytes = (size_t)request.rect_count * sizeof(Rect);
                if (!validate_user_ptr(request.rects, rect_bytes, false))
                    return static_cast<uint64_t>(-1);
                STAC();
                kstring::memcpy(rects, request.rects, rect_bytes);
                CLAC();
                request.rects = rects;
            }

            if (!validate_display_buffer_access(request, caps.width, caps.height))
                return static_cast<uint64_t>(-1);

            STAC();
            uint32_t completed = display_present(request);
            CLAC();
            return completed;
        }
        case SYS_DISPLAY_COMPOSE_SUBMIT: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayComposeRequest), false))
                return static_cast<uint64_t>(-1);

            DisplayComposeRequest request = {};
            STAC();
            request = *reinterpret_cast<const DisplayComposeRequest *>(arg1);
            CLAC();

            if (!request.layers || request.layer_count == 0 || request.layer_count > 32u)
                return static_cast<uint64_t>(-1);
            if (!validate_user_ptr(request.layers, (size_t)request.layer_count * sizeof(DisplayComposeLayer), false))
                return static_cast<uint64_t>(-1);

            DisplayComposeLayer layers[32] = {};
            STAC();
            kstring::memcpy(layers, request.layers, (size_t)request.layer_count * sizeof(DisplayComposeLayer));
            CLAC();
            request.layers = layers;

            Rect damage_rects[32] = {};
            if (request.damage_rect_count > 32u)
                return static_cast<uint64_t>(-1);
            if (request.damage_rect_count > 0) {
                if (!request.damage_rects ||
                    !validate_user_ptr(request.damage_rects, (size_t)request.damage_rect_count * sizeof(Rect), false)) {
                    return static_cast<uint64_t>(-1);
                }
                STAC();
                kstring::memcpy(damage_rects, request.damage_rects, (size_t)request.damage_rect_count * sizeof(Rect));
                CLAC();
                request.damage_rects = damage_rects;
            }

            return display_compose_submit(request);
        }
        case SYS_DISPLAY_WAIT:
            return display_wait();
        case SYS_DISPLAY_EVENT_WAIT: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(DisplayEvent), true))
                return static_cast<uint64_t>(-1);

            DisplayEvent event = {};
            if (!display_event_wait(&event, arg2 != 0))
                return static_cast<uint64_t>(-1);

            STAC();
            *reinterpret_cast<DisplayEvent *>(arg1) = event;
            CLAC();
            return 0;
        }
        case SYS_FB_INFO: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), 16, true))
                return static_cast<uint64_t>(-1);
            extern const BootFramebuffer *g_framebuffer;
            if (!g_framebuffer)
                return static_cast<uint64_t>(-1);
            STAC();
            uint32_t *info = reinterpret_cast<uint32_t *>(arg1);
            info[0] = static_cast<uint32_t>(g_framebuffer->width);
            info[1] = static_cast<uint32_t>(g_framebuffer->height);
            info[2] = static_cast<uint32_t>(g_framebuffer->pitch);
            info[3] = static_cast<uint32_t>(g_framebuffer->bpp);
            CLAC();
            return 0;
        }
        case SYS_FB_MMAP: {
            extern const BootFramebuffer *g_framebuffer;
            if (!g_framebuffer)
                return static_cast<uint64_t>(-1);
            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);

            uint64_t virt_addr = reinterpret_cast<uint64_t>(g_framebuffer->address);
            size_t size = 0;
            if (!checked_mul_size((size_t)g_framebuffer->pitch, (size_t)g_framebuffer->height, &size) ||
                size > SIZE_MAX - 0xFFFULL)
                return static_cast<uint64_t>(-1);
            size_t num_pages = (size + 0xFFF) / 0x1000;

            uint64_t virt_start = 0x200000000ULL; // Dedicated region for FB mapping
            uint64_t flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_WC;

            for (size_t i = 0; i < num_pages; i++) {
                uint64_t phys = vmm_virt_to_phys(virt_addr + (i * 0x1000));
                if (phys) {
                    if (!vmm_map_page_in(p->page_table, virt_start + (i * 0x1000), phys, flags).ok()) {
                        // For FB mapping, failure is more critical but rollback is simpler (VMA not added yet)
                        // All we need is to stop and return error.
                        return static_cast<uint64_t>(-1);
                    }
                }
            }

            spinlock_acquire(&p->vma_lock);
            if (!vma_add(&p->vma_list, virt_start, virt_start + size, flags, VMAType::Shared)) {
                spinlock_release(&p->vma_lock);
                return static_cast<uint64_t>(-1);
            }
            spinlock_release(&p->vma_lock);

            asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
            return virt_start;
        }
        case SYS_FB_FLUSH: {
            // Flush Write-Combining buffers to ensure all pixels reach the screen
            asm volatile("sfence" ::: "memory");
            return 0;
        }
        case SYS_FB_BLIT: {
            if (!validate_user_ptr(reinterpret_cast<const void *>(arg1), static_cast<size_t>(arg2), false))
                return static_cast<uint64_t>(-1);

            extern const BootFramebuffer *g_framebuffer;
            if (!g_framebuffer)
                return static_cast<uint64_t>(-1);

            size_t size = static_cast<size_t>(arg2);
            size_t fb_size = 0;
            if (!checked_mul_size((size_t)g_framebuffer->pitch, (size_t)g_framebuffer->height, &fb_size))
                return static_cast<uint64_t>(-1);
            if (size > fb_size)
                size = fb_size;

            const uint8_t *src = reinterpret_cast<const uint8_t *>(arg1);
            uint8_t *dest = reinterpret_cast<uint8_t *>(g_framebuffer->address);

            STAC();
            // Pre-fault all source pages to avoid page faults during VRAM copy
            for (size_t i = 0; i < size; i += 4096) {
                volatile uint8_t dummy = src[i];
                (void)dummy;
            }
            if (size != 0) {
                volatile uint8_t dummy = src[size - 1];
                (void)dummy;
            }

            uint32_t height = static_cast<uint32_t>(g_framebuffer->height);
            uint32_t width = static_cast<uint32_t>(g_framebuffer->width);
            uint32_t pitch = static_cast<uint32_t>(g_framebuffer->pitch);
            const uint32_t *src32 = reinterpret_cast<const uint32_t *>(src);
            volatile uint32_t *dst32 = reinterpret_cast<volatile uint32_t *>(dest);
            uint32_t src_stride = pitch / 4;
            uint32_t dst_stride = pitch / 4;

            uint64_t irq_flags = interrupts_save_disable();
            for (uint32_t y = 0; y < height; y++) {
                const uint32_t *s = &src32[y * src_stride];
                volatile uint32_t *d = &dst32[y * dst_stride];
                for (uint32_t x = 0; x < width; x++) {
                    d[x] = s[x];
                }
            }
            interrupts_restore(irq_flags);
            CLAC();

            asm volatile("sfence" ::: "memory");
            return 0;
        }
        case SYS_FB_BLIT_RECT: {
            extern const BootFramebuffer *g_framebuffer;
            if (!g_framebuffer)
                return static_cast<uint64_t>(-1);

            uint32_t *src_buf = reinterpret_cast<uint32_t *>(arg1);
            uint32_t x = (uint32_t)arg2;
            uint32_t y = (uint32_t)arg3;
            uint32_t w = (uint32_t)frame->arg4;
            uint32_t h = (uint32_t)frame->arg5;
            uint32_t stride = (uint32_t)frame->arg6;

            if (stride == 0)
                stride = w;

            // Bounds check
            if (x >= g_framebuffer->width || y >= g_framebuffer->height)
                return 0;
            if ((uint64_t)x + w > g_framebuffer->width)
                w = (uint32_t)(g_framebuffer->width - x);
            if ((uint64_t)y + h > g_framebuffer->height)
                h = (uint32_t)(g_framebuffer->height - y);

            if (w == 0 || h == 0)
                return 0;
            if (stride < w)
                return static_cast<uint64_t>(-1); // Prevent buffer over-read

            // Validate user buffer access - use the actual accessed range
            size_t last_row_words = 0;
            size_t last_row_bytes = 0;
            size_t row_bytes = 0;
            size_t access_size = 0;
            if (!checked_mul_size((size_t)(h - 1), (size_t)stride, &last_row_words) ||
                !checked_mul_size(last_row_words, sizeof(uint32_t), &last_row_bytes) ||
                !checked_mul_size((size_t)w, sizeof(uint32_t), &row_bytes) ||
                !checked_add_size(last_row_bytes, row_bytes, &access_size)) {
                return static_cast<uint64_t>(-1);
            }
            if (!validate_user_ptr(src_buf, access_size, false)) {
                DEBUG_ERROR("FB_BLIT_RECT: Validation failed! src=%p w=%u h=%u stride=%u", src_buf, w, h, stride);
                return static_cast<uint64_t>(-1);
            }

            // Final blit implementation (v13 SSE2)

            uint32_t fb_stride = g_framebuffer->pitch / 4;
            uint32_t *dest_buf = reinterpret_cast<uint32_t *>(g_framebuffer->address);

            STAC();
            // Pre-fault the source buffer to avoid take faults during the blit loop
            for (size_t i = 0; i < access_size; i += 4096) {
                volatile uint32_t dummy = ((volatile uint32_t *)src_buf)[i / 4];
                (void)dummy;
            }

            // Ensure userspace writes to the backbuffer are globally visible before we read them
            asm volatile("sfence" ::: "memory");

            // Copy whole rows instead of per-pixel stores; this is the hot path for cursor
            // motion and hover effects, especially on high-resolution framebuffers.
            for (uint32_t py = 0; py < h; py++) {
                uint32_t *d_row = &dest_buf[(y + py) * fb_stride + x];
                uint32_t *s_row = &src_buf[py * stride];
                kstring::memcpy(d_row, s_row, (size_t)w * 4);
            }
            CLAC();

            // Minimal post-blit safety
            asm volatile("sfence" ::: "memory");
            return 0;
        }
        case SYS_EXEC:
            return static_cast<uint64_t>(do_exec(reinterpret_cast<const char *>(arg1), frame));
        case SYS_WAIT4: {
            if (arg2 != 0 && !validate_user_ptr(reinterpret_cast<void *>(arg2), sizeof(int32_t), true))
                return static_cast<uint64_t>(-1);
            int32_t status = 0;
            int64_t waited = process_waitpid(static_cast<int64_t>(arg1), arg2 != 0 ? &status : nullptr);
            if (waited >= 0 && arg2 != 0) {
                STAC();
                *reinterpret_cast<int32_t *>(arg2) = status;
                CLAC();
            }
            return static_cast<uint64_t>(waited);
        }
        case SYS_GETTIME: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(SysTime), true))
                return static_cast<uint64_t>(-1);
            RTCTime rtc;
            rtc_get_time(&rtc);
            SysTime out_time = {};
            out_time.year = rtc.year;
            out_time.month = rtc.month;
            out_time.day = rtc.day;
            out_time.hour = rtc.hour;
            out_time.minute = rtc.minute;
            out_time.second = rtc.second;
            STAC();
            *reinterpret_cast<SysTime *>(arg1) = out_time;
            CLAC();
            return 0;
        }
        case SYS_GET_TSC_FREQ: {
            return 2000;
        }
        case SYS_GETRANDOM: {
            size_t len = static_cast<size_t>(arg2);
            if (len == 0)
                return 0;
            if (len > 65536 || !validate_user_ptr(reinterpret_cast<void *>(arg1), len, true))
                return static_cast<uint64_t>(-1);

            STAC();
            auto *out = reinterpret_cast<uint8_t *>(arg1);
            size_t pos = 0;
            while (pos < len) {
                uint64_t r = random_next_u64();
                size_t chunk = len - pos;
                if (chunk > sizeof(r))
                    chunk = sizeof(r);
                for (size_t i = 0; i < chunk; i++) {
                    const unsigned shift = static_cast<unsigned>(i * 8u);
                    out[pos + i] = static_cast<uint8_t>((r >> shift) & 0xFFu);
                }
                pos += chunk;
            }
            CLAC();
            return static_cast<uint64_t>(len);
        }
        case SYS_GETUPTIME:
            return timer_get_ticks() / (timer_get_frequency() ? timer_get_frequency() : 1);
        case SYS_GET_TICKS:
            return timer_get_ticks();
        case SYS_GET_EVENT: {
            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);

            while (true) {
                pump_events();
                Event ev = {};
                if (event_poll(p->event_queue, ev)) {
                    if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(Event), true))
                        return static_cast<uint64_t>(-1);
                    STAC();
                    *reinterpret_cast<Event *>(arg1) = ev;
                    CLAC();
                    return 1;
                }

                if (arg2 == 0)
                    return 0; // Non-blocking

                const uint64_t irq_flags = interrupts_save_disable();
                if (event_poll(p->event_queue, ev)) {
                    interrupts_restore(irq_flags);
                    if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(Event), true))
                        return static_cast<uint64_t>(-1);
                    STAC();
                    *reinterpret_cast<Event *>(arg1) = ev;
                    CLAC();
                    return 1;
                }
                scheduler_wait(&p->event_wait_queue, nullptr);
                interrupts_restore(irq_flags);
            }
        }
        case SYS_POST_EVENT: {
            Process *target = process_find_by_pid(arg1);
            if (!target)
                return static_cast<uint64_t>(-1);
            if (!validate_user_ptr(reinterpret_cast<void *>(arg2), sizeof(Event), false))
                return static_cast<uint64_t>(-1);

            Event ev;
            STAC();
            ev = *reinterpret_cast<Event *>(arg2);
            CLAC();

            event_push(target->event_queue, ev);
            scheduler_wake_all(&target->event_wait_queue);
            return 0;
        }
        case SYS_GUI_REGISTER_WM: {
            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);
            gui_set_wm_pid(p->pid);
            return 0;
        }
        case SYS_GUI_SET_FOCUS: {
            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);
            uint64_t wm_pid = gui_get_wm_pid();
            if (wm_pid != 0 && p->pid != wm_pid)
                return static_cast<uint64_t>(-1);
            if (arg1 == 0) {
                gui_set_focus_pid(0);
                return 0;
            }
            Process *target = process_find_by_pid(arg1);
            if (!target)
                return static_cast<uint64_t>(-1);
            gui_set_focus_pid(target->pid);
            return 0;
        }
        case SYS_GETMEMINFO: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(MemInfo), true))
                return static_cast<uint64_t>(-1);
            MemInfo info = {};
            info.total_kb = pmm_get_total_memory() / 1024;
            info.free_kb = pmm_get_free_memory() / 1024;
            info.used_kb = info.total_kb - info.free_kb;
            info.heap_total_kb = 0;
            info.heap_used_kb = 0;
            STAC();
            *reinterpret_cast<MemInfo *>(arg1) = info;
            CLAC();
            return 0;
        }

        case SYS_SOUND_PLAY: {
            char k_path[512];
            if (!copy_string_from_user(reinterpret_cast<const char *>(arg1), k_path, 511))
                return static_cast<uint64_t>(-1);
            if (kstring::strstr(k_path, ".wav"))
                sound_play_wav_file(k_path);
            else if (kstring::strstr(k_path, ".mp3"))
                sound_play_mp3_file(k_path);
            else
                sound_play_pcm_file(k_path);
            return 0;
        }
        case SYS_SOUND_WRITE: {
            if (!validate_user_ptr(reinterpret_cast<const void *>(arg1), static_cast<size_t>(arg2), false))
                return static_cast<uint64_t>(-1);
            sound_play(reinterpret_cast<uint8_t *>(const_cast<void *>(reinterpret_cast<const void *>(arg1))),
                       static_cast<uint32_t>(arg2));
            return 0;
        }
        case SYS_SOUND_CONFIG: {
            // arg1: sample_rate, arg2: channels, arg3: bits_per_sample
            if (arg1 > 0)
                sound_set_sample_rate(static_cast<uint32_t>(arg1));
            if (arg2 > 0)
                sound_set_channels(static_cast<uint8_t>(arg2));
            if (arg3 > 0)
                sound_set_bits_per_sample(static_cast<uint8_t>(arg3));
            return 0;
        }
        case SYS_STAT: {
            char k_path[512];
            if (!copy_string_from_user(reinterpret_cast<const char *>(arg1), k_path, 511))
                return static_cast<uint64_t>(-1);
            if (!validate_user_ptr(reinterpret_cast<void *>(arg2), sizeof(VNodeStat), true))
                return static_cast<uint64_t>(-1);
            Process *p = process_get_current();
            char resolved[512];
            vfs_resolve_relative_path(p->cwd, k_path, resolved);
            VNodeStat st = {};
            int res = vfs_stat(resolved, &st);
            if (res == 0) {
                STAC();
                *reinterpret_cast<VNodeStat *>(arg2) = st;
                CLAC();
            }
            return static_cast<uint64_t>(res);
        }
        case SYS_MKDIR: {
            char k_path[512];
            if (!copy_string_from_user(reinterpret_cast<const char *>(arg1), k_path, 511))
                return static_cast<uint64_t>(-1);
            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);
            char resolved[512];
            vfs_resolve_relative_path(p->cwd, k_path, resolved);
            return static_cast<uint64_t>(vfs_mkdir(resolved));
        }
        case SYS_UNLINK: {
            char k_path[512];
            if (!copy_string_from_user(reinterpret_cast<const char *>(arg1), k_path, 511))
                return static_cast<uint64_t>(-1);
            Process *p = process_get_current();
            if (!p || k_path[0] == '\0')
                return static_cast<uint64_t>(-1);
            char resolved[512];
            vfs_resolve_relative_path(p->cwd, k_path, resolved);
            if (resolved[0] == '\0' || (resolved[0] == '/' && resolved[1] == '\0'))
                return static_cast<uint64_t>(-1);
            return static_cast<uint64_t>(vfs_unlink(resolved));
        }
        case SYS_RMDIR: {
            char k_path[512];
            if (!copy_string_from_user(reinterpret_cast<const char *>(arg1), k_path, 511))
                return static_cast<uint64_t>(-1);
            Process *p = process_get_current();
            if (!p || k_path[0] == '\0')
                return static_cast<uint64_t>(-1);
            char resolved[512];
            vfs_resolve_relative_path(p->cwd, k_path, resolved);
            if (resolved[0] == '\0' || (resolved[0] == '/' && resolved[1] == '\0'))
                return static_cast<uint64_t>(-1);
            return static_cast<uint64_t>(vfs_rmdir(resolved));
        }
        case SYS_RENAME: {
            char old_path[512];
            char new_path[512];
            if (!copy_string_from_user(reinterpret_cast<const char *>(arg1), old_path, 511) ||
                !copy_string_from_user(reinterpret_cast<const char *>(arg2), new_path, 511))
                return static_cast<uint64_t>(-1);
            Process *p = process_get_current();
            if (!p || old_path[0] == '\0' || new_path[0] == '\0')
                return static_cast<uint64_t>(-1);
            char resolved_old[512];
            char resolved_new[512];
            vfs_resolve_relative_path(p->cwd, old_path, resolved_old);
            vfs_resolve_relative_path(p->cwd, new_path, resolved_new);
            return static_cast<uint64_t>(vfs_rename(resolved_old, resolved_new));
        }
        case SYS_GETVOLUMES: {
            if (arg2 > 16)
                arg2 = 16;
            if (arg1 == 0 || arg2 == 0)
                return 0;
            if (!validate_user_ptr(reinterpret_cast<void *>(arg1), sizeof(VolumeInfo) * static_cast<size_t>(arg2),
                                   true))
                return static_cast<uint64_t>(-1);
            auto *volumes = static_cast<VolumeInfo *>(malloc(sizeof(VolumeInfo) * static_cast<size_t>(arg2)));
            if (!volumes)
                return static_cast<uint64_t>(-1);
            int count = volume_list(volumes, static_cast<int>(arg2));
            STAC();
            for (int i = 0; i < count; i++)
                reinterpret_cast<VolumeInfo *>(arg1)[i] = volumes[i];
            CLAC();
            free(volumes);
            return static_cast<uint64_t>(count);
        }
        case SYS_STORAGE_GET_MODE:
            return storage_get_mode();
        case SYS_STORAGE_SET_MODE: {
            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);
            uint64_t wm_pid = gui_get_wm_pid();
            if (wm_pid == 0 || p->pid != wm_pid)
                return static_cast<uint64_t>(-1);
            storage_set_mode(static_cast<uint32_t>(arg1));
            return 0;
        }
        case SYS_SOCKET: {
            constexpr uint64_t AF_INET_NUM = 2;
            constexpr uint64_t SOCK_STREAM_NUM = 1;
            constexpr uint64_t SOCK_DGRAM_NUM = 2;

            if (arg1 != 0 && arg1 != AF_INET_NUM)
                return static_cast<uint64_t>(-1);

            if (arg2 == SOCK_STREAM_NUM) {
                int sock = tcp_socket();
                return sock >= 0 ? static_cast<uint64_t>(socket_make_handle(SOCKET_KIND_TCP, sock))
                                 : static_cast<uint64_t>(-1);
            }

            if (arg2 == 0 || arg2 == SOCK_DGRAM_NUM) {
                int sock = udp_socket();
                return sock >= 0 ? static_cast<uint64_t>(socket_make_handle(SOCKET_KIND_UDP, sock))
                                 : static_cast<uint64_t>(-1);
            }

            return static_cast<uint64_t>(-1);
        }
        case SYS_BIND: {
            int kind = 0;
            int sock = -1;
            if (!socket_decode_handle(static_cast<int>(arg1), &kind, &sock))
                return static_cast<uint64_t>(-1);
            bool ok = false;
            if (kind == SOCKET_KIND_UDP)
                ok = udp_bind(sock, static_cast<uint16_t>(arg2));
            else if (kind == SOCKET_KIND_TCP)
                ok = tcp_bind(sock, static_cast<uint16_t>(arg2));
            return ok ? 0 : static_cast<uint64_t>(-1);
        }
        case SYS_SENDTO: {
            // arg1: sock, arg2: buf, arg3: len, arg4: dst_ip, arg5: dst_port
            if (arg3 > UINT16_MAX)
                return static_cast<uint64_t>(-1);
            if (!validate_user_ptr(reinterpret_cast<const void *>(arg2), arg3, false))
                return static_cast<uint64_t>(-1);
            int kind = 0;
            int sock = -1;
            if (!socket_decode_handle(static_cast<int>(arg1), &kind, &sock) || kind != SOCKET_KIND_UDP)
                return static_cast<uint64_t>(-1);
            STAC();
            bool ok = udp_sendto(sock, static_cast<uint32_t>(frame->arg4), static_cast<uint16_t>(frame->arg5),
                                 reinterpret_cast<const void *>(arg2), static_cast<uint16_t>(arg3));
            CLAC();
            return ok ? arg3 : static_cast<uint64_t>(-1);
        }
        case SYS_RECVFROM: {
            // arg1: sock, arg2: buf, arg3: len, arg4: src_ip_ptr, arg5: src_port_ptr
            if (arg3 > UINT16_MAX)
                arg3 = UINT16_MAX;
            if (!validate_user_ptr(reinterpret_cast<void *>(arg2), arg3, true))
                return static_cast<uint64_t>(-1);
            int kind = 0;
            int sock = -1;
            if (!socket_decode_handle(static_cast<int>(arg1), &kind, &sock) || kind != SOCKET_KIND_UDP)
                return static_cast<uint64_t>(-1);
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            int r = udp_recvfrom(sock, reinterpret_cast<void *>(arg2), static_cast<uint16_t>(arg3), &src_ip, &src_port);
            if (r >= 0) {
                if (frame->arg4 && validate_user_ptr(reinterpret_cast<void *>(frame->arg4), 4, true)) {
                    STAC();
                    *reinterpret_cast<uint32_t *>(frame->arg4) = src_ip;
                    CLAC();
                }
                if (frame->arg5 && validate_user_ptr(reinterpret_cast<void *>(frame->arg5), 2, true)) {
                    STAC();
                    *reinterpret_cast<uint16_t *>(frame->arg5) = src_port;
                    CLAC();
                }
            }
            return static_cast<uint64_t>(r);
        }
        case SYS_CONNECT: {
            int kind = 0;
            int sock = -1;
            if (!socket_decode_handle(static_cast<int>(arg1), &kind, &sock) || kind != SOCKET_KIND_TCP)
                return static_cast<uint64_t>(-1);
            return tcp_connect(sock, static_cast<uint32_t>(arg2), static_cast<uint16_t>(arg3))
                       ? 0
                       : static_cast<uint64_t>(-1);
        }
        case SYS_SEND: {
            if (!validate_user_ptr(reinterpret_cast<const void *>(arg2), arg3, false))
                return static_cast<uint64_t>(-1);
            int kind = 0;
            int sock = -1;
            if (!socket_decode_handle(static_cast<int>(arg1), &kind, &sock) || kind != SOCKET_KIND_TCP)
                return static_cast<uint64_t>(-1);
            uint16_t len = arg3 > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(arg3);
            STAC();
            int r = tcp_send(sock, reinterpret_cast<const void *>(arg2), len);
            CLAC();
            return static_cast<uint64_t>(r);
        }
        case SYS_RECV: {
            if (!validate_user_ptr(reinterpret_cast<void *>(arg2), arg3, true))
                return static_cast<uint64_t>(-1);
            int kind = 0;
            int sock = -1;
            if (!socket_decode_handle(static_cast<int>(arg1), &kind, &sock) || kind != SOCKET_KIND_TCP)
                return static_cast<uint64_t>(-1);
            uint16_t len = arg3 > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(arg3);
            STAC();
            int r = tcp_recv(sock, reinterpret_cast<void *>(arg2), len);
            CLAC();
            return static_cast<uint64_t>(r);
        }
        case SYS_CLOSESOCKET: {
            int kind = 0;
            int sock = -1;
            if (!socket_decode_handle(static_cast<int>(arg1), &kind, &sock))
                return static_cast<uint64_t>(-1);
            if (kind == SOCKET_KIND_UDP)
                udp_close(sock);
            else if (kind == SOCKET_KIND_TCP)
                tcp_close(sock);
            return 0;
        }
        case SYS_RESOLVE: {
            char hostname[DNS_MAX_NAME_LEN];
            if (!copy_string_from_user(reinterpret_cast<const char *>(arg1), hostname, DNS_MAX_NAME_LEN - 1))
                return static_cast<uint64_t>(-1);
            if (!validate_user_ptr(reinterpret_cast<void *>(arg2), sizeof(uint32_t), true))
                return static_cast<uint64_t>(-1);
            uint32_t ip = dns_resolve(hostname);
            if (ip == 0)
                return static_cast<uint64_t>(-1);
            STAC();
            *reinterpret_cast<uint32_t *>(arg2) = ip;
            CLAC();
            return 0;
        }
        case SYS_SHM_GET: {
            size_t size = 0;
            if (!round_up_page_size(arg1, &size))
                return static_cast<uint64_t>(-1);
            // Limit size to 16MB to fit within our fixed mapping slots
            if (size == 0 || size > 0x1000000)
                return static_cast<uint64_t>(-1);

            spinlock_acquire(&g_shm_lock);
            int id = -1;
            for (int i = 0; i < 64; i++) {
                if (!g_shm_blocks[i].used) {
                    id = i;
                    break;
                }
            }
            if (id == -1) {
                spinlock_release(&g_shm_lock);
                return static_cast<uint64_t>(-1);
            }

            size_t num_pages = size / 4096;
            void *phys_ptr = pmm_alloc_frames(num_pages);
            if (!phys_ptr) {
                spinlock_release(&g_shm_lock);
                return static_cast<uint64_t>(-1);
            }

            uint64_t phys_start = reinterpret_cast<uint64_t>(phys_ptr);
            kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(phys_start)), size);

            g_shm_blocks[id].phys_addr = phys_start;
            g_shm_blocks[id].size = size;
            g_shm_blocks[id].owner_pid = process_get_current() ? process_get_current()->pid : 0;
            g_shm_blocks[id].used = true;
            spinlock_release(&g_shm_lock);
            return static_cast<uint64_t>(id);
        }
        case SYS_SHM_MAP: {
            int id = (int)arg1;
            if (id < 0 || id >= 64)
                return static_cast<uint64_t>(-1);

            Process *p = process_get_current();
            if (!p)
                return static_cast<uint64_t>(-1);

            spinlock_acquire(&g_shm_lock);
            if (!g_shm_blocks[id].used) {
                spinlock_release(&g_shm_lock);
                return static_cast<uint64_t>(-1);
            }

            uint64_t phys_start = g_shm_blocks[id].phys_addr;
            size_t size = g_shm_blocks[id].size;
            spinlock_release(&g_shm_lock);

            // Fixed-offset mapping for SHM to avoid VMA list walking races
            uint64_t virt_start = SHM_BASE + (uint64_t)((uint32_t)id) * SHM_SLOT_SIZE;

            spinlock_acquire(&p->vma_lock);
            VMA *existing = vma_find(p->vma_list, virt_start);
            if (existing) {
                bool same_mapping = existing->start == virt_start && existing->end >= virt_start + size &&
                                    (existing->flags & PTE_SHARED);
                spinlock_release(&p->vma_lock);
                return same_mapping ? virt_start : static_cast<uint64_t>(-1);
            }
            spinlock_release(&p->vma_lock);

            uint64_t flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_SHARED;
            uint64_t mapped = 0;
            for (; mapped < size; mapped += 4096) {
                pmm_refcount_inc(reinterpret_cast<void *>(phys_start + mapped));
                if (!vmm_map_page_in(p->page_table, virt_start + mapped, phys_start + mapped, flags).ok()) {
                    pmm_refcount_dec(reinterpret_cast<void *>(phys_start + mapped));
                    for (uint64_t rollback = 0; rollback < mapped; rollback += 4096) {
                        vmm_unmap_page_in(p->page_table, virt_start + rollback);
                        pmm_refcount_dec(reinterpret_cast<void *>(phys_start + rollback));
                    }
                    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
                    return static_cast<uint64_t>(-1);
                }
            }

            asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
            spinlock_acquire(&p->vma_lock);
            bool vma_ok = vma_add(&p->vma_list, virt_start, virt_start + size, flags, VMAType::Shared) != nullptr;
            spinlock_release(&p->vma_lock);
            if (!vma_ok) {
                // Rollback: Unmap and Dec Refcounts
                for (uint64_t i = 0; i < size; i += 4096) {
                    vmm_unmap_page_in(p->page_table, virt_start + i);
                    pmm_refcount_dec(reinterpret_cast<void *>(phys_start + i));
                }
                asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
                return static_cast<uint64_t>(-1);
            }

            return virt_start;
        }
        case SYS_SHM_FREE: {
            Process *current = process_get_current();
            shm_unmap_from_process(current, (int)arg1);
            return shm_release_owner_reference(current, (int)arg1) ? 0 : static_cast<uint64_t>(-1);
        }
        case SYS_SHM_INFO: {
            int id = (int)arg1;
            if (id < 0 || id >= 64)
                return static_cast<uint64_t>(-1);
            spinlock_acquire(&g_shm_lock);
            uint64_t size = g_shm_blocks[id].used ? g_shm_blocks[id].size : (uint64_t)-1;
            spinlock_release(&g_shm_lock);
            return size;
        }
        case SYS_SHM_UNMAP:
            return shm_unmap_from_process(process_get_current(), (int)arg1) ? 0 : static_cast<uint64_t>(-1);
        default:
            DEBUG_WARN("Unknown syscall: %d", syscall_num);
            return static_cast<uint64_t>(-1);
    }
}
