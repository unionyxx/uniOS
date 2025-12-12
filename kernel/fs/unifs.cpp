#include "unifs.h"
#include "kstring.h"
#include "heap.h"

// ============================================================================
// uniFS Implementation
// ============================================================================
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

// ELF magic bytes
static const uint8_t ELF_MAGIC[] = {0x7F, 'E', 'L', 'F'};

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

const UniFSFile* unifs_open(const char* name) {
    static UniFSFile file;
    
    // Check RAM files first (they can shadow boot files)
    RAMFile* ram = find_ram_file(name);
    if (ram) {
        file.name = ram->name;
        file.size = ram->size;
        file.data = ram->data;
        return &file;
    }
    
    // Check boot files
    UniFSEntry* entry = find_boot_entry(name);
    if (entry) {
        file.name = entry->name;
        file.size = entry->size;
        file.data = fs_start + entry->offset;
        return &file;
    }
    
    return nullptr;
}

bool unifs_file_exists(const char* name) {
    return find_ram_file(name) != nullptr || find_boot_entry(name) != nullptr;
}

uint64_t unifs_get_file_size(const char* name) {
    RAMFile* ram = find_ram_file(name);
    if (ram) return ram->size;
    
    UniFSEntry* entry = find_boot_entry(name);
    return entry ? entry->size : 0;
}

int unifs_get_file_type(const char* name) {
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
    uint64_t count = mounted ? boot_header->file_count : 0;
    count += ram_file_count;
    return count;
}

const char* unifs_get_file_name(uint64_t index) {
    // Boot files first
    if (mounted && index < boot_header->file_count) {
        return boot_entries[index].name;
    }
    
    // Then RAM files
    uint64_t ram_index = mounted ? index - boot_header->file_count : index;
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

uint64_t unifs_get_file_size_by_index(uint64_t index) {
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

int unifs_create(const char* name) {
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
    RAMFile* slot = find_free_slot();
    if (!slot) {
        return UNIFS_ERR_FULL;
    }
    
    // Initialize new file
    kstring::strcpy(slot->name, name);
    slot->data = nullptr;
    slot->size = 0;
    slot->capacity = 0;
    slot->used = true;
    ram_file_count++;
    
    return UNIFS_OK;
}

int unifs_write(const char* name, const void* data, uint64_t size) {
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
    }
    
    // Allocate/reallocate buffer if needed
    if (size > file->capacity) {
        if (file->data) {
            free(file->data);
        }
        file->data = (uint8_t*)malloc(size);
        if (!file->data) {
            file->size = 0;
            file->capacity = 0;
            return UNIFS_ERR_NO_MEMORY;
        }
        file->capacity = size;
    }
    
    // Copy data
    if (data && size > 0) {
        kstring::memcpy(file->data, data, size);
    }
    file->size = size;
    
    return UNIFS_OK;
}

int unifs_append(const char* name, const void* data, uint64_t size) {
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
    }
    
    uint64_t new_size = file->size + size;
    if (new_size > UNIFS_MAX_FILE_SIZE) return UNIFS_ERR_NO_MEMORY;
    
    // Reallocate if needed
    if (new_size > file->capacity) {
        uint64_t new_capacity = new_size * 2;  // Grow by 2x
        if (new_capacity > UNIFS_MAX_FILE_SIZE) new_capacity = UNIFS_MAX_FILE_SIZE;
        
        uint8_t* new_data = (uint8_t*)malloc(new_capacity);
        if (!new_data) return UNIFS_ERR_NO_MEMORY;
        
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
    
    return UNIFS_OK;
}

int unifs_delete(const char* name) {
    if (!name) return UNIFS_ERR_NOT_FOUND;
    
    // Cannot delete boot files
    if (find_boot_entry(name) && !find_ram_file(name)) {
        return UNIFS_ERR_READONLY;
    }
    
    // Find RAM file
    RAMFile* file = find_ram_file(name);
    if (!file) {
        return UNIFS_ERR_NOT_FOUND;
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
    
    return UNIFS_OK;
}

// ============================================================================
// Stats
// ============================================================================

uint64_t unifs_get_total_size() {
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
    
    return total;
}

uint64_t unifs_get_used_size() {
    return unifs_get_total_size();  // Same as total for now
}

uint64_t unifs_get_free_slots() {
    uint64_t used = 0;
    for (int i = 0; i < UNIFS_MAX_FILES; i++) {
        if (ram_files[i].used) used++;
    }
    return UNIFS_MAX_FILES - used;
}

uint64_t unifs_get_boot_file_count() {
    return mounted ? boot_header->file_count : 0;
}

