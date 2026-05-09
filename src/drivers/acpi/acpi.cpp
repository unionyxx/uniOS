#include <boot/boot_info.h>
#include <drivers/acpi/acpi.h>
#include <drivers/video/framebuffer.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>
#include <kernel/mm/vmm.h>
#include <libk/kstring.h>

// ACPI state
static bool acpi_available = false;
static uint32_t pm1a_cnt = 0;       // PM1a Control port
static uint32_t pm1b_cnt = 0;       // PM1b Control port (optional)
static uint16_t slp_typa = 0;       // Sleep type value for S5
static uint16_t slp_typb = 0;       // Sleep type value for S5 (optional)
static uint32_t smi_cmd_port = 0;   // SMI command port
static uint8_t acpi_enable_val = 0; // Value to write to enable ACPI
static AcpiGas g_reset_reg = {};    // ACPI Reset Register
static uint8_t g_reset_value = 0;   // ACPI Reset Value
static bool g_rsdp_logged = false;


// Sleep enable bit
#define ACPI_SLP_EN (1 << 13)

// QEMU/Bochs shutdown port (fallback)
#define QEMU_SHUTDOWN_PORT 0x604
#define QEMU_SHUTDOWN_VALUE 0x2000
#define BOCHS_SHUTDOWN_PORT 0xB004
#define VIRTUALBOX_SHUTDOWN_PORT 0x4004
#define VIRTUALBOX_SHUTDOWN_VALUE 0x3400

// Validate RSDP checksum
static bool rsdp_checksum_valid(const AcpiRsdp *rsdp)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(rsdp);
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

// Validate SDT checksum
static bool sdt_checksum_valid(const AcpiSdtHeader *header)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(header);
    uint8_t sum = 0;
    for (uint32_t i = 0; i < header->length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static AcpiRsdp *find_rsdp_in_range(uint64_t start, uint64_t end)
{
    for (uint64_t addr = start; addr < end; addr += 16) {
        uint8_t *ptr = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(addr));

        if (ptr[0] == 'R' && ptr[1] == 'S' && ptr[2] == 'D' && ptr[3] == ' ' && ptr[4] == 'P' && ptr[5] == 'T' &&
            ptr[6] == 'R' && ptr[7] == ' ') {
            AcpiRsdp *rsdp = reinterpret_cast<AcpiRsdp *>(ptr);
            if (rsdp_checksum_valid(rsdp)) {
                return rsdp;
            }
        }
    }
    return nullptr;
}

static AcpiRsdp *find_rsdp()
{
    const BootInfo *boot_info = boot_get_info();
    if (boot_info && boot_info->rsdp_address) {
        if (boot_info->rsdp_address < 0x0000800000000000ULL)
            return reinterpret_cast<AcpiRsdp *>(vmm_phys_to_virt(boot_info->rsdp_address));
        return reinterpret_cast<AcpiRsdp *>(boot_info->rsdp_address);
    }

    uint16_t ebda_segment = *reinterpret_cast<uint16_t *>(vmm_phys_to_virt(0x40E));
    uint64_t ebda_addr = static_cast<uint64_t>(ebda_segment) << 4;

    AcpiRsdp *rsdp = find_rsdp_in_range(ebda_addr, ebda_addr + 0x400);
    if (rsdp)
        return rsdp;

    return find_rsdp_in_range(0xE0000, 0x100000);
}

// Parse DSDT/SSDT to find \_S5 sleep type values
// This is a simplified parser - real ACPI requires full AML parsing
static bool find_s5_in_dsdt(uint64_t dsdt_phys)
{
    AcpiSdtHeader *dsdt = reinterpret_cast<AcpiSdtHeader *>(vmm_phys_to_virt(dsdt_phys));

    if (!dsdt || dsdt->length < sizeof(AcpiSdtHeader) || !sdt_checksum_valid(dsdt)) {
        return false;
    }

    // Search for "_S5_" in the DSDT
    uint8_t *data = reinterpret_cast<uint8_t *>(dsdt);
    uint32_t length = dsdt->length;

    for (uint32_t i = sizeof(AcpiSdtHeader); i + 4 < length; i++) {
        // Look for "_S5_" package
        if (data[i] == '_' && data[i + 1] == 'S' && data[i + 2] == '5' && data[i + 3] == '_') {
            // Found \_S5_ - now parse the package
            // Skip ahead to find the package data
            // The structure is typically: "_S5_" + NameOp(0x12) + PkgLength + NumElements + ByteData...
            uint32_t j = i + 4;

            // Skip any AML prefix opcodes
            while (j < length && (data[j] == 0x08 || data[j] == 0x12)) {
                j++;
            }

            // Look for PackageOp (0x12) or directly find byte values
            while (j + 2 < length) {
                if (data[j] == 0x12) {
                    // Package found, skip package length
                    j++;
                    if ((data[j] & 0xC0) == 0)
                        j += 1;
                    else if ((data[j] & 0xC0) == 0x40)
                        j += 2;
                    else if ((data[j] & 0xC0) == 0x80)
                        j += 3;
                    else
                        j += 4;

                    // Skip element count
                    if (j >= length)
                        return false;
                    j++;
                    if (j >= length)
                        return false;

                    // First element is SLP_TYPa
                    if (data[j] == 0x0A && j + 1 < length) { // BytePrefix
                        slp_typa = data[j + 1] << 10;
                        j += 2;
                    } else if (data[j] == 0x0B && j + 2 < length) { // WordPrefix
                        slp_typa = (data[j + 1] | (data[j + 2] << 8)) << 10;
                        j += 3;
                    } else if (data[j] < 64) { // Small integer
                        slp_typa = data[j] << 10;
                        j++;
                    }

                    // Second element is SLP_TYPb (optional)
                    if (j < length) {
                        if (data[j] == 0x0A && j + 1 < length) {
                            slp_typb = data[j + 1] << 10;
                        } else if (data[j] < 64) {
                            slp_typb = data[j] << 10;
                        }
                    }

                    return true;
                }
                j++;
            }
        }
    }

    // Fallback: use common default values
    // Most systems use SLP_TYP = 5 for S5 (sleep type 5)
    slp_typa = 5 << 10;
    slp_typb = 5 << 10;
    return true;
}

void *acpi_find_table(const char *signature)
{
    AcpiRsdp *rsdp = find_rsdp();
    if (!rsdp)
        return nullptr;

    uint64_t rsdt_phys;
    bool use_xsdt = false;

    if (rsdp->revision >= 2) {
        AcpiRsdp20 *rsdp20 = reinterpret_cast<AcpiRsdp20 *>(rsdp);
        if (rsdp20->xsdt_address != 0) {
            rsdt_phys = rsdp20->xsdt_address;
            use_xsdt = true;
        } else {
            rsdt_phys = rsdp->rsdt_address;
        }
    } else {
        rsdt_phys = rsdp->rsdt_address;
    }

    AcpiSdtHeader *rsdt = reinterpret_cast<AcpiSdtHeader *>(vmm_phys_to_virt(rsdt_phys));
    if (!sdt_checksum_valid(rsdt))
        return nullptr;

    uint32_t entries = (rsdt->length - sizeof(AcpiSdtHeader)) / (use_xsdt ? 8 : 4);
    uint8_t *entry_base = reinterpret_cast<uint8_t *>(rsdt) + sizeof(AcpiSdtHeader);

    for (uint32_t i = 0; i < entries; i++) {
        uint64_t table_phys;
        if (use_xsdt) {
            table_phys = *reinterpret_cast<uint64_t *>(entry_base + i * 8);
        } else {
            table_phys = *reinterpret_cast<uint32_t *>(entry_base + i * 4);
        }

        AcpiSdtHeader *table = reinterpret_cast<AcpiSdtHeader *>(vmm_phys_to_virt(table_phys));
        if (kstring::strncmp(table->signature, signature, 4) == 0) {
            return static_cast<void *>(table);
        }
    }

    return nullptr;
}

void acpi_init()
{
    // Find RSDP
    AcpiRsdp *rsdp = find_rsdp();
    if (!rsdp) {
        DEBUG_WARN("acpi: rsdp not found");
        return;
    }

    if (!g_rsdp_logged) {
        BOOT_LOG("ACPI RSDP at %p", rsdp);
        g_rsdp_logged = true;
    }

    // Get RSDT or XSDT
    uint64_t rsdt_phys;
    bool use_xsdt = false;

    if (rsdp->revision >= 2) {
        AcpiRsdp20 *rsdp20 = (AcpiRsdp20 *)rsdp;
        if (rsdp20->xsdt_address != 0) {
            rsdt_phys = rsdp20->xsdt_address;
            use_xsdt = true;
        } else {
            rsdt_phys = rsdp->rsdt_address;
        }
    } else {
        rsdt_phys = rsdp->rsdt_address;
    }

    AcpiSdtHeader *rsdt = reinterpret_cast<AcpiSdtHeader *>(vmm_phys_to_virt(rsdt_phys));
    if (!sdt_checksum_valid(rsdt)) {
        DEBUG_ERROR("acpi: rsdt checksum failed");
        return;
    }

    // Find FADT in RSDT/XSDT entries
    uint32_t entries = (rsdt->length - sizeof(AcpiSdtHeader)) / (use_xsdt ? 8 : 4);
    uint8_t *entry_base = reinterpret_cast<uint8_t *>(rsdt) + sizeof(AcpiSdtHeader);

    for (uint32_t i = 0; i < entries; i++) {
        uint64_t table_phys;
        if (use_xsdt) {
            table_phys = *(uint64_t *)(entry_base + i * 8);
        } else {
            table_phys = *(uint32_t *)(entry_base + i * 4);
        }

        AcpiSdtHeader *table = (AcpiSdtHeader *)vmm_phys_to_virt(table_phys);

        // Check for FACP (FADT signature in ACPI)
        if (table->signature[0] == 'F' && table->signature[1] == 'A' && table->signature[2] == 'C' &&
            table->signature[3] == 'P') {
            AcpiFadt *fadt = reinterpret_cast<AcpiFadt *>(table);
            pm1a_cnt = fadt->pm1a_cnt_blk;
            pm1b_cnt = fadt->pm1b_cnt_blk;
            smi_cmd_port = fadt->smi_cmd;
            acpi_enable_val = fadt->acpi_enable;
            g_reset_reg = fadt->reset_reg;
            g_reset_value = fadt->reset_value;

            // Parse DSDT to find S5 sleep type
            if (fadt->dsdt) {
                find_s5_in_dsdt(fadt->dsdt);
            }

            acpi_available = true;
            DEBUG_SUCCESS("ACPI ready (PM1a=0x%x, Reset=0x%lx)", pm1a_cnt, g_reset_reg.address);
            return;
        }
    }

    DEBUG_WARN("acpi: fadt not found");
}

bool acpi_is_available()
{
    return acpi_available;
}

// SCI_EN bit in PM1_CNT register
#define ACPI_SCI_EN (1 << 0)

bool acpi_reboot()
{
    if (!acpi_available || g_reset_reg.address == 0)
        return false;

    DEBUG_INFO("acpi: attempting hardware reset via 0x%lx...", g_reset_reg.address);
    asm volatile("cli");

    if (g_reset_reg.address_space == 1) { // System I/O
        outb((uint16_t)g_reset_reg.address, g_reset_value);
    } else if (g_reset_reg.address_space == 0) { // System Memory
        // We'd need to map this if it's not in HHDM or already mapped
        // For simplicity, we assume HHDM or identity for now if low enough
        if (g_reset_reg.address < 0x100000000ULL) {
            uint8_t *ptr = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(g_reset_reg.address));
            *ptr = g_reset_value;
        }
    }

    for (volatile int i = 0; i < 1000000; i++)
        ;
    return false;
}

bool acpi_poweroff()
{
    DEBUG_INFO("acpi: starting shutdown sequence...");

    if (acpi_available && pm1a_cnt != 0) {
        asm volatile("cli");

        uint16_t pm1_value = inw(pm1a_cnt);
        if (!(pm1_value & ACPI_SCI_EN)) {
            if (smi_cmd_port != 0 && acpi_enable_val != 0) {
                outb(smi_cmd_port, acpi_enable_val);

                for (int i = 0; i < 1000; i++) {
                    pm1_value = inw(pm1a_cnt);
                    if (pm1_value & ACPI_SCI_EN)
                        break;
                    for (volatile int j = 0; j < 10000; j++)
                        ;
                }
            }
        }

        if (slp_typa != 0) {
            outw(pm1a_cnt, slp_typa | ACPI_SLP_EN);
            if (pm1b_cnt != 0)
                outw(pm1b_cnt, slp_typb | ACPI_SLP_EN);
            io_wait();
            for (volatile int i = 0; i < 1000000; i++)
                ;
        }

        static const uint16_t common_slp_types[] = {
            (5 << 10), // Most common: SLP_TYP = 5
            (0 << 10), // SLP_TYP = 0 (older systems)
            (1 << 10), (2 << 10), (3 << 10), (4 << 10), (6 << 10), (7 << 10),
        };

        for (int i = 0; i < 8; i++) {
            outw(pm1a_cnt, common_slp_types[i] | ACPI_SLP_EN);
            if (pm1b_cnt != 0) {
                outw(pm1b_cnt, common_slp_types[i] | ACPI_SLP_EN);
            }
            io_wait();
            for (volatile int j = 0; j < 100000; j++)
                ;
        }
    }

    DEBUG_WARN("acpi: fallback to virtual machine shutdown ports...");

    outw(QEMU_SHUTDOWN_PORT, QEMU_SHUTDOWN_VALUE);
    io_wait();
    outw(BOCHS_SHUTDOWN_PORT, QEMU_SHUTDOWN_VALUE);
    io_wait();
    outw(VIRTUALBOX_SHUTDOWN_PORT, VIRTUALBOX_SHUTDOWN_VALUE);
    io_wait();

    return false;
}
