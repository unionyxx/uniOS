#include <kernel/fs/unifs.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/sync/spinlock.h>
#include <kernel/syscall.h>
#include <libk/kstd.h>
#include <libk/kstring.h>

using kstd::KBuffer;
using kstd::unique_ptr;
using kstring::string_view;

enum class UnifsType
{
    Unknown = 0,
    Text,
    Binary,
    Elf
};

enum class UnifsError
{
    Ok = 0,
    NotFound = -1,
    Exists = -2,
    Full = -3,
    NoMemory = -4,
    NameTooLong = -5,
    ReadOnly = -6,
    InUse = -7
};

static constexpr size_t MAX_FILES = 64;
static constexpr size_t MAX_FILENAME = 63;
static constexpr size_t MAX_FILE_SIZE = 1024 * 1024;

struct UniFSHeader
{
    char magic[8];
    uint64_t file_count;
} __attribute__((packed));

struct UniFSEntry
{
    char name[64];
    uint64_t offset;
    uint64_t size;
} __attribute__((packed));

struct UniFSFile
{
    string_view name;
    uint64_t size;
    const uint8_t *data;
};

struct RAMFile
{
    char name[64];
    uint8_t *data;
    uint64_t size;
    uint64_t capacity;
    bool used;
};

static uint8_t *g_fs_start = nullptr;
static UniFSHeader *g_boot_header = nullptr;
static UniFSEntry *g_boot_entries = nullptr;
static bool g_mounted = false;

static RAMFile g_ram_files[MAX_FILES];
static uint64_t g_ram_file_count = 0;
static Spinlock g_ram_lock = SPINLOCK_INIT;

static void normalize_boot_entry_name(char *name)
{
    if (!name)
        return;

    for (size_t i = 0; name[i] != '\0'; i++) {
        if (name[i] == '\\')
            name[i] = '/';
    }
}

static const uint8_t ELF_MAGIC[] = {0x7F, 'E', 'L', 'F'};

[[nodiscard]] static UniFSEntry *find_boot_entry(string_view name)
{
    if (!g_mounted)
        return nullptr;

    for (uint64_t i = 0; i < g_boot_header->file_count; i++) {
        if (name == string_view(g_boot_entries[i].name)) {
            return &g_boot_entries[i];
        }
    }
    return nullptr;
}

[[nodiscard]] static RAMFile *find_ram_file(string_view name)
{
    for (auto &file : g_ram_files) {
        if (file.used && name == string_view(file.name)) {
            return &file;
        }
    }
    return nullptr;
}

[[nodiscard]] static RAMFile *find_free_slot()
{
    for (auto &file : g_ram_files) {
        if (!file.used)
            return &file;
    }
    return nullptr;
}

[[nodiscard]] static bool unifs_file_exists(string_view name)
{
    return find_ram_file(name) || find_boot_entry(name);
}

[[nodiscard]] static uint64_t unifs_get_file_size(string_view name)
{
    if (auto *ram = find_ram_file(name))
        return ram->size;
    if (auto *boot = find_boot_entry(name))
        return boot->size;
    return 0;
}

[[nodiscard]] static bool unifs_open_into(string_view name, UniFSFile &out_file)
{
    if (auto *ram = find_ram_file(name)) {
        out_file.name = ram->name;
        out_file.size = ram->size;
        out_file.data = ram->data;
        return true;
    }
    if (auto *boot = find_boot_entry(name)) {
        out_file.name = boot->name;
        out_file.size = boot->size;
        out_file.data = g_fs_start + boot->offset;
        return true;
    }
    return false;
}

[[nodiscard]] static UnifsError unifs_create(string_view name)
{
    if (name.empty())
        return UnifsError::NotFound;
    if (name.size() > MAX_FILENAME)
        return UnifsError::NameTooLong;
    if (find_ram_file(name))
        return UnifsError::Exists;

    uint64_t flags = spinlock_acquire_irqsave(&g_ram_lock);
    RAMFile *slot = find_free_slot();
    if (!slot) {
        spinlock_release_irqrestore(&g_ram_lock, flags);
        return UnifsError::Full;
    }

    kstring::strncpy(slot->name, name.data(), MAX_FILENAME);
    slot->name[MAX_FILENAME] = '\0';
    slot->data = nullptr;
    slot->size = 0;
    slot->capacity = 0;
    slot->used = true;
    g_ram_file_count++;
    spinlock_release_irqrestore(&g_ram_lock, flags);

    return UnifsError::Ok;
}

[[nodiscard]] static UnifsError unifs_write(string_view name, const void *data, uint64_t size)
{
    if (size > MAX_FILE_SIZE)
        return UnifsError::NoMemory;

    RAMFile *file = find_ram_file(name);
    if (!file) {
        if (auto err = unifs_create(name); err != UnifsError::Ok)
            return err;
        file = find_ram_file(name);
    } else if (is_file_open(file->name)) {
        return UnifsError::InUse;
    }

    uint64_t flags = spinlock_acquire_irqsave(&g_ram_lock);
    if (size > file->capacity) {
        free(file->data);
        file->data = static_cast<uint8_t *>(malloc(size));
        if (!file->data) {
            file->size = 0;
            file->capacity = 0;
            spinlock_release_irqrestore(&g_ram_lock, flags);
            return UnifsError::NoMemory;
        }
        file->capacity = size;
    }

    if (data && size > 0)
        kstring::memcpy(file->data, data, size);
    file->size = size;
    spinlock_release_irqrestore(&g_ram_lock, flags);

    return UnifsError::Ok;
}

[[nodiscard]] static UnifsError unifs_append(string_view name, const void *data, uint64_t size)
{
    if (size == 0)
        return UnifsError::Ok;

    RAMFile *file = find_ram_file(name);
    uint64_t flags = 0;
    if (!file) {
        auto *boot_entry = find_boot_entry(name);
        if (boot_entry) {
            if (auto err = unifs_create(name); err != UnifsError::Ok)
                return err;
            file = find_ram_file(name);
            // Copy original boot entry data
            uint64_t boot_size = boot_entry->size;
            uint8_t *boot_data = g_fs_start + boot_entry->offset;

            flags = spinlock_acquire_irqsave(&g_ram_lock);
            uint64_t initial_cap = (boot_size * 2 > MAX_FILE_SIZE) ? MAX_FILE_SIZE : boot_size * 2;
            if (initial_cap == 0)
                initial_cap = 4096;
            file->data = static_cast<uint8_t *>(malloc(initial_cap));
            if (file->data) {
                kstring::memcpy(file->data, boot_data, boot_size);
                file->capacity = initial_cap;
                file->size = boot_size;
            }
            spinlock_release_irqrestore(&g_ram_lock, flags);
        } else {
            if (auto err = unifs_create(name); err != UnifsError::Ok)
                return err;
            file = find_ram_file(name);
        }
    } else if (is_file_open(file->name)) {
        return UnifsError::InUse;
    }

    flags = spinlock_acquire_irqsave(&g_ram_lock);
    uint64_t new_size = file->size + size;
    if (new_size > MAX_FILE_SIZE) {
        spinlock_release_irqrestore(&g_ram_lock, flags);
        return UnifsError::NoMemory;
    }

    if (new_size > file->capacity) {
        uint64_t new_capacity = (new_size * 2 > MAX_FILE_SIZE) ? MAX_FILE_SIZE : new_size * 2;
        uint8_t *new_data = static_cast<uint8_t *>(malloc(new_capacity));
        if (!new_data) {
            spinlock_release_irqrestore(&g_ram_lock, flags);
            return UnifsError::NoMemory;
        }

        if (file->data && file->size > 0) {
            kstring::memcpy(new_data, file->data, file->size);
            free(file->data);
        }
        file->data = new_data;
        file->capacity = new_capacity;
    }

    kstring::memcpy(file->data + file->size, data, size);
    file->size = new_size;
    spinlock_release_irqrestore(&g_ram_lock, flags);

    return UnifsError::Ok;
}

[[nodiscard]] static UnifsError unifs_delete(string_view name)
{
    RAMFile *file = find_ram_file(name);
    if (!file) {
        if (find_boot_entry(name))
            return UnifsError::Ok;
        return UnifsError::NotFound;
    }

    if (is_file_open(file->name))
        return UnifsError::InUse;

    uint64_t flags = spinlock_acquire_irqsave(&g_ram_lock);
    free(file->data);
    file->data = nullptr;
    file->size = 0;
    file->capacity = 0;
    file->used = false;
    file->name[0] = '\0';
    g_ram_file_count--;
    spinlock_release_irqrestore(&g_ram_lock, flags);

    return UnifsError::Ok;
}

static int64_t unifs_vfs_read(VNode *node, void *buf, uint64_t size, uint64_t offset, FileDescriptor *)
{
    if (node->is_dir)
        return -1;

    UniFSFile file;
    if (!unifs_open_into(static_cast<const char *>(node->fs_data), file))
        return -1;
    if (offset >= file.size)
        return 0;

    uint64_t to_read = (size < file.size - offset) ? size : file.size - offset;
    kstring::memcpy(buf, file.data + offset, to_read);
    return static_cast<int64_t>(to_read);
}

static int64_t unifs_vfs_write(VNode *node, const void *buf, uint64_t size, uint64_t offset, FileDescriptor *)
{
    if (node->is_dir)
        return -1;
    string_view name(static_cast<const char *>(node->fs_data));

    if (offset == 0) {
        if (unifs_write(name, buf, size) == UnifsError::Ok)
            return static_cast<int64_t>(size);
        return -1;
    }

    if (is_file_open(name.data()))
        return -1;

    RAMFile *file = find_ram_file(name);
    uint64_t flags = 0;
    if (!file) {
        auto *boot = find_boot_entry(name);
        if (boot) {
            // COW: create RAM file and copy boot data
            if (unifs_create(name) != UnifsError::Ok)
                return -1;
            file = find_ram_file(name);

            uint8_t *boot_data_copy = nullptr;
            if (boot->size > 0) {
                boot_data_copy = static_cast<uint8_t *>(malloc(boot->size));
                if (!boot_data_copy)
                    return -1;
                kstring::memcpy(boot_data_copy, g_fs_start + boot->offset, boot->size);
            }

            flags = spinlock_acquire_irqsave(&g_ram_lock);
            file->data = boot_data_copy;
            file->size = boot->size;
            file->capacity = boot->size;
            spinlock_release_irqrestore(&g_ram_lock, flags);
        } else {
            if (unifs_append(name, buf, size) == UnifsError::Ok)
                return static_cast<int64_t>(size);
            return -1;
        }
    }

    flags = spinlock_acquire_irqsave(&g_ram_lock);
    uint64_t new_end = offset + size;
    if (new_end > MAX_FILE_SIZE) {
        spinlock_release_irqrestore(&g_ram_lock, flags);
        return -1;
    }

    if (new_end > file->capacity) {
        uint64_t new_cap = (new_end * 2 > MAX_FILE_SIZE) ? MAX_FILE_SIZE : new_end * 2;
        uint8_t *new_data = static_cast<uint8_t *>(malloc(new_cap));
        if (!new_data) {
            spinlock_release_irqrestore(&g_ram_lock, flags);
            return -1;
        }

        if (file->data) {
            kstring::memcpy(new_data, file->data, file->size);
            free(file->data);
        }
        file->data = new_data;
        file->capacity = new_cap;
    }

    kstring::memcpy(file->data + offset, buf, size);
    if (new_end > file->size)
        file->size = new_end;
    spinlock_release_irqrestore(&g_ram_lock, flags);

    return static_cast<int64_t>(size);
}

static int unifs_vfs_truncate(VNode *node, uint64_t size)
{
    if (node->is_dir)
        return -1;
    string_view name(static_cast<const char *>(node->fs_data));

    RAMFile *file = find_ram_file(name);
    uint64_t flags = 0;
    if (!file) {
        auto *boot_entry = find_boot_entry(name);
        if (boot_entry) {
            // Copy-on-Write for truncation too
            if (unifs_create(name) != UnifsError::Ok)
                return -1;
            file = find_ram_file(name);
            if (size > 0) {
                flags = spinlock_acquire_irqsave(&g_ram_lock);
                file->data = static_cast<uint8_t *>(malloc(boot_entry->size));
                if (file->data) {
                    kstring::memcpy(file->data, g_fs_start + boot_entry->offset, boot_entry->size);
                    file->size = boot_entry->size;
                    file->capacity = boot_entry->size;
                }
                spinlock_release_irqrestore(&g_ram_lock, flags);
                if (!file->data)
                    return -1;
            }
        } else {
            return -1;
        }
    }

    if (is_file_open(name.data()))
        return -1;

    flags = spinlock_acquire_irqsave(&g_ram_lock);
    if (size == 0) {
        file->size = 0;
    } else if (size <= file->capacity) {
        file->size = size;
    } else {
        // Grow
        uint8_t *new_data = static_cast<uint8_t *>(malloc(size));
        if (!new_data) {
            spinlock_release_irqrestore(&g_ram_lock, flags);
            return -1;
        }
        if (file->data) {
            kstring::memcpy(new_data, file->data, file->size);
            free(file->data);
        }
        file->data = new_data;
        file->size = size;
        file->capacity = size;
    }
    spinlock_release_irqrestore(&g_ram_lock, flags);
    node->size = size;
    return 0;
}

static int unifs_vfs_rename(VNode *old_dir, const char *old_name, VNode *new_dir, const char *new_name)
{
    const char *old_prefix = static_cast<const char *>(old_dir->fs_data);
    const char *new_prefix = static_cast<const char *>(new_dir->fs_data);

    char old_path[256], new_path[256];
    kstring::strncpy(old_path, old_prefix ? old_prefix : "", 255);
    kstring::strncat(old_path, old_name, 255 - kstring::strlen(old_path));

    kstring::strncpy(new_path, new_prefix ? new_prefix : "", 255);
    kstring::strncat(new_path, new_name, 255 - kstring::strlen(new_path));

    // Check if source exists
    if (!unifs_file_exists(old_path))
        return -1;

    // Check if dest exists - if so, delete it first (atomic rename behavior)
    if (unifs_file_exists(new_path)) {
        if (unifs_delete(new_path) != UnifsError::Ok)
            return -1;
    }

    // Create new entry
    if (unifs_create(new_path) != UnifsError::Ok)
        return -1;

    RAMFile *old_file = find_ram_file(old_path);
    RAMFile *new_file = find_ram_file(new_path);

    if (old_file && new_file) {
        // Copy data from old to new
        uint64_t flags = spinlock_acquire_irqsave(&g_ram_lock);
        new_file->data = old_file->data;
        new_file->size = old_file->size;
        new_file->capacity = old_file->capacity;

        // Clear old file entry so it doesn't free the data we just stole
        old_file->data = nullptr;
        old_file->used = false;
        spinlock_release_irqrestore(&g_ram_lock, flags);

        static_cast<void>(unifs_delete(old_path));
        return 0;
    } else {
        // If it was a boot file, we need a different approach
        auto *boot_entry = find_boot_entry(old_path);
        if (boot_entry) {
            uint64_t size = boot_entry->size;
            uint8_t *data = g_fs_start + boot_entry->offset;
            if (unifs_write(new_path, data, size) == UnifsError::Ok) {
                // We can't "delete" a boot file, but it's now "hidden" by the RAM entry if we created one
                return 0;
            }
        }
    }

    return -1;
}

static void unifs_vfs_close(VNode *node)
{
    if (node->fs_data) {
        free(node->fs_data);
        node->fs_data = nullptr;
    }
}

static VNodeOps unifs_file_ops = {.read = unifs_vfs_read,
                                  .write = unifs_vfs_write,
                                  .readdir = nullptr,
                                  .lookup = nullptr,
                                  .create = nullptr,
                                  .mkdir = nullptr,
                                  .unlink = nullptr,
                                  .rename = nullptr,
                                  .truncate = unifs_vfs_truncate,
                                  .close = unifs_vfs_close};

static VNode *unifs_vfs_lookup(VNode *dir, const char *name);
static int unifs_vfs_readdir(VNode *node, uint64_t index, char *name_out);
static int unifs_vfs_create(VNode *dir, const char *name);
static int unifs_vfs_mkdir(VNode *dir, const char *name);
static int unifs_vfs_unlink(VNode *dir, const char *name);

static VNodeOps unifs_dir_ops = {.read = nullptr,
                                 .write = nullptr,
                                 .readdir = unifs_vfs_readdir,
                                 .lookup = unifs_vfs_lookup,
                                 .create = unifs_vfs_create,
                                 .mkdir = unifs_vfs_mkdir,
                                 .unlink = unifs_vfs_unlink,
                                 .rename = unifs_vfs_rename,
                                 .truncate = nullptr,
                                 .close = unifs_vfs_close};

[[nodiscard]] uint64_t unifs_get_file_count()
{
    uint64_t count = 0;
    if (g_mounted) {
        for (uint64_t i = 0; i < g_boot_header->file_count; i++) {
            if (!find_ram_file(g_boot_entries[i].name))
                count++;
        }
    }
    count += g_ram_file_count;
    return count;
}

[[nodiscard]] static string_view unifs_get_entry_name(uint64_t index)
{
    uint64_t visible_idx = 0;
    if (g_mounted) {
        for (uint64_t i = 0; i < g_boot_header->file_count; i++) {
            if (find_ram_file(g_boot_entries[i].name))
                continue;
            if (visible_idx == index)
                return g_boot_entries[i].name;
            visible_idx++;
        }
    }

    uint64_t ram_index = index - visible_idx;
    uint64_t found = 0;
    for (auto &file : g_ram_files) {
        if (file.used) {
            if (found == ram_index)
                return file.name;
            found++;
        }
    }
    return "";
}

static VNode *unifs_vfs_lookup(VNode *dir, const char *name)
{
    if (!dir->is_dir)
        return nullptr;
    string_view n_view(name);
    if (n_view.empty() || n_view.data()[0] == '/')
        return nullptr;

    const char *prefix = static_cast<const char *>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));

    string_view p_view(path);
    if (unifs_file_exists(p_view)) {
        string_view real_name;
        if (auto *ram = find_ram_file(p_view))
            real_name = ram->name;
        else
            real_name = find_boot_entry(p_view)->name;

        if (!real_name.empty() && real_name.data()[real_name.size() - 1] != '/') {
            char *name_copy = static_cast<char *>(malloc(real_name.size() + 1));
            if (!name_copy)
                return nullptr;
            kstring::strncpy(name_copy, real_name.data(), real_name.size());
            name_copy[real_name.size()] = '\0';
            return vfs_create_vnode(0, unifs_get_file_size(p_view), false, &unifs_file_ops, name_copy);
        }
    }

    char dir_path[256];
    kstring::strncpy(dir_path, path, 255);
    kstring::strncat(dir_path, "/", 255 - kstring::strlen(dir_path));
    string_view dp_view(dir_path);
    if (unifs_file_exists(dp_view)) {
        string_view real_name;
        if (auto *ram = find_ram_file(dp_view))
            real_name = ram->name;
        else
            real_name = find_boot_entry(dp_view)->name;

        char *name_copy = static_cast<char *>(malloc(real_name.size() + 1));
        if (!name_copy)
            return nullptr;
        kstring::strncpy(name_copy, real_name.data(), real_name.size());
        name_copy[real_name.size()] = '\0';
        return vfs_create_vnode(0, 0, true, &unifs_dir_ops, name_copy);
    }

    return nullptr;
}

static int unifs_vfs_readdir(VNode *node, uint64_t index, char *name_out)
{
    if (!node->is_dir)
        return -1;

    const char *prefix = static_cast<const char *>(node->fs_data);
    size_t prefix_len = kstring::strlen(prefix);

    // Normalize prefix: skip leading slash
    const char *p_ptr = prefix;
    if (*p_ptr == '/') {
        p_ptr++;
        prefix_len--;
    }

    uint64_t found_count = 0;
    uint64_t total_boot = g_mounted ? g_boot_header->file_count : 0;
    uint64_t total_ram = MAX_FILES;

    for (uint64_t i = 0; i < total_boot + total_ram; i++) {
        string_view entry_full;
        if (i < total_boot) {
            entry_full = g_boot_entries[i].name;
        } else {
            uint64_t ram_idx = i - total_boot;
            if (ram_idx >= MAX_FILES || !g_ram_files[ram_idx].used)
                continue;
            entry_full = g_ram_files[ram_idx].name;
        }

        if (entry_full.empty())
            continue;

        // Normalize entry: skip leading slash
        const char *e_ptr = entry_full.data();
        size_t e_len = entry_full.size();
        if (e_len > 0 && e_ptr[0] == '/') {
            e_ptr++;
            e_len--;
        }

        // Entry must start with normalized prefix
        if (e_len < prefix_len)
            continue;
        if (prefix_len > 0 && kstring::strncmp(e_ptr, p_ptr, prefix_len) != 0)
            continue;

        // Component is the part after prefix
        const char *sub = e_ptr + prefix_len;
        if (prefix_len > 0 && *sub == '/')
            sub++;
        if (*sub == '\0')
            continue;

        // Isolate immediate component
        char comp[64];
        const char *slash = kstring::strchr(sub, '/');
        size_t comp_len = slash ? (size_t)(slash - sub) : kstring::strlen(sub);

        if (comp_len == 0)
            continue;
        if (comp_len > 63)
            comp_len = 63;
        kstring::strncpy(comp, sub, comp_len);
        comp[comp_len] = '\0';

        // Check for duplicates in already found unique entries
        bool is_duplicate = false;
        for (uint64_t j = 0; j < i; j++) {
            string_view prev_full;
            if (j < total_boot)
                prev_full = g_boot_entries[j].name;
            else {
                uint64_t r_idx = j - total_boot;
                if (r_idx < MAX_FILES && g_ram_files[r_idx].used)
                    prev_full = g_ram_files[r_idx].name;
                else
                    continue;
            }

            const char *pe_ptr = prev_full.data();
            size_t pe_len = prev_full.size();
            if (pe_len > 0 && pe_ptr[0] == '/') {
                pe_ptr++;
                pe_len--;
            }

            if (pe_len >= prefix_len && (prefix_len == 0 || kstring::strncmp(pe_ptr, p_ptr, prefix_len) == 0)) {
                const char *p_sub = pe_ptr + prefix_len;
                if (prefix_len > 0 && *p_sub == '/')
                    p_sub++;
                if (*p_sub == '\0')
                    continue;

                const char *p_slash = kstring::strchr(p_sub, '/');
                size_t p_comp_len = p_slash ? (size_t)(p_slash - p_sub) : kstring::strlen(p_sub);

                if (p_comp_len == comp_len) {
                    if (kstring::strncmp(sub, p_sub, comp_len) == 0) {
                        is_duplicate = true;
                        break;
                    }
                }
            }
        }

        if (!is_duplicate) {
            if (found_count == index) {
                kstring::strncpy(name_out, comp, 255);
                return 0;
            }
            found_count++;
        }
    }

    return -1;
}

static int unifs_vfs_create(VNode *dir, const char *name)
{
    const char *prefix = static_cast<const char *>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    return (unifs_create(path) == UnifsError::Ok) ? 0 : -1;
}

static int unifs_vfs_mkdir(VNode *dir, const char *name)
{
    const char *prefix = static_cast<const char *>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    kstring::strncat(path, "/", 255 - kstring::strlen(path));
    return (unifs_create(path) == UnifsError::Ok) ? 0 : -1;
}

static int unifs_vfs_unlink(VNode *dir, const char *name)
{
    if (!dir || !name || name[0] == '\0')
        return -1;
    const char *prefix = static_cast<const char *>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));

    if (unifs_delete(path) == UnifsError::Ok)
        return 0;
    kstring::strncat(path, "/", 255 - kstring::strlen(path));
    return (unifs_delete(path) == UnifsError::Ok) ? 0 : -1;
}

VNode *unifs_get_root()
{
    char *root_prefix = static_cast<char *>(malloc(1));
    if (!root_prefix)
        return nullptr;
    root_prefix[0] = '\0';
    return vfs_create_vnode(0, 0, true, &unifs_dir_ops, root_prefix);
}

void unifs_init(void *start_addr)
{
    for (auto &file : g_ram_files) {
        file.used = false;
        file.data = nullptr;
        file.size = 0;
        file.capacity = 0;
    }
    g_ram_file_count = 0;

    if (!start_addr) {
        g_mounted = false;
        return;
    }

    g_fs_start = static_cast<uint8_t *>(start_addr);
    g_boot_header = reinterpret_cast<UniFSHeader *>(g_fs_start);
    g_boot_entries = reinterpret_cast<UniFSEntry *>(g_fs_start + sizeof(UniFSHeader));
    g_mounted = (kstring::memcmp(g_boot_header->magic, UNIFS_MAGIC, 8) == 0);
    if (g_mounted) {
        for (uint64_t i = 0; i < g_boot_header->file_count; i++)
            normalize_boot_entry_name(g_boot_entries[i].name);
    }
}

bool unifs_is_mounted()
{
    return g_mounted;
}

uint64_t unifs_get_total_size()
{
    uint64_t flags = spinlock_acquire_irqsave(&g_ram_lock);
    uint64_t total = 0;
    if (g_mounted) {
        for (uint64_t i = 0; i < g_boot_header->file_count; i++)
            total += g_boot_entries[i].size;
    }
    for (auto &file : g_ram_files) {
        if (file.used)
            total += file.size;
    }
    spinlock_release_irqrestore(&g_ram_lock, flags);
    return total;
}

uint64_t unifs_get_boot_file_count()
{
    return g_mounted ? g_boot_header->file_count : 0;
}
