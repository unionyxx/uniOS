#pragma once
#include <stdint.h>

// ACPI RSDP (Root System Description Pointer) structures
struct AcpiRsdp {
    char signature[8];      // "RSD PTR "
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;       // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;  // Physical address of RSDT
} __attribute__((packed));

struct AcpiRsdp20 {
    AcpiRsdp base;
    uint32_t length;
    uint64_t xsdt_address;  // Physical address of XSDT (64-bit)
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

// ACPI SDT Header (common to all tables)
struct AcpiSdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

// ACPI FADT (Fixed ACPI Description Table) - partial, we only need power management fields
struct AcpiFadt {
    AcpiSdtHeader header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved1;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;      // PM1a Control Block (we need this for shutdown)
    uint32_t pm1b_cnt_blk;      // PM1b Control Block (optional)
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    // ... more fields we don't need
} __attribute__((packed));

// ACPI functions
void acpi_init();
bool acpi_poweroff();
bool acpi_is_available();
