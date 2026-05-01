#include <drivers/bus/pci/msi.h>
#include <drivers/bus/pci/pci.h>
#include <drivers/storage/ahci.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/debug.h>
#include <kernel/fs/block_dev.h>
#include <kernel/fs/storage_guard.h>
#include <kernel/irq.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vmm.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

namespace {

static constexpr uint8_t PCI_CLASS_MASS_STORAGE = 0x01;
static constexpr uint8_t PCI_SUBCLASS_SATA = 0x06;
static constexpr uint8_t PCI_PROGIF_AHCI = 0x01;

static constexpr uint32_t AHCI_MAX_CONTROLLERS = 4;
static constexpr uint32_t AHCI_MAX_PORTS = 32;
static constexpr uint32_t AHCI_CMD_SLOT = 0;
static constexpr uint32_t AHCI_BOUNCE_BYTES = 64 * 1024;
static constexpr uint32_t AHCI_BOUNCE_SECTORS = AHCI_BOUNCE_BYTES / 512;
static constexpr uint64_t AHCI_CMD_TIMEOUT_MS = 3000;

static constexpr uint32_t ATA_DEV_BUSY = 0x80;
static constexpr uint32_t ATA_DEV_DRQ = 0x08;

static constexpr uint8_t ATA_CMD_IDENTIFY_DEVICE = 0xEC;
static constexpr uint8_t ATA_CMD_READ_DMA_EXT = 0x25;
static constexpr uint8_t ATA_CMD_WRITE_DMA_EXT = 0x35;

static constexpr uint32_t HBA_GHC_HR = 1u << 0;
static constexpr uint32_t HBA_GHC_IE = 1u << 1;
static constexpr uint32_t HBA_GHC_AE = 1u << 31;

static constexpr uint32_t HBA_CAP2_BOH = 1u << 0;
static constexpr uint32_t HBA_BOHC_BOS = 1u << 0;
static constexpr uint32_t HBA_BOHC_OOS = 1u << 1;
static constexpr uint32_t HBA_BOHC_BB = 1u << 4;

static constexpr uint32_t HBA_PxCMD_ST = 1u << 0;
static constexpr uint32_t HBA_PxCMD_SUD = 1u << 1;
static constexpr uint32_t HBA_PxCMD_POD = 1u << 2;
static constexpr uint32_t HBA_PxCMD_FRE = 1u << 4;
static constexpr uint32_t HBA_PxCMD_FR = 1u << 14;
static constexpr uint32_t HBA_PxCMD_CR = 1u << 15;

static constexpr uint32_t HBA_PxIS_TFES = 1u << 30;

static constexpr uint32_t HBA_PxSCTL_DET_NONE = 0;
static constexpr uint32_t HBA_PxSCTL_DET_INIT = 1;
static constexpr uint32_t HBA_PORT_DET_PRESENT = 3;

static constexpr uint32_t SATA_SIG_ATA = 0x00000101;
static constexpr uint32_t SATA_SIG_ATAPI = 0xEB140101;
static constexpr uint32_t SATA_SIG_SEMB = 0xC33C0101;
static constexpr uint32_t SATA_SIG_PM = 0x96690101;

struct [[gnu::packed]] HbaPortRegs
{
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t reserved1[11];
    uint32_t vendor[4];
};

struct [[gnu::packed]] HbaMem
{
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t reserved[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
    HbaPortRegs ports[AHCI_MAX_PORTS];
};

struct [[gnu::packed]] AhciCommandHeader
{
    uint8_t cfl;
    uint8_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct [[gnu::packed]] AhciPrdtEntry
{
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
};

struct [[gnu::packed]] AhciFisRegH2D
{
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t reserved0 : 3;
    uint8_t c : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved1[4];
};

struct [[gnu::packed]] AhciCommandTable
{
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    AhciPrdtEntry prdt[1];
};

struct AhciController;

struct AhciPortState
{
    bool present;
    uint8_t port_index;
    uint64_t sector_count;
    char model[64];
    BlockDevice block_dev;
    AhciController *controller;
    volatile HbaPortRegs *regs;
    DMAAllocation clb_dma;
    DMAAllocation fis_dma;
    DMAAllocation table_dma;
    DMAAllocation bounce_dma;
    Spinlock lock;
    volatile bool irq_seen;
};

struct AhciController
{
    bool initialized;
    PciDevice pci_dev;
    volatile HbaMem *abar;
    uint8_t irq_line;
    bool using_msix;
    bool using_msi;
    uint8_t vector;
    MsixState msix;
    AhciPortState ports[AHCI_MAX_PORTS];
};

static AhciController g_controllers[AHCI_MAX_CONTROLLERS];
static uint32_t g_controller_count = 0;
static uint32_t g_disk_count = 0;

static void ahci_delay_ms(uint64_t delay_ms)
{
    while (delay_ms != 0) {
        uint32_t chunk = delay_ms > 50u ? 50u : static_cast<uint32_t>(delay_ms);
        timer_poll_wait_ms(chunk);
        delay_ms -= chunk;
    }
}

static bool port_wait_idle(volatile HbaPortRegs *port, uint64_t timeout_ms)
{
    uint64_t waited_ms = 0;
    while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) != 0) {
        if (waited_ms >= timeout_ms) {
            DEBUG_WARN("ahci: port timeout waiting for idle (tfd=0x%x)", port->tfd);
            return false;
        }
        ahci_delay_ms(1);
        waited_ms++;
    }
    return true;
}

static bool port_stop_engine(volatile HbaPortRegs *port)
{
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    uint64_t waited_ms = 0;
    while (((port->cmd & HBA_PxCMD_CR) != 0) || ((port->cmd & HBA_PxCMD_FR) != 0)) {
        if (waited_ms >= 500)
            return false;
        ahci_delay_ms(1);
        waited_ms++;
    }
    return true;
}

static bool port_start_engine(volatile HbaPortRegs *port)
{
    uint64_t waited_ms = 0;
    while ((port->cmd & HBA_PxCMD_CR) != 0) {
        if (waited_ms >= 500) {
            DEBUG_WARN("ahci: engine start timeout (cmd=0x%x)", port->cmd);
            return false;
        }
        ahci_delay_ms(1);
        waited_ms++;
    }
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
    return true;
}

static uint32_t port_det(volatile HbaPortRegs *port)
{
    return port->ssts & 0x0F;
}

static uint32_t port_ipm(volatile HbaPortRegs *port)
{
    return (port->ssts >> 8) & 0x0F;
}

static bool port_link_up(volatile HbaPortRegs *port)
{
    return port_det(port) == HBA_PORT_DET_PRESENT;
}

static bool port_wait_link_up(volatile HbaPortRegs *port, uint64_t timeout_ms)
{
    uint64_t waited_ms = 0;
    while (!port_link_up(port)) {
        if (waited_ms >= timeout_ms)
            return false;
        ahci_delay_ms(1);
        waited_ms++;
    }
    return true;
}

static void port_issue_comreset(volatile HbaPortRegs *port, uint64_t settle_ms)
{
    uint32_t sctl = port->sctl;
    port->sctl = (sctl & ~0x0Fu) | HBA_PxSCTL_DET_INIT;
    ahci_delay_ms(5);
    port->sctl = (sctl & ~0x0Fu) | HBA_PxSCTL_DET_NONE;
    if (settle_ms != 0)
        ahci_delay_ms(settle_ms);
}

static bool port_prepare_link(AhciController *controller, uint32_t port_idx, volatile HbaPortRegs *port)
{
    (void)controller;
    if (!port_stop_engine(port))
        return false;

    port->is = 0xFFFFFFFFu;
    port->serr = 0xFFFFFFFFu;
    port->cmd |= HBA_PxCMD_SUD | HBA_PxCMD_POD;

    if (!port_wait_link_up(port, 50)) {
        port_issue_comreset(port, 20);
        if (!port_wait_link_up(port, 200)) {
            if (port_det(port) == 0 && port_ipm(port) == 0) {
                DEBUG_INFO("ahci: port %u has no SATA link, skipping (ssts=0x%x sig=0x%x serr=0x%x tfd=0x%x)", port_idx,
                           port->ssts, port->sig, port->serr, port->tfd);
            } else {
                DEBUG_WARN("ahci: port %u link did not come up (ssts=0x%x det=%u ipm=%u sig=0x%x serr=0x%x tfd=0x%x)",
                           port_idx, port->ssts, port_det(port), port_ipm(port), port->sig, port->serr, port->tfd);
            }
            return false;
        }
    }

    port->is = 0xFFFFFFFFu;
    port->serr = 0xFFFFFFFFu;
    return true;
}

static uint32_t port_read_signature(volatile HbaPortRegs *port)
{
    uint32_t sig = port->sig;
    if (sig != 0xFFFFFFFFu)
        return sig;

    for (uint64_t waited_ms = 0; waited_ms < 100; ++waited_ms) {
        ahci_delay_ms(1);
        sig = port->sig;
        if (sig != 0xFFFFFFFFu)
            break;
    }
    return sig;
}

static bool log_unsupported_signature(const AhciController *controller, uint32_t port_idx, uint32_t sig)
{
    if (!controller)
        return false;

    if (sig == SATA_SIG_ATAPI) {
        DEBUG_INFO("ahci: controller %u port %u has ATAPI device, skipping", controller->pci_dev.device, port_idx);
        return true;
    }
    if (sig == SATA_SIG_SEMB || sig == SATA_SIG_PM) {
        DEBUG_INFO("ahci: controller %u port %u has unsupported SATA enclosure/PM device, skipping",
                   controller->pci_dev.device, port_idx);
        return true;
    }
    return false;
}

static void log_identify_failure(const AhciController *controller, uint32_t port_idx, volatile HbaPortRegs *port)
{
    if (!controller || !port)
        return;

    uint32_t sig = port_read_signature(port);
    if (log_unsupported_signature(controller, port_idx, sig))
        return;

    if (sig == 0xFFFFFFFFu || sig == 0u) {
        DEBUG_INFO("ahci: controller %u port %u did not expose an ATA signature, skipping (sig=0x%x ssts=0x%x det=%u "
                   "ipm=%u serr=0x%x tfd=0x%x)",
                   controller->pci_dev.device, port_idx, sig, port->ssts, port_det(port), port_ipm(port), port->serr,
                   port->tfd);
        return;
    }

    DEBUG_WARN("ahci: controller %u port %u identify failed (sig=0x%x ssts=0x%x det=%u ipm=%u serr=0x%x tfd=0x%x)",
               controller->pci_dev.device, port_idx, sig, port->ssts, port_det(port), port_ipm(port), port->serr,
               port->tfd);
}

static void extract_model(const uint16_t *identify, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    size_t pos = 0;
    for (int i = 27; i <= 46 && pos + 1 < out_size; i++) {
        uint16_t word = identify[i];
        char hi = static_cast<char>(word >> 8);
        char lo = static_cast<char>(word & 0xFF);
        if (hi >= 32 && hi < 127 && pos + 1 < out_size)
            out[pos++] = hi;
        if (lo >= 32 && lo < 127 && pos + 1 < out_size)
            out[pos++] = lo;
    }
    while (pos > 0 && out[pos - 1] == ' ')
        pos--;
    out[pos] = '\0';
}

static bool ahci_exec(AhciPortState *state, uint8_t command, uint64_t lba, uint32_t sector_count, bool write,
                      uint32_t byte_count)
{
    if (!state || !state->regs)
        return false;

    volatile HbaPortRegs *port = state->regs;
    if (!port_wait_idle(port, 500))
        return false;

    auto *headers = reinterpret_cast<AhciCommandHeader *>(state->clb_dma.virt);
    auto *header = &headers[AHCI_CMD_SLOT];
    auto *table = reinterpret_cast<AhciCommandTable *>(state->table_dma.virt);
    kstring::memset(header, 0, sizeof(*header));
    kstring::memset(table, 0, sizeof(AhciCommandTable));

    header->cfl = sizeof(AhciFisRegH2D) / sizeof(uint32_t);
    header->flags = write ? (1u << 6) : 0u;
    header->prdtl = 1;
    header->ctba = static_cast<uint32_t>(state->table_dma.phys);
    header->ctbau = static_cast<uint32_t>(state->table_dma.phys >> 32);
    table->prdt[0].dba = static_cast<uint32_t>(state->bounce_dma.phys);
    table->prdt[0].dbau = static_cast<uint32_t>(state->bounce_dma.phys >> 32);
    table->prdt[0].dbc = (byte_count - 1) | (1u << 31);

    auto *fis = reinterpret_cast<AhciFisRegH2D *>(table->cfis);
    fis->fis_type = 0x27;
    fis->c = 1;
    fis->command = command;
    fis->device = 1u << 6;
    fis->lba0 = static_cast<uint8_t>(lba);
    fis->lba1 = static_cast<uint8_t>(lba >> 8);
    fis->lba2 = static_cast<uint8_t>(lba >> 16);
    fis->lba3 = static_cast<uint8_t>(lba >> 24);
    fis->lba4 = static_cast<uint8_t>(lba >> 32);
    fis->lba5 = static_cast<uint8_t>(lba >> 40);
    fis->countl = static_cast<uint8_t>(sector_count);
    fis->counth = static_cast<uint8_t>(sector_count >> 8);

    port->is = 0xFFFFFFFFu;
    state->irq_seen = false;
    port->ci = 1u << AHCI_CMD_SLOT;

    uint64_t waited_ms = 0;
    while ((port->ci & (1u << AHCI_CMD_SLOT)) != 0) {
        if ((port->is & HBA_PxIS_TFES) != 0)
            return false;
        if (waited_ms >= AHCI_CMD_TIMEOUT_MS)
            return false;
        ahci_delay_ms(1);
        waited_ms++;
    }

    return (port->is & HBA_PxIS_TFES) == 0;
}

static bool ahci_identify(AhciPortState *state)
{
    if (!state)
        return false;
    kstring::memset(reinterpret_cast<void *>(state->bounce_dma.virt), 0, 512);
    if (!ahci_exec(state, ATA_CMD_IDENTIFY_DEVICE, 0, 1, false, 512))
        return false;

    auto *identify = reinterpret_cast<uint16_t *>(state->bounce_dma.virt);
    extract_model(identify, state->model, sizeof(state->model));
    uint64_t total_sectors = ((uint64_t)identify[103] << 48) | ((uint64_t)identify[102] << 32) |
                             ((uint64_t)identify[101] << 16) | (uint64_t)identify[100];
    if (total_sectors == 0)
        total_sectors = ((uint64_t)identify[61] << 16) | (uint64_t)identify[60];
    state->sector_count = total_sectors;
    return total_sectors != 0;
}

static bool ahci_recover_port(AhciPortState *state)
{
    if (!state || !state->regs)
        return false;

    volatile HbaPortRegs *port = state->regs;
    port_stop_engine(port);
    port->is = 0xFFFFFFFFu;
    port->serr = 0xFFFFFFFFu;
    port->cmd |= HBA_PxCMD_SUD | HBA_PxCMD_POD;
    port_issue_comreset(port, 20);

    if (!port_wait_link_up(port, 1000))
        return false;

    if (!port_start_engine(port))
        return false;
    return ahci_identify(state);
}

static int64_t ahci_port_io(AhciPortState *state, uint64_t lba, uint32_t count, void *buffer, bool write)
{
    if (!state || !buffer || count == 0 || !state->present)
        return -1;
    if (lba + count > state->sector_count)
        return -1;

    spinlock_acquire(&state->lock);
    uint8_t *cursor = static_cast<uint8_t *>(buffer);
    uint32_t done = 0;

    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > AHCI_BOUNCE_SECTORS)
            chunk = AHCI_BOUNCE_SECTORS;
        uint32_t bytes = chunk * 512u;

        if (write)
            kstring::memcpy(reinterpret_cast<void *>(state->bounce_dma.virt), cursor + done * 512u, bytes);

        bool ok =
            ahci_exec(state, write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT, lba + done, chunk, write, bytes);
        if (!ok) {
            ok = ahci_recover_port(state) && ahci_exec(state, write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                                                       lba + done, chunk, write, bytes);
        }
        if (!ok) {
            spinlock_release(&state->lock);
            return -1;
        }

        if (!write)
            kstring::memcpy(cursor + done * 512u, reinterpret_cast<void *>(state->bounce_dma.virt), bytes);
        done += chunk;
    }

    spinlock_release(&state->lock);
    return count;
}

static int64_t ahci_read_blocks(BlockDevice *dev, uint64_t lba, uint32_t count, void *buffer)
{
    return ahci_port_io(static_cast<AhciPortState *>(dev ? dev->private_data : nullptr), lba, count, buffer, false);
}

static int64_t ahci_write_blocks(BlockDevice *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!storage_writes_allowed())
        return -1;
    return ahci_port_io(static_cast<AhciPortState *>(dev ? dev->private_data : nullptr), lba, count,
                        const_cast<void *>(buffer), true);
}

static void ahci_firmware_handoff(volatile HbaMem *abar)
{
    if (!abar || (abar->cap2 & HBA_CAP2_BOH) == 0)
        return;
    abar->bohc |= HBA_BOHC_OOS;
    uint64_t waited_ms = 0;
    while ((abar->bohc & HBA_BOHC_BOS) != 0 || (abar->bohc & HBA_BOHC_BB) != 0) {
        if (waited_ms >= 2000)
            break;
        ahci_delay_ms(1);
        waited_ms++;
    }
}

static void controller_configure_polled_mode(AhciController *controller)
{
    if (!controller)
        return;

    controller->using_msix = false;
    controller->using_msi = false;
    controller->irq_line = 0;
    controller->vector = 0;
    pci_disable_interrupts(&controller->pci_dev);
}

static void free_port_dma(AhciPortState *state)
{
    if (!state)
        return;
    if (state->clb_dma.size != 0) {
        vmm_free_dma(state->clb_dma);
        state->clb_dma = {};
    }
    if (state->fis_dma.size != 0) {
        vmm_free_dma(state->fis_dma);
        state->fis_dma = {};
    }
    if (state->table_dma.size != 0) {
        vmm_free_dma(state->table_dma);
        state->table_dma = {};
    }
    if (state->bounce_dma.size != 0) {
        vmm_free_dma(state->bounce_dma);
        state->bounce_dma = {};
    }
}

static bool port_rebase(AhciPortState *state)
{
    state->clb_dma = vmm_alloc_dma(1);
    state->fis_dma = vmm_alloc_dma(1);
    state->table_dma = vmm_alloc_dma(1);
    state->bounce_dma = vmm_alloc_dma(AHCI_BOUNCE_BYTES / 4096);
    if (state->clb_dma.size == 0 || state->fis_dma.size == 0 || state->table_dma.size == 0 ||
        state->bounce_dma.size == 0) {
        free_port_dma(state);
        return false;
    }

    kstring::zero_memory(reinterpret_cast<void *>(state->clb_dma.virt), state->clb_dma.size);
    kstring::zero_memory(reinterpret_cast<void *>(state->fis_dma.virt), state->fis_dma.size);
    kstring::zero_memory(reinterpret_cast<void *>(state->table_dma.virt), state->table_dma.size);
    kstring::zero_memory(reinterpret_cast<void *>(state->bounce_dma.virt), state->bounce_dma.size);

    if (!port_stop_engine(state->regs)) {
        free_port_dma(state);
        return false;
    }

    state->regs->clb = static_cast<uint32_t>(state->clb_dma.phys);
    state->regs->clbu = static_cast<uint32_t>(state->clb_dma.phys >> 32);
    state->regs->fb = static_cast<uint32_t>(state->fis_dma.phys);
    state->regs->fbu = static_cast<uint32_t>(state->fis_dma.phys >> 32);
    state->regs->is = 0xFFFFFFFFu;
    // Keep early AHCI boot in polled mode until the bare-metal path is solid.
    state->regs->ie = 0u;

    auto *headers = reinterpret_cast<AhciCommandHeader *>(state->clb_dma.virt);
    kstring::memset(headers, 0, sizeof(AhciCommandHeader) * 32);
    headers[AHCI_CMD_SLOT].prdtl = 1;
    headers[AHCI_CMD_SLOT].ctba = static_cast<uint32_t>(state->table_dma.phys);
    headers[AHCI_CMD_SLOT].ctbau = static_cast<uint32_t>(state->table_dma.phys >> 32);

    return port_start_engine(state->regs);
}

static bool register_disk(AhciPortState *state)
{
    if (!state)
        return false;

    char name_buf[16];
    name_buf[0] = '\0';
    kstring::strncpy(name_buf, "ahci", sizeof(name_buf) - 1);
    char num_buf[16];
    kstring::itoa(g_disk_count, num_buf, 10);
    kstring::strncat(name_buf, num_buf, sizeof(name_buf) - 1 - kstring::strlen(name_buf));

    state->block_dev = {};
    char *name = static_cast<char *>(malloc(kstring::strlen(name_buf) + 1));
    if (!name)
        return false;
    kstring::strncpy(name, name_buf, kstring::strlen(name_buf));
    name[kstring::strlen(name_buf)] = '\0';

    state->block_dev.name = name;
    kstring::strncpy(state->block_dev.model, state->model, sizeof(state->block_dev.model) - 1);
    if (state->model[0] != '\0')
        kstring::strncpy(state->block_dev.display_name, state->model, sizeof(state->block_dev.display_name) - 1);
    else
        kstring::strncpy(state->block_dev.display_name, name_buf, sizeof(state->block_dev.display_name) - 1);
    state->block_dev.block_size = 512;
    state->block_dev.total_blocks = state->sector_count;
    state->block_dev.read_blocks = ahci_read_blocks;
    state->block_dev.write_blocks = ahci_write_blocks;
    state->block_dev.private_data = state;
    block_dev_register(&state->block_dev);
    g_disk_count++;
    return true;
}

static bool init_port(AhciController *controller, uint32_t port_idx)
{
    auto *port = &controller->abar->ports[port_idx];
    if ((controller->abar->pi & (1u << port_idx)) == 0)
        return false;

    if (!port_prepare_link(controller, port_idx, port))
        return false;

    uint32_t sig = port_read_signature(port);
    if (log_unsupported_signature(controller, port_idx, sig))
        return false;
    auto *state = &controller->ports[port_idx];
    *state = {};
    state->present = true;
    state->port_index = static_cast<uint8_t>(port_idx);
    state->controller = controller;
    state->regs = port;
    state->lock = SPINLOCK_INIT;

    if (!port_rebase(state))
        return false;
    sig = port_read_signature(port);
    if (log_unsupported_signature(controller, port_idx, sig)) {
        free_port_dma(state);
        state->present = false;
        return false;
    }
    if (!ahci_identify(state)) {
        log_identify_failure(controller, port_idx, port);
        free_port_dma(state);
        state->present = false;
        return false;
    }
    if (!register_disk(state)) {
        free_port_dma(state);
        state->present = false;
        return false;
    }

    if (sig != SATA_SIG_ATA && sig != 0 && sig != 0xFFFFFFFFu) {
        DEBUG_WARN("ahci: controller %u port %u accepted non-standard signature 0x%x after successful identify",
                   controller->pci_dev.device, port_idx, sig);
    }
    DEBUG_INFO("ahci: port %u model=%s sectors=%llu", port_idx, state->model[0] ? state->model : "(unknown)",
               state->sector_count);
    return true;
}

static bool init_controller(const PciDevice &pci_dev)
{
    if (g_controller_count >= AHCI_MAX_CONTROLLERS)
        return false;
    auto *controller = &g_controllers[g_controller_count];
    *controller = {};
    controller->pci_dev = pci_dev;

    pci_enable_memory_space(&controller->pci_dev);
    pci_enable_bus_mastering(&controller->pci_dev);

    uint64_t bar_size = 0;
    uint64_t abar_phys = pci_get_bar(&controller->pci_dev, 5, &bar_size);
    if (abar_phys == 0)
        return false;
    abar_phys &= ~0x0FULL;
    if (bar_size < 0x1100)
        bar_size = 0x2000;
    uint64_t abar_virt = vmm_map_mmio(abar_phys, bar_size);
    if (abar_virt == 0)
        return false;

    controller->abar = reinterpret_cast<volatile HbaMem *>(abar_virt);
    ahci_firmware_handoff(controller->abar);
    controller_configure_polled_mode(controller);

    controller->abar->ghc |= HBA_GHC_AE;
    controller->abar->ghc |= HBA_GHC_HR;
    uint64_t waited_ms = 0;
    while ((controller->abar->ghc & HBA_GHC_HR) != 0) {
        if (waited_ms >= 1000)
            break;
        ahci_delay_ms(1);
        waited_ms++;
    }
    controller->abar->ghc |= HBA_GHC_AE;
    controller->abar->ghc &= ~HBA_GHC_IE;
    controller->abar->is = 0xFFFFFFFFu;

    bool any_port = false;
    for (uint32_t port = 0; port < AHCI_MAX_PORTS; port++) {
        if (init_port(controller, port))
            any_port = true;
    }

    controller->initialized = any_port;
    if (any_port)
        g_controller_count++;
    return any_port;
}

static void enumerate_controllers()
{
    for (uint16_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            uint16_t vendor = pci_config_read16(bus, dev, 0, PCI_VENDOR_ID);
            if (vendor == 0xFFFF)
                continue;

            DEBUG_INFO("ahci: scanning PCI device %02x:%02x", bus, dev);

            uint8_t header_type = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
            uint8_t max_func = (header_type & 0x80) ? PCI_MAX_FUNC : 1;
            for (uint8_t func = 0; func < max_func; func++) {
                vendor = pci_config_read16(bus, dev, func, PCI_VENDOR_ID);
                if (vendor == 0xFFFF)
                    continue;
                uint8_t class_code = pci_config_read8(bus, dev, func, PCI_CLASS);
                uint8_t subclass = pci_config_read8(bus, dev, func, PCI_SUBCLASS);
                uint8_t prog_if = pci_config_read8(bus, dev, func, PCI_PROG_IF);
                if (class_code != PCI_CLASS_MASS_STORAGE || subclass != PCI_SUBCLASS_SATA || prog_if != PCI_PROGIF_AHCI)
                    continue;

                PciDevice pci_dev = {};
                pci_dev.bus = bus;
                pci_dev.device = dev;
                pci_dev.function = func;
                pci_dev.vendor_id = vendor;
                pci_dev.device_id = pci_config_read16(bus, dev, func, PCI_DEVICE_ID);
                pci_dev.class_code = class_code;
                pci_dev.subclass = subclass;
                pci_dev.prog_if = prog_if;
                pci_dev.header_type = pci_config_read8(bus, dev, func, PCI_HEADER_TYPE);
                pci_dev.irq_line = pci_config_read8(bus, dev, func, PCI_INTERRUPT_LINE);
                init_controller(pci_dev);
            }
        }
    }
}

} // namespace

bool ahci_signature_requires_immediate_skip(uint32_t signature)
{
    return signature == 0xEB140101u || signature == 0xC33C0101u || signature == 0x96690101u;
}

void ahci_init()
{
    g_controller_count = 0;
    g_disk_count = 0;
    enumerate_controllers();
    if (g_disk_count == 0)
        DEBUG_INFO("ahci: no SATA disks detected");
}
