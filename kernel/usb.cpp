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

// Simple on-screen logger
static int log_y = 20;

// Minimal vsnprintf implementation
static void simple_vsnprintf(char* buf, size_t size, const char* fmt, va_list args) {
    char* p = buf;
    char* end = buf + size - 1;
    
    while (*fmt && p < end) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd') {
                int val = va_arg(args, int);
                if (val < 0) {
                    if (p < end) *p++ = '-';
                    val = -val;
                }
                char tmp[32];
                int i = 0;
                do {
                    tmp[i++] = (val % 10) + '0';
                    val /= 10;
                } while (val > 0);
                while (i > 0 && p < end) *p++ = tmp[--i];
            } else if (*fmt == 'x' || *fmt == 'X') {
                unsigned int val = va_arg(args, unsigned int);
                char tmp[32];
                int i = 0;
                do {
                    int d = val % 16;
                    tmp[i++] = (d < 10) ? (d + '0') : (d - 10 + 'a');
                    val /= 16;
                } while (val > 0);
                // Pad with 0 if needed? No, simple implementation.
                if (i == 0) tmp[i++] = '0';
                while (i > 0 && p < end) *p++ = tmp[--i];
            } else if (*fmt == 's') {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s && p < end) *p++ = *s++;
            } else if (*fmt == 'c') {
                if (p < end) *p++ = (char)va_arg(args, int);
            } else {
                if (p < end) *p++ = *fmt;
            }
        } else {
            if (p < end) *p++ = *fmt;
        }
        fmt++;
    }
    *p = 0;
}

void usb_log(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    simple_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    if (g_framebuffer) {
        gfx_draw_string(10, log_y, buf, COLOR_WHITE);
        log_y += 16;
        if (log_y > (int)g_framebuffer->height - 20) log_y = 20; // Wrap around
    }
}

// Parse configuration descriptor to find interfaces and endpoints
static bool usb_parse_config(uint8_t slot_id, UsbDeviceInfo* dev, uint8_t* config_data, uint16_t total_length) {
    uint16_t offset = 0;
    UsbInterfaceDescriptor* current_iface = nullptr;
    
    while (offset < total_length) {
        uint8_t length = config_data[offset];
        uint8_t type = config_data[offset + 1];
        
        if (length == 0) break;  // Malformed descriptor
        
        if (type == USB_DESC_INTERFACE) {
            current_iface = (UsbInterfaceDescriptor*)&config_data[offset];
            usb_log("  Interface %d: Class %d Sub %d Proto %d", 
                current_iface->bInterfaceNumber, current_iface->bInterfaceClass, 
                current_iface->bInterfaceSubClass, current_iface->bInterfaceProtocol);
            
            // Check for HID boot keyboard OR generic HID
            if (current_iface->bInterfaceClass == USB_CLASS_HID) {
                // Boot Keyboard (SubClass=Boot, Protocol=Keyboard)
                if (current_iface->bInterfaceSubClass == USB_SUBCLASS_BOOT && 
                    current_iface->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) {
                    // Only claim if this device doesn't already have a keyboard role
                    if (!dev->is_keyboard) {
                        dev->is_keyboard = true;
                        dev->is_boot_interface = true;
                        dev->hid_interface = current_iface->bInterfaceNumber;
                        usb_log("    -> Found Boot Keyboard!");
                    }
                } 
                // Boot Mouse (SubClass=Boot, Protocol=Mouse)
                else if (current_iface->bInterfaceSubClass == USB_SUBCLASS_BOOT && 
                         current_iface->bInterfaceProtocol == USB_PROTOCOL_MOUSE) {
                    // Only claim if this device doesn't already have a mouse role
                    if (!dev->is_mouse) {
                        dev->is_mouse = true;
                        dev->is_boot_interface = true;
                        // Use secondary interface if we already have keyboard
                        if (dev->is_keyboard) {
                            dev->hid_interface2 = current_iface->bInterfaceNumber;
                        } else {
                            dev->hid_interface = current_iface->bInterfaceNumber;
                        }
                        usb_log("    -> Found Boot Mouse!");
                    }
                }
                // Generic HID (SubClass=0, Protocol=0)
                // For composite devices, we need to handle this based on interface order
                // within THIS device, not globally
                else if (current_iface->bInterfaceSubClass == 0 && 
                         current_iface->bInterfaceProtocol == 0) {
                    // If device already has keyboard, try mouse for additional interfaces
                    if (dev->is_keyboard && !dev->is_mouse) {
                        dev->is_mouse = true;
                        dev->is_boot_interface = false;
                        dev->hid_interface2 = current_iface->bInterfaceNumber;
                        usb_log("    -> Found Generic HID (assuming Mouse)");
                    } 
                    // If device has neither role, assume first generic HID is keyboard
                    else if (!dev->is_keyboard && !dev->is_mouse) {
                        dev->is_keyboard = true;
                        dev->is_boot_interface = false;
                        dev->hid_interface = current_iface->bInterfaceNumber;
                        usb_log("    -> Found Generic HID (assuming Keyboard)");
                    } else {
                        usb_log("    -> Skipping extra Generic HID interface");
                    }
                } else {
                    usb_log("    -> Skipping non-boot HID (Sub=%d Proto=%d)", 
                        current_iface->bInterfaceSubClass, current_iface->bInterfaceProtocol);
                }
            }
        } else if (type == USB_DESC_ENDPOINT && current_iface) {
            UsbEndpointDescriptor* ep = (UsbEndpointDescriptor*)&config_data[offset];
            
            // Look for interrupt IN endpoint for HID
            if ((current_iface->bInterfaceClass == USB_CLASS_HID) &&
                (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) &&
                ((ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_INTERRUPT)) {
                
                // Calculate xHCI endpoint index (DCI - Device Context Index)
                uint8_t ep_num = ep->bEndpointAddress & 0x0F;
                uint8_t ep_dir = (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) ? 1 : 0;
                uint8_t xhci_ep = ep_num * 2 + ep_dir;
                
                // Check if this is the interface we're using for keyboard or mouse
                // For composite devices: keyboard uses hid_interface, mouse uses hid_interface2
                // For standalone devices: keyboard OR mouse uses hid_interface
                bool is_kbd_iface = dev->is_keyboard && 
                                   current_iface->bInterfaceNumber == dev->hid_interface;
                
                // Mouse interface detection:
                // - Composite device: mouse is on hid_interface2
                // - Standalone mouse: mouse is on hid_interface (no keyboard)
                bool is_mouse_iface = dev->is_mouse && (
                    (dev->hid_interface2 != 0 && current_iface->bInterfaceNumber == dev->hid_interface2) ||
                    (!dev->is_keyboard && current_iface->bInterfaceNumber == dev->hid_interface)
                );
                
                if (is_kbd_iface && dev->hid_endpoint == 0) {
                    dev->hid_max_packet = ep->wMaxPacketSize;
                    dev->hid_interval = ep->bInterval;
                    dev->hid_endpoint = xhci_ep;
                    usb_log("    -> KBD Endpoint: Addr 0x%x, DCI %d", 
                        ep->bEndpointAddress, xhci_ep);
                } else if (is_mouse_iface) {
                    // For composite devices, store in endpoint2
                    // For standalone mice, store in primary endpoint
                    if (dev->is_keyboard && dev->hid_endpoint2 == 0) {
                        dev->hid_max_packet2 = ep->wMaxPacketSize;
                        dev->hid_interval2 = ep->bInterval;
                        dev->hid_endpoint2 = xhci_ep;
                        usb_log("    -> Mouse Endpoint2: Addr 0x%x, DCI %d", 
                            ep->bEndpointAddress, xhci_ep);
                    } else if (!dev->is_keyboard && dev->hid_endpoint == 0) {
                        dev->hid_max_packet = ep->wMaxPacketSize;
                        dev->hid_interval = ep->bInterval;
                        dev->hid_endpoint = xhci_ep;
                        usb_log("    -> Mouse Endpoint: Addr 0x%x, DCI %d", 
                            ep->bEndpointAddress, xhci_ep);
                    }
                }
            }
        }
        
        offset += length;
    }
    
    return true;
}

int usb_enumerate_device(uint8_t port) {
    usb_log("Enumerating Port %d...", port);

    if (usb_device_count >= USB_MAX_DEVICES) {
        usb_log("Error: Max devices reached");
        return -1;
    }
    
    // Reset port
    if (!xhci_reset_port(port)) {
        usb_log("Error: Port reset failed");
        return -1;
    }
    
    // Get port speed
    uint8_t speed = xhci_get_port_speed(port);
    if (speed == 0) {
        usb_log("Error: Invalid port speed");
        return -1;
    }
    usb_log("Port Speed: %d", speed);
    
    // Enable slot
    int slot_id = xhci_enable_slot();
    if (slot_id < 0) {
        usb_log("Error: Enable Slot failed");
        return -1;
    }
    usb_log("Slot ID: %d", slot_id);
    
    // Address device
    if (!xhci_address_device(slot_id, port, speed)) {
        usb_log("Error: Address Device failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    
    // Get device descriptor
    UsbDeviceDescriptor dev_desc;
    if (!usb_get_device_descriptor(slot_id, &dev_desc)) {
        usb_log("Error: Get Device Descriptor failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    usb_log("Device: VID 0x%04x PID 0x%04x Class %d", dev_desc.idVendor, dev_desc.idProduct, dev_desc.bDeviceClass);
    
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
        usb_log("Error: Get Config Header failed");
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
        usb_log("Error: Set Configuration failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    
    // NOW configure HID endpoint in xHCI (after device is configured)
    if (dev->hid_endpoint != 0) {
        // EP type for interrupt IN = 7
        if (!xhci_configure_endpoint(slot_id, dev->hid_endpoint, 7,
                                dev->hid_max_packet, dev->hid_interval)) {
             usb_log("Error: Configure Endpoint failed");
        } else {
             usb_log("Primary Endpoint Configured");
        }
    }
    
    // Configure secondary endpoint for composite devices (e.g., mouse on keyboard device)
    if (dev->hid_endpoint2 != 0) {
        if (!xhci_configure_endpoint(slot_id, dev->hid_endpoint2, 7,
                                dev->hid_max_packet2, dev->hid_interval2)) {
             usb_log("Error: Configure Secondary Endpoint failed");
        } else {
             usb_log("Secondary Endpoint Configured");
        }
    }
    
    dev->configured = true;
    usb_device_count++;
    
    usb_log("Device Enumerated Successfully!");
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
        usb_log("USB Init complete. No devices found.");
    } else {
        usb_log("USB Init complete. Found %d devices.", found);
    }
}

