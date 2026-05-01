#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    ProcessState_Ready = 0,
    ProcessState_Running,
    ProcessState_Blocked,
    ProcessState_Sleeping,
    ProcessState_Zombie,
    ProcessState_Waiting
} ProcessStateU;

struct ProcessInfo
{
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t uid;
    char name[32];
    ProcessStateU state;
    uint8_t priority;
};

struct SysTime
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

struct MemInfo
{
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t used_kb;
    uint64_t heap_total_kb;
    uint64_t heap_used_kb;
};

struct SystemProfile
{
    char kernel_commit[16];
    char bootloader_name[32];
    char bootloader_version[32];
    uint32_t timer_hz;
    uint8_t kernel_build_debug;
    uint8_t reserved[11];
};

#ifdef __cplusplus
}
#endif
