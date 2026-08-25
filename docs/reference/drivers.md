# Drivers: PCI, ACPI, APIC

Driver code lives in `src/drivers/`. There is no unified driver model: each driver self-scans PCI by class or vendor/device and claims its BARs at fixed init call sites in `kmain` and the deferred boot task.

## PCI

`src/drivers/bus/pci/pci.cpp` supports both config mechanisms:

- **ECAM** (MMIO) parsed from the ACPI MCFG table — primary path, full 4 KiB per function, up to 4 segments cached.
- **Mechanism #1** I/O ports `0xCF8`/`0xCFC` — fallback, first 256 bytes only.

Discovery scans bus 0-255, device 0-31, function 0-7 (multi-function only when the header type says so). Helpers exist for xHCI, AC97, HDA (vendor-restricted), and display devices.

BAR handling detects I/O vs memory, 32/64-bit, probes sizes with the write-all-ones/read-back method, and enables bus mastering / memory / I/O space per device.

MSI/MSI-X (`msi.cpp`): capability walk with a hop limit, MSI-X table mapping with up to 32 vectors, message address `0xFEE00000 | dest << 12`, fixed edge delivery. Requires the APIC.

## ACPI

`src/drivers/acpi/acpi.cpp`:

- RSDP from `BootInfo->rsdp_address`, with the legacy EBDA / `0xE0000-0x100000` scan as fallback.
- XSDT when revision >= 2 and present, else RSDT; per-table checksums.
- **FADT**: PM1a/PM1b control blocks, SMI command, ACPI enable value, reset register/value. The DSDT is scanned for the `_S5_` package to obtain SLP_TYP values.
- **MADT**: consumed by the APIC layer and SMP bring-up.
- **MCFG**: consumed by PCI for ECAM.
- No HPET support.

Poweroff enables ACPI if needed, writes `SLP_TYP | SLP_EN` to the PM1 control blocks, brute-forces common SLP_TYP values, then falls back to VM ports (QEMU `0x604`, Bochs `0xB004`, VBox `0x4004`). Reboot writes the FADT reset value to the reset register (I/O or MMIO).

## APIC

- **MADT parse**: up to 8 IOAPICs (MMIO-mapped, all redirection entries masked at init) and up to 16 interrupt source overrides (bus 0, polarity/trigger decoded).
- **LAPIC**: base from the MADT header (with address-override record), default `0xFEE00000`. The 8259 PIC is remapped and fully masked; spurious vector `0xFF`.
- **Routing**: ISA IRQs map to vectors `32 + irq` with ISO remapping; the IOAPIC covering the target GSI gets a fixed-delivery physical entry with the ISO's polarity and trigger bits.
- **Vector space**: 0-31 exceptions, 32 timer, 33 keyboard, 44 mouse, `0x80` syscall gate, dynamic allocations from 48-254 (xHCI MSI-X, TLB shootdown, RESCHED IPI, STOP IPI, MSI), `0xFF` spurious.
- The interrupt dispatcher handles the timer (tick + possible reschedule), RESCHED (schedule), STOP (halt), PS/2 keyboard/mouse, and registered vector handlers; EOI via LAPIC.

## Interrupts vs Polling

| Device | Model |
| --- | --- |
| PS/2 keyboard/mouse | Interrupt (IRQ1 / IRQ12) |
| xHCI | MSI-X preferred, MSI, legacy IRQ, or polling fallback |
| AHCI | Polled (PCI interrupts disabled) |
| ATA | Polled PIO |
| e1000, RTL8139 | Polled (interrupt masks cleared) |
| AC97, HDA | Polled buffer refill |

The kernel idle loop pumps input and network polling; device drivers that poll are driven from those paths or from their consumers.
