#include <drivers/bus/usb/usb_hub.h>
#include <kernel/debug.h>

#define MAX_HUBS 4
static UsbHub hubs[MAX_HUBS];
static int num_hubs = 0;

void usb_hub_init() {
    DEBUG_INFO("USB Hub Driver Initialized");
}

bool usb_hub_register(uint8_t address, uint8_t port, uint8_t speed) {
    if (num_hubs >= MAX_HUBS) {
        DEBUG_WARN("USB Hub: Maximum number of hubs reached.");
        return false;
    }

    // This is a stub for enumerating the hub's ports.
    // In a real implementation, we would send a GET_DESCRIPTOR request for the
    // Hub Descriptor, then power on each port, and recursive enumerate devices.
    DEBUG_INFO("USB Hub found at address %d, port %d, speed %d", address, port, speed);
    
    hubs[num_hubs].device_address = address;
    hubs[num_hubs].port = port;
    hubs[num_hubs].num_ports = 0; // Unknown until we read the descriptor
    num_hubs++;

    return true;
}
