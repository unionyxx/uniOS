#include <boot/boot_info.h>
#include <kernel/boot_display_timing.h>
#include <kernel/debug.h>
#include <kernel/mm/vmm.h>
#include <libk/kstring.h>

namespace {

struct EfiGuid
{
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t d[8];
};

struct EfiTableHeader
{
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct EfiConfigurationTable
{
    EfiGuid vendor_guid;
    void *vendor_table;
};

struct EfiSystemTable
{
    EfiTableHeader hdr;
    void *firmware_vendor;
    uint32_t firmware_revision;
    uint32_t pad0;
    void *console_in_handle;
    void *con_in;
    void *console_out_handle;
    void *con_out;
    void *standard_error_handle;
    void *std_err;
    void *runtime_services;
    void *boot_services;
    uint64_t number_of_table_entries;
    EfiConfigurationTable *configuration_table;
};

static constexpr uint64_t k_efi_system_table_signature = 0x5453595320494249ULL;
static constexpr EfiGuid k_boot_display_timing_guid = {
    0x7f7d5f2a, 0x5ebd, 0x4a51, {0xa8, 0x6f, 0x72, 0x7a, 0x5a, 0x19, 0x31, 0x0c}};
static constexpr uint64_t k_max_efi_config_table_entries = 256;

static BootDisplayTiming s_boot_display_timing = {};
static bool s_boot_display_timing_valid = false;

static bool efi_guid_equal(const EfiGuid &a, const EfiGuid &b)
{
    return a.a == b.a && a.b == b.b && a.c == b.c && kstring::memcmp(a.d, b.d, sizeof(a.d)) == 0;
}

template <typename T>
static const T *efi_ptr_to_kernel_ptr(uint64_t addr)
{
    if (addr == 0)
        return nullptr;
    if (addr < 0x0000800000000000ULL) {
        return reinterpret_cast<const T *>(vmm_phys_to_virt(addr));
    }
    return reinterpret_cast<const T *>(addr);
}

static bool boot_display_timing_sane(const BootDisplayTiming &timing)
{
    if (timing.revision != BOOT_DISPLAY_TIMING_REVISION)
        return false;
    if ((timing.flags & BOOT_DISPLAY_TIMING_FLAG_EXACT_ACTIVE) == 0)
        return false;
    if (timing.width == 0 || timing.height == 0)
        return false;
    if (timing.refresh_millihz < 1000u || timing.refresh_millihz > 1000000u)
        return false;
    if (timing.pixel_clock_khz == 0 || timing.h_total == 0 || timing.v_total == 0)
        return false;
    if (timing.width > timing.h_total || timing.height > timing.v_total)
        return false;
    return true;
}

static const char *format_millihz_fraction(uint32_t refresh_millihz, char out[4])
{
    uint32_t fraction = refresh_millihz % 1000u;
    out[0] = (char)('0' + (fraction / 100u) % 10u);
    out[1] = (char)('0' + (fraction / 10u) % 10u);
    out[2] = (char)('0' + (fraction % 10u));
    out[3] = '\0';
    return out;
}

} // namespace

void boot_display_timing_init(uint64_t efi_system_table_addr, uint64_t firmware_type)
{
    s_boot_display_timing = {};
    s_boot_display_timing_valid = false;

    if (firmware_type != BOOT_FIRMWARE_UEFI64 || efi_system_table_addr == 0) {
        DEBUG_TRACE("display timing handoff unavailable on non-UEFI boot");
        return;
    }

    const EfiSystemTable *system_table = efi_ptr_to_kernel_ptr<EfiSystemTable>(efi_system_table_addr);
    if (!system_table || system_table->hdr.signature != k_efi_system_table_signature) {
        BOOT_WARN("display timing handoff unavailable: invalid EFI system table");
        return;
    }
    if (!system_table->configuration_table || system_table->number_of_table_entries == 0) {
        DEBUG_TRACE("display timing handoff unavailable: no EFI configuration table");
        return;
    }
    if (system_table->number_of_table_entries > k_max_efi_config_table_entries) {
        BOOT_WARN("Display: EFI system table entry count %llu is unreasonable, exact timing handoff unavailable",
                  (unsigned long long)system_table->number_of_table_entries);
        return;
    }

    const EfiConfigurationTable *config_tables =
        efi_ptr_to_kernel_ptr<EfiConfigurationTable>(reinterpret_cast<uint64_t>(system_table->configuration_table));
    if (!config_tables) {
        BOOT_WARN("Display: EFI configuration table pointer is invalid, exact timing handoff unavailable");
        return;
    }

    for (uint64_t i = 0; i < system_table->number_of_table_entries; i++) {
        const EfiConfigurationTable &entry = config_tables[i];
        if (!efi_guid_equal(entry.vendor_guid, k_boot_display_timing_guid))
            continue;
        if (!entry.vendor_table) {
            BOOT_WARN("Display: EFI exact timing handoff entry is present but empty");
            continue;
        }

        const BootDisplayTiming *timing =
            efi_ptr_to_kernel_ptr<BootDisplayTiming>(reinterpret_cast<uint64_t>(entry.vendor_table));
        if (!timing || !boot_display_timing_sane(*timing)) {
            BOOT_WARN("Display: EFI exact timing handoff present but invalid");
            return;
        }

        s_boot_display_timing = *timing;
        s_boot_display_timing_valid = true;
        char frac_buf[4];
        BOOT_LOG("Display: exact boot timing handoff found: %ux%u @ %u.%s Hz (pixel clock %u kHz, totals %ux%u%s)",
                 timing->width, timing->height, timing->refresh_millihz / 1000u,
                 format_millihz_fraction(timing->refresh_millihz, frac_buf), timing->pixel_clock_khz, timing->h_total,
                 timing->v_total, timing->interlaced ? ", interlaced" : "");
        return;
    }

    BOOT_LOG("Display: EFI exact timing handoff not published by boot stage");
}

bool boot_display_timing_get(BootDisplayTiming *out_timing)
{
    if (!out_timing || !s_boot_display_timing_valid)
        return false;
    *out_timing = s_boot_display_timing;
    return true;
}
