#include <kernel/fs/vfs.h>
#include <kernel/syscall.h>
#include <kernel/mm/heap.h>
#include <libk/kstring.h>
#include <libk/kstd.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>

using kstring::string_view;
using kstd::unique_ptr;
using kstd::KBuffer;

static Mount* g_mount_list = nullptr;
static Spinlock g_mount_lock = SPINLOCK_INIT;
static FileDescriptor g_fd_table[MAX_VFS_FDS];

void vfs_init() {
    for (auto& fd : g_fd_table) {
        fd.used = false;
        fd.vnode = nullptr;
        fd.offset = 0;
        fd.dir_pos = 0;
    }
}

[[nodiscard]] VNode* vfs_create_vnode(uint64_t inode_id, uint64_t size, bool is_dir, VNodeOps* ops, void* fs_data) {
    VNode* node = static_cast<VNode*>(malloc(sizeof(VNode)));
    if (!node) return nullptr;
    
    node->inode_id = inode_id;
    node->size = size;
    node->is_dir = is_dir;
    node->ops = ops;
    node->fs_data = fs_data;
    node->ref_count = 1;
    return node;
}

int vfs_mount(const char* path, VNode* root) {
    if (!path || !root) return -1;
    
    Mount* m = static_cast<Mount*>(malloc(sizeof(Mount)));
    if (!m) return -1;
    
    kstring::strncpy(m->path, path, 63);
    m->path[63] = '\0';
    m->root = root;
    
    spinlock_acquire(&g_mount_lock);
    m->next = g_mount_list;
    g_mount_list = m;
    spinlock_release(&g_mount_lock);
    
    DEBUG_INFO("VFS: Mounted filesystem at %s", path);
    return 0;
}

[[nodiscard]] static VNode* vfs_resolve_path(string_view path, const char** out_rel_path) {
    if (path.empty() || path[0] != '/') return nullptr;
    
    Mount* best_mount = nullptr;
    size_t max_len = 0;
    
    spinlock_acquire(&g_mount_lock);
    for (Mount* curr = g_mount_list; curr; curr = curr->next) {
        string_view m_path(curr->path);
        if (path.starts_with(m_path)) {
            size_t len = m_path.size();
            if (path.size() == len || path[len] == '/' || (len == 1 && m_path[0] == '/')) {
                if (len >= max_len) {
                    max_len = len;
                    best_mount = curr;
                }
            }
        }
    }
    spinlock_release(&g_mount_lock);
    
    if (!best_mount) return nullptr;
    
    const char* rel_ptr = path.data() + max_len;
    while (*rel_ptr == '/') rel_ptr++;
    
    if (out_rel_path) *out_rel_path = rel_ptr;
    return best_mount->root;
}

void vfs_resolve_relative_path(const char* cwd, const char* path, char* out) {
    char temp_path[512];
    string_view p_view(path);

    if (p_view.starts_with("/")) {
        kstring::strncpy(temp_path, path, 511);
    } else {
        kstring::strncpy(temp_path, cwd, 511);
        if (temp_path[kstring::strlen(temp_path) - 1] != '/') {
            kstring::strncat(temp_path, "/", 511 - kstring::strlen(temp_path));
        }
        kstring::strncat(temp_path, path, 511 - kstring::strlen(temp_path));
    }
    
    char* segments[64];
    int depth = 0;
    char copy[512];
    kstring::strncpy(copy, temp_path, 511);

    char* tok = copy;
    if (*tok == '/') tok++;
    
    for (char* p = tok; ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            
            if (kstring::strcmp(tok, "..") == 0) {
                if (depth > 0) depth--;
            } else if (kstring::strcmp(tok, ".") != 0 && tok[0] != '\0') {
                if (depth < 64) segments[depth++] = tok;
            }
            
            if (saved == '\0') break;
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

[[nodiscard]] VNode* vfs_lookup_vnode(const char* path) {
    const char* rel_path = nullptr;
    VNode* root = vfs_resolve_path(path, &rel_path);
    if (!root) return nullptr;
    
    if (*rel_path == '\0') {
        root->ref_count++;
        return root;
    }
    
    if (!root->ops->lookup) return nullptr;
    
    VNode* current = root;
    current->ref_count++;
    
    char path_copy[512];
    kstring::strncpy(path_copy, rel_path, 511);
    
    char* name = path_copy;
    char* next = nullptr;
    
    while (name) {
        next = nullptr;
        for (char* p = name; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                next = p + 1;
                break;
            }
        }
        
        if (name[0] != '\0') {
            VNode* next_node = current->ops->lookup(current, name);
            vfs_close_vnode(current);
            if (!next_node) return nullptr;
            current = next_node;
        }
        name = next;
    }
    
    return current;
}

void vfs_close_vnode(VNode* node) {
    if (!node) return;
    
    if (--node->ref_count == 0) {
        bool is_mount_root = false;
        spinlock_acquire(&g_mount_lock);
        for (Mount* m = g_mount_list; m; m = m->next) {
            if (m->root == node) { is_mount_root = true; break; }
        }
        spinlock_release(&g_mount_lock);

        if (!is_mount_root) {
            if (node->ops->close) node->ops->close(node);
            free(node);
        }
    }
}

Mount* vfs_get_mounts() {
    return g_mount_list;
}

int vfs_open(const char* path, int flags) {
    VNode* node = vfs_lookup_vnode(path);
    
    if (!node && (flags & O_CREAT)) {
        char parent_path[512];
        kstring::strncpy(parent_path, path, 511);
        
        char* last_slash = nullptr;
        for (char* p = parent_path; *p; p++) {
            if (*p == '/') last_slash = p;
        }
        
        if (last_slash) {
            const char* name = last_slash + 1;
            const char* lookup_path = (last_slash == parent_path) ? "/" : parent_path;
            if (last_slash != parent_path) *last_slash = '\0';
            
            VNode* parent = vfs_lookup_vnode(lookup_path);
            if (parent) {
                if (parent->ops->create && parent->ops->create(parent, name) == 0) {
                    node = parent->ops->lookup(parent, name);
                }
                vfs_close_vnode(parent);
            }
        }
    }
    
    if (!node) return -1;
    if (node->is_dir && (flags & (O_WRONLY | O_RDWR))) {
        vfs_close_vnode(node);
        return -1;
    }

    for (int i = 0; i < MAX_VFS_FDS; i++) {
        if (!g_fd_table[i].used) {
            g_fd_table[i].used = true;
            g_fd_table[i].vnode = node;
            g_fd_table[i].offset = (flags & O_APPEND) ? node->size : 0;
            g_fd_table[i].dir_pos = 0;
            g_fd_table[i].last_cluster = 0;
            g_fd_table[i].last_offset = 0;
            return i;
        }
    }
    
    vfs_close_vnode(node);
    return -1;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !g_fd_table[fd].used) return -1;
    vfs_close_vnode(g_fd_table[fd].vnode);
    g_fd_table[fd].used = false;
    return 0;
}

int64_t vfs_read(int fd, void* buf, uint64_t size) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !g_fd_table[fd].used) return -1;
    FileDescriptor* f = &g_fd_table[fd];
    if (!f->vnode->ops->read) return -1;
    
    int64_t res = f->vnode->ops->read(f->vnode, buf, size, f->offset, f);
    if (res > 0) f->offset += res;
    return res;
}

int64_t vfs_write(int fd, const void* buf, uint64_t size) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !g_fd_table[fd].used) return -1;
    FileDescriptor* f = &g_fd_table[fd];
    if (!f->vnode->ops->write) return -1;
    
    int64_t res = f->vnode->ops->write(f->vnode, buf, size, f->offset, f);
    if (res > 0) f->offset += res;
    return res;
}

int64_t vfs_seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !g_fd_table[fd].used) return -1;
    FileDescriptor* f = &g_fd_table[fd];
    
    int64_t new_offset = f->offset;
    if (whence == SEEK_SET) new_offset = offset;
    else if (whence == SEEK_CUR) new_offset += offset;
    else if (whence == SEEK_END) new_offset = f->vnode->size + offset;
    
    if (new_offset < 0) return -1;
    f->offset = new_offset;
    return f->offset;
}

int vfs_readdir(int fd, char* name_out) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !g_fd_table[fd].used) return -1;
    FileDescriptor* f = &g_fd_table[fd];
    if (!f->vnode->ops->readdir) return -1;
    int res = f->vnode->ops->readdir(f->vnode, f->dir_pos, name_out);
    if (res == 0) f->dir_pos++;
    return res;
}

int vfs_stat(const char* path, VNodeStat* out) {
    VNode* node = vfs_lookup_vnode(path);
    if (!node) return -1;
    
    out->inode = node->inode_id;
    out->size = node->size;
    out->is_dir = node->is_dir;
    
    vfs_close_vnode(node);
    return 0;
}

int vfs_mkdir(const char* path) {
    char parent_path[512];
    kstring::strncpy(parent_path, path, 511);
    
    char* last_slash = nullptr;
    for (char* p = parent_path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    
    if (!last_slash) return -1;
    
    const char* name = last_slash + 1;
    const char* lookup_path = (last_slash == parent_path) ? "/" : parent_path;
    if (last_slash != parent_path) *last_slash = '\0';
    
    VNode* parent = vfs_lookup_vnode(lookup_path);
    int res = -1;
    if (parent) {
        if (parent->ops->mkdir) res = parent->ops->mkdir(parent, name);
        vfs_close_vnode(parent);
    }
    return res;
}

int vfs_unlink(const char* path) {
    char parent_path[512];
    kstring::strncpy(parent_path, path, 511);
    
    char* last_slash = nullptr;
    for (char* p = parent_path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    
    if (!last_slash) return -1;
    
    const char* name = last_slash + 1;
    const char* lookup_path = (last_slash == parent_path) ? "/" : parent_path;
    if (last_slash != parent_path) *last_slash = '\0';
    
    VNode* parent = vfs_lookup_vnode(lookup_path);
    int res = -1;
    if (parent) {
        if (parent->ops->unlink) res = parent->ops->unlink(parent, name);
        vfs_close_vnode(parent);
    }
    return res;
}
