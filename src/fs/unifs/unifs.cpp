#include <kernel/fs/unifs.h>
#include <kernel/fs/vfs.h>
#include <libk/kstring.h>
#include <libk/kstd.h>
#include <kernel/mm/heap.h>
#include <kernel/syscall.h>
#include <kernel/sync/spinlock.h>

using kstring::string_view;
using kstd::unique_ptr;
using kstd::KBuffer;

enum class UnifsType {
    Unknown = 0,
    Text,
    Binary,
    Elf
};

enum class UnifsError {
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

struct UniFSHeader {
    char magic[8];
    uint64_t file_count;
} __attribute__((packed));

struct UniFSEntry {
    char name[64];
    uint64_t offset;
    uint64_t size;
} __attribute__((packed));

struct UniFSFile {
    string_view name;
    uint64_t size;
    const uint8_t* data;
};

struct RAMFile {
    char name[64];
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
    bool used;
};

static uint8_t* g_fs_start = nullptr;
static UniFSHeader* g_boot_header = nullptr;
static UniFSEntry* g_boot_entries = nullptr;
static bool g_mounted = false;

static RAMFile g_ram_files[MAX_FILES];
static uint64_t g_ram_file_count = 0;
static Spinlock g_ram_lock = SPINLOCK_INIT;

static const uint8_t ELF_MAGIC[] = {0x7F, 'E', 'L', 'F'};

[[nodiscard]] static UniFSEntry* find_boot_entry(string_view name) {
    if (!g_mounted) return nullptr;
    
    for (uint64_t i = 0; i < g_boot_header->file_count; i++) {
        if (name == string_view(g_boot_entries[i].name)) {
            return &g_boot_entries[i];
        }
    }
    return nullptr;
}

[[nodiscard]] static RAMFile* find_ram_file(string_view name) {
    for (auto& file : g_ram_files) {
        if (file.used && name == string_view(file.name)) {
            return &file;
        }
    }
    return nullptr;
}

[[nodiscard]] static RAMFile* find_free_slot() {
    for (auto& file : g_ram_files) {
        if (!file.used) return &file;
    }
    return nullptr;
}

[[nodiscard]] static bool unifs_file_exists(string_view name) {
    return find_ram_file(name) || find_boot_entry(name);
}

[[nodiscard]] static uint64_t unifs_get_file_size(string_view name) {
    if (auto* ram = find_ram_file(name)) return ram->size;
    if (auto* boot = find_boot_entry(name)) return boot->size;
    return 0;
}

[[nodiscard]] static bool unifs_open_into(string_view name, UniFSFile& out_file) {
    if (auto* ram = find_ram_file(name)) {
        out_file.name = ram->name;
        out_file.size = ram->size;
        out_file.data = ram->data;
        return true;
    }
    if (auto* boot = find_boot_entry(name)) {
        out_file.name = boot->name;
        out_file.size = boot->size;
        out_file.data = g_fs_start + boot->offset;
        return true;
    }
    return false;
}

[[nodiscard]] static UnifsError unifs_create(string_view name) {
    if (name.empty()) return UnifsError::NotFound;
    if (name.size() > MAX_FILENAME) return UnifsError::NameTooLong;
    if (unifs_file_exists(name)) return UnifsError::Exists;
    
    spinlock_acquire(&g_ram_lock);
    RAMFile* slot = find_free_slot();
    if (!slot) {
        spinlock_release(&g_ram_lock);
        return UnifsError::Full;
    }
    
    kstring::strncpy(slot->name, name.data(), MAX_FILENAME);
    slot->name[MAX_FILENAME] = '\0';
    slot->data = nullptr;
    slot->size = 0;
    slot->capacity = 0;
    slot->used = true;
    g_ram_file_count++;
    spinlock_release(&g_ram_lock);
    
    return UnifsError::Ok;
}

[[nodiscard]] static UnifsError unifs_write(string_view name, const void* data, uint64_t size) {
    if (size > MAX_FILE_SIZE) return UnifsError::NoMemory;
    if (find_boot_entry(name) && !find_ram_file(name)) return UnifsError::ReadOnly;
    
    RAMFile* file = find_ram_file(name);
    if (!file) {
        if (auto err = unifs_create(name); err != UnifsError::Ok) return err;
        file = find_ram_file(name);
    } else if (is_file_open(file->name)) {
        return UnifsError::InUse;
    }
    
    spinlock_acquire(&g_ram_lock);
    if (size > file->capacity) {
        free(file->data);
        file->data = static_cast<uint8_t*>(malloc(size));
        if (!file->data) {
            file->size = 0;
            file->capacity = 0;
            spinlock_release(&g_ram_lock);
            return UnifsError::NoMemory;
        }
        file->capacity = size;
    }
    
    if (data && size > 0) kstring::memcpy(file->data, data, size);
    file->size = size;
    spinlock_release(&g_ram_lock);
    
    return UnifsError::Ok;
}

[[nodiscard]] static UnifsError unifs_append(string_view name, const void* data, uint64_t size) {
    if (find_boot_entry(name) && !find_ram_file(name)) return UnifsError::ReadOnly;
    
    RAMFile* file = find_ram_file(name);
    if (!file) {
        if (auto err = unifs_create(name); err != UnifsError::Ok) return err;
        file = find_ram_file(name);
    } else if (is_file_open(file->name)) {
        return UnifsError::InUse;
    }
    
    spinlock_acquire(&g_ram_lock);
    uint64_t new_size = file->size + size;
    if (new_size > MAX_FILE_SIZE) {
        spinlock_release(&g_ram_lock);
        return UnifsError::NoMemory;
    }
    
    if (new_size > file->capacity) {
        uint64_t new_capacity = (new_size * 2 > MAX_FILE_SIZE) ? MAX_FILE_SIZE : new_size * 2;
        uint8_t* new_data = static_cast<uint8_t*>(malloc(new_capacity));
        if (!new_data) {
            spinlock_release(&g_ram_lock);
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
    spinlock_release(&g_ram_lock);
    
    return UnifsError::Ok;
}

[[nodiscard]] static UnifsError unifs_delete(string_view name) {
    if (find_boot_entry(name) && !find_ram_file(name)) return UnifsError::ReadOnly;
    
    spinlock_acquire(&g_ram_lock);
    RAMFile* file = find_ram_file(name);
    if (!file) {
        spinlock_release(&g_ram_lock);
        return UnifsError::NotFound;
    }
    
    if (is_file_open(file->name)) {
        spinlock_release(&g_ram_lock);
        return UnifsError::InUse;
    }
    
    free(file->data);
    file->data = nullptr;
    file->size = 0;
    file->capacity = 0;
    file->used = false;
    g_ram_file_count--;
    spinlock_release(&g_ram_lock);
    
    return UnifsError::Ok;
}

static int64_t unifs_vfs_read(VNode* node, void* buf, uint64_t size, uint64_t offset, FileDescriptor*) {
    if (node->is_dir) return -1;
    
    UniFSFile file;
    if (!unifs_open_into(static_cast<const char*>(node->fs_data), file)) return -1;
    if (offset >= file.size) return 0;
    
    uint64_t to_read = (size < file.size - offset) ? size : file.size - offset;
    kstring::memcpy(buf, file.data + offset, to_read);
    return static_cast<int64_t>(to_read);
}

static int64_t unifs_vfs_write(VNode* node, const void* buf, uint64_t size, uint64_t offset, FileDescriptor*) {
    if (node->is_dir) return -1;
    string_view name(static_cast<const char*>(node->fs_data));
    
    if (offset == 0) {
        if (unifs_write(name, buf, size) == UnifsError::Ok) return static_cast<int64_t>(size);
        return -1;
    }

    if (is_file_open(name.data())) return -1;

    RAMFile* file = find_ram_file(name);
    if (!file) {
        if (find_boot_entry(name)) return -1;
        if (unifs_append(name, buf, size) == UnifsError::Ok) return static_cast<int64_t>(size);
        return -1;
    }
    
    spinlock_acquire(&g_ram_lock);
    uint64_t new_end = offset + size;
    if (new_end > MAX_FILE_SIZE) {
        spinlock_release(&g_ram_lock);
        return -1;
    }
    
    if (new_end > file->capacity) {
        uint64_t new_cap = (new_end * 2 > MAX_FILE_SIZE) ? MAX_FILE_SIZE : new_end * 2;
        uint8_t* new_data = static_cast<uint8_t*>(malloc(new_cap));
        if (!new_data) {
            spinlock_release(&g_ram_lock);
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
    if (new_end > file->size) file->size = new_end;
    spinlock_release(&g_ram_lock);
    
    return static_cast<int64_t>(size);
}

static void unifs_vfs_close(VNode* node) {
    if (node->fs_data) {
        free(node->fs_data);
        node->fs_data = nullptr;
    }
}

static VNodeOps unifs_file_ops = {
    .read = unifs_vfs_read,
    .write = unifs_vfs_write,
    .readdir = nullptr,
    .lookup = nullptr,
    .create = nullptr,
    .mkdir = nullptr,
    .unlink = nullptr,
    .close = unifs_vfs_close
};

static VNode* unifs_vfs_lookup(VNode* dir, const char* name);
static int unifs_vfs_readdir(VNode* node, uint64_t index, char* name_out);
static int unifs_vfs_create(VNode* dir, const char* name);
static int unifs_vfs_mkdir(VNode* dir, const char* name);
static int unifs_vfs_unlink(VNode* dir, const char* name);

static VNodeOps unifs_dir_ops = {
    .read = nullptr,
    .write = nullptr,
    .readdir = unifs_vfs_readdir,
    .lookup = unifs_vfs_lookup,
    .create = unifs_vfs_create,
    .mkdir = unifs_vfs_mkdir,
    .unlink = unifs_vfs_unlink,
    .close = unifs_vfs_close
};

[[nodiscard]] uint64_t unifs_get_file_count() {
    uint64_t count = 0;
    if (g_mounted) {
        for (uint64_t i = 0; i < g_boot_header->file_count; i++) {
            if (!find_ram_file(g_boot_entries[i].name)) count++;
        }
    }
    count += g_ram_file_count;
    return count;
}

[[nodiscard]] static string_view unifs_get_entry_name(uint64_t index) {
    uint64_t visible_idx = 0;
    if (g_mounted) {
        for (uint64_t i = 0; i < g_boot_header->file_count; i++) {
            if (find_ram_file(g_boot_entries[i].name)) continue;
            if (visible_idx == index) return g_boot_entries[i].name;
            visible_idx++;
        }
    }
    
    uint64_t ram_index = index - visible_idx;
    uint64_t found = 0;
    for (auto& file : g_ram_files) {
        if (file.used) {
            if (found == ram_index) return file.name;
            found++;
        }
    }
    return "";
}

static VNode* unifs_vfs_lookup(VNode* dir, const char* name) {
    if (!dir->is_dir) return nullptr;
    string_view n_view(name);
    if (n_view.empty() || n_view.data()[0] == '/') return nullptr;

    const char* prefix = static_cast<const char*>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    
    string_view p_view(path);
    if (unifs_file_exists(p_view)) {
        string_view real_name;
        if (auto* ram = find_ram_file(p_view)) real_name = ram->name;
        else real_name = find_boot_entry(p_view)->name;

        if (!real_name.empty() && real_name.data()[real_name.size() - 1] != '/') {
            char* name_copy = static_cast<char*>(malloc(real_name.size() + 1));
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
        if (auto* ram = find_ram_file(dp_view)) real_name = ram->name;
        else real_name = find_boot_entry(dp_view)->name;

        char* name_copy = static_cast<char*>(malloc(real_name.size() + 1));
        kstring::strncpy(name_copy, real_name.data(), real_name.size());
        name_copy[real_name.size()] = '\0';
        return vfs_create_vnode(0, 0, true, &unifs_dir_ops, name_copy);
    }
    
    return nullptr;
}

static int unifs_vfs_readdir(VNode* node, uint64_t index, char* name_out) {
    if (!node->is_dir) return -1;
    
    string_view prefix(static_cast<const char*>(node->fs_data));
    uint64_t current_idx = 0;
    uint64_t total_entries = unifs_get_file_count();
    
    for (uint64_t i = 0; i < total_entries; i++) {
        string_view entry_name = unifs_get_entry_name(i);
        if (entry_name.empty() || entry_name == prefix || !entry_name.starts_with(prefix)) continue;
        
        string_view sub(entry_name.data() + prefix.size(), entry_name.size() - prefix.size());
        if (sub.empty()) continue;
        
        const char* first_slash = nullptr;
        for (size_t k = 0; k < sub.size(); k++) {
            if (sub[k] == '/') { first_slash = sub.data() + k; break; }
        }
        
        if (!first_slash || *(first_slash + 1) == '\0') {
            if (current_idx == index) {
                kstring::strncpy(name_out, sub.data(), 255);
                size_t len = kstring::strlen(name_out);
                if (len > 0 && name_out[len-1] == '/') name_out[len-1] = '\0';
                return 0;
            }
            current_idx++;
        }
    }
    return -1;
}

static int unifs_vfs_create(VNode* dir, const char* name) {
    const char* prefix = static_cast<const char*>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    return (unifs_create(path) == UnifsError::Ok) ? 0 : -1;
}

static int unifs_vfs_mkdir(VNode* dir, const char* name) {
    const char* prefix = static_cast<const char*>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    kstring::strncat(path, "/", 255 - kstring::strlen(path));
    return (unifs_create(path) == UnifsError::Ok) ? 0 : -1;
}

static int unifs_vfs_unlink(VNode* dir, const char* name) {
    const char* prefix = static_cast<const char*>(dir->fs_data);
    char path[256];
    kstring::strncpy(path, prefix ? prefix : "", 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    
    if (unifs_delete(path) == UnifsError::Ok) return 0;
    kstring::strncat(path, "/", 255 - kstring::strlen(path));
    return (unifs_delete(path) == UnifsError::Ok) ? 0 : -1;
}

VNode* unifs_get_root() {
    char* root_prefix = static_cast<char*>(malloc(1));
    root_prefix[0] = '\0';
    return vfs_create_vnode(0, 0, true, &unifs_dir_ops, root_prefix);
}

void unifs_init(void* start_addr) {
    for (auto& file : g_ram_files) {
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
    
    g_fs_start = static_cast<uint8_t*>(start_addr);
    g_boot_header = reinterpret_cast<UniFSHeader*>(g_fs_start);
    g_boot_entries = reinterpret_cast<UniFSEntry*>(g_fs_start + sizeof(UniFSHeader));
    g_mounted = (kstring::memcmp(g_boot_header->magic, UNIFS_MAGIC, 8) == 0);
}

bool unifs_is_mounted() { return g_mounted; }

uint64_t unifs_get_total_size() {
    spinlock_acquire(&g_ram_lock);
    uint64_t total = 0;
    if (g_mounted) {
        for (uint64_t i = 0; i < g_boot_header->file_count; i++) total += g_boot_entries[i].size;
    }
    for (auto& file : g_ram_files) {
        if (file.used) total += file.size;
    }
    spinlock_release(&g_ram_lock);
    return total;
}

uint64_t unifs_get_boot_file_count() { return g_mounted ? g_boot_header->file_count : 0; }
