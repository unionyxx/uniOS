#include <kernel/debug.h>
#include <kernel/fs/storage_guard.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/syscall.h>
#include <libk/kstd.h>
#include <libk/kstring.h>

using kstd::KBuffer;
using kstd::unique_ptr;
using kstring::string_view;

static Mount *g_mount_list = nullptr;
static Spinlock g_mount_lock = SPINLOCK_INIT;

static Mount *vfs_find_best_mount_locked(string_view path)
{
    Mount *best_mount = nullptr;
    size_t max_len = 0;

    for (Mount *curr = g_mount_list; curr; curr = curr->next) {
        string_view m_path(curr->path);
        if (!path.starts_with(m_path))
            continue;

        size_t len = m_path.size();
        if (path.size() != len && path[len] != '/' && !(len == 1 && m_path[0] == '/'))
            continue;

        if (len >= max_len) {
            max_len = len;
            best_mount = curr;
        }
    }

    return best_mount;
}

static bool vfs_path_is_storage_guarded(const char *path)
{
    if (!path || path[0] != '/')
        return false;

    spinlock_acquire(&g_mount_lock);
    Mount *mount = vfs_find_best_mount_locked(string_view(path));
    bool guarded = mount && (mount->flags & VFS_MOUNT_FLAG_STORAGE_GUARDED) != 0;
    spinlock_release(&g_mount_lock);
    return guarded;
}

void vfs_init()
{
}

// Permission bits
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

static constexpr uint16_t VFS_DEFAULT_FILE_MODE = 0644;
static constexpr uint16_t VFS_DEFAULT_DIR_MODE = 0755;
static constexpr uint16_t VFS_USER_STORAGE_DIR_MODE = 0777;

static uint16_t vfs_sanitize_file_mode(uint16_t mode)
{
    uint16_t sanitized = mode & 0777;
    if ((sanitized & S_IRUSR) == 0 || (sanitized & S_IWUSR) == 0)
        return VFS_DEFAULT_FILE_MODE;
    return sanitized;
}

static bool vfs_check_permission(VNode *node, int mask)
{
    Process *p = process_get_current();
    if (!p)
        return true; // Kernel-level tasks often have no process struct initially
    if (p->uid == 0)
        return true; // Root bypass

    int mode = node->mode;
    if (node->uid == p->uid) {
        // Owner checks
        if ((mask & 4) && !(mode & S_IRUSR))
            return false;
        if ((mask & 2) && !(mode & S_IWUSR))
            return false;
        if ((mask & 1) && !(mode & S_IXUSR))
            return false;
        return true;
    }

    // Other checks (no groups yet)
    if ((mask & 4) && !(mode & S_IROTH))
        return false;
    if ((mask & 2) && !(mode & S_IWOTH))
        return false;
    if ((mask & 1) && !(mode & S_IXOTH))
        return false;
    return true;
}

[[nodiscard]] VNode *vfs_create_vnode(uint64_t inode_id, uint64_t size, bool is_dir, VNodeOps *ops, void *fs_data)
{
    VNode *node = static_cast<VNode *>(malloc(sizeof(VNode)));
    if (!node)
        return nullptr;

    node->inode_id = inode_id;
    node->size = size;
    node->uid = 0; // Default to root, filesystem drivers should override
    node->mode = is_dir ? VFS_DEFAULT_DIR_MODE : VFS_DEFAULT_FILE_MODE;
    node->is_dir = is_dir;
    node->ops = ops;
    node->fs_data = fs_data;
    node->ref_count = 1;
    return node;
}

int vfs_mount_ex(const char *path, VNode *root, uint32_t flags)
{
    if (!path || !root)
        return -1;

    Mount *m = static_cast<Mount *>(malloc(sizeof(Mount)));
    if (!m)
        return -1;

    kstring::strncpy(m->path, path, 63);
    m->path[63] = '\0';
    m->root = root;
    m->flags = flags;

    if ((flags & VFS_MOUNT_FLAG_STORAGE_GUARDED) != 0 && root->is_dir) {
        // Storage-mode guarded mounts, such as /data, already require the user
        // to switch storage into writable mode before writes are allowed. Do not
        // additionally make the mount root unwritable to regular app UIDs just
        // because the VNode default owner is root.
        root->mode = VFS_USER_STORAGE_DIR_MODE;
    }

    spinlock_acquire(&g_mount_lock);
    m->next = g_mount_list;
    g_mount_list = m;
    spinlock_release(&g_mount_lock);

    DEBUG_INFO("vfs: mounted filesystem at %s", path);
    return 0;
}

int vfs_mount(const char *path, VNode *root)
{
    return vfs_mount_ex(path, root, 0);
}

[[nodiscard]] static VNode *vfs_resolve_path(string_view path, const char **out_rel_path)
{
    if (path.empty() || path[0] != '/')
        return nullptr;

    spinlock_acquire(&g_mount_lock);
    Mount *best_mount = vfs_find_best_mount_locked(path);
    spinlock_release(&g_mount_lock);

    if (!best_mount)
        return nullptr;

    size_t max_len = kstring::strlen(best_mount->path);

    const char *rel_ptr = path.data() + max_len;
    while (*rel_ptr == '/')
        rel_ptr++;

    if (out_rel_path)
        *out_rel_path = rel_ptr;
    return best_mount->root;
}

void vfs_resolve_relative_path(const char *cwd, const char *path, char *out)
{
    char temp_path[512];
    string_view p_view(path);
    const char *base_cwd = (cwd && cwd[0] != '\0') ? cwd : "/";

    if (p_view.starts_with("/")) {
        kstring::strncpy(temp_path, path, 511);
    } else {
        kstring::strncpy(temp_path, base_cwd, 511);
        size_t cwd_len = kstring::strlen(temp_path);
        if (cwd_len == 0) {
            kstring::strncpy(temp_path, "/", 511);
            cwd_len = 1;
        }
        if (temp_path[cwd_len - 1] != '/') {
            kstring::strncat(temp_path, "/", 511 - kstring::strlen(temp_path));
        }
        kstring::strncat(temp_path, path, 511 - kstring::strlen(temp_path));
    }

    char *segments[256];
    int depth = 0;
    char copy[512];
    kstring::strncpy(copy, temp_path, 511);

    char *tok = copy;
    if (*tok == '/')
        tok++;

    for (char *p = tok;; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';

            if (kstring::strcmp(tok, "..") == 0) {
                if (depth > 0)
                    depth--;
            } else if (kstring::strcmp(tok, ".") != 0 && tok[0] != '\0') {
                if (depth < 256)
                    segments[depth++] = tok;
            }

            if (saved == '\0')
                break;
            tok = p + 1;
            p = tok;
        }
    }

    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        kstring::strncat(out, "/", 511 - kstring::strlen(out));
        kstring::strncat(out, segments[i], 511 - kstring::strlen(out));
    }
    if (out[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
    }
}

[[nodiscard]] VNode *vfs_lookup_vnode(const char *path)
{
    const char *rel_path = nullptr;
    VNode *root = vfs_resolve_path(path, &rel_path);
    if (!root)
        return nullptr;

    if (*rel_path == '\0') {
        __sync_fetch_and_add(&root->ref_count, 1);
        return root;
    }

    if (!root->ops->lookup)
        return nullptr;

    VNode *current = root;
    __sync_fetch_and_add(&current->ref_count, 1);

    char path_copy[512];
    kstring::strncpy(path_copy, rel_path, 511);

    char *name = path_copy;
    char *next = nullptr;

    while (name) {
        next = nullptr;
        for (char *p = name; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                next = p + 1;
                break;
            }
        }

        if (name[0] != '\0') {
            if (!vfs_check_permission(current, 1)) { // X permission needed for lookup
                vfs_close_vnode(current);
                return nullptr;
            }
            VNode *next_node = current->ops->lookup(current, name);
            vfs_close_vnode(current);
            if (!next_node)
                return nullptr;
            current = next_node;
        }
        name = next;
    }

    return current;
}

void vfs_sync()
{
    spinlock_acquire(&g_mount_lock);
    for (Mount *m = g_mount_list; m; m = m->next) {
        if (m->root && m->root->ops->sync) {
            m->root->ops->sync(m->root);
        }
    }
    spinlock_release(&g_mount_lock);
}

void vfs_close_vnode(VNode *node)
{
    if (!node)
        return;

    if (__sync_sub_and_fetch(&node->ref_count, 1) == 0) {
        bool is_mount_root = false;
        spinlock_acquire(&g_mount_lock);
        for (Mount *m = g_mount_list; m; m = m->next) {
            if (m->root == node) {
                is_mount_root = true;
                break;
            }
        }
        spinlock_release(&g_mount_lock);

        if (!is_mount_root) {
            if (node->ops->close)
                node->ops->close(node);
            free(node);
        }
    }
}

Mount *vfs_get_mounts()
{
    return g_mount_list;
}

int vfs_open(const char *path, int flags, uint16_t mode)
{
    bool write_like = (flags & (O_WRONLY | O_RDWR)) != 0;
    bool mutating_open = write_like || (flags & (O_CREAT | O_TRUNC)) != 0;
    bool guarded_mount = vfs_path_is_storage_guarded(path);
    if (guarded_mount && !storage_reads_allowed())
        return -1;
    if (mutating_open && guarded_mount && !storage_writes_allowed())
        return -1;

    VNode *node = vfs_lookup_vnode(path);

    if (!node && (flags & O_CREAT)) {
        char parent_path[512];
        kstring::strncpy(parent_path, path, 511);

        char *last_slash = nullptr;
        for (char *p = parent_path; *p; p++) {
            if (*p == '/')
                last_slash = p;
        }

        if (last_slash) {
            const char *name = last_slash + 1;
            const char *lookup_path = (last_slash == parent_path) ? "/" : parent_path;
            if (last_slash != parent_path)
                *last_slash = '\0';

            VNode *parent = vfs_lookup_vnode(lookup_path);
            if (parent) {
                if (vfs_check_permission(parent, 2)) { // W permission needed for create
                    if (parent->ops->create && parent->ops->create(parent, name) == 0) {
                        node = parent->ops->lookup(parent, name);
                        if (node) {
                            Process *p = process_get_current();
                            if (p) {
                                node->uid = p->uid;
                                node->mode = vfs_sanitize_file_mode(mode);
                            }
                        }
                    }
                }
                vfs_close_vnode(parent);
            }
        }
    }

    if (!node)
        return -1;

    // Check permissions for open
    int mask = 0;
    if (flags & O_RDONLY || flags & O_RDWR)
        mask |= 4;
    if (flags & O_WRONLY || flags & O_RDWR)
        mask |= 2;
    if (!vfs_check_permission(node, mask)) {
        vfs_close_vnode(node);
        return -1;
    }

    if (node->is_dir && (flags & (O_WRONLY | O_RDWR))) {
        vfs_close_vnode(node);
        return -1;
    }

    if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR))) {
        if (node->ops->truncate) {
            node->ops->truncate(node, 0);
        }
    }

    Process *p = process_get_current();
    if (!p) {
        vfs_close_vnode(node);
        return -1;
    }

    spinlock_acquire(&p->fd_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!p->fd_table[i].used) {
            p->fd_table[i].used = true;
            p->fd_table[i].flags = guarded_mount ? FD_FLAG_STORAGE_GUARDED : 0;
            if (guarded_mount && write_like)
                p->fd_table[i].flags |= FD_FLAG_STORAGE_GUARDED_WRITE;
            kstring::zero_memory(p->fd_table[i].reserved, sizeof(p->fd_table[i].reserved));
            p->fd_table[i].vnode = node;
            p->fd_table[i].offset = (flags & O_APPEND) ? node->size : 0;
            p->fd_table[i].dir_pos = 0;
            spinlock_release(&p->fd_lock);
            return i;
        }
    }
    spinlock_release(&p->fd_lock);

    vfs_close_vnode(node);
    return -1;
}

int vfs_close(int fd)
{
    Process *p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES)
        return -1;

    spinlock_acquire(&p->fd_lock);
    if (!p->fd_table[fd].used) {
        spinlock_release(&p->fd_lock);
        return -1;
    }

    VNode *node = p->fd_table[fd].vnode;
    p->fd_table[fd].used = false;
    p->fd_table[fd].flags = 0;
    p->fd_table[fd].vnode = nullptr;
    spinlock_release(&p->fd_lock);

    vfs_close_vnode(node);
    return 0;
}

int64_t vfs_read(int fd, void *buf, uint64_t size)
{
    Process *p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used)
        return -1;
    FileDescriptor *f = &p->fd_table[fd];
    if (!f->vnode->ops->read)
        return -1;
    if ((f->flags & FD_FLAG_STORAGE_GUARDED) != 0 && !storage_reads_allowed())
        return -1;

    int64_t res = f->vnode->ops->read(f->vnode, buf, size, f->offset, f);
    if (res > 0)
        f->offset += static_cast<uint64_t>(res);
    return res;
}

int64_t vfs_write(int fd, const void *buf, uint64_t size)
{
    Process *p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used)
        return -1;
    FileDescriptor *f = &p->fd_table[fd];
    if (!f->vnode->ops->write)
        return -1;
    if ((f->flags & FD_FLAG_STORAGE_GUARDED_WRITE) != 0 && !storage_writes_allowed())
        return -1;

    int64_t res = f->vnode->ops->write(f->vnode, buf, size, f->offset, f);
    if (res > 0)
        f->offset += static_cast<uint64_t>(res);
    return res;
}

int64_t vfs_seek(int fd, int64_t offset, int whence)
{
    Process *p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used)
        return -1;
    FileDescriptor *f = &p->fd_table[fd];

    int64_t new_offset = (int64_t)f->offset;
    if (whence == SEEK_SET)
        new_offset = offset;
    else if (whence == SEEK_CUR)
        new_offset += offset;
    else if (whence == SEEK_END)
        new_offset = (int64_t)f->vnode->size + offset;

    if (new_offset < 0)
        return -1;
    f->offset = static_cast<uint64_t>(new_offset);
    return (int64_t)f->offset;
}

int vfs_readdir(int fd, char *name_out)
{
    Process *p = process_get_current();
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd].used)
        return -1;
    FileDescriptor *f = &p->fd_table[fd];
    if (!f->vnode->ops->readdir)
        return -1;
    if ((f->flags & FD_FLAG_STORAGE_GUARDED) != 0 && !storage_reads_allowed())
        return -1;

    uint64_t pos = f->dir_pos;
    int res = f->vnode->ops->readdir(f->vnode, pos, name_out);

    // DEBUG_TRACE("vfs_readdir: fd %d, pos %llu -> res %d, name '%s'", fd, pos, res, res == 0 ? name_out : "");

    if (res == 0)
        f->dir_pos++;
    return res;
}

int vfs_stat(const char *path, VNodeStat *out)
{
    if (vfs_path_is_storage_guarded(path) && !storage_reads_allowed())
        return -1;
    VNode *node = vfs_lookup_vnode(path);
    if (!node)
        return -1;

    out->inode = node->inode_id;
    out->size = node->size;
    out->uid = node->uid;
    out->mode = node->mode;
    out->is_dir = node->is_dir;

    vfs_close_vnode(node);
    return 0;
}

int vfs_mkdir(const char *path)
{
    if (vfs_path_is_storage_guarded(path) && !storage_writes_allowed())
        return -1;

    char parent_path[512];
    kstring::strncpy(parent_path, path, 511);

    char *last_slash = nullptr;
    for (char *p = parent_path; *p; p++) {
        if (*p == '/')
            last_slash = p;
    }

    if (!last_slash)
        return -1;

    const char *name = last_slash + 1;
    const char *lookup_path = (last_slash == parent_path) ? "/" : parent_path;
    if (last_slash != parent_path)
        *last_slash = '\0';

    VNode *parent = vfs_lookup_vnode(lookup_path);
    int res = -1;
    if (parent) {
        if (vfs_check_permission(parent, 2)) { // W permission needed
            if (parent->ops->mkdir) {
                res = parent->ops->mkdir(parent, name);
                if (res == 0) {
                    VNode *node = parent->ops->lookup(parent, name);
                    if (node) {
                        Process *p = process_get_current();
                        if (p) {
                            node->uid = p->uid;
                            node->mode = 0755;
                        }
                        vfs_close_vnode(node);
                    }
                }
            }
        }
        vfs_close_vnode(parent);
    }
    return res;
}

int vfs_unlink(const char *path)
{
    if (!path || path[0] == '\0' || kstring::strcmp(path, "/") == 0)
        return -1;
    if (vfs_path_is_storage_guarded(path) && !storage_writes_allowed())
        return -1;

    VNode *target = vfs_lookup_vnode(path);
    if (!target)
        return -1;
    bool is_dir = target->is_dir;
    vfs_close_vnode(target);
    if (is_dir)
        return -1;

    char parent_path[512];
    kstring::strncpy(parent_path, path, 511);

    char *last_slash = nullptr;
    for (char *p = parent_path; *p; p++) {
        if (*p == '/')
            last_slash = p;
    }

    if (!last_slash)
        return -1;

    const char *name = last_slash + 1;
    if (!name || name[0] == '\0')
        return -1;
    const char *lookup_path = (last_slash == parent_path) ? "/" : parent_path;
    if (last_slash != parent_path)
        *last_slash = '\0';

    VNode *parent = vfs_lookup_vnode(lookup_path);
    int res = -1;
    if (parent) {
        if (parent->ops && vfs_check_permission(parent, 2)) { // W permission needed
            if (parent->ops->unlink)
                res = parent->ops->unlink(parent, name);
        }
        vfs_close_vnode(parent);
    }
    return res;
}

int vfs_rmdir(const char *path)
{
    if (!path || path[0] == '\0' || kstring::strcmp(path, "/") == 0)
        return -1;
    if (vfs_path_is_storage_guarded(path) && !storage_writes_allowed())
        return -1;

    VNode *target = vfs_lookup_vnode(path);
    if (!target)
        return -1;
    bool is_dir = target->is_dir;
    vfs_close_vnode(target);
    if (!is_dir)
        return -1;

    char parent_path[512];
    kstring::strncpy(parent_path, path, 511);

    char *last_slash = nullptr;
    for (char *p = parent_path; *p; p++) {
        if (*p == '/')
            last_slash = p;
    }

    if (!last_slash)
        return -1;

    const char *name = last_slash + 1;
    if (!name || name[0] == '\0')
        return -1;
    const char *lookup_path = (last_slash == parent_path) ? "/" : parent_path;
    if (last_slash != parent_path)
        *last_slash = '\0';

    VNode *parent = vfs_lookup_vnode(lookup_path);
    int res = -1;
    if (parent) {
        if (parent->ops && vfs_check_permission(parent, 2) && parent->ops->unlink)
            res = parent->ops->unlink(parent, name);
        vfs_close_vnode(parent);
    }
    return res;
}

int vfs_rename(const char *oldpath, const char *newpath)
{
    if ((vfs_path_is_storage_guarded(oldpath) || vfs_path_is_storage_guarded(newpath)) && !storage_writes_allowed())
        return -1;

    char old_parent_path[512], new_parent_path[512];
    kstring::strncpy(old_parent_path, oldpath, 511);
    kstring::strncpy(new_parent_path, newpath, 511);

    char *old_name = nullptr, *new_name = nullptr;
    for (char *p = old_parent_path; *p; p++)
        if (*p == '/')
            old_name = p;
    for (char *p = new_parent_path; *p; p++)
        if (*p == '/')
            new_name = p;

    if (!old_name || !new_name)
        return -1;

    const char *old_lookup = (old_name == old_parent_path) ? "/" : old_parent_path;
    const char *new_lookup = (new_name == new_parent_path) ? "/" : new_parent_path;

    *old_name++ = '\0';
    *new_name++ = '\0';

    VNode *old_parent = vfs_lookup_vnode(old_lookup);
    VNode *new_parent = vfs_lookup_vnode(new_lookup);

    int res = -1;
    if (old_parent && new_parent) {
        if (vfs_check_permission(old_parent, 2) && vfs_check_permission(new_parent, 2)) {
            if (old_parent->ops->rename && old_parent->ops == new_parent->ops) {
                res = old_parent->ops->rename(old_parent, old_name, new_parent, new_name);
            }
        }
    }

    if (old_parent)
        vfs_close_vnode(old_parent);
    if (new_parent)
        vfs_close_vnode(new_parent);
    return res;
}

bool is_file_open(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    char resolved[512];
    vfs_resolve_relative_path("/", path, resolved);
    VNode *target = vfs_lookup_vnode(resolved);
    if (!target)
        return false;

    bool found = false;
    // Iterate all processes
    Process *start = scheduler_get_process_list();
    if (start) {
        Process *curr = start;
        do {
            for (int i = 0; i < MAX_OPEN_FILES; i++) {
                if (curr->fd_table[i].used && curr->fd_table[i].vnode == target) {
                    found = true;
                    break;
                }
            }
            if (found)
                break;
            curr = curr->next;
        } while (curr != start);
    }

    vfs_close_vnode(target);
    return found;
}
