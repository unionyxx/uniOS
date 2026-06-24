#pragma once
#include <stddef.h>
#include <stdint.h>
#include <uapi/fs.h>

struct VNode;
struct FileDescriptor;

struct VNodeOps
{
    int64_t (*read)(struct VNode *node, void *buf, uint64_t size, uint64_t offset, struct FileDescriptor *fd);
    int64_t (*write)(struct VNode *node, const void *buf, uint64_t size, uint64_t offset, struct FileDescriptor *fd);
    int (*readdir)(struct VNode *node, uint64_t index, char *name_out);
    VNode *(*lookup)(struct VNode *dir, const char *name);
    int (*create)(struct VNode *dir, const char *name);
    int (*mkdir)(struct VNode *dir, const char *name);
    int (*unlink)(struct VNode *dir, const char *name);
    int (*rename)(struct VNode *old_dir, const char *old_name, struct VNode *new_dir, const char *new_name);
    int (*truncate)(struct VNode *node, uint64_t size);
    int (*sync)(struct VNode *node);
    void (*close)(struct VNode *node);
};

struct VNode
{
    uint64_t inode_id;
    uint64_t size;
    uint32_t uid;  // Owner UID
    uint16_t mode; // Permissions (Unix-style)
    bool is_dir;
    VNodeOps *ops;
    void *fs_data;
    volatile uint32_t ref_count;
};

struct Mount
{
    char path[64];
    VNode *root;
    uint32_t flags;
    struct Mount *next;
};

#define VFS_MOUNT_FLAG_STORAGE_GUARDED 0x01u

#define MAX_VFS_FDS 128

extern VNodeOps fat32_file_ops;
extern VNodeOps unifs_file_ops;

// VFS API
void vfs_init();
int vfs_mount(const char *path, VNode *root);
int vfs_mount_ex(const char *path, VNode *root, uint32_t flags);
int vfs_open(const char *path, int flags, uint16_t mode = 0644);
int vfs_close(int fd);
int64_t vfs_read(int fd, void *buf, uint64_t size);
int64_t vfs_write(int fd, const void *buf, uint64_t size);
int vfs_readdir(int fd, char *name_out); // Stateful readdir
int64_t vfs_seek(int fd, int64_t offset, int whence);
int vfs_stat(const char *path, VNodeStat *out);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);
void vfs_close_vnode(VNode *node);
void vfs_sync();
Mount *vfs_get_mounts();

// Path resolution helper
// vfs_lookup_vnode returns a VNode with ref_count incremented.
// Caller must call vfs_close_vnode when done.
VNode *vfs_lookup_vnode(const char *path);
bool is_file_open(const char *path);
void vfs_resolve_relative_path(const char *cwd, const char *path, char *out);

// Helper to create a VNode (filesystem drivers will use this)
VNode *vfs_create_vnode(uint64_t inode_id, uint64_t size, bool is_dir, VNodeOps *ops, void *fs_data);
