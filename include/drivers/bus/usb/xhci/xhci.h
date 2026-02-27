#pragma once
#include <stdint.h>

// xHCI Capability Registers (relative to MMIO base)
struct XhciCapRegs {
    uint8_t  caplength;      // 0x00: Capability Register Length
    uint8_t  reserved;       // 0x01
    uint16_t hciversion;     // 0x02: Interface Version Number
    uint32_t hcsparams1;     // 0x04: Structural Parameters 1
    uint32_t hcsparams2;     // 0x08: Structural Parameters 2
    uint32_t hcsparams3;     // 0x0C: Structural Parameters 3
    uint32_t hccparams1;     // 0x10: Capability Parameters 1
    uint32_t dboff;          // 0x14: Doorbell Offset
    uint32_t rtsoff;         // 0x18: Runtime Register Offset
    uint32_t hccparams2;     // 0x1C: Capability Parameters 2
} __attribute__((packed));

// HCSPARAMS1 bit fields
#define HCSPARAMS1_MAX_SLOTS(x)    ((x) & 0xFF)
#define HCSPARAMS1_MAX_INTRS(x)    (((x) >> 8) & 0x7FF)
#define HCSPARAMS1_MAX_PORTS(x)    (((x) >> 24) & 0xFF)

// HCSPARAMS2 bit fields
#define HCSPARAMS2_IST(x)          ((x) & 0xF)
#define HCSPARAMS2_ERST_MAX(x)     (((x) >> 4) & 0xF)
#define HCSPARAMS2_SPR(x)          (((x) >> 26) & 0x1)
#define HCSPARAMS2_MAX_SCRATCHPAD_HI(x) (((x) >> 21) & 0x1F)

#define HCSPARAMS2_MAX_SCRATCHPAD_LO(x) (((x) >> 27) & 0x1F)
#define HCSPARAMS2_MAX_SCRATCHPAD(x) ((HCSPARAMS2_MAX_SCRATCHPAD_HI(x) << 5) | HCSPARAMS2_MAX_SCRATCHPAD_LO(x))

// HCCPARAMS1 bit fields
#define HCCPARAMS1_AC64(x)         ((x) & 0x1)
#define HCCPARAMS1_CSZ(x)          (((x) >> 2) & 0x1)
#define HCCPARAMS1_XECP(x)         (((x) >> 16) & 0xFFFF)

// xHCI Extended Capability
struct XhciExtendedCap {
    uint32_t cap_id;         // Capability ID (0-7), Next Capability Pointer (8-15)
    uint32_t cap_specific;   // Capability Specific
} __attribute__((packed));

// Extended Capability IDs
#define XECP_ID_LEGACY           1
#define XECP_ID_PROTOCOLS        2
#define XECP_ID_POWER            3
#define XECP_ID_VIRT             4

// USB Legacy Support Capability (USBLEGSUP)
#define USBLEGSUP_BIOS_SEM       (1 << 16) // BIOS Owned Semaphore
#define USBLEGSUP_OS_SEM         (1 << 24) // OS Owned Semaphore

// USB Legacy Control/Status (USBLEGCTLSTS)
#define USBLEGCTLSTS_SMI_ENABLE  0xFFFF0000 // SMI Enable bits (disable all)

// xHCI Operational Registers (relative to MMIO base + caplength)
struct XhciOpRegs {
    uint32_t usbcmd;         // 0x00: USB Command
    uint32_t usbsts;         // 0x04: USB Status
    uint32_t pagesize;       // 0x08: Page Size
    uint32_t reserved1[2];   // 0x0C-0x10
    uint32_t dnctrl;         // 0x14: Device Notification Control
    uint64_t crcr;           // 0x18: Command Ring Control Register
#define CRCR_RCS (1 << 0)    // Ring Cycle State
    uint32_t reserved2[4];   // 0x20-0x2C
    uint64_t dcbaap;         // 0x30: Device Context Base Address Array Pointer
    uint32_t config;         // 0x38: Configure
} __attribute__((packed));

// USBCMD bit fields
#define USBCMD_RS        (1 << 0)  // Run/Stop
#define USBCMD_HCRST     (1 << 1)  // Host Controller Reset
#define USBCMD_INTE      (1 << 2)  // Interrupter Enable
#define USBCMD_HSEE      (1 << 3)  // Host System Error Enable

// USBSTS bit fields
#define USBSTS_HCH       (1 << 0)  // Host Controller Halted
#define USBSTS_HSE       (1 << 2)  // Host System Error
#define USBSTS_EINT      (1 << 3)  // Event Interrupt
#define USBSTS_PCD       (1 << 4)  // Port Change Detect
#define USBSTS_CNR       (1 << 11) // Controller Not Ready

// Port Status and Control Register (per-port, offset 0x400 + 0x10*port)
struct XhciPortRegs {
    uint32_t portsc;         // Port Status and Control
    uint32_t portpmsc;       // Port Power Management Status and Control
    uint32_t portli;         // Port Link Info
    uint32_t porthlpmc;      // Port Hardware LPM Control
} __attribute__((packed, aligned(4)));

// PORTSC bit fields
#define PORTSC_CCS       (1 << 0)   // Current Connect Status
#define PORTSC_PED       (1 << 1)   // Port Enabled/Disabled
#define PORTSC_OCA       (1 << 3)   // Overcurrent Active
#define PORTSC_PR        (1 << 4)   // Port Reset
#define PORTSC_PLS_MASK  (0xF << 5) // Port Link State
#define PORTSC_PP        (1 << 9)   // Port Power
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK (0xF << 10) // Port Speed
#define PORTSC_CSC       (1 << 17)  // Connect Status Change
#define PORTSC_PEC       (1 << 18)  // Port Enabled/Disabled Change
#define PORTSC_PRC       (1 << 21)  // Port Reset Change

#define PORTSC_WCE       (1 << 25)  // Wake on Connect Enable
#define PORTSC_WPR       (1U << 31) // Warm Port Reset
#define PORTSC_CHANGE_MASK (PORTSC_CSC | PORTSC_PEC | PORTSC_PRC)

// Port speeds
#define PORTSC_SPEED_FS   1  // Full Speed
#define PORTSC_SPEED_LS   2  // Low Speed
#define PORTSC_SPEED_HS   3  // High Speed
#define PORTSC_SPEED_SS   4  // SuperSpeed

// Protocol Speeds
#define XHCI_SPEED_FULL 1
#define XHCI_SPEED_LOW  2
#define XHCI_SPEED_HIGH 3
#define XHCI_SPEED_SUPER 4
#define XHCI_SPEED_SUPER_PLUS 5

// Typical empty port PORTSC value (PP set, no device)
#define PORTSC_TYPICAL_EMPTY  0x2A0

// Runtime Registers (relative to MMIO base + rtsoff)
struct XhciRuntimeRegs {
    uint32_t mfindex;        // Microframe Index
    uint32_t reserved[7];
    // Interrupter Register Sets follow at offset 0x20
} __attribute__((packed));

// Interrupter Register Set (32 bytes each)
struct XhciInterrupterRegs {
    uint32_t iman;           // Interrupter Management
    uint32_t imod;           // Interrupter Moderation
    uint32_t erstsz;         // Event Ring Segment Table Size
    uint32_t reserved;
    uint64_t erstba;         // Event Ring Segment Table Base Address
    uint64_t erdp;           // Event Ring Dequeue Pointer
#define ERDP_EHB (1 << 3)    // Event Handler Busy
} __attribute__((packed));

// IMAN bit fields
#define IMAN_IP          (1 << 0)  // Interrupt Pending
#define IMAN_IE          (1 << 1)  // Interrupt Enable

// Doorbell Register (4 bytes each slot, offset = dboff + 4*slot_id)
#define DB_HOST          0  // Doorbell for host controller (slot 0)
#define DB_EP0_IN        1  // Doorbell target for EP0
#define DB_TARGET_MASK   0xFF

// Transfer Request Block (TRB) - 16 bytes
struct Trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

// TRB Types (in control field bits 10-15)
#define TRB_TYPE(x)      (((x) & 0x3F) << 10)
#define TRB_GET_TYPE(x)  (((x) >> 10) & 0x3F)

// TRB Type codes
#define TRB_TYPE_NORMAL          1
#define TRB_TYPE_SETUP           2
#define TRB_TYPE_DATA            3
#define TRB_TYPE_STATUS          4
#define TRB_TYPE_ISOCH           5
#define TRB_TYPE_LINK            6
#define TRB_TYPE_EVENT_DATA      7
#define TRB_TYPE_NOOP            8
#define TRB_TYPE_ENABLE_SLOT     9
#define TRB_TYPE_DISABLE_SLOT    10
#define TRB_TYPE_ADDRESS_DEVICE  11
#define TRB_TYPE_CONFIG_EP       12
#define TRB_TYPE_EVAL_CONTEXT    13
#define TRB_TYPE_RESET_EP        14
#define TRB_TYPE_STOP_EP         15
#define TRB_TYPE_SET_TR_DEQUEUE  16
#define TRB_TYPE_RESET_DEVICE    17
#define TRB_TYPE_NOOP_CMD        23
#define TRB_TYPE_TRANSFER_EVENT  32
#define TRB_TYPE_COMMAND_COMPLETION 33
#define TRB_TYPE_PORT_STATUS_CHANGE 34
#define TRB_TYPE_HOST_CONTROLLER    37

// TRB control field flags
#define TRB_CYCLE       (1 << 0)
#define TRB_ENT         (1 << 1)   // Evaluate Next TRB
#define TRB_ISP         (1 << 2)   // Interrupt on Short Packet
#define TRB_NS          (1 << 3)   // No Snoop
#define TRB_CHAIN       (1 << 4)   // Chain bit
#define TRB_IOC         (1 << 5)   // Interrupt on Completion
#define TRB_IDT         (1 << 6)   // Immediate Data
#define TRB_TC          (1 << 1)   // Toggle Cycle (for Link TRB)
#define TRB_BSR         (1 << 9)   // Block Set Address Request (Address Device)
#define TRB_DIR_IN      (1 << 16)  // Direction: IN (for Data TRB)
#define TRB_TRT_OUT     (2 << 16)  // Transfer Type: OUT (Setup Stage)
#define TRB_TRT_IN      (3 << 16)  // Transfer Type: IN (Setup Stage)

// Command Completion codes (TRB status field bits 24-31)
#define TRB_COMP_SUCCESS         1
#define TRB_COMP_DATA_BUFFER     2
#define TRB_COMP_BABBLE          3
#define TRB_COMP_USB_TRANSACTION 4
#define TRB_COMP_TRB             5
#define TRB_COMP_STALL           6
#define TRB_COMP_SLOT_NOT_ENABLED 9
#define TRB_COMP_EP_NOT_ENABLED  10
#define TRB_COMP_SHORT_PACKET    13

// Event Ring Segment Table Entry
struct ErstEntry {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((packed));

// Slot Context (32 or 64 bytes depending on CSZ)
struct SlotContext {
    uint32_t route_speed_entries;  // Route String, Speed, Context Entries
    uint32_t latency_hub_port;     // Max Exit Latency, Hub info, Root Port
    uint32_t tt_info;              // TT info for LS/FS devices
    uint32_t slot_state;           // Slot State, Device Address
    uint32_t reserved[4];
} __attribute__((packed));

// Endpoint Context (32 or 64 bytes depending on CSZ)
struct EndpointContext {
    uint32_t ep_state;            // EP State, Mult, MaxPStreams, LSA, Interval, MaxESITPayloadHi
    uint32_t ep_info;             // MaxPacketSize, MaxBurstSize, HID, EP Type, CErr 
    uint64_t tr_dequeue;          // TR Dequeue Pointer
    uint32_t avg_trb_length;      // Average TRB Length, Max ESIT Payload Lo
    uint32_t reserved[3];
} __attribute__((packed));

// Endpoint Types
#define EP_TYPE_NOT_VALID     0
#define EP_TYPE_ISOCH_OUT     1
#define EP_TYPE_BULK_OUT      2
#define EP_TYPE_INTERRUPT_OUT 3
#define EP_TYPE_CONTROL       4
#define EP_TYPE_ISOCH_IN      5
#define EP_TYPE_BULK_IN       6
#define EP_TYPE_INTERRUPT_IN  7

// Device Context (Slot + 31 Endpoint Contexts)
struct DeviceContext {
    SlotContext slot;
    EndpointContext endpoints[31];
} __attribute__((packed));

// Input Control Context
struct InputControlContext {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} __attribute__((packed));

// Input Context (for commands)
struct InputContext {
    InputControlContext control;
    SlotContext slot;
    EndpointContext endpoints[31];
} __attribute__((packed));

// Ring size (number of TRBs)
#define XHCI_RING_SIZE    256
#define XHCI_RING_SIZE    256
#define XHCI_EVENT_RING_SIZE 256
#define XHCI_MAX_SLOTS    256
#define XHCI_MAX_ENDPOINTS 32

// xHCI Controller state
struct XhciController {
    volatile XhciCapRegs* cap;
    volatile XhciOpRegs* op;
    volatile XhciRuntimeRegs* runtime;
    volatile uint32_t* doorbell;
    volatile XhciPortRegs* ports;
    
    uint8_t max_slots;
    uint8_t max_ports;
    uint16_t max_intrs;

    bool context_size_64;
    uint32_t page_size;
    uint32_t num_scratchpad;
    
    uint8_t usb2_port_start;
    uint8_t usb2_port_count;
    uint8_t usb3_port_start;
    uint8_t usb3_port_count;
    
    // Device Context Base Address Array
    uint64_t* dcbaa;
    uint64_t dcbaa_phys;
    
    // Command Ring
    Trb* cmd_ring;
    uint64_t cmd_ring_phys;
    uint32_t cmd_enqueue;
    uint8_t cmd_cycle;
    
    // Event Ring
    Trb* event_ring;
    uint64_t event_ring_phys;
    ErstEntry* erst;
    uint64_t erst_phys;
    uint32_t event_dequeue;
    uint8_t event_cycle;
    
    // Scratchpad
    uint64_t* scratchpad_array;
    uint64_t scratchpad_array_phys;
    
    // Device contexts
    DeviceContext* device_contexts[256];
    uint64_t device_context_phys[256];
    InputContext* input_contexts[256];
    uint64_t input_context_phys[256];
    
    // Transfer rings per slot/endpoint
    Trb* transfer_rings[256][32];
    uint64_t transfer_ring_phys[256][32];
    uint32_t transfer_enqueue[256][32];
    uint8_t transfer_cycle[256][32];
    
    // Interrupt transfer state
    bool intr_pending[256][32];     // Transfer started, waiting for completion
    bool intr_complete[256][32];    // Transfer completed, result available
    uint64_t intr_start_time[256][32]; // Time when transfer was started (for timeout)
    Trb transfer_result[256][32];   // Result of completed transfer
};

// USB device info
struct UsbDevice {
    uint8_t slot_id;
    uint8_t port;
    uint8_t speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t config_value;
    uint8_t num_interfaces;
};

// xHCI functions
bool xhci_init();
bool xhci_reset();
bool xhci_start();
void xhci_stop();
bool xhci_is_initialized();
uint8_t xhci_get_max_ports();
uint8_t xhci_get_irq();

// Port operations
uint8_t xhci_get_port_speed(uint8_t port);
bool xhci_port_connected(uint8_t port);
bool xhci_reset_port(uint8_t port);

// Slot operations
int xhci_enable_slot();
bool xhci_disable_slot(uint8_t slot_id);
bool xhci_address_device(uint8_t slot_id, uint8_t port, uint8_t speed);
bool xhci_configure_endpoint(uint8_t slot_id, uint8_t ep_num, uint8_t ep_type, 
                             uint16_t max_packet, uint8_t interval);

// Transfer operations
bool xhci_control_transfer(uint8_t slot_id, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, uint16_t length,
                          void* data, uint16_t* transferred);
bool xhci_interrupt_transfer(uint8_t slot_id, uint8_t ep_num, void* data, 
                             uint16_t length, uint16_t* transferred);

// Event handling
void xhci_poll_events();
bool xhci_wait_for_event(uint32_t timeout_ms);

// Debug
void xhci_dump_status();
