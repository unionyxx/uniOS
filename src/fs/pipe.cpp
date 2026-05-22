#include <kernel/debug.h>
#include <kernel/fs/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <stddef.h>

static int64_t pipe_vfs_read(VNode *node, void *buf, uint64_t size, uint64_t, FileDescriptor *)
{
    return pipe_read((int)(uintptr_t)node->fs_data, (char *)buf, size);
}

static int64_t pipe_vfs_write(VNode *node, const void *buf, uint64_t size, uint64_t, FileDescriptor *)
{
    return pipe_write((int)(uintptr_t)node->fs_data, (const char *)buf, size);
}

static void pipe_vfs_close(VNode *node)
{
    int pipe_id = (int)(uintptr_t)node->fs_data;
    if (node->inode_id == 0) {
        pipe_close_read(pipe_id);
    } else {
        pipe_close_write(pipe_id);
    }
}

static VNodeOps pipe_ops = {.read = pipe_vfs_read,
                            .write = pipe_vfs_write,
                            .readdir = nullptr,
                            .lookup = nullptr,
                            .create = nullptr,
                            .mkdir = nullptr,
                            .unlink = nullptr,
                            .rename = nullptr,
                            .truncate = nullptr,
                            .sync = nullptr,
                            .close = pipe_vfs_close};

VNode *pipe_get_vnode(int pipe_id, bool is_write)
{
    return vfs_create_vnode(is_write ? 1 : 0, 0, false, &pipe_ops, (void *)(uintptr_t)pipe_id);
}

struct PipeInternal : public Pipe
{
    Spinlock lock;
};

static PipeInternal pipes[MAX_PIPES];
static bool pipes_initialized = false;
static int g_pipe_debug_reads = 0;
static int g_pipe_debug_writes = 0;

void pipe_init()
{
    if (pipes_initialized)
        return;
    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].in_use = false;
        pipes[i].lock = SPINLOCK_INIT;
    }
    pipes_initialized = true;
}

int pipe_create()
{
    pipe_init();

    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) {
            pipes[i].in_use = true;
            pipes[i].read_pos = 0;
            pipes[i].write_pos = 0;
            pipes[i].count = 0;
            pipes[i].write_closed = false;
            pipes[i].read_closed = false;
            pipes[i].data_wait = {nullptr, nullptr};
            pipes[i].space_wait = {nullptr, nullptr};
            return i;
        }
    }
    return -1;
}

int64_t pipe_read(int pipe_id, char *buf, uint64_t count)
{
    if (pipe_id < 0 || pipe_id >= MAX_PIPES || !pipes[pipe_id].in_use) {
        return -1;
    }

    PipeInternal *p = &pipes[pipe_id];

    uint64_t flags = spinlock_acquire_irqsave(&p->lock);
    while (true) {
        if (p->count > 0) {
            uint64_t to_read = (count < p->count) ? count : p->count;
            for (uint64_t i = 0; i < to_read; i++) {
                buf[i] = (char)p->buffer[p->read_pos];
                p->read_pos = (p->read_pos + 1) % PIPE_BUFFER_SIZE;
            }
            p->count -= to_read;

            if (g_pipe_debug_reads < 8) {
                DEBUG_INFO("pipe_read id=%d bytes=%llu first=0x%02x remain=%llu", pipe_id, to_read,
                           (unsigned char)buf[0], p->count);
                g_pipe_debug_reads++;
            }

            scheduler_wake_all(&p->space_wait);
            spinlock_release_irqrestore(&p->lock, flags);
            return (int64_t)to_read;
        }

        if (p->write_closed) {
            spinlock_release_irqrestore(&p->lock, flags);
            return 0; // EOF
        }

        scheduler_wait(&p->data_wait, &p->lock);
    }
}

int64_t pipe_write(int pipe_id, const char *buf, uint64_t count)
{
    if (pipe_id < 0 || pipe_id >= MAX_PIPES || !pipes[pipe_id].in_use) {
        return -1;
    }

    PipeInternal *p = &pipes[pipe_id];

    uint64_t flags = spinlock_acquire_irqsave(&p->lock);
    while (true) {
        if (p->read_closed) {
            spinlock_release_irqrestore(&p->lock, flags);
            return -1; // Broken pipe
        }

        uint64_t space = PIPE_BUFFER_SIZE - p->count;
        if (space > 0) {
            uint64_t to_write = (count < space) ? count : space;
            for (uint64_t i = 0; i < to_write; i++) {
                p->buffer[p->write_pos] = (uint8_t)buf[i];
                p->write_pos = (p->write_pos + 1) % PIPE_BUFFER_SIZE;
            }
            p->count += to_write;

            if (g_pipe_debug_writes < 8) {
                DEBUG_INFO("pipe_write id=%d bytes=%llu first=0x%02x used=%llu", pipe_id, to_write,
                           (unsigned char)buf[0], p->count);
                g_pipe_debug_writes++;
            }

            scheduler_wake_all(&p->data_wait);
            spinlock_release_irqrestore(&p->lock, flags);
            return (int64_t)to_write;
        }

        scheduler_wait(&p->space_wait, &p->lock);
    }
}

void pipe_close_read(int pipe_id)
{
    if (pipe_id < 0 || pipe_id >= MAX_PIPES)
        return;
    PipeInternal *p = &pipes[pipe_id];

    uint64_t flags = spinlock_acquire_irqsave(&p->lock);
    p->read_closed = true;
    scheduler_wake_all(&p->space_wait);

    if (p->write_closed) {
        p->in_use = false;
    }
    spinlock_release_irqrestore(&p->lock, flags);
}

void pipe_close_write(int pipe_id)
{
    if (pipe_id < 0 || pipe_id >= MAX_PIPES)
        return;
    PipeInternal *p = &pipes[pipe_id];

    uint64_t flags = spinlock_acquire_irqsave(&p->lock);
    p->write_closed = true;
    scheduler_wake_all(&p->data_wait);

    if (p->read_closed) {
        p->in_use = false;
    }
    spinlock_release_irqrestore(&p->lock, flags);
}

bool pipe_is_pipe(VNode *node)
{
    return node && node->ops == &pipe_ops;
}

bool pipe_is_ready(VNode *node, uint32_t events, uint32_t *out_occurred)
{
    if (!node || node->ops != &pipe_ops)
        return false;
    int pipe_id = (int)(uintptr_t)node->fs_data;
    if (pipe_id < 0 || pipe_id >= MAX_PIPES || !pipes[pipe_id].in_use)
        return false;

    PipeInternal *p = &pipes[pipe_id];
    uint64_t flags = spinlock_acquire_irqsave(&p->lock);

    uint32_t occurred = 0;
    if (node->inode_id == 0) {
        if (p->count > 0)
            occurred |= EPOLLIN;
        if (p->write_closed)
            occurred |= EPOLLIN | EPOLLHUP;
    } else {
        if (PIPE_BUFFER_SIZE - p->count > 0 && !p->read_closed)
            occurred |= EPOLLOUT;
        if (p->read_closed)
            occurred |= EPOLLERR | EPOLLHUP;
    }

    spinlock_release_irqrestore(&p->lock, flags);

    uint32_t active = occurred & events;
    if (active) {
        if (out_occurred)
            *out_occurred = active;
        return true;
    }
    return false;
}

