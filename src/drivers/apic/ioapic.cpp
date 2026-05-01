#include <drivers/apic/ioapic.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/debug.h>
#include <kernel/mm/vmm.h>
#include <stdint.h>

#define IOREGSEL 0x00
#define IOWIN 0x10

#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REDIR_BASE 0x10

#define IOAPIC_REDIR_LOW_ACTIVE (1u << 13)
#define IOAPIC_REDIR_LEVEL (1u << 15)
#define IOAPIC_REDIR_MASKED (1u << 16)

static IoApic g_ioapics[8];
static int g_num_ioapics = 0;

static Iso g_isos[16];
static int g_num_isos = 0;

static uint32_t ioapic_read(uint64_t base, uint8_t reg)
{
    *reinterpret_cast<volatile uint32_t *>(base + IOREGSEL) = reg;
    return *reinterpret_cast<volatile uint32_t *>(base + IOWIN);
}

static void ioapic_write(uint64_t base, uint8_t reg, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + IOREGSEL) = reg;
    *reinterpret_cast<volatile uint32_t *>(base + IOWIN) = value;
}

static const Iso *ioapic_lookup_iso(uint8_t irq)
{
    for (int i = 0; i < g_num_isos; ++i) {
        if (g_isos[i].irq == irq)
            return &g_isos[i];
    }
    return nullptr;
}

static IoApic *ioapic_find_for_gsi(uint32_t gsi)
{
    for (int i = 0; i < g_num_ioapics; ++i) {
        const uint32_t first = g_ioapics[i].gsi_base;
        const uint32_t last = g_ioapics[i].gsi_base + g_ioapics[i].max_interrupts; // inclusive max redir entry

        if (gsi >= first && gsi <= last)
            return &g_ioapics[i];
    }
    return nullptr;
}

static bool ioapic_polarity_low(uint16_t flags)
{
    switch (flags & 0x3) {
        case 0x0: // bus conform
        case 0x1: // active high
            return false;
        case 0x3: // active low
            return true;
        default:
            BOOT_WARN("IOAPIC: reserved polarity encoding 0x%x in ISO flags", flags & 0x3);
            return false;
    }
}

static bool ioapic_trigger_level(uint16_t flags)
{
    switch ((flags >> 2) & 0x3) {
        case 0x0: // bus conform (ISA default is edge)
        case 0x1: // edge
            return false;
        case 0x3: // level
            return true;
        default:
            BOOT_WARN("IOAPIC: reserved trigger encoding 0x%x in ISO flags", (flags >> 2) & 0x3);
            return false;
    }
}

static void ioapic_mask_redir(const IoApic &ioapic, uint32_t index)
{
    const uint8_t low_index = static_cast<uint8_t>(IOAPIC_REDIR_BASE + index * 2);
    uint32_t low = ioapic_read(ioapic.base, low_index);
    low |= IOAPIC_REDIR_MASKED;
    ioapic_write(ioapic.base, low_index, low);
}

static void ioapic_mask_all(const IoApic &ioapic)
{
    for (uint32_t i = 0; i <= ioapic.max_interrupts; ++i) {
        const uint8_t low_index = static_cast<uint8_t>(IOAPIC_REDIR_BASE + i * 2);
        const uint8_t high_index = static_cast<uint8_t>(low_index + 1);

        ioapic_write(ioapic.base, high_index, 0);
        ioapic_write(ioapic.base, low_index, IOAPIC_REDIR_MASKED);
    }
}

void ioapic_init()
{
    AcpiMadtHeader *madt = reinterpret_cast<AcpiMadtHeader *>(acpi_find_table("APIC"));
    if (!madt) {
        BOOT_WARN("APIC: MADT not found, IOAPIC not initialized");
        return;
    }

    g_num_ioapics = 0;
    g_num_isos = 0;

    uint8_t *ptr = reinterpret_cast<uint8_t *>(madt) + sizeof(AcpiMadtHeader);
    uint8_t *end = reinterpret_cast<uint8_t *>(madt) + madt->header.length;

    while (ptr + sizeof(AcpiMadtRecord) <= end) {
        auto *record = reinterpret_cast<AcpiMadtRecord *>(ptr);

        if (record->length < sizeof(AcpiMadtRecord) || ptr + record->length > end) {
            BOOT_WARN("MADT: malformed record type %u length %u", record->type, record->length);
            break;
        }

        switch (record->type) {
            case 1: { // I/O APIC
                if (record->length < sizeof(AcpiMadtIoApic)) {
                    BOOT_WARN("MADT: short IOAPIC record");
                    break;
                }

                if (g_num_ioapics >= static_cast<int>(sizeof(g_ioapics) / sizeof(g_ioapics[0]))) {
                    BOOT_WARN("IOAPIC: too many IOAPICs, ignoring extras");
                    break;
                }

                auto *ioapic = reinterpret_cast<AcpiMadtIoApic *>(ptr);
                const uint64_t base = vmm_map_mmio(ioapic->io_apic_address, 0x1000);
                if (!base) {
                    BOOT_WARN("IOAPIC: failed to map MMIO at 0x%x", ioapic->io_apic_address);
                    break;
                }

                const uint32_t ver = ioapic_read(base, IOAPIC_REG_VER);
                const uint32_t max_redir = (ver >> 16) & 0xFF;

                g_ioapics[g_num_ioapics] = {base, ioapic->global_system_interrupt_base, max_redir};
                ioapic_mask_all(g_ioapics[g_num_ioapics]);

                BOOT_LOG("IOAPIC %d: phys=0x%x gsi=%u-%u ver=0x%x", g_num_ioapics, ioapic->io_apic_address,
                         ioapic->global_system_interrupt_base, ioapic->global_system_interrupt_base + max_redir, ver);

                ++g_num_ioapics;
                break;
            }

            case 2: { // Interrupt Source Override
                if (record->length < sizeof(AcpiMadtIso)) {
                    BOOT_WARN("MADT: short ISO record");
                    break;
                }

                if (g_num_isos >= static_cast<int>(sizeof(g_isos) / sizeof(g_isos[0]))) {
                    BOOT_WARN("IOAPIC: too many ISOs, ignoring extras");
                    break;
                }

                auto *iso = reinterpret_cast<AcpiMadtIso *>(ptr);

                if (iso->bus != 0) {
                    BOOT_WARN("MADT: unsupported ISO bus %u for source %u", iso->bus, iso->irq);
                    break;
                }

                g_isos[g_num_isos++] = {iso->irq, iso->gsi, iso->flags};

                BOOT_LOG("MADT ISO: IRQ %u -> GSI %u flags=0x%x", iso->irq, iso->gsi, iso->flags);
                break;
            }

            default:
                break;
        }

        ptr += record->length;
    }

    if (g_num_ioapics > 0)
        pic_disable();
}

bool ioapic_is_ready()
{
    return g_num_ioapics > 0;
}

uint32_t ioapic_irq_to_gsi(uint8_t irq)
{
    if (const Iso *iso = ioapic_lookup_iso(irq))
        return iso->gsi;

    return irq;
}

void ioapic_set_entry(uint8_t irq, uint8_t vector)
{
    const Iso *iso = ioapic_lookup_iso(irq);
    const uint32_t gsi = iso ? iso->gsi : irq;

    IoApic *target = ioapic_find_for_gsi(gsi);
    if (!target) {
        BOOT_ERROR("IOAPIC: no controller for IRQ %u (GSI %u)", irq, gsi);
        return;
    }

    const uint32_t relative_gsi = gsi - target->gsi_base;
    const uint8_t low_index = static_cast<uint8_t>(IOAPIC_REDIR_BASE + relative_gsi * 2);
    const uint8_t high_index = static_cast<uint8_t>(low_index + 1);

    uint32_t low = vector; // fixed delivery, physical destination, unmasked

    if (iso) {
        if (ioapic_polarity_low(iso->flags))
            low |= IOAPIC_REDIR_LOW_ACTIVE;
        if (ioapic_trigger_level(iso->flags))
            low |= IOAPIC_REDIR_LEVEL;
    }

    ioapic_mask_redir(*target, relative_gsi);

    ioapic_write(target->base, high_index, apic_get_current_id() << 24);
    ioapic_write(target->base, low_index, low);
}
