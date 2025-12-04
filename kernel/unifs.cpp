#include "unifs.h"

static uint8_t* fs_start = nullptr;
static UniFSHeader* header = nullptr;
static UniFSEntry* entries = nullptr;

// Helper for string comparison
static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

void unifs_init(void* start_addr) {
    fs_start = (uint8_t*)start_addr;
    header = (UniFSHeader*)fs_start;
    entries = (UniFSEntry*)(fs_start + sizeof(UniFSHeader));
    
    // Verify magic
    if (header->magic[0] != 'U' || header->magic[1] != 'N' || header->magic[2] != 'I') {
        // Invalid magic
        return;
    }
}

const UniFSFile* unifs_open(const char* name) {
    if (!fs_start || !header) return nullptr;
    
    for (uint64_t i = 0; i < header->file_count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            // Found it
            // We return a static struct for simplicity, or we could allocate
            static UniFSFile file;
            file.name = entries[i].name;
            file.size = entries[i].size;
            file.data = fs_start + entries[i].offset;
            return &file;
        }
    }
    return nullptr;
}
