#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// File Access Modes
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400

// Seek Constants
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// File Statistics Structure
struct VNodeStat
{
    uint64_t size;
    uint64_t inode;
    uint32_t uid;
    uint16_t mode;
    bool is_dir;
};

enum
{
    VOLUME_FLAG_MOUNTED = 1u << 0,
    VOLUME_FLAG_WRITABLE = 1u << 1,
    VOLUME_FLAG_SYSTEM_DATA = 1u << 2,
    VOLUME_FLAG_STORAGE_DEVICE = 1u << 3,
};

enum
{
    STORAGE_MODE_OFF = 0,
    STORAGE_MODE_READ_ONLY = 1,
    STORAGE_MODE_WRITABLE = 2,
};

struct VolumeInfo
{
    char display_name[64];
    char mount_path[64];
    char source_device[32];
    uint32_t flags;
};

#ifdef __cplusplus
}
#endif
