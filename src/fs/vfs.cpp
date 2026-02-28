#include <kernel/fs/vfs.h>
#include <kernel/syscall.h>
#include <kernel/mm/heap.h>
#include <libk/kstring.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>

static Mount* mount_list = nullptr;
static Spinlock mount_lock = SPINLOCK_INIT;
static FileDescriptor fd_table[MAX_VFS_FDS];

void vfs_init() {
    for (int i = 0; i < MAX_VFS_FDS; i++) {
        fd_table[i].used = false;
        fd_table[i].vnode = nullptr;
        fd_table[i].offset = 0;
        fd_table[i].dir_pos = 0;
    }
}

VNode* vfs_create_vnode(uint64_t inode_id, uint64_t size, bool is_dir, VNodeOps* ops, void* fs_data) {
    VNode* node = (VNode*)malloc(sizeof(VNode));
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
    
    Mount* m = (Mount*)malloc(sizeof(Mount));
    if (!m) return -1;
    
    kstring::strncpy(m->path, path, 63);
    m->path[63] = '\0';
    m->root = root;
    
    spinlock_acquire(&mount_lock);
    m->next = mount_list;
    mount_list = m;
    spinlock_release(&mount_lock);
    
    DEBUG_INFO("VFS: Mounted filesystem at %s", path);
    return 0;
}

static VNode* vfs_resolve_path(const char* path, const char** out_rel_path) {
    if (!path || path[0] != '/') return nullptr;
    
    Mount* best_mount = nullptr;
    size_t max_len = 0;
    
    spinlock_acquire(&mount_lock);
    Mount* current = mount_list;
    while (current) {
        size_t len = kstring::strlen(current->path);
        if (kstring::strncmp(path, current->path, len) == 0) {
            // Fix greedy matching: must end on / or be exact match
            if (path[len] == '\0' || path[len] == '/' || (len == 1 && current->path[0] == '/')) {
                if (len >= max_len) {
                    max_len = len;
                    best_mount = current;
                }
            }
        }
        current = current->next;
    }
    
    if (!best_mount) {
        spinlock_release(&mount_lock);
        return nullptr;
    }
    
    spinlock_release(&mount_lock);
    
    const char* rel_path = path + max_len;
    while (*rel_path == '/') rel_path++; // Skip leading slashes
    
    if (out_rel_path) *out_rel_path = rel_path;
    return best_mount->root;
}

void vfs_resolve_relative_path(const char* cwd, const char* path, char* out) {
    char temp_path[512];
    size_t cwd_len = kstring::strlen(cwd);
    size_t path_len = kstring::strlen(path);

    if (path[0] == '/') {
        if (path_len >= 511) {
            kstring::strncpy(temp_path, path, 511);
            temp_path[511] = '\0';
        } else {
            kstring::strncpy(temp_path, path, 511);
        }
    } else {
        if (cwd_len + path_len + 2 >= 512) {
            // Path too long, truncate or handle error
            kstring::strncpy(temp_path, cwd, 255);
            temp_path[255] = '\0';
            kstring::strncat(temp_path, "/", 511 - kstring::strlen(temp_path));
            kstring::strncat(temp_path, path, 511 - kstring::strlen(temp_path));
        } else {
            kstring::strncpy(temp_path, cwd, 511);
            if (temp_path[kstring::strlen(temp_path) - 1] != '/') {
                kstring::strncat(temp_path, "/", 511 - kstring::strlen(temp_path));
            }
            kstring::strncat(temp_path, path, 511 - kstring::strlen(temp_path));
        }
    }
    
    // Normalize: resolve . and .. components
    char* segments[64];
    int depth = 0;
    char copy[512];
    kstring::strncpy(copy, temp_path, 511);

    char* tok = copy;
    if (*tok == '/') tok++;
    
    char* p2 = tok;
    while (true) {
        if (*p2 == '/' || *p2 == '\0') {
            char saved = *p2;
            *p2 = '\0';
            
            if (kstring::strcmp(tok, "..") == 0) {
                if (depth > 0) depth--;
            } else if (kstring::strcmp(tok, ".") != 0 && tok[0] != '\0') {
                if (depth < 64) segments[depth++] = tok;
            }
            
            if (saved == '\0') break;
            tok = p2 + 1;
            p2 = tok;
            continue;
        }
        p2++;
    }

    // Rebuild path correctly
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

VNode* vfs_lookup_vnode(const char* path) {
    if (!path) return nullptr;
    
    const char* rel_path = nullptr;
    VNode* current = vfs_resolve_path(path, &rel_path);
    if (!current) return nullptr;
    
    current->ref_count++;
    
    if (*rel_path == '\0') {
        return current;
    }
    
    char component[256];
    const char* p = rel_path;
    
    while (*p) {
        // Extract next component
        size_t i = 0;
        while (*p && *p != '/' && i < 255) {
            component[i++] = *p++;
        }
        component[i] = '\0';
        
        // Skip consecutive slashes
        while (*p == '/') p++;
        
        if (component[0] == '\0') continue;
        
        if (!current->ops->lookup) {
            vfs_close_vnode(current);
            return nullptr;
        }
        
        VNode* next = current->ops->lookup(current, component);
        vfs_close_vnode(current);
        
        if (!next) return nullptr;
        
        // Check if 'next' is a mount point
        spinlock_acquire(&mount_lock);
        Mount* m = mount_list;
        while (m) {
            // This is a bit simplified: cross-mount lookup needs to check if the vnode 
            // we just found IS a mount point.
            // In a more complex VFS, we'd have a 'mount' flag on the VNode.
            m = m->next;
        }
        spinlock_release(&mount_lock);
        
        current = next;
        // next already has ref_count 1 from lookup
    }
    
    return current;
}

void vfs_close_vnode(VNode* node) {
    if (!node) return;
    
    node->ref_count--;
    if (node->ref_count == 0) {
        // Check if it's a mount root before freeing
        bool is_mount_root = false;
        Mount* m = mount_list;
        while (m) {
            if (m->root == node) { is_mount_root = true; break; }
            m = m->next;
        }
        if (!is_mount_root) {
            if (node->ops->close) node->ops->close(node);
            free(node);
        }
    }
}

int vfs_open(const char* path, int flags) {
    const char* rel_path = nullptr;
    VNode* root = vfs_resolve_path(path, &rel_path);
    if (!root) return -1;
    
    VNode* node = nullptr;
    if (*rel_path == '\0') {
        node = root;
        node->ref_count++;
    } else {
        if (!root->ops->lookup) return -1;
        node = root->ops->lookup(root, rel_path);
    }
    
    // If not found and O_CREAT is set, try to create it
    if (!node && (flags & O_CREAT)) {
        char parent_path[512];
        kstring::strncpy(parent_path, path, 511);
        
        char* last_slash = nullptr;
        for (char* p = parent_path; *p; p++) {
            if (*p == '/') last_slash = p;
        }
        
        if (last_slash) {
            const char* name = last_slash + 1;
            if (last_slash == parent_path) kstring::strncpy(parent_path, "/", 511);
            else *last_slash = '\0';
            
            VNode* parent = vfs_lookup_vnode(parent_path);
            if (parent) {
                if (parent->ops->create) {
                    if (parent->ops->create(parent, name) == 0) {
                        node = parent->ops->lookup(parent, name);
                    }
                }
                vfs_close_vnode(parent);
            }
        }
    }
    
    if (!node) return -1;

    if (node->is_dir && (flags & O_WRONLY || flags & O_RDWR)) {
        vfs_close_vnode(node);
        return -1;
    }
    
    // Find free FD (skipping 0, 1, 2)
    for (int i = 3; i < MAX_VFS_FDS; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = true;
            fd_table[i].vnode = node;
            fd_table[i].offset = (flags & O_APPEND) ? node->size : 0;
            fd_table[i].dir_pos = 0;
            return i;
        }
    }
    
    vfs_close_vnode(node);
    return -1;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !fd_table[fd].used) return -1;
    
    VNode* node = fd_table[fd].vnode;
    vfs_close_vnode(node);
    
    fd_table[fd].used = false;
    fd_table[fd].vnode = nullptr;
    return 0;
}

int64_t vfs_read(int fd, void* buf, uint64_t size) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !fd_table[fd].used) return -1;

    FileDescriptor* f = &fd_table[fd];
    if (!f->vnode->ops->read) return -1;

    int64_t bytes_read = f->vnode->ops->read(f->vnode, buf, size, f->offset, f);
    if (bytes_read > 0) {
        f->offset += bytes_read;
    }
    return bytes_read;
}

int64_t vfs_write(int fd, const void* buf, uint64_t size) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !fd_table[fd].used) return -1;

    FileDescriptor* f = &fd_table[fd];
    if (!f->vnode->ops->write) return -1;

    int64_t bytes_written = f->vnode->ops->write(f->vnode, buf, size, f->offset, f);
    if (bytes_written > 0) {
        f->offset += bytes_written;
    }
    return bytes_written;
}
int vfs_readdir(int fd, char* name_out) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !fd_table[fd].used) return -1;
    
    FileDescriptor* f = &fd_table[fd];
    if (!f->vnode->ops->readdir) return -1;
    
    int res = f->vnode->ops->readdir(f->vnode, f->dir_pos, name_out);
    if (res == 0) {
        f->dir_pos++;
    }
    return res;
}

int64_t vfs_seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= MAX_VFS_FDS || !fd_table[fd].used) return -1;
    
    FileDescriptor* f = &fd_table[fd];
    uint64_t new_off;
    
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = f->offset + offset; break;
        case SEEK_END: new_off = f->vnode->size + offset; break;
        default: return -1;
    }
    
    f->offset = new_off;
    return new_off;
}

int vfs_stat(const char* path, VNodeStat* out) {
    VNode* node = vfs_lookup_vnode(path);
    if (!node) return -1;
    
    out->size = node->size;
    out->is_dir = node->is_dir;
    out->inode = node->inode_id;
    
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
    if (last_slash == parent_path) kstring::strncpy(parent_path, "/", 511);
    else *last_slash = '\0';
    
    VNode* parent = vfs_lookup_vnode(parent_path);
    int res = -1;
    if (parent) {
        if (parent->ops->mkdir) {
            res = parent->ops->mkdir(parent, name);
        } else {
            DEBUG_WARN("VFS: mkdir not supported on this filesystem");
        }
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
    if (last_slash == parent_path) kstring::strncpy(parent_path, "/", 511);
    else *last_slash = '\0';
    
    VNode* parent = vfs_lookup_vnode(parent_path);
    int res = -1;
    if (parent) {
        if (parent->ops->unlink) {
            res = parent->ops->unlink(parent, name);
        } else {
            DEBUG_WARN("VFS: unlink not supported on this filesystem");
        }
        vfs_close_vnode(parent);
    }
    
    return res;
}
