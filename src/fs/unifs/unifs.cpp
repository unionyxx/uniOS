#include <kernel/fs/unifs.h>
#include <kernel/fs/vfs.h>
#include <libk/kstring.h>
#include <kernel/mm/heap.h>
#include <kernel/syscall.h>  // For is_file_open()
#include <kernel/sync/spinlock.h>

// ============================================================================
// uniFS Implementation
// ============================================================================

// File type detection (based on extension/content)
#define UNIFS_TYPE_UNKNOWN  0
#define UNIFS_TYPE_TEXT     1
#define UNIFS_TYPE_BINARY   2
#define UNIFS_TYPE_ELF      3

// Error codes
#define UNIFS_OK            0
#define UNIFS_ERR_NOT_FOUND -1
#define UNIFS_ERR_EXISTS    -2
#define UNIFS_ERR_FULL      -3
#define UNIFS_ERR_NO_MEMORY -4
#define UNIFS_ERR_NAME_TOO_LONG -5
#define UNIFS_ERR_READONLY  -6
#define UNIFS_ERR_IN_USE    -7

// Limits
#define UNIFS_MAX_FILES     64
#define UNIFS_MAX_FILENAME  63
#define UNIFS_MAX_FILE_SIZE (1024 * 1024)

// On-disk structures
struct UniFSHeader {
    char magic[8];        // "UNIFS v1"
    uint64_t file_count;  // Number of files
} __attribute__((packed));

struct UniFSEntry {
    char name[64];        // Null-terminated filename
    uint64_t offset;      // Offset from start of filesystem
    uint64_t size;        // File size in bytes
} __attribute__((packed));

// In-memory file handle
struct UniFSFile {
    const char* name;
    uint64_t size;
    const uint8_t* data;
};

// The filesystem has two parts:
// 1. Boot files: Read from Limine module at boot (read-only)
// 2. RAM files: Created at runtime (read-write, lost on reboot)
// ============================================================================

// Boot filesystem (read-only, from boot module)
static uint8_t* fs_start = nullptr;
static UniFSHeader* boot_header = nullptr;
static UniFSEntry* boot_entries = nullptr;
static bool mounted = false;

// RAM filesystem (read-write)
struct RAMFile {
    char name[64];        // Filename
    uint8_t* data;        // File data (heap allocated)
    uint64_t size;        // File size
    uint64_t capacity;    // Allocated capacity
    bool used;            // Slot in use
};

static RAMFile ram_files[UNIFS_MAX_FILES];
static uint64_t ram_file_count = 0;
static Spinlock ram_lock = SPINLOCK_INIT;

// ELF magic bytes
static const uint8_t ELF_MAGIC[] = {0x7F, 'E', 'L', 'F'};

// ============================================================================
// Forward Declarations
// ============================================================================
static UniFSEntry* find_boot_entry(const char* name);
static RAMFile* find_ram_file(const char* name);
static bool unifs_file_exists(const char* name);
static uint64_t unifs_get_file_size(const char* name);
static int unifs_create(const char* name);
static int unifs_write(const char* name, const void* data, uint64_t size);
static int unifs_append(const char* name, const void* data, uint64_t size);
static int unifs_delete(const char* name);
static bool unifs_open_into(const char* name, UniFSFile* out_file);
static const char* unifs_get_file_name(uint64_t index);

// ============================================================================
// Internal Helpers
// ============================================================================

// Find entry in boot files (returns nullptr if not found)
static UniFSEntry* find_boot_entry(const char* name) {
    if (!mounted || !name) return nullptr;
    
    for (uint64_t i = 0; i < boot_header->file_count; i++) {
        if (kstring::strcmp(boot_entries[i].name, name) == 0) {
            return &boot_entries[i];
        }
    }
    return nullptr;
}

// Find entry in RAM files (returns nullptr if not found)
static RAMFile* find_ram_file(const char* name) {
    if (!name) return nullptr;
    
    for (int i = 0; i < UNIFS_MAX_FILES; i++) {
        if (ram_files[i].used && kstring::strcmp(ram_files[i].name, name) == 0) {
            return &ram_files[i];
        }
    }
    return nullptr;
}

// Find free RAM file slot
static RAMFile* find_free_slot() {
    for (int i = 0; i < UNIFS_MAX_FILES; i++) {
        if (!ram_files[i].used) {
            return &ram_files[i];
        }
    }
    return nullptr;
}

// Check if file content looks like text
static bool is_text_content(const uint8_t* data, uint64_t size) {
    uint64_t check_size = (size < 256) ? size : 256;
    
    for (uint64_t i = 0; i < check_size; i++) {
        uint8_t c = data[i];
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
            return false;
        }
        if (c > 126 && c < 160) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// uniFS VFS Integration
// ============================================================================

static int64_t unifs_vfs_read(VNode* node, void* buf, uint64_t size, uint64_t offset, FileDescriptor* fd) {
    if (node->is_dir) return -1;
    
    UniFSFile file;
    if (!unifs_open_into((const char*)node->fs_data, &file)) return -1;
    
    if (offset >= file.size) return 0;
    uint64_t to_read = (size < file.size - offset) ? size : file.size - offset;
    
    kstring::memcpy(buf, file.data + offset, to_read);
    return to_read;
}

static int64_t unifs_vfs_write(VNode* node, const void* buf, uint64_t size, uint64_t offset, FileDescriptor* fd) {
    if (node->is_dir) return -1;
    const char* name = (const char*)node->fs_data;
    
    if (offset == 0) {
        if (unifs_write(name, buf, size) == UNIFS_OK) return size;
    } else {
        // Handle mid-file write or append
        if (is_file_open(name)) return -1;

        RAMFile* file = find_ram_file(name);
        if (!file) {
            // Cannot write mid-file to read-only boot files
            if (find_boot_entry(name)) return -1;
            
            // For new files, if offset is not 0, we could fail or just create it
            // Let's support append if offset == current_size
            if (unifs_append(name, buf, size) == UNIFS_OK) return size;
            return -1;
        }
        
        spinlock_acquire(&ram_lock);
        uint64_t new_end = offset + size;
        if (new_end > UNIFS_MAX_FILE_SIZE) {
            spinlock_release(&ram_lock);
            return -1;
        }
        
        // Reallocate if needed
        if (new_end > file->capacity) {
            uint64_t new_cap = (new_end > file->capacity * 2) ? new_end : file->capacity * 2;
            if (new_cap > UNIFS_MAX_FILE_SIZE) new_cap = UNIFS_MAX_FILE_SIZE;
            
            uint8_t* new_data = (uint8_t*)malloc(new_cap);
            if (!new_data) {
                spinlock_release(&ram_lock);
                return -1;
            }
            
            if (file->data) {
                kstring::memcpy(new_data, file->data, file->size);
                free(file->data);
            }
            file->data = new_data;
            file->capacity = new_cap;
        }
        
        // Copy new data
        kstring::memcpy(file->data + offset, buf, size);
        if (new_end > file->size) file->size = new_end;
        spinlock_release(&ram_lock);
        
        return size;
    }
    return -1;
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

static VNode* unifs_vfs_lookup(VNode* dir, const char* name) {
    if (!dir->is_dir) return nullptr;
    
    // Safety: uniFS VFS driver only handles single-component lookups now.
    for (const char* p = name; *p; p++) {
        if (*p == '/') return nullptr;
    }

    const char* prefix = (const char*)dir->fs_data;
    if (!prefix) prefix = "";
    
    char path[256];
    kstring::strncpy(path, prefix, 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    
    // 1. Check if it's a file
    if (unifs_file_exists(path)) {
        RAMFile* ram = find_ram_file(path);
        const char* p_name = ram ? ram->name : find_boot_entry(path)->name;
        size_t plen = kstring::strlen(p_name);
        if (plen > 0 && p_name[plen-1] != '/') {
            char* name_copy = (char*)malloc(plen + 1);
            kstring::strncpy(name_copy, p_name, plen);
            name_copy[plen] = '\0';
            return vfs_create_vnode(0, unifs_get_file_size(p_name), false, &unifs_file_ops, (void*)name_copy);
        }
    }
    
    // 2. Check if it's a directory (ends in / in our internal representation)
    char dir_path[256];
    kstring::strncpy(dir_path, path, 255);
    kstring::strncat(dir_path, "/", 255 - kstring::strlen(dir_path));
    if (unifs_file_exists(dir_path)) {
        RAMFile* ram = find_ram_file(dir_path);
        const char* p_name = ram ? ram->name : find_boot_entry(dir_path)->name;
        size_t plen = kstring::strlen(p_name);
        char* name_copy = (char*)malloc(plen + 1);
        kstring::strncpy(name_copy, p_name, plen);
        name_copy[plen] = '\0';
        return vfs_create_vnode(0, 0, true, &unifs_dir_ops, (void*)name_copy);
    }
    
    return nullptr;
}

static int unifs_vfs_readdir(VNode* node, uint64_t index, char* name_out) {
    if (!node->is_dir) return -1;
    
    const char* prefix = (const char*)node->fs_data;
    size_t prefix_len = prefix ? kstring::strlen(prefix) : 0;
    
    uint64_t current_idx = 0;
    uint64_t total_entries = unifs_get_file_count();
    
    for (uint64_t i = 0; i < total_entries; i++) {
        const char* entry_name = unifs_get_file_name(i);
        if (!entry_name) continue;
        if (prefix && kstring::strcmp(entry_name, prefix) == 0) continue;
        if (prefix_len > 0 && kstring::strncmp(entry_name, prefix, prefix_len) != 0) continue;
        
        const char* sub = entry_name + prefix_len;
        if (*sub == '\0') continue;
        
        const char* first_slash = nullptr;
        for (const char* p = sub; *p; p++) {
            if (*p == '/') { first_slash = p; break; }
        }
        
        bool is_direct = false;
        if (!first_slash) is_direct = true;
        else if (*(first_slash + 1) == '\0') is_direct = true;
        
        if (is_direct) {
            if (current_idx == index) {
                kstring::strncpy(name_out, sub, 255);
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
    const char* prefix = (const char*)dir->fs_data;
    char path[256];
    kstring::strncpy(path, prefix, 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    
    return (unifs_create(path) == UNIFS_OK) ? 0 : -1;
}

static int unifs_vfs_mkdir(VNode* dir, const char* name) {
    const char* prefix = (const char*)dir->fs_data;
    char path[256];
    kstring::strncpy(path, prefix, 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    kstring::strncat(path, "/", 255 - kstring::strlen(path)); // Signal directory intent
    
    return (unifs_create(path) == UNIFS_OK) ? 0 : -1;
}

static int unifs_vfs_unlink(VNode* dir, const char* name) {
    const char* prefix = (const char*)dir->fs_data;
    char path[256];
    kstring::strncpy(path, prefix, 255);
    kstring::strncat(path, name, 255 - kstring::strlen(path));
    
    // Try file first
    int res = unifs_delete(path);
    if (res != UNIFS_OK) {
        // Try directory
        kstring::strncat(path, "/", 255 - kstring::strlen(path));
        res = unifs_delete(path);
    }
    
    return (res == UNIFS_OK) ? 0 : -1;
}

VNode* unifs_get_root() {
    char* root_prefix = (char*)malloc(1);
    root_prefix[0] = '\0';
    return vfs_create_vnode(0, 0, true, &unifs_dir_ops, (void*)root_prefix);
}

// ============================================================================
// Read API Implementation
// ============================================================================

void unifs_init(void* start_addr) {
    // Initialize RAM files
    for (int i = 0; i < UNIFS_MAX_FILES; i++) {
        ram_files[i].used = false;
        ram_files[i].data = nullptr;
        ram_files[i].size = 0;
        ram_files[i].capacity = 0;
    }
    ram_file_count = 0;
    
    if (!start_addr) {
        mounted = false;
        return;
    }
    
    fs_start = (uint8_t*)start_addr;
    boot_header = (UniFSHeader*)fs_start;
    boot_entries = (UniFSEntry*)(fs_start + sizeof(UniFSHeader));
    
    // Verify magic
    if (kstring::memcmp(boot_header->magic, UNIFS_MAGIC, 8) != 0) {
        mounted = false;
        return;
    }
    
    mounted = true;
}

bool unifs_is_mounted() {
    return mounted;
}

// Thread-safe version: fills caller-provided buffer
static bool unifs_open_into(const char* name, UniFSFile* out_file) {
    if (!out_file) return false;
    
    // Check RAM files first (they can shadow boot files)
    RAMFile* ram = find_ram_file(name);
    if (ram) {
        out_file->name = ram->name;
        out_file->size = ram->size;
        out_file->data = ram->data;
        return true;
    }
    
    // Check boot files
    UniFSEntry* entry = find_boot_entry(name);
    if (entry) {
        out_file->name = entry->name;
        out_file->size = entry->size;
        out_file->data = fs_start + entry->offset;
        return true;
    }
    
    return false;
}

static bool unifs_file_exists(const char* name) {
    return find_ram_file(name) != nullptr || find_boot_entry(name) != nullptr;
}

static uint64_t unifs_get_file_size(const char* name) {
    RAMFile* ram = find_ram_file(name);
    if (ram) return ram->size;
    
    UniFSEntry* entry = find_boot_entry(name);
    return entry ? entry->size : 0;
}

static int unifs_get_file_type(const char* name) {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    
    RAMFile* ram = find_ram_file(name);
    if (ram) {
        data = ram->data;
        size = ram->size;
    } else {
        UniFSEntry* entry = find_boot_entry(name);
        if (!entry) return UNIFS_TYPE_UNKNOWN;
        data = fs_start + entry->offset;
        size = entry->size;
    }
    
    if (size >= 4 && kstring::memcmp(data, ELF_MAGIC, 4) == 0) {
        return UNIFS_TYPE_ELF;
    }
    
    if (is_text_content(data, size)) {
        return UNIFS_TYPE_TEXT;
    }
    
    return UNIFS_TYPE_BINARY;
}

uint64_t unifs_get_file_count() {
    // Count boot files (excluding those shadowed by RAM files)
    uint64_t count = 0;
    if (mounted) {
        for (uint64_t i = 0; i < boot_header->file_count; i++) {
            if (!find_ram_file(boot_entries[i].name)) {
                count++;
            }
        }
    }
    // Add RAM files
    count += ram_file_count;
    return count;
}

static const char* unifs_get_file_name(uint64_t index) {
    // Boot files first (skip files shadowed by RAM files)
    uint64_t visible_idx = 0;
    if (mounted) {
        for (uint64_t i = 0; i < boot_header->file_count; i++) {
            // Skip if this boot file is shadowed by a RAM file
            if (find_ram_file(boot_entries[i].name)) continue;
            
            if (visible_idx == index) {
                return boot_entries[i].name;
            }
            visible_idx++;
        }
    }
    
    // Then RAM files (index is now relative to visible boot files)
    uint64_t ram_index = index - visible_idx;
    uint64_t found = 0;
    for (int i = 0; i < UNIFS_MAX_FILES; i++) {
        if (ram_files[i].used) {
            if (found == ram_index) {
                return ram_files[i].name;
            }
            found++;
        }
    }
    
    return nullptr;
}

static uint64_t unifs_get_file_size_by_index(uint64_t index) {
    // Boot files first
    if (mounted && index < boot_header->file_count) {
        return boot_entries[index].size;
    }
    
    // Then RAM files
    uint64_t ram_index = mounted ? index - boot_header->file_count : index;
    uint64_t found = 0;
    for (int i = 0; i < UNIFS_MAX_FILES; i++) {
        if (ram_files[i].used) {
            if (found == ram_index) {
                return ram_files[i].size;
            }
            found++;
        }
    }
    
    return 0;
}

// ============================================================================
// Write API Implementation (RAM-only)
// ============================================================================

static int unifs_create(const char* name) {
    if (!name) return UNIFS_ERR_NOT_FOUND;
    
    // Check name length
    if (kstring::strlen(name) > UNIFS_MAX_FILENAME) {
        return UNIFS_ERR_NAME_TOO_LONG;
    }
    
    // Check if file already exists
    if (unifs_file_exists(name)) {
        return UNIFS_ERR_EXISTS;
    }
    
    // Find free slot
    spinlock_acquire(&ram_lock);
    RAMFile* slot = find_free_slot();
    if (!slot) {
        spinlock_release(&ram_lock);
        return UNIFS_ERR_FULL;
    }
    
    // Initialize new file
    kstring::strncpy(slot->name, name, UNIFS_MAX_FILENAME);
    slot->name[UNIFS_MAX_FILENAME] = '\0';
    slot->data = nullptr;
    slot->size = 0;
    slot->capacity = 0;
    slot->used = true;
    ram_file_count++;
    spinlock_release(&ram_lock);
    
    return UNIFS_OK;
}

static int unifs_write(const char* name, const void* data, uint64_t size) {
    if (!name) return UNIFS_ERR_NOT_FOUND;
    if (size > UNIFS_MAX_FILE_SIZE) return UNIFS_ERR_NO_MEMORY;
    
    // Check if it's a boot file (read-only)
    if (find_boot_entry(name) && !find_ram_file(name)) {
        return UNIFS_ERR_READONLY;
    }
    
    // Find or create RAM file
    RAMFile* file = find_ram_file(name);
    if (!file) {
        int result = unifs_create(name);
        if (result != UNIFS_OK) return result;
        file = find_ram_file(name);
    } else {
        // Prevent modifying files currently open via fd (prevents use-after-free)
        if (is_file_open(name)) {
            return UNIFS_ERR_IN_USE;
        }
    }
    
    // Allocate/reallocate buffer if needed
    spinlock_acquire(&ram_lock);
    if (size > file->capacity) {
        if (file->data) {
            free(file->data);
        }
        file->data = (uint8_t*)malloc(size);
        if (!file->data) {
            file->size = 0;
            file->capacity = 0;
            spinlock_release(&ram_lock);
            return UNIFS_ERR_NO_MEMORY;
        }
        file->capacity = size;
    }
    
    // Copy data
    if (data && size > 0) {
        kstring::memcpy(file->data, data, size);
    }
    file->size = size;
    spinlock_release(&ram_lock);
    
    return UNIFS_OK;
}

static int unifs_append(const char* name, const void* data, uint64_t size) {
    if (!name || !data || size == 0) return UNIFS_ERR_NOT_FOUND;
    
    // Check if it's a boot file (read-only)
    if (find_boot_entry(name) && !find_ram_file(name)) {
        return UNIFS_ERR_READONLY;
    }
    
    // Find or create RAM file
    RAMFile* file = find_ram_file(name);
    if (!file) {
        int result = unifs_create(name);
        if (result != UNIFS_OK) return result;
        file = find_ram_file(name);
    } else {
        // Prevent modifying files currently open via fd (prevents use-after-free)
        if (is_file_open(name)) {
            return UNIFS_ERR_IN_USE;
        }
    }
    
    spinlock_acquire(&ram_lock);
    uint64_t new_size = file->size + size;
    if (new_size > UNIFS_MAX_FILE_SIZE) {
        spinlock_release(&ram_lock);
        return UNIFS_ERR_NO_MEMORY;
    }
    
    // Reallocate if needed
    if (new_size > file->capacity) {
        uint64_t new_capacity = new_size * 2;  // Grow by 2x
        if (new_capacity > UNIFS_MAX_FILE_SIZE) new_capacity = UNIFS_MAX_FILE_SIZE;
        
        uint8_t* new_data = (uint8_t*)malloc(new_capacity);
        if (!new_data) {
            spinlock_release(&ram_lock);
            return UNIFS_ERR_NO_MEMORY;
        }
        
        if (file->data && file->size > 0) {
            kstring::memcpy(new_data, file->data, file->size);
            free(file->data);
        }
        file->data = new_data;
        file->capacity = new_capacity;
    }
    
    // Append data
    kstring::memcpy(file->data + file->size, data, size);
    file->size = new_size;
    spinlock_release(&ram_lock);
    
    return UNIFS_OK;
}

static int unifs_delete(const char* name) {
    if (!name) return UNIFS_ERR_NOT_FOUND;
    
    // Cannot delete boot files
    if (find_boot_entry(name) && !find_ram_file(name)) {
        return UNIFS_ERR_READONLY;
    }
    
    spinlock_acquire(&ram_lock);
    // Find RAM file
    RAMFile* file = find_ram_file(name);
    if (!file) {
        spinlock_release(&ram_lock);
        return UNIFS_ERR_NOT_FOUND;
    }
    
    // Check if file is currently open (prevent use-after-free)
    // is_file_open() is declared in syscall.h
    if (is_file_open(name)) {
        spinlock_release(&ram_lock);
        return UNIFS_ERR_IN_USE;
    }
    
    // Free memory and mark slot as unused
    if (file->data) {
        free(file->data);
    }
    file->data = nullptr;
    file->size = 0;
    file->capacity = 0;
    file->used = false;
    ram_file_count--;
    spinlock_release(&ram_lock);
    
    return UNIFS_OK;
}

// ============================================================================
// Stats
// ============================================================================

uint64_t unifs_get_total_size() {
    spinlock_acquire(&ram_lock);
    uint64_t total = 0;
    
    // Boot files
    if (mounted) {
        for (uint64_t i = 0; i < boot_header->file_count; i++) {
            total += boot_entries[i].size;
        }
    }
    
    // RAM files
    for (int i = 0; i < UNIFS_MAX_FILES; i++) {
        if (ram_files[i].used) {
            total += ram_files[i].size;
        }
    }
    spinlock_release(&ram_lock);
    
    return total;
}

uint64_t unifs_get_boot_file_count() {
    return mounted ? boot_header->file_count : 0;
}
