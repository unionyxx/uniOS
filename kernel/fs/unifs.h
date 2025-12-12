#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// uniFS - Simple Flat Filesystem for uniOS
// ============================================================================
// Format: Header + Entries[] + Data blob
// - Header: 8-byte magic + 8-byte file count
// - Entry:  64-byte name + 8-byte offset + 8-byte size
// - Data:   Raw file contents concatenated
//
// Note: Runtime file modifications are stored in RAM only.
// Changes are lost on reboot (no persistent storage driver yet).
// ============================================================================

// uniFS magic signature
#define UNIFS_MAGIC "UNIFS v1"

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

// Limits
#define UNIFS_MAX_FILES     64
#define UNIFS_MAX_FILENAME  63
#define UNIFS_MAX_FILE_SIZE (1024 * 1024)  // 1 MB per file

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

// ============================================================================
// Read API
// ============================================================================

// Initialize filesystem from memory address (typically from Limine module)
void unifs_init(void* start_addr);

// Check if filesystem is mounted and valid
bool unifs_is_mounted();

// Open a file by name (returns nullptr if not found)
const UniFSFile* unifs_open(const char* name);

// Check if a file exists
bool unifs_file_exists(const char* name);

// Get file size by name (returns 0 if not found)
uint64_t unifs_get_file_size(const char* name);

// Get file type (UNIFS_TYPE_*)
int unifs_get_file_type(const char* name);

// Get total number of files
uint64_t unifs_get_file_count();

// Get filename by index
const char* unifs_get_file_name(uint64_t index);

// Get file size by index
uint64_t unifs_get_file_size_by_index(uint64_t index);

// ============================================================================
// Write API (RAM-only - changes lost on reboot)
// ============================================================================

// Create a new empty file
// Returns: UNIFS_OK on success, or error code
int unifs_create(const char* name);

// Write data to a file (overwrites existing content)
// Returns: UNIFS_OK on success, or error code
int unifs_write(const char* name, const void* data, uint64_t size);

// Append data to a file
// Returns: UNIFS_OK on success, or error code
int unifs_append(const char* name, const void* data, uint64_t size);

// Delete a file
// Returns: UNIFS_OK on success, or error code
int unifs_delete(const char* name);

// Get filesystem stats
uint64_t unifs_get_total_size();
uint64_t unifs_get_used_size();
uint64_t unifs_get_free_slots();
uint64_t unifs_get_boot_file_count();

