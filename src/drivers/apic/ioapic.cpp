#include <drivers/apic/ioapic.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/cpu.h>
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
    const auto *madt = reinterpret_cast<const AcpiMadtHeader *>(acpi_find_table("APIC"));
    if (!madt) {
        BOOT_WARN("APIC: MADT not found, IOAPIC not initialized");
        return;
    }

    g_num_ioapics = 0;
    g_num_isos = 0;

    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(madt) + sizeof(AcpiMadtHeader);
    const uint8_t *end = reinterpret_cast<const uint8_t *>(madt) + madt->header.length;

    while (ptr + sizeof(AcpiMadtRecord) <= end) {
        const auto *record = reinterpret_cast<const AcpiMadtRecord *>(ptr);

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

                const auto *ioapic = reinterpret_cast<const AcpiMadtIoApic *>(ptr);
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

                const auto *iso = reinterpret_cast<const AcpiMadtIso *>(ptr);

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

// ---------------------------------------------------------------------------
// IRQ destination policy (spec: scheduler modernization, phase 2 — "IRQ
// distribution").
//
// Device-IRQ destinations are picked at registration time by a round-robin
// over the CPUs that are online *at registration time*. Every registration
// site today (PS/2 keyboard/mouse, xHCI legacy line, MSI/MSI-X setup) runs
// on the BSP during the boot critical path, before smp_init() starts the
// APs — so the online set at registration is {BSP} and every entry still
// lands on the BSP. That is the accepted behavior: the spread engages
// unchanged for anything that registers after SMP bring-up (e.g. phase-4
// IRQ-mode NICs) or when the boot order moves registration later. There is
// no CPU hotplug, so a CPU that is online stays online.
//
// Pinned carve-out: ISA IRQ 0 (PIT legacy line) and ISA IRQ 2 (PIC cascade)
// are routed explicitly to the BSP instead of round-robining — the legacy
// timer path must never migrate off the BSP. Per-CPU local vectors (LAPIC
// timer, spurious, error, thermal) are LVT entries programmed in irq.cpp
// (apic_timer_init / apic_enable_this_core) and never pass through the
// IOAPIC at all.
//
// Destination IDs are 8-bit *physical* APIC IDs taken from PerCpu::apic_id
// (CPUID/MADT-sourced: cpu_init fills slot 0, start_ap fills AP slots).
// irq.cpp forces xAPIC mode (apic_force_xapic_mode) before the IOAPIC is
// programmed, so the physical-destination encodings here and in MSI/MSI-X
// addresses stay valid; x2APIC IDs wider than 8 bits would not fit, which
// the CONFIG_SMP_MAX_CPUS design point precludes.
// ---------------------------------------------------------------------------

// ISA lines that must not round-robin. Everything else registered through
// ioapic_set_entry takes the next online CPU.
constexpr uint8_t kIsaIrqPit = 0;
constexpr uint8_t kIsaIrqCascade = 2;

// Round-robin cursor over g_cpus[] slots. Registration is boot-time BSP
// code (interrupts off, APs parked), so an unsynchronized counter is
// race-free today; if a post-SMP registration path ever appears, a race can
// only skew the distribution (two registrations sharing a CPU), never route
// to an offline one — the returned slot is always observed online under the
// acquire load below.
static uint32_t g_irq_dest_cursor = 0;

// Returns the g_cpus[] slot of the next online CPU after the cursor.
static uint32_t irq_next_destination_slot()
{
    for (uint32_t step = 0; step < CONFIG_SMP_MAX_CPUS; ++step) {
        const uint32_t slot = (g_irq_dest_cursor + 1u + step) % CONFIG_SMP_MAX_CPUS;
        // Acquire pairs with the release store in scheduler_enter_idle():
        // once online is observed set, this slot's apic_id (written by
        // start_ap on the BSP before the release) is visible here.
        if (__atomic_load_n(&g_cpus[slot].online, __ATOMIC_ACQUIRE)) {
            g_irq_dest_cursor = slot;
            return slot;
        }
    }
    return 0; // Unreachable: the BSP is online from cpu_init() onward.
}

uint32_t irq_next_destination_apic()
{
    return g_cpus[irq_next_destination_slot()].apic_id;
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

    // Destination: pinned legacy lines stay on the BSP (g_cpus[0] is the BSP
    // by the PerCpu ABI); every other line takes the round-robin above.
    uint32_t dest_slot = 0;
    bool bsp_pinned = true;
    if (irq != kIsaIrqPit && irq != kIsaIrqCascade) {
        dest_slot = irq_next_destination_slot();
        bsp_pinned = false;
    }
    const uint32_t dest_apic = g_cpus[dest_slot].apic_id;

    ioapic_mask_redir(*target, relative_gsi);

    ioapic_write(target->base, high_index, dest_apic << 24);
    ioapic_write(target->base, low_index, low);

    BOOT_LOG("IOAPIC: IRQ %u (GSI %u) vector %u -> cpu %u (APIC %u)%s", irq, gsi, vector, dest_slot, dest_apic,
             bsp_pinned ? " [BSP-pinned]" : "");
}
