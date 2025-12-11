#include "usb.h"
#include "xhci.h"
#include "heap.h"

// USB devices storage
static UsbDeviceInfo usb_devices[USB_MAX_DEVICES];
static int usb_device_count = 0;

#include "graphics.h"
#include <stdarg.h>
#include <stdio.h>

extern struct limine_framebuffer* g_framebuffer;

// Debug flag for verbose logging
static bool usb_debug = false;

void usb_set_debug(bool enabled) {
    usb_debug = enabled;
}

static void usb_print_device_info(UsbDeviceInfo* dev) {
    if (!usb_debug) return;
    DEBUG_LOG("Device Info:");
    DEBUG_LOG("  Slot: %d, Port: %d, Speed: %d", dev->slot_id, dev->port, dev->speed);
    DEBUG_LOG("  Vendor: 0x%04x, Product: 0x%04x", dev->vendor_id, dev->product_id);
    DEBUG_LOG("  Class: %d, Sub: %d, Proto: %d", dev->device_class, dev->device_subclass, dev->device_protocol);
    if (dev->is_keyboard) {
        DEBUG_LOG("  [Keyboard] Interface: %d, EP: %d", dev->hid_interface, dev->hid_endpoint);
    }
    if (dev->is_mouse) {
        DEBUG_LOG("  [Mouse] Interface: %d, EP: %d", 
            dev->hid_interface2 ? dev->hid_interface2 : dev->hid_interface, 
            dev->hid_endpoint2 ? dev->hid_endpoint2 : dev->hid_endpoint);
    }
}

// Helper to handle HID interface
static void usb_handle_hid_interface(UsbDeviceInfo* dev, UsbInterfaceDescriptor* iface) {
    if (usb_debug) DEBUG_LOG("  Interface %d: Class %d Sub %d Proto %d", 
        iface->bInterfaceNumber, iface->bInterfaceClass, 
        iface->bInterfaceSubClass, iface->bInterfaceProtocol);
    
    if (iface->bInterfaceClass != USB_CLASS_HID) return;

    // Boot Keyboard
    if (iface->bInterfaceSubClass == USB_SUBCLASS_BOOT && 
        iface->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) {
        if (!dev->is_keyboard) {
            dev->is_keyboard = true;
            dev->is_boot_interface = true;
            dev->hid_interface = iface->bInterfaceNumber;
            if (usb_debug) DEBUG_LOG("    -> Found Boot Keyboard!");
        }
    } 
    // Boot Mouse
    else if (iface->bInterfaceSubClass == USB_SUBCLASS_BOOT && 
             iface->bInterfaceProtocol == USB_PROTOCOL_MOUSE) {
        if (!dev->is_mouse) {
            dev->is_mouse = true;
            dev->is_boot_interface = true;
            if (dev->is_keyboard) {
                dev->hid_interface2 = iface->bInterfaceNumber;
            } else {
                dev->hid_interface = iface->bInterfaceNumber;
            }
            if (usb_debug) DEBUG_LOG("    -> Found Boot Mouse!");
        }
    }
    // Generic HID
    else if (iface->bInterfaceSubClass == 0 && iface->bInterfaceProtocol == 0) {
        if (dev->is_keyboard && !dev->is_mouse) {
            dev->is_mouse = true;
            dev->is_boot_interface = false;
            dev->hid_interface2 = iface->bInterfaceNumber;
            if (usb_debug) DEBUG_LOG("    -> Found Generic HID (assuming Mouse)");
        } 
        else if (!dev->is_keyboard && !dev->is_mouse) {
            dev->is_keyboard = true;
            dev->is_boot_interface = false;
            dev->hid_interface = iface->bInterfaceNumber;
            if (usb_debug) DEBUG_LOG("    -> Found Generic HID (assuming Keyboard)");
        }
    }
}

// Helper to handle HID endpoint
static void usb_handle_hid_endpoint(UsbDeviceInfo* dev, UsbInterfaceDescriptor* iface, UsbEndpointDescriptor* ep) {
    if (iface->bInterfaceClass != USB_CLASS_HID) return;
    if (!(ep->bEndpointAddress & USB_ENDPOINT_DIR_IN)) return;
    if ((ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_TYPE_INTERRUPT) return;

    uint8_t ep_num = ep->bEndpointAddress & 0x0F;
    uint8_t ep_dir = (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) ? 1 : 0;
    uint8_t xhci_ep = ep_num * 2 + ep_dir;

    bool matches_kbd = dev->is_keyboard && iface->bInterfaceNumber == dev->hid_interface;
    bool matches_mouse_composite = dev->is_mouse && dev->hid_interface2 != 0 && iface->bInterfaceNumber == dev->hid_interface2;
    bool matches_mouse_standalone = dev->is_mouse && !dev->is_keyboard && iface->bInterfaceNumber == dev->hid_interface;

    if (matches_kbd && dev->hid_endpoint == 0) {
        dev->hid_max_packet = ep->wMaxPacketSize;
        dev->hid_interval = ep->bInterval;
        dev->hid_endpoint = xhci_ep;
        if (usb_debug) DEBUG_LOG("    -> KBD Endpoint: Addr 0x%x, DCI %d, MaxP %d, Int %d", 
            ep->bEndpointAddress, xhci_ep, ep->wMaxPacketSize, ep->bInterval);
    } else if (matches_mouse_composite && dev->hid_endpoint2 == 0) {
        dev->hid_max_packet2 = ep->wMaxPacketSize;
        dev->hid_interval2 = ep->bInterval;
        dev->hid_endpoint2 = xhci_ep;
        if (usb_debug) DEBUG_LOG("    -> Mouse Endpoint2: Addr 0x%x, DCI %d, MaxP %d, Int %d", 
            ep->bEndpointAddress, xhci_ep, ep->wMaxPacketSize, ep->bInterval);
    } else if (matches_mouse_standalone && dev->hid_endpoint == 0) {
        dev->hid_max_packet = ep->wMaxPacketSize;
        dev->hid_interval = ep->bInterval;
        dev->hid_endpoint = xhci_ep;
        if (usb_debug) DEBUG_LOG("    -> Mouse Endpoint: Addr 0x%x, DCI %d, MaxP %d, Int %d", 
            ep->bEndpointAddress, xhci_ep, ep->wMaxPacketSize, ep->bInterval);
    } else if (dev->is_keyboard && iface->bInterfaceNumber == dev->hid_interface && dev->hid_endpoint == 0) {
        dev->hid_max_packet = ep->wMaxPacketSize;
        dev->hid_interval = ep->bInterval;
        dev->hid_endpoint = xhci_ep;
        if (usb_debug) DEBUG_LOG("    -> HID Endpoint: Addr 0x%x, DCI %d, MaxP %d, Int %d", 
            ep->bEndpointAddress, xhci_ep, ep->wMaxPacketSize, ep->bInterval);
    }
}

// Parse configuration descriptor to find interfaces and endpoints
static bool usb_parse_config(uint8_t slot_id, UsbDeviceInfo* dev, uint8_t* config_data, uint16_t total_length) {
    (void)slot_id; // Unused
    uint16_t offset = 0;
    UsbInterfaceDescriptor* current_iface = nullptr;
    
    while (offset < total_length) {
        uint8_t length = config_data[offset];
        uint8_t type = config_data[offset + 1];
        
        if (length == 0) break;
        
        if (type == USB_DESC_INTERFACE) {
            current_iface = (UsbInterfaceDescriptor*)&config_data[offset];
            usb_handle_hid_interface(dev, current_iface);
        } else if (type == USB_DESC_ENDPOINT && current_iface) {
            UsbEndpointDescriptor* ep = (UsbEndpointDescriptor*)&config_data[offset];
            usb_handle_hid_endpoint(dev, current_iface, ep);
        }
        
        offset += length;
    }
    
    return true;
}

int usb_enumerate_device(uint8_t port) {
    DEBUG_LOG("Enumerating Port %d...", port);

    if (usb_device_count >= USB_MAX_DEVICES) {
        DEBUG_ERROR("Error: Max devices reached");
        return -1;
    }
    
    // Reset port
    if (!xhci_reset_port(port)) {
        DEBUG_ERROR("Error: Port reset failed");
        return -1;
    }
    
    // Get port speed
    uint8_t speed = xhci_get_port_speed(port);
    if (speed == 0) {
        DEBUG_ERROR("Error: Invalid port speed");
        return -1;
    }
    DEBUG_LOG("Port Speed: %d", speed);
    
    // Enable slot
    int slot_id = xhci_enable_slot();
    if (slot_id < 0) {
        DEBUG_ERROR("Error: Enable Slot failed");
        return -1;
    }
    DEBUG_LOG("Slot ID: %d", slot_id);
    
    // Address device
    if (!xhci_address_device(slot_id, port, speed)) {
        DEBUG_ERROR("Error: Address Device failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    
    // Get device descriptor
    UsbDeviceDescriptor dev_desc;
    if (!usb_get_device_descriptor(slot_id, &dev_desc)) {
        DEBUG_ERROR("Error: Get Device Descriptor failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    DEBUG_LOG("Device: VID 0x%04x PID 0x%04x Class %d", dev_desc.idVendor, dev_desc.idProduct, dev_desc.bDeviceClass);
    
    // Create device info
    UsbDeviceInfo* dev = &usb_devices[usb_device_count];
    dev->slot_id = slot_id;
    dev->port = port;
    dev->speed = speed;
    dev->vendor_id = dev_desc.idVendor;
    dev->product_id = dev_desc.idProduct;
    dev->device_class = dev_desc.bDeviceClass;
    dev->device_subclass = dev_desc.bDeviceSubClass;
    dev->device_protocol = dev_desc.bDeviceProtocol;
    dev->is_keyboard = false;
    dev->is_mouse = false;
    dev->is_boot_interface = false;
    dev->configured = false;
    
    // Initialize primary HID fields
    dev->hid_interface = 0;
    dev->hid_endpoint = 0;
    dev->hid_max_packet = 0;
    dev->hid_interval = 0;
    
    // Initialize secondary HID fields (for composite devices)
    dev->hid_interface2 = 0;
    dev->hid_endpoint2 = 0;
    dev->hid_max_packet2 = 0;
    dev->hid_interval2 = 0;
    
    // Get configuration descriptor (first 9 bytes to get total length)
    uint8_t config_header[9];
    if (!usb_get_config_descriptor(slot_id, 0, config_header, 9)) {
        DEBUG_ERROR("Error: Get Config Header failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    
    UsbConfigDescriptor* config_desc = (UsbConfigDescriptor*)config_header;
    uint16_t total_length = config_desc->wTotalLength;
    
    // Get full configuration descriptor
    uint8_t* full_config = (uint8_t*)malloc(total_length);
    if (!full_config) {
        xhci_disable_slot(slot_id);
        return -1;
    }
    
    if (!usb_get_config_descriptor(slot_id, 0, full_config, total_length)) {
        free(full_config);
        xhci_disable_slot(slot_id);
        return -1;
    }
    
    // Parse configuration
    dev->config_value = config_desc->bConfigurationValue;
    dev->num_interfaces = config_desc->bNumInterfaces;
    usb_parse_config(slot_id, dev, full_config, total_length);
    
    free(full_config);
    
    // Set configuration
    if (!usb_set_configuration(slot_id, dev->config_value)) {
        DEBUG_ERROR("Error: Set Configuration failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    
    // NOW configure HID endpoint in xHCI (after device is configured)
    if (dev->hid_endpoint != 0) {
        // EP type for interrupt IN = 7
        if (!xhci_configure_endpoint(slot_id, dev->hid_endpoint, 7,
                                dev->hid_max_packet, dev->hid_interval)) {
             DEBUG_ERROR("Error: Configure Endpoint failed");
        } else {
             DEBUG_LOG("Primary Endpoint Configured");
        }
    }
    
    // Configure secondary endpoint for composite devices (e.g., mouse on keyboard device)
    if (dev->hid_endpoint2 != 0) {
        if (!xhci_configure_endpoint(slot_id, dev->hid_endpoint2, 7,
                                dev->hid_max_packet2, dev->hid_interval2)) {
             DEBUG_ERROR("Error: Configure Secondary Endpoint failed");
        } else {
             DEBUG_LOG("Secondary Endpoint Configured");
        }
    }
    
    dev->configured = true;
    usb_device_count++;
    
    DEBUG_INFO("Device Enumerated Successfully!");
    return usb_device_count - 1;
}

bool usb_get_device_descriptor(uint8_t slot_id, UsbDeviceDescriptor* desc) {
    uint16_t transferred;
    return xhci_control_transfer(
        slot_id,
        USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
        USB_REQ_GET_DESCRIPTOR,
        (USB_DESC_DEVICE << 8) | 0,
        0,
        sizeof(UsbDeviceDescriptor),
        desc,
        &transferred
    );
}

bool usb_get_config_descriptor(uint8_t slot_id, uint8_t index, void* buffer, uint16_t size) {
    uint16_t transferred;
    return xhci_control_transfer(
        slot_id,
        USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
        USB_REQ_GET_DESCRIPTOR,
        (USB_DESC_CONFIGURATION << 8) | index,
        0,
        size,
        buffer,
        &transferred
    );
}

bool usb_set_configuration(uint8_t slot_id, uint8_t config_value) {
    uint16_t transferred;
    return xhci_control_transfer(
        slot_id,
        USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
        USB_REQ_SET_CONFIGURATION,
        config_value,
        0,
        0,
        nullptr,
        &transferred
    );
}

int usb_get_device_count() {
    return usb_device_count;
}

UsbDeviceInfo* usb_get_device(int index) {
    if (index < 0 || index >= usb_device_count) return nullptr;
    return &usb_devices[index];
}

UsbDeviceInfo* usb_find_keyboard() {
    for (int i = 0; i < usb_device_count; i++) {
        if (usb_devices[i].is_keyboard && usb_devices[i].configured) {
            return &usb_devices[i];
        }
    }
    return nullptr;
}

UsbDeviceInfo* usb_find_mouse() {
    for (int i = 0; i < usb_device_count; i++) {
        if (usb_devices[i].is_mouse && usb_devices[i].configured) {
            return &usb_devices[i];
        }
    }
    return nullptr;
}

void usb_poll() {
    xhci_poll_events();
}

void usb_init() {
    usb_device_count = 0;
    
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_devices[i].slot_id = 0;
        usb_devices[i].configured = false;
    }
    
    // Initialize xHCI
    if (!xhci_init()) {
        return;
    }
    
    // Scan all ports for connected devices
    uint8_t max_ports = xhci_get_max_ports();
    int found = 0;
    for (uint8_t port = 1; port <= max_ports; port++) {
        if (xhci_port_connected(port)) {
            usb_enumerate_device(port);
            found++;
        }
    }
    
    if (found == 0) {
        DEBUG_INFO("USB Init complete. No devices found.");
    } else {
        DEBUG_INFO("USB Init complete. Found %d devices.", found);
    }
}

