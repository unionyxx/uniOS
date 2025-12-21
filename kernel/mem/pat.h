#pragma once
#include <stdint.h>

// IA32_PAT MSR address
#define IA32_PAT_MSR 0x277

// PAT memory type values
#define PAT_UC    0x00  // Uncacheable
#define PAT_WC    0x01  // Write-Combining
#define PAT_WT    0x04  // Write-Through
#define PAT_WP    0x05  // Write-Protected
#define PAT_WB    0x06  // Write-Back
#define PAT_UC_   0x07  // Uncached (alias)

// Initialize PAT with Write-Combining support
void pat_init();

// Check if PAT is supported
bool pat_is_supported();
