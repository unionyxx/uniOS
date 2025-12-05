#pragma once
#include <stdint.h>
#include <stddef.h>

struct UniFSHeader {
    char magic[8]; // "UNIFS v1"
    uint64_t file_count;
};

struct UniFSEntry {
    char name[64];
    uint64_t offset;
    uint64_t size;
};

struct UniFSFile {
    const char* name;
    uint64_t size;
    const uint8_t* data;
};

void unifs_init(void* start_addr);
const UniFSFile* unifs_open(const char* name);
uint64_t unifs_get_file_count();
const char* unifs_get_file_name(uint64_t index);
