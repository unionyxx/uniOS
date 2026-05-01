#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/usb_hub.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/time/timer.h>

#define MAX_HUBS 8
static UsbHub g_hubs[MAX_HUBS];
static int g_num_hubs = 0;

// Hub Class Requests
#define HUB_REQ_GET_STATUS 0x00
#define HUB_REQ_CLEAR_FEATURE 0x01
#define HUB_REQ_SET_FEATURE 0x03
#define HUB_REQ_GET_DESCRIPTOR 0x06

// Hub Port Features
#define HUB_FEAT_PORT_CONNECTION 0
#define HUB_FEAT_PORT_ENABLE 1
#define HUB_FEAT_PORT_RESET 4
#define HUB_FEAT_PORT_POWER 8
#define HUB_FEAT_C_PORT_CONNECTION 16
#define HUB_FEAT_C_PORT_ENABLE 17
#define HUB_FEAT_C_PORT_RESET 20

// Hub Port Status Bits
#define HUB_STAT_PORT_CONNECTION (1 << 0)
#define HUB_STAT_PORT_ENABLE (1 << 1)
#define HUB_STAT_PORT_LOW_SPEED (1 << 9)
#define HUB_STAT_PORT_HIGH_SPEED (1 << 10)

struct HubDescriptor
{
    uint8_t bDescLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
    // ... followed by device removable and port power control masks
} __attribute__((packed));

static void hub_interrupt_cb(uint8_t slot_id, uint8_t ep_num, void *data, uint16_t length)
{
    (void)ep_num;
    (void)data;
    (void)length;
    for (int i = 0; i < g_num_hubs; i++) {
        if (g_hubs[i].device_slot == slot_id) {
            g_hubs[i].status_changed = true;
            break;
        }
    }
    // Re-arm is handled centrally by xhci_poll_events() after the callback returns.
}

void usb_hub_init()
{
    KLOG(LogModule::Usb, LogLevel::Info, "USB Hub Driver Initialized");
    g_num_hubs = 0;
}

bool usb_hub_register(uint8_t slot_id, uint8_t root_port, uint8_t speed)
{
    (void)speed;
    if (g_num_hubs >= MAX_HUBS)
        return false;

    HubDescriptor desc;
    uint16_t transferred;
    if (!xhci_control_transfer(slot_id, USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_DEVICE,
                               HUB_REQ_GET_DESCRIPTOR, (0x29 << 8), 0, sizeof(HubDescriptor), &desc, &transferred)) {
        KLOG(LogModule::Usb, LogLevel::Error, "Failed to get Hub Descriptor for slot %d", slot_id);
        return false;
    }

    KLOG(LogModule::Usb, LogLevel::Info, "USB Hub: %d ports, PowerOn2Good=%dms", desc.bNbrPorts,
         desc.bPwrOn2PwrGood * 2);

    UsbHub *hub = &g_hubs[g_num_hubs];
    hub->device_slot = slot_id;
    hub->root_port = root_port;
    hub->num_ports = desc.bNbrPorts;

    // Power on all ports
    for (uint8_t i = 1; i <= hub->num_ports; i++) {
        xhci_control_transfer(slot_id, USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_OTHER,
                              HUB_REQ_SET_FEATURE, HUB_FEAT_PORT_POWER, i, 0, nullptr, &transferred);
    }

    // Wait for ports to stabilize (In a real OS, this would be an async state machine)
    sleep(desc.bPwrOn2PwrGood * 2);

    // Register interrupt endpoint (standard Hub is EP1 IN)
    hub->intr_endpoint = 1 * 2 + 1; // EP1 IN (idx 3)
    hub->status_changed = true;     // Initial poll

    xhci_register_interrupt_callback(slot_id, hub->intr_endpoint, hub_interrupt_cb);
    xhci_submit_interrupt_transfer(slot_id, hub->intr_endpoint, 1); // Hub mask is usually small

    g_num_hubs++;
    return true;
}

void usb_hub_poll()
{
    for (int i = 0; i < g_num_hubs; i++) {
        UsbHub *hub = &g_hubs[i];
        if (!hub->status_changed)
            continue;
        hub->status_changed = false;

        for (uint8_t port = 1; port <= hub->num_ports; port++) {
            uint32_t status;
            uint16_t transferred;
            if (xhci_control_transfer(hub->device_slot,
                                      USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_OTHER,
                                      HUB_REQ_GET_STATUS, 0, port, 4, &status, &transferred)) {
                // Check Connection Status Change (C_PORT_CONNECTION)
                if (status & (static_cast<uint32_t>(1) << 16)) {
                    // Clear the change bit
                    xhci_control_transfer(
                        hub->device_slot, USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_OTHER,
                        HUB_REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_CONNECTION, port, 0, nullptr, &transferred);

                    // If device connected, reset and enumerate
                    if (status & HUB_STAT_PORT_CONNECTION) {
                        KLOG(LogModule::Usb, LogLevel::Info, "Device connected on Hub Slot %d Port %d",
                             hub->device_slot, port);

                        // Reset the port
                        xhci_control_transfer(hub->device_slot,
                                              USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_OTHER,
                                              HUB_REQ_SET_FEATURE, HUB_FEAT_PORT_RESET, port, 0, nullptr, &transferred);

                        // Wait for reset to complete
                        for (int retry = 0; retry < 10; retry++) {
                            sleep(10);
                            xhci_control_transfer(hub->device_slot,
                                                  USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_OTHER,
                                                  HUB_REQ_GET_STATUS, 0, port, 4, &status, &transferred);
                            if (status & (static_cast<uint32_t>(1) << 20))
                                break; // C_PORT_RESET
                        }

                        // Clear C_PORT_RESET
                        xhci_control_transfer(
                            hub->device_slot, USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_CLASS | USB_REQ_RECIPIENT_OTHER,
                            HUB_REQ_CLEAR_FEATURE, HUB_FEAT_C_PORT_RESET, port, 0, nullptr, &transferred);

                        uint8_t speed = XHCI_SPEED_FULL;
                        if (status & HUB_STAT_PORT_LOW_SPEED)
                            speed = XHCI_SPEED_LOW;
                        else if (status & HUB_STAT_PORT_HIGH_SPEED)
                            speed = XHCI_SPEED_HIGH;

                        KLOG(LogModule::Usb, LogLevel::Trace, "Hub Port Speed: %d", speed);
                        usb_enumerate_device(port, hub->device_slot, port, speed);
                    } else {
                        KLOG(LogModule::Usb, LogLevel::Info, "Device disconnected from Hub Slot %d Port %d",
                             hub->device_slot, port);
                        usb_remove_device(hub->device_slot, port);
                    }
                }
            }
        }
    }
}
