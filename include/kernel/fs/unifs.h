#pragma once
#include <stdint.h>
#include <stddef.h>

struct VNode;

/**
 * @brief Simple Flat Filesystem for uniOS
 * 
 * uniFS supports two layers:
 * 1. Boot files: Read-only files loaded as Limine modules.
 * 2. RAM files: Read-write files stored in memory at runtime.
 */

// uniFS magic signature
#define UNIFS_MAGIC "UNIFS v1"

// Initialize filesystem from memory address (typically from Limine module)
void unifs_init(void* start_addr);

// Get root VNode for VFS integration
struct VNode* unifs_get_root();

// Check if filesystem is mounted and valid
bool unifs_is_mounted();

// uniFS specific stats (for df command)
uint64_t unifs_get_total_size();
uint64_t unifs_get_file_count();
uint64_t unifs_get_boot_file_count();

