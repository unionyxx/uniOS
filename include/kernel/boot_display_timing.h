#pragma once

#include <stdint.h>

static constexpr uint32_t BOOT_DISPLAY_TIMING_REVISION = 1;
static constexpr uint32_t BOOT_DISPLAY_TIMING_FLAG_EXACT_ACTIVE = 1u << 0;

struct BootDisplayTiming
{
    uint32_t revision;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihz;
    uint32_t pixel_clock_khz;
    uint32_t h_total;
    uint32_t v_total;
    uint8_t interlaced;
    uint8_t reserved0;
    uint16_t reserved1;
};

static_assert(sizeof(BootDisplayTiming) == 36, "BootDisplayTiming layout must stay stable");

void boot_display_timing_init(uint64_t efi_system_table_addr, uint64_t firmware_type);
bool boot_display_timing_get(BootDisplayTiming *out_timing);
