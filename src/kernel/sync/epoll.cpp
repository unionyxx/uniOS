#include <kernel/sync/epoll.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/pipe.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/event.h>
#include <kernel/sync/spinlock.h>
#include <kernel/mm/heap.h>
#include <kernel/debug.h>
#include <kernel/cpu.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

#define STAC() \
    do { \
        if (g_cpu_features.has_smap) \
            asm volatile("stac" ::: "memory"); \
    } while (0)

#define CLAC() \
    do { \
        if (g_cpu_features.has_smap) \
            asm volatile("clac" ::: "memory"); \
    } while (0)

struct EpollItem {
    int fd;
    uint32_t events;
    epoll_data_t data;
    EpollItem *next;
};

struct EpollInstance {
    Spinlock lock;
    EpollItem *items;
};

static void epoll_vnode_close(VNode *node)
{
    if (!node || !node->fs_data)
        return;

    EpollInstance *inst = static_cast<EpollInstance *>(node->fs_data);
    uint64_t flags = spinlock_acquire_irqsave(&inst->lock);

    EpollItem *curr = inst->items;
    while (curr) {
        EpollItem *next = curr->next;
        free(curr);
        curr = next;
    }

    spinlock_release_irqrestore(&inst->lock, flags);
    free(inst);
    node->fs_data = nullptr;
}

static VNodeOps epoll_ops = {
    .read = nullptr,
    .write = nullptr,
    .readdir = nullptr,
    .lookup = nullptr,
    .create = nullptr,
    .mkdir = nullptr,
    .unlink = nullptr,
    .rename = nullptr,
    .truncate = nullptr,
    .sync = nullptr,
    .close = epoll_vnode_close
};

int64_t sys_epoll_create(int size)
{
    if (size <= 0)
        return -22; // EINVAL

    Process *p = process_get_current();
    if (!p)
        return -1;

    EpollInstance *inst = static_cast<EpollInstance *>(malloc(sizeof(EpollInstance)));
    if (!inst)
        return -12; // ENOMEM

    spinlock_init(&inst->lock);
    inst->items = nullptr;

    static uint64_t epoll_inode_counter = 0xF0000000;
    VNode *node = vfs_create_vnode(__sync_fetch_and_add(&epoll_inode_counter, 1), 0, false, &epoll_ops, inst);
    if (!node) {
        free(inst);
        return -12; // ENOMEM
    }

    uint64_t flags = spinlock_acquire_irqsave(&p->fd_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!p->fd_table[i].used) {
            p->fd_table[i].used = true;
            p->fd_table[i].flags = 0;
            kstring::zero_memory(p->fd_table[i].reserved, sizeof(p->fd_table[i].reserved));
            p->fd_table[i].vnode = node;
            p->fd_table[i].offset = 0;
            p->fd_table[i].dir_pos = 0;
            spinlock_release_irqrestore(&p->fd_lock, flags);
            return i;
        }
    }
    spinlock_release_irqrestore(&p->fd_lock, flags);

    vfs_close_vnode(node);
    return -24; // EMFILE
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    Process *p = process_get_current();
    if (!p)
        return -1;

    uint64_t flags = spinlock_acquire_irqsave(&p->fd_lock);
    if (epfd < 0 || epfd >= MAX_OPEN_FILES || !p->fd_table[epfd].used) {
        spinlock_release_irqrestore(&p->fd_lock, flags);
        return -9; // EBADF
    }
    VNode *ep_vnode = p->fd_table[epfd].vnode;
    if (!ep_vnode || ep_vnode->ops != &epoll_ops) {
        spinlock_release_irqrestore(&p->fd_lock, flags);
        return -22; // EINVAL: Not an epoll file descriptor
    }
    EpollInstance *inst = static_cast<EpollInstance *>(ep_vnode->fs_data);

    if (fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used) {
        spinlock_release_irqrestore(&p->fd_lock, flags);
        return -9; // EBADF
    }
    spinlock_release_irqrestore(&p->fd_lock, flags);

    struct epoll_event local_event = {0, {0}};
    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (!event)
            return -14; // EFAULT
    }

    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        STAC();
        local_event = *event;
        CLAC();
    }

    uint64_t inst_flags = spinlock_acquire_irqsave(&inst->lock);

    if (op == EPOLL_CTL_ADD) {
        for (EpollItem *curr = inst->items; curr; curr = curr->next) {
            if (curr->fd == fd) {
                spinlock_release_irqrestore(&inst->lock, inst_flags);
                return -17; // EEXIST
            }
        }

        EpollItem *item = static_cast<EpollItem *>(malloc(sizeof(EpollItem)));
        if (!item) {
            spinlock_release_irqrestore(&inst->lock, inst_flags);
            return -12; // ENOMEM
        }
        item->fd = fd;
        item->events = local_event.events;
        item->data = local_event.data;
        item->next = inst->items;
        inst->items = item;

        spinlock_release_irqrestore(&inst->lock, inst_flags);
        return 0;
    }
    else if (op == EPOLL_CTL_MOD) {
        for (EpollItem *curr = inst->items; curr; curr = curr->next) {
            if (curr->fd == fd) {
                curr->events = local_event.events;
                curr->data = local_event.data;
                spinlock_release_irqrestore(&inst->lock, inst_flags);
                return 0;
            }
        }
        spinlock_release_irqrestore(&inst->lock, inst_flags);
        return -2; // ENOENT
    }
    else if (op == EPOLL_CTL_DEL) {
        EpollItem **link = &inst->items;
        while (*link) {
            if ((*link)->fd == fd) {
                EpollItem *target = *link;
                *link = target->next;
                free(target);
                spinlock_release_irqrestore(&inst->lock, inst_flags);
                return 0;
            }
            link = &(*link)->next;
        }
        spinlock_release_irqrestore(&inst->lock, inst_flags);
        return -2; // ENOENT
    }

    spinlock_release_irqrestore(&inst->lock, inst_flags);
    return -22; // EINVAL: Invalid operation
}

int64_t sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    if (maxevents <= 0)
        return -22; // EINVAL
    if (!events)
        return -14; // EFAULT

    Process *current = process_get_current();
    if (!current)
        return -1;

    // Validate epfd
    uint64_t flags = spinlock_acquire_irqsave(&current->fd_lock);
    if (epfd < 0 || epfd >= MAX_OPEN_FILES || !current->fd_table[epfd].used) {
        spinlock_release_irqrestore(&current->fd_lock, flags);
        return -9; // EBADF
    }
    VNode *ep_vnode = current->fd_table[epfd].vnode;
    if (!ep_vnode || ep_vnode->ops != &epoll_ops) {
        spinlock_release_irqrestore(&current->fd_lock, flags);
        return -22; // EINVAL
    }
    EpollInstance *inst = static_cast<EpollInstance *>(ep_vnode->fs_data);
    spinlock_release_irqrestore(&current->fd_lock, flags);

    uint64_t start_ticks = timer_get_ticks();
    int num_ready = 0;

    while (true) {
        uint64_t inst_flags = spinlock_acquire_irqsave(&inst->lock);
        EpollItem *curr = inst->items;
        while (curr && num_ready < maxevents) {
            uint32_t occurred = 0;
            bool ready = false;

            uint64_t fd_flags = spinlock_acquire_irqsave(&current->fd_lock);
            if (curr->fd >= 0 && curr->fd < MAX_OPEN_FILES && current->fd_table[curr->fd].used) {
                VNode *vnode = current->fd_table[curr->fd].vnode;
                if (pipe_is_pipe(vnode)) {
                    ready = pipe_is_ready(vnode, curr->events, &occurred);
                } else {
                    occurred = (EPOLLIN | EPOLLOUT) & curr->events;
                    ready = (occurred != 0);
                }
            }
            spinlock_release_irqrestore(&current->fd_lock, fd_flags);

            if (ready) {
                STAC();
                events[num_ready].events = occurred;
                events[num_ready].data = curr->data;
                CLAC();
                num_ready++;
            }
            curr = curr->next;
        }
        spinlock_release_irqrestore(&inst->lock, inst_flags);

        if (num_ready > 0 || timeout == 0 || !event_empty(current->event_queue)) {
            break;
        }

        if (timeout > 0) {
            uint64_t ticks_passed = timer_get_ticks() - start_ticks;
            uint64_t ticks_to_wait = (static_cast<uint64_t>(timeout) * timer_get_frequency()) / 1000;
            if (ticks_passed >= ticks_to_wait) {
                break;
            }
        }

        uint64_t inst_flags2 = spinlock_acquire_irqsave(&inst->lock);

        bool quick_ready = false;
        EpollItem *scan = inst->items;
        uint64_t fd_flags2 = spinlock_acquire_irqsave(&current->fd_lock);
        while (scan) {
            if (scan->fd >= 0 && scan->fd < MAX_OPEN_FILES && current->fd_table[scan->fd].used) {
                VNode *vnode = current->fd_table[scan->fd].vnode;
                uint32_t occ = 0;
                if (pipe_is_pipe(vnode)) {
                    if (pipe_is_ready(vnode, scan->events, &occ)) {
                        quick_ready = true;
                        break;
                    }
                } else {
                    if (((EPOLLIN | EPOLLOUT) & scan->events) != 0) {
                        quick_ready = true;
                        break;
                    }
                }
            }
            scan = scan->next;
        }
        spinlock_release_irqrestore(&current->fd_lock, fd_flags2);

        if (quick_ready) {
            spinlock_release_irqrestore(&inst->lock, inst_flags2);
            continue;
        }

        scheduler_wait(&g_epoll_wait_queue, &inst->lock);
        // scheduler_wait re-acquires the lock raw before returning, so we must release it raw
        // so that the next loop iteration's spinlock_acquire_irqsave succeeds.
        spinlock_release(&inst->lock);
    }

    return num_ready;
}
