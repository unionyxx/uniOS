#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint64_t BOOT_INFO_MAGIC = 0x554E49424F4F5431ULL;
constexpr uint32_t BOOT_INFO_REVISION = 1;

enum BootFirmwareType : uint64_t
{
    BOOT_FIRMWARE_UEFI32 = 1,
    BOOT_FIRMWARE_UEFI64 = 2,
};

enum BootMemoryType : uint64_t
{
    BOOT_MEM_USABLE = 0,
    BOOT_MEM_RESERVED = 1,
    BOOT_MEM_ACPI_RECLAIMABLE = 2,
    BOOT_MEM_ACPI_NVS = 3,
    BOOT_MEM_BAD = 4,
    BOOT_MEM_BOOTLOADER_RECLAIMABLE = 5,
    BOOT_MEM_KERNEL_AND_MODULES = 6,
    BOOT_MEM_FRAMEBUFFER = 7,
};

struct BootVideoMode
{
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

struct BootFramebuffer
{
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
    uint64_t edid_size;
    void *edid;
    uint64_t mode_count;
    BootVideoMode **modes;
};

struct BootModule
{
    void *address;
    uint64_t size;
    const char *path;
    const char *cmdline;
};

struct BootMemoryMapEntry
{
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct BootInfo
{
    uint64_t magic;
    uint32_t revision;
    uint32_t size;
    uint64_t hhdm_offset;
    uint64_t kernel_physical_base;
    uint64_t kernel_virtual_base;
    BootFramebuffer *framebuffer;
    uint64_t framebuffer_count;
    uint64_t firmware_type;
    uint64_t rsdp_address;
    uint64_t efi_system_table_address;
    const char *bootloader_name;
    const char *bootloader_version;
    uint64_t module_count;
    BootModule *modules;
    uint64_t memory_map_count;
    BootMemoryMapEntry *memory_map;
};

inline const BootInfo *g_boot_info = nullptr;

inline void boot_set_info(const BootInfo *info)
{
    g_boot_info = info;
}

inline const BootInfo *boot_get_info()
{
    return g_boot_info;
}

inline const BootFramebuffer *boot_get_framebuffer()
{
    if (!g_boot_info)
        return nullptr;
    return g_boot_info->framebuffer;
}
