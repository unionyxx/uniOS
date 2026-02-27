#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/usb_hub.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <kernel/mm/heap.h>
#include <kernel/debug.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>
#include <stddef.h>

static UsbDeviceInfo usb_devices[USB_MAX_DEVICES];
static int usb_device_count = 0;
static bool usb_debug = false;

void usb_set_debug(bool enabled) {
    usb_debug = enabled;
}

static void usb_handle_hid_interface(UsbDeviceInfo* dev, UsbInterfaceDescriptor* iface) {
    if (iface->bInterfaceClass != USB_CLASS_HID) return;
    
    if (iface->bInterfaceSubClass == USB_SUBCLASS_BOOT &&
        iface->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) {
        if (!dev->is_keyboard) {
            dev->is_keyboard = true;
            dev->is_boot_interface = true;
            dev->hid_interface = iface->bInterfaceNumber;
            KLOG(MOD_USB, LOG_INFO, "Found boot keyboard interface %d", iface->bInterfaceNumber);
        }
    }
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
            KLOG(MOD_USB, LOG_INFO, "Found boot mouse interface %d", iface->bInterfaceNumber);
        }
    }
    else if (iface->bInterfaceSubClass == 0 && iface->bInterfaceProtocol == 0) {
        if (!dev->is_keyboard && !dev->is_mouse) {
            dev->hid_interface = iface->bInterfaceNumber;
        } else if (dev->is_keyboard && !dev->is_mouse) {
            dev->hid_interface2 = iface->bInterfaceNumber;
        }
    }
}

static void usb_handle_hid_endpoint(UsbDeviceInfo* dev, UsbInterfaceDescriptor* iface,
                                    UsbEndpointDescriptor* ep) {
    if (iface->bInterfaceClass != USB_CLASS_HID) return;
    if (!(ep->bEndpointAddress & USB_ENDPOINT_DIR_IN)) return;
    if ((ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_TYPE_INTERRUPT) return;
    
    uint8_t ep_num = ep->bEndpointAddress & 0x0F;
    uint8_t ep_dir = (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) ? 1 : 0;
    uint8_t xhci_ep = ep_num * 2 + ep_dir;
    uint16_t max_packet = ep->wMaxPacketSize & 0x7FF;
    
    if (!dev->is_keyboard && !dev->is_mouse && 
        iface->bInterfaceNumber == dev->hid_interface) {
        dev->is_mouse = true;
        dev->is_boot_interface = false;
        KLOG(MOD_USB, LOG_TRACE, "Classified generic HID as mouse");
    }
    
    if (iface->bInterfaceNumber == dev->hid_interface && dev->hid_endpoint == 0) {
        dev->hid_endpoint = xhci_ep;
        dev->hid_max_packet = max_packet;
        dev->hid_interval = ep->bInterval;
    } else if (dev->hid_interface2 != 0 &&
               iface->bInterfaceNumber == dev->hid_interface2 && 
               dev->hid_endpoint2 == 0) {
        dev->hid_endpoint2 = xhci_ep;
        dev->hid_max_packet2 = max_packet;
        dev->hid_interval2 = ep->bInterval;
    }
}

static bool usb_parse_config(UsbDeviceInfo* dev, uint8_t* data, uint16_t length) {
    uint16_t offset = 0;
    UsbInterfaceDescriptor* current_iface = nullptr;
    
    while (offset + 2 <= length) {
        uint8_t len = data[offset];
        uint8_t type = data[offset + 1];
        if (len < 2 || offset + len > length) break;
        
        if (type == USB_DESC_INTERFACE && len >= sizeof(UsbInterfaceDescriptor)) {
            current_iface = (UsbInterfaceDescriptor*)&data[offset];
            usb_handle_hid_interface(dev, current_iface);
        } else if (type == USB_DESC_ENDPOINT && current_iface &&
                   len >= sizeof(UsbEndpointDescriptor)) {
            UsbEndpointDescriptor* ep = (UsbEndpointDescriptor*)&data[offset];
            usb_handle_hid_endpoint(dev, current_iface, ep);
        }
        offset += len;
    }
    return true;
}

int usb_enumerate_device(uint8_t port) {
    DEBUG_INFO("Enumerating port %d...", port);
    if (usb_device_count >= USB_MAX_DEVICES) {
        DEBUG_ERROR("Max devices reached");
        return -1;
    }
    if (!xhci_reset_port(port)) {
        DEBUG_ERROR("Port reset failed");
        return -1;
    }
    sleep(10);
    uint8_t speed = xhci_get_port_speed(port);
    if (speed == 0) {
        DEBUG_ERROR("Invalid port speed");
        return -1;
    }
    KLOG(MOD_USB, LOG_TRACE, "Port speed: %d", speed);
    int slot_id = xhci_enable_slot();
    if (slot_id < 0) {
        DEBUG_ERROR("Enable Slot failed");
        return -1;
    }
    KLOG(MOD_USB, LOG_TRACE, "Slot ID: %d", slot_id);
    if (!xhci_address_device(slot_id, port, speed)) {
        DEBUG_ERROR("Address Device failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    sleep(5);
    UsbDeviceDescriptor dev_desc;
    bool got_desc = false;
    for (int retry = 0; retry < 3 && !got_desc; retry++) {
        if (retry > 0) sleep(10);
        got_desc = usb_get_device_descriptor(slot_id, &dev_desc);
    }
    if (!got_desc) {
        DEBUG_ERROR("Get Device Descriptor failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    KLOG(MOD_USB, LOG_TRACE, "Device: VID=0x%04x PID=0x%04x Class=%d MaxPkt=%d",
              dev_desc.idVendor, dev_desc.idProduct,
              dev_desc.bDeviceClass, dev_desc.bMaxPacketSize0);
    
    // Check if it's a hub
    if (dev_desc.bDeviceClass == 0x09) {
        usb_hub_register(0, port, speed); // address will be set later in a real driver
    }

    UsbDeviceInfo* dev = &usb_devices[usb_device_count];
    kstring::zero_memory(dev, sizeof(UsbDeviceInfo));
    dev->slot_id = slot_id;
    dev->port = port;
    dev->speed = speed;
    dev->vendor_id = dev_desc.idVendor;
    dev->product_id = dev_desc.idProduct;
    dev->device_class = dev_desc.bDeviceClass;
    dev->device_subclass = dev_desc.bDeviceSubClass;
    dev->device_protocol = dev_desc.bDeviceProtocol;
    uint8_t config_header[9];
    if (!usb_get_config_descriptor(slot_id, 0, config_header, 9)) {
        DEBUG_ERROR("Get Config Header failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    UsbConfigDescriptor* cfg = (UsbConfigDescriptor*)config_header;
    uint16_t total_length = cfg->wTotalLength;
    uint8_t* full_config = (uint8_t*)malloc(total_length);
    if (!full_config) {
        xhci_disable_slot(slot_id);
        return -1;
    }
    if (!usb_get_config_descriptor(slot_id, 0, full_config, total_length)) {
        DEBUG_ERROR("Get Full Config failed");
        free(full_config);
        xhci_disable_slot(slot_id);
        return -1;
    }
    dev->config_value = cfg->bConfigurationValue;
    dev->num_interfaces = cfg->bNumInterfaces;
    usb_parse_config(dev, full_config, total_length);
    free(full_config);
    if (!usb_set_configuration(slot_id, dev->config_value)) {
        DEBUG_ERROR("Set Configuration failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    if (dev->hid_endpoint != 0) {
        if (!xhci_configure_endpoint(slot_id, dev->hid_endpoint, EP_TYPE_INTERRUPT_IN,
                                     dev->hid_max_packet, dev->hid_interval)) {
            DEBUG_ERROR("Configure primary endpoint failed");
        } else {
            KLOG(MOD_USB, LOG_TRACE, "Primary endpoint %d configured", dev->hid_endpoint);
        }
    }
    if (dev->hid_endpoint2 != 0) {
        if (!xhci_configure_endpoint(slot_id, dev->hid_endpoint2, EP_TYPE_INTERRUPT_IN,
                                     dev->hid_max_packet2, dev->hid_interval2)) {
            DEBUG_ERROR("Configure secondary endpoint failed");
        } else {
            KLOG(MOD_USB, LOG_TRACE, "Secondary endpoint %d configured", dev->hid_endpoint2);
        }
    }
    dev->configured = true;
    usb_device_count++;
    DEBUG_INFO("Device enumerated: Slot=%d KBD=%d MOUSE=%d",
               slot_id, dev->is_keyboard ? 1 : 0, dev->is_mouse ? 1 : 0);
    return usb_device_count - 1;
}

bool usb_get_device_descriptor(uint8_t slot_id, UsbDeviceDescriptor* desc) {
    uint16_t transferred;
    return xhci_control_transfer(slot_id,
        USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
        USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8) | 0, 0,
        sizeof(UsbDeviceDescriptor), desc, &transferred) && transferred >= 8;
}

bool usb_get_config_descriptor(uint8_t slot_id, uint8_t index, void* buffer, uint16_t size) {
    uint16_t transferred;
    return xhci_control_transfer(slot_id,
        USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
        USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIGURATION << 8) | index, 0,
        size, buffer, &transferred);
}

bool usb_set_configuration(uint8_t slot_id, uint8_t config_value) {
    uint16_t transferred;
    return xhci_control_transfer(slot_id,
        USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
        USB_REQ_SET_CONFIGURATION, config_value, 0, 0, nullptr, &transferred);
}

int usb_get_device_count() { return usb_device_count; }

UsbDeviceInfo* usb_get_device(int index) {
    if (index < 0 || index >= usb_device_count) return nullptr;
    return &usb_devices[index];
}

UsbDeviceInfo* usb_find_keyboard() {
    for (int i = 0; i < usb_device_count; i++) {
        if (usb_devices[i].is_keyboard && usb_devices[i].configured) return &usb_devices[i];
    }
    return nullptr;
}

UsbDeviceInfo* usb_find_mouse() {
    for (int i = 0; i < usb_device_count; i++) {
        if (usb_devices[i].is_mouse && usb_devices[i].configured) return &usb_devices[i];
    }
    return nullptr;
}

void usb_poll() { 
    // xHCI is now interrupt-driven; no polling needed here
}

void usb_init() {
    usb_device_count = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        kstring::zero_memory(&usb_devices[i], sizeof(UsbDeviceInfo));
    }
    if (!xhci_init()) {
        DEBUG_ERROR("xHCI initialization failed");
        return;
    }
    
    usb_hub_init();

    uint8_t max_ports = xhci_get_max_ports();
    int found = 0;
    for (uint8_t port = 1; port <= max_ports; port++) {
        if (xhci_port_connected(port)) {
            if (usb_enumerate_device(port) >= 0) found++;
        }
    }
    DEBUG_INFO("USB init complete: %d device(s) found", found);
}
