#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/usb_hub.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <drivers/storage/usb_msc.h>
#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/time/timer.h>
#include <libk/kstd.h>
#include <libk/kstring.h>
#include <stddef.h>

static UsbDeviceInfo g_usb_devices[USB_MAX_DEVICES];
static int g_usb_device_count = 0;
static bool g_usb_debug = false;
static bool g_usb_initialized = false;

void usb_set_debug(bool enabled)
{
    g_usb_debug = enabled;
}

namespace {
enum class HidCandidateKind : uint8_t
{
    None = 0,
    Keyboard = 1,
    Mouse = 2,
};

struct HidInterfaceCandidate
{
    HidCandidateKind kind = HidCandidateKind::None;
    bool present = false;
    bool has_interrupt_in = false;
    bool is_boot = false;
    uint8_t interface_number = 0;
    uint8_t alt_setting = 0;
    uint8_t endpoint = 0;
    uint16_t max_packet = 0;
    uint8_t interval = 0;
    uint16_t report_desc_length = 0;
    uint8_t max_burst = 0;
    uint8_t mult = 0;
    uint16_t max_esit_payload = 0;
};

enum class ParsedEndpointKind : uint8_t
{
    None = 0,
    HidInterruptIn,
    MscBulkIn,
    MscBulkOut,
};

struct MassStorageInterfaceCandidate
{
    bool present = false;
    bool has_bulk_in = false;
    bool has_bulk_out = false;
    uint8_t interface_number = 0;
    uint8_t alt_setting = 0;
    uint8_t bulk_in_endpoint = 0;
    uint8_t bulk_out_endpoint = 0;
    uint16_t bulk_in_max_packet = 0;
    uint16_t bulk_out_max_packet = 0;
    uint8_t bulk_in_max_burst = 0;
    uint8_t bulk_out_max_burst = 0;
};

static void usb_commit_hid_candidate(UsbDeviceInfo *dev, const HidInterfaceCandidate &candidate)
{
    if (!dev || !candidate.present || !candidate.has_interrupt_in)
        return;

    if (candidate.kind == HidCandidateKind::Keyboard) {
        const bool replace = !dev->has_keyboard || (candidate.interface_number == dev->kbd_interface &&
                                                    candidate.alt_setting == 0 && dev->kbd_alt_setting != 0);
        if (!replace)
            return;

        dev->has_keyboard = true;
        dev->kbd_interface = candidate.interface_number;
        dev->kbd_alt_setting = candidate.alt_setting;
        dev->kbd_endpoint = candidate.endpoint;
        dev->kbd_max_packet = candidate.max_packet;
        dev->kbd_interval = candidate.interval;
        dev->kbd_report_desc_length = candidate.report_desc_length;
        dev->kbd_max_burst = candidate.max_burst;
        dev->kbd_mult = candidate.mult;
        dev->kbd_max_esit_payload = candidate.max_esit_payload;
        dev->kbd_is_boot = candidate.is_boot;
        KLOG(LogModule::Usb, LogLevel::Info, "Found keyboard interface %d alt %d (Boot: %s, EP %d, Report %d bytes)",
             dev->kbd_interface, dev->kbd_alt_setting, dev->kbd_is_boot ? "YES" : "NO", dev->kbd_endpoint,
             dev->kbd_report_desc_length);
        return;
    }

    const bool replace = !dev->has_mouse || (candidate.interface_number == dev->mouse_interface &&
                                             candidate.alt_setting == 0 && dev->mouse_alt_setting != 0);
    if (!replace)
        return;

    dev->has_mouse = true;
    dev->mouse_interface = candidate.interface_number;
    dev->mouse_alt_setting = candidate.alt_setting;
    dev->mouse_endpoint = candidate.endpoint;
    dev->mouse_max_packet = candidate.max_packet;
    dev->mouse_interval = candidate.interval;
    dev->mouse_report_desc_length = candidate.report_desc_length;
    dev->mouse_max_burst = candidate.max_burst;
    dev->mouse_mult = candidate.mult;
    dev->mouse_max_esit_payload = candidate.max_esit_payload;
    dev->mouse_is_boot = candidate.is_boot;
    KLOG(LogModule::Usb, LogLevel::Info, "Found mouse interface %d alt %d (Boot: %s, EP %d, Report %d bytes)",
         dev->mouse_interface, dev->mouse_alt_setting, dev->mouse_is_boot ? "YES" : "NO", dev->mouse_endpoint,
         dev->mouse_report_desc_length);
}

static void usb_commit_msc_candidate(UsbDeviceInfo *dev, const MassStorageInterfaceCandidate &candidate)
{
    if (!dev || !candidate.present || !candidate.has_bulk_in || !candidate.has_bulk_out)
        return;

    const bool replace = !dev->has_mass_storage || (candidate.interface_number == dev->msc_interface &&
                                                    candidate.alt_setting == 0 && dev->msc_alt_setting != 0);
    if (!replace)
        return;

    dev->has_mass_storage = true;
    dev->msc_interface = candidate.interface_number;
    dev->msc_alt_setting = candidate.alt_setting;
    dev->msc_bulk_in_endpoint = candidate.bulk_in_endpoint;
    dev->msc_bulk_out_endpoint = candidate.bulk_out_endpoint;
    dev->msc_bulk_in_max_packet = candidate.bulk_in_max_packet;
    dev->msc_bulk_out_max_packet = candidate.bulk_out_max_packet;
    dev->msc_bulk_in_max_burst = candidate.bulk_in_max_burst;
    dev->msc_bulk_out_max_burst = candidate.bulk_out_max_burst;
    KLOG(LogModule::Usb, LogLevel::Info, "Found mass-storage interface %d alt %d (Bulk IN EP %d, OUT EP %d)",
         dev->msc_interface, dev->msc_alt_setting, dev->msc_bulk_in_endpoint, dev->msc_bulk_out_endpoint);
}

static bool usb_parse_config(UsbDeviceInfo *dev, uint8_t *data, uint16_t length)
{
    if (!dev || !data || length < sizeof(UsbConfigDescriptor))
        return false;

    uint16_t offset = 0;
    HidInterfaceCandidate current_hid = {};
    MassStorageInterfaceCandidate current_msc = {};
    ParsedEndpointKind last_endpoint = ParsedEndpointKind::None;

    while (offset + 2 <= length) {
        const uint8_t len = data[offset];
        const uint8_t type = data[offset + 1];
        if (len < 2 || offset + len > length)
            break;

        if (type == USB_DESC_INTERFACE && len >= sizeof(UsbInterfaceDescriptor)) {
            usb_commit_hid_candidate(dev, current_hid);
            usb_commit_msc_candidate(dev, current_msc);
            current_hid = {};
            current_msc = {};
            last_endpoint = ParsedEndpointKind::None;

            auto *iface = reinterpret_cast<UsbInterfaceDescriptor *>(&data[offset]);
            KLOG(LogModule::Usb, LogLevel::Trace, "Interface %d alt %d: Class 0x%x, SubClass 0x%x, Protocol 0x%x",
                 iface->bInterfaceNumber, iface->bAlternateSetting, iface->bInterfaceClass, iface->bInterfaceSubClass,
                 iface->bInterfaceProtocol);

            if (iface->bInterfaceClass == USB_CLASS_HID) {
                current_hid.present = true;
                current_hid.interface_number = iface->bInterfaceNumber;
                current_hid.alt_setting = iface->bAlternateSetting;
                current_hid.is_boot = iface->bInterfaceSubClass == USB_SUBCLASS_BOOT;
                if (iface->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD)
                    current_hid.kind = HidCandidateKind::Keyboard;
                else if (iface->bInterfaceProtocol == USB_PROTOCOL_MOUSE)
                    current_hid.kind = HidCandidateKind::Mouse;
            } else if (iface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                       iface->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI &&
                       iface->bInterfaceProtocol == USB_MSC_PROTOCOL_BULK_ONLY) {
                current_msc.present = true;
                current_msc.interface_number = iface->bInterfaceNumber;
                current_msc.alt_setting = iface->bAlternateSetting;
            }
        } else if (type == USB_DESC_HID && current_hid.present && len >= sizeof(UsbHidDescriptor)) {
            auto *hid = reinterpret_cast<UsbHidDescriptor *>(&data[offset]);
            current_hid.report_desc_length = hid->wReportDescriptorLength;
        } else if (type == USB_DESC_ENDPOINT && len >= sizeof(UsbEndpointDescriptor)) {
            auto *ep = reinterpret_cast<UsbEndpointDescriptor *>(&data[offset]);
            KLOG(LogModule::Usb, LogLevel::Trace, "Endpoint 0x%x: Attr 0x%x, MaxPkt %d, Interval %d",
                 ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize, ep->bInterval);

            last_endpoint = ParsedEndpointKind::None;
            if (current_hid.present && (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) &&
                (ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_INTERRUPT &&
                !current_hid.has_interrupt_in) {
                const uint8_t ep_num = ep->bEndpointAddress & 0x0F;
                current_hid.endpoint = static_cast<uint8_t>(ep_num * 2 + 1);
                current_hid.max_packet = ep->wMaxPacketSize & 0x7FF;
                current_hid.interval = ep->bInterval;
                current_hid.has_interrupt_in = true;
                current_hid.max_burst = 0;
                current_hid.mult = 0;
                current_hid.max_esit_payload = current_hid.max_packet;
                last_endpoint = ParsedEndpointKind::HidInterruptIn;
            } else if (current_msc.present &&
                       (ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_BULK) {
                const uint8_t ep_num = ep->bEndpointAddress & 0x0F;
                const uint16_t max_packet = ep->wMaxPacketSize & 0x7FF;
                if ((ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) != 0 && !current_msc.has_bulk_in) {
                    current_msc.bulk_in_endpoint = static_cast<uint8_t>(ep_num * 2 + 1);
                    current_msc.bulk_in_max_packet = max_packet;
                    current_msc.has_bulk_in = true;
                    last_endpoint = ParsedEndpointKind::MscBulkIn;
                } else if ((ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) == 0 && !current_msc.has_bulk_out) {
                    current_msc.bulk_out_endpoint = static_cast<uint8_t>(ep_num * 2);
                    current_msc.bulk_out_max_packet = max_packet;
                    current_msc.has_bulk_out = true;
                    last_endpoint = ParsedEndpointKind::MscBulkOut;
                }
            }
        } else if (type == USB_DESC_SS_ENDPOINT_COMPANION && current_hid.present &&
                   last_endpoint == ParsedEndpointKind::HidInterruptIn && current_hid.has_interrupt_in &&
                   len >= sizeof(UsbSsEpCompDescriptor)) {
            auto *companion = reinterpret_cast<UsbSsEpCompDescriptor *>(&data[offset]);
            current_hid.max_burst = companion->bMaxBurst;
            current_hid.mult = static_cast<uint8_t>(companion->bmAttributes & 0x3);
            current_hid.max_esit_payload = companion->wBytesPerInterval;
        } else if (type == USB_DESC_SS_ENDPOINT_COMPANION && current_msc.present &&
                   len >= sizeof(UsbSsEpCompDescriptor)) {
            auto *companion = reinterpret_cast<UsbSsEpCompDescriptor *>(&data[offset]);
            if (last_endpoint == ParsedEndpointKind::MscBulkIn)
                current_msc.bulk_in_max_burst = companion->bMaxBurst;
            else if (last_endpoint == ParsedEndpointKind::MscBulkOut)
                current_msc.bulk_out_max_burst = companion->bMaxBurst;
        }

        offset += len;
    }

    usb_commit_hid_candidate(dev, current_hid);
    usb_commit_msc_candidate(dev, current_msc);
    return true;
}
} // namespace

int usb_enumerate_device(uint8_t port, uint8_t parent_hub_slot, uint8_t parent_hub_port, uint8_t speed)
{
    KLOG(LogModule::Usb, LogLevel::Info, "Enumerating port %d...", port);
    if (g_usb_device_count >= USB_MAX_DEVICES) {
        KLOG(LogModule::Usb, LogLevel::Error, "Max devices reached");
        return -1;
    }

    // Determine the root hub port for this device's topology
    uint8_t root_port = port;
    if (parent_hub_slot != 0) {
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            if (g_usb_devices[i].configured && g_usb_devices[i].slot_id == parent_hub_slot) {
                root_port = g_usb_devices[i].root_hub_port;
                break;
            }
        }
    }

    // For root ports, we must reset the port first.
    // For hub ports, the hub driver already did the reset.
    if (parent_hub_slot == 0) {
        if (!xhci_reset_port(port)) {
            KLOG(LogModule::Usb, LogLevel::Error, "USB: Port %d reset failed", port);
            return -1;
        }
        // Recovery time after reset (USB spec says 10ms, but some need more)
        sleep(100);
        if (speed == 0)
            speed = xhci_get_port_speed(port);
    }

    if (speed == 0) {
        KLOG(LogModule::Usb, LogLevel::Error, "Invalid port speed");
        return -1;
    }
    KLOG(LogModule::Usb, LogLevel::Trace, "Port speed: %d", speed);
    const int slot_id = xhci_enable_slot();
    if (slot_id < 0) {
        KLOG(LogModule::Usb, LogLevel::Error, "Enable Slot failed");
        return -1;
    }
    KLOG(LogModule::Usb, LogLevel::Trace, "Slot ID: %d", slot_id);
    // Address the device (with 1 retry for flaky hardware)
    bool addressed =
        xhci_address_device(static_cast<uint8_t>(slot_id), root_port, speed, parent_hub_slot, parent_hub_port);
    if (!addressed) {
        sleep(100);
        addressed =
            xhci_address_device(static_cast<uint8_t>(slot_id), root_port, speed, parent_hub_slot, parent_hub_port);
    }

    if (!addressed) {
        KLOG(LogModule::Usb, LogLevel::Error, "USB: Address Device failed for Slot %d (Port %d)", slot_id, port);
        xhci_disable_slot(static_cast<uint8_t>(slot_id));
        return -1;
    }
    // Give the device time to process the address
    sleep(20);
    UsbDeviceDescriptor dev_desc;
    kstring::zero_memory(&dev_desc, sizeof(dev_desc));
    bool got_desc = false;
    // Only request 8 bytes initially to prevent Babble errors on Intel!
    for (int retry = 0; retry < 3 && !got_desc; retry++) {
        if (retry > 0)
            sleep(10);
        got_desc = usb_get_device_descriptor(static_cast<uint8_t>(slot_id), &dev_desc, 8);
    }
    if (!got_desc) {
        KLOG(LogModule::Usb, LogLevel::Error, "USB: Get Device Descriptor (8 bytes) failed (Slot %d)", slot_id);
        xhci_disable_slot(static_cast<uint8_t>(slot_id));
        return -1;
    }

    // Update the packet size using the 8th byte we just read safely
    extern bool xhci_update_ep0_packet_size(uint8_t slot_id, uint16_t max_packet);
    uint16_t actual_mps = (speed >= XHCI_SPEED_SUPER && dev_desc.bMaxPacketSize0 == 9) ? 512 : dev_desc.bMaxPacketSize0;
    xhci_update_ep0_packet_size(static_cast<uint8_t>(slot_id), actual_mps);

    // Now safe to fetch the full descriptor
    got_desc = usb_get_device_descriptor(static_cast<uint8_t>(slot_id), &dev_desc, sizeof(dev_desc));
    if (!got_desc) {
        KLOG(LogModule::Usb, LogLevel::Error, "USB: Get Full Device Descriptor failed (Slot %d)", slot_id);
        xhci_disable_slot(static_cast<uint8_t>(slot_id));
        return -1;
    }

    int dev_idx = -1;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_usb_devices[i].configured) {
            dev_idx = i;
            break;
        }
    }
    if (dev_idx == -1) {
        KLOG(LogModule::Usb, LogLevel::Error, "Max devices reached (no free slots)");
        return -1;
    }

    UsbDeviceInfo *dev = &g_usb_devices[dev_idx];
    kstring::zero_memory(dev, sizeof(UsbDeviceInfo));
    dev->slot_id = slot_id;
    dev->port = port;
    dev->parent_hub_slot = parent_hub_slot;
    dev->parent_hub_port = parent_hub_port;
    dev->speed = speed;
    dev->vendor_id = dev_desc.idVendor;
    dev->product_id = dev_desc.idProduct;
    dev->root_hub_port = root_port;
    dev->device_class = dev_desc.bDeviceClass;
    dev->device_subclass = dev_desc.bDeviceSubClass;
    dev->device_protocol = dev_desc.bDeviceProtocol;

    KLOG(LogModule::Usb, LogLevel::Info,
         "Device Descriptor: Class 0x%x, SubClass 0x%x, Protocol 0x%x, Vendor 0x%x, Product 0x%x", dev->device_class,
         dev->device_subclass, dev->device_protocol, dev->vendor_id, dev->product_id);

    uint8_t config_header[9];
    if (!usb_get_config_descriptor(slot_id, 0, config_header, 9)) {
        KLOG(LogModule::Usb, LogLevel::Error, "Get Config Header failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    auto *cfg = reinterpret_cast<UsbConfigDescriptor *>(config_header);
    const uint16_t total_length = cfg->wTotalLength;
    kstd::unique_ptr<uint8_t[]> full_config(new uint8_t[total_length]);
    if (!full_config) {
        xhci_disable_slot(slot_id);
        return -1;
    }
    if (!usb_get_config_descriptor(slot_id, 0, full_config.get(), total_length)) {
        KLOG(LogModule::Usb, LogLevel::Error, "Get Full Config failed");
        xhci_disable_slot(slot_id);
        return -1;
    }
    dev->config_value = cfg->bConfigurationValue;
    dev->num_interfaces = cfg->bNumInterfaces;
    usb_parse_config(dev, full_config.get(), total_length);
    if (!usb_set_configuration(slot_id, dev->config_value)) {
        KLOG(LogModule::Usb, LogLevel::Error, "USB: Set Configuration failed (Slot %d)", slot_id);
        xhci_disable_slot(slot_id);
        return -1;
    }
    KLOG(LogModule::Usb, LogLevel::Trace, "USB: Device configured (Slot %d)", slot_id);

    if (dev->has_keyboard && dev->kbd_endpoint != 0) {
        if (dev->kbd_alt_setting != 0 && !usb_set_interface(slot_id, dev->kbd_interface, dev->kbd_alt_setting)) {
            KLOG(LogModule::Usb, LogLevel::Error, "USB: Set keyboard interface %d alt %d failed (Slot %d)",
                 dev->kbd_interface, dev->kbd_alt_setting, slot_id);
            dev->kbd_endpoint = 0;
        } else if (!xhci_configure_endpoint(slot_id, dev->kbd_endpoint, EP_TYPE_INTERRUPT_IN, dev->kbd_max_packet,
                                            dev->kbd_interval, dev->kbd_max_burst, dev->kbd_mult,
                                            dev->kbd_max_esit_payload)) {
            KLOG(LogModule::Usb, LogLevel::Error, "USB: Configure keyboard endpoint failed (Slot %d, EP %d)", slot_id,
                 dev->kbd_endpoint);
            dev->kbd_endpoint = 0;
        } else {
            KLOG(LogModule::Usb, LogLevel::Trace, "USB: Keyboard endpoint %d configured (Slot %d)", dev->kbd_endpoint,
                 slot_id);
        }
    }
    if (dev->has_mouse && dev->mouse_endpoint != 0) {
        if (dev->mouse_alt_setting != 0 && !usb_set_interface(slot_id, dev->mouse_interface, dev->mouse_alt_setting)) {
            KLOG(LogModule::Usb, LogLevel::Error, "USB: Set mouse interface %d alt %d failed (Slot %d)",
                 dev->mouse_interface, dev->mouse_alt_setting, slot_id);
            dev->mouse_endpoint = 0;
        } else if (!xhci_configure_endpoint(slot_id, dev->mouse_endpoint, EP_TYPE_INTERRUPT_IN, dev->mouse_max_packet,
                                            dev->mouse_interval, dev->mouse_max_burst, dev->mouse_mult,
                                            dev->mouse_max_esit_payload)) {
            KLOG(LogModule::Usb, LogLevel::Error, "USB: Configure mouse endpoint failed (Slot %d, EP %d)", slot_id,
                 dev->mouse_endpoint);
            dev->mouse_endpoint = 0;
        } else {
            KLOG(LogModule::Usb, LogLevel::Trace, "USB: Mouse endpoint %d configured (Slot %d)", dev->mouse_endpoint,
                 slot_id);
        }
    }
    if (dev->has_mass_storage && dev->msc_bulk_in_endpoint != 0 && dev->msc_bulk_out_endpoint != 0) {
        if (dev->msc_alt_setting != 0 && !usb_set_interface(slot_id, dev->msc_interface, dev->msc_alt_setting)) {
            KLOG(LogModule::Usb, LogLevel::Error, "USB: Set MSC interface %d alt %d failed (Slot %d)",
                 dev->msc_interface, dev->msc_alt_setting, slot_id);
            dev->has_mass_storage = false;
        } else if (!xhci_configure_endpoint(slot_id, dev->msc_bulk_out_endpoint, EP_TYPE_BULK_OUT,
                                            dev->msc_bulk_out_max_packet, 0, dev->msc_bulk_out_max_burst) ||
                   !xhci_configure_endpoint(slot_id, dev->msc_bulk_in_endpoint, EP_TYPE_BULK_IN,
                                            dev->msc_bulk_in_max_packet, 0, dev->msc_bulk_in_max_burst)) {
            KLOG(LogModule::Usb, LogLevel::Error, "USB: Configure MSC endpoints failed (Slot %d, IN %d, OUT %d)",
                 slot_id, dev->msc_bulk_in_endpoint, dev->msc_bulk_out_endpoint);
            dev->has_mass_storage = false;
        } else {
            KLOG(LogModule::Usb, LogLevel::Trace, "USB: Mass-storage endpoints configured (Slot %d)", slot_id);
        }
    }
    dev->configured = true;

    // Register hub ONLY AFTER device is configured
    if (dev->device_class == 0x09)
        usb_hub_register(slot_id, port, speed);

    // Notify HID driver for hotplug support
    usb_hid_device_connected(dev);
    usb_msc_device_connected(dev);

    if (dev_idx >= g_usb_device_count)
        g_usb_device_count = dev_idx + 1;
    return dev_idx;
}

bool usb_get_device_descriptor(uint8_t slot_id, UsbDeviceDescriptor *desc, uint16_t size)
{
    uint16_t transferred;
    return xhci_control_transfer(slot_id, USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                                 USB_REQ_GET_DESCRIPTOR, (USB_DESC_DEVICE << 8) | 0, 0, size, desc, &transferred) &&
           transferred >= 8;
}

bool usb_get_config_descriptor(uint8_t slot_id, uint8_t index, void *buffer, uint16_t size)
{
    uint16_t transferred;
    return xhci_control_transfer(slot_id, USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                                 USB_REQ_GET_DESCRIPTOR, (USB_DESC_CONFIGURATION << 8) | index, 0, size, buffer,
                                 &transferred);
}

bool usb_set_configuration(uint8_t slot_id, uint8_t config_value)
{
    uint16_t transferred;
    return xhci_control_transfer(slot_id, USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_DEVICE,
                                 USB_REQ_SET_CONFIGURATION, config_value, 0, 0, nullptr, &transferred);
}

bool usb_set_interface(uint8_t slot_id, uint8_t interface_number, uint8_t alt_setting)
{
    uint16_t transferred;
    return xhci_control_transfer(slot_id, USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_INTERFACE,
                                 USB_REQ_SET_INTERFACE, alt_setting, interface_number, 0, nullptr, &transferred);
}

bool usb_get_hid_report_descriptor(uint8_t slot_id, uint8_t interface_number, void *buffer, uint16_t size,
                                   uint16_t *transferred)
{
    uint16_t actual = 0;
    const bool ok = xhci_control_transfer(
        slot_id, USB_REQ_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_INTERFACE, USB_REQ_GET_DESCRIPTOR,
        static_cast<uint16_t>(USB_DESC_HID_REPORT << 8), interface_number, size, buffer, &actual);
    if (transferred)
        *transferred = actual;
    return ok;
}

void usb_remove_device(uint8_t parent_hub_slot, uint8_t parent_hub_port)
{
    for (int i = 0; i < g_usb_device_count; i++) {
        if (g_usb_devices[i].parent_hub_slot == parent_hub_slot &&
            g_usb_devices[i].parent_hub_port == parent_hub_port && g_usb_devices[i].configured) {
            KLOG(LogModule::Usb, LogLevel::Info, "Removing USB device from Hub %d Port %d", parent_hub_slot,
                 parent_hub_port);
            usb_hid_device_disconnected(&g_usb_devices[i]);
            usb_msc_device_disconnected(&g_usb_devices[i]);
            xhci_disable_slot(g_usb_devices[i].slot_id);
            g_usb_devices[i].configured = false;
            return;
        }
    }
}

int usb_get_device_count()
{
    return g_usb_device_count;
}
UsbDeviceInfo *usb_get_device(int index)
{
    return (index < 0 || index >= g_usb_device_count) ? nullptr : &g_usb_devices[index];
}

UsbDeviceInfo *usb_find_keyboard()
{
    for (int i = 0; i < g_usb_device_count; i++) {
        if (g_usb_devices[i].has_keyboard && g_usb_devices[i].configured && g_usb_devices[i].kbd_endpoint != 0)
            return &g_usb_devices[i];
    }
    return nullptr;
}

UsbDeviceInfo *usb_find_mouse()
{
    for (int i = 0; i < g_usb_device_count; i++) {
        if (g_usb_devices[i].has_mouse && g_usb_devices[i].configured && g_usb_devices[i].mouse_endpoint != 0)
            return &g_usb_devices[i];
    }
    return nullptr;
}

void usb_init()
{
    if (g_usb_initialized)
        return;

    g_usb_device_count = 0;
    for (auto &dev : g_usb_devices)
        kstring::zero_memory(&dev, sizeof(UsbDeviceInfo));
    usb_msc_init();
    if (!xhci_init())
        return;
    usb_hub_init();
    const uint8_t max_ports = xhci_get_max_ports();
    for (uint8_t port = 1; port <= max_ports; port++) {
        if (xhci_port_connected(port)) {
            usb_enumerate_device(port);
        }
    }
    g_usb_initialized = true;
}

void usb_poll()
{
    if (!xhci_is_initialized())
        return;
    xhci_poll_events();
    usb_hub_poll();

    const uint8_t max_ports = xhci_get_max_ports();
    for (uint8_t port = 1; port <= max_ports; port++) {
        if (xhci_get_port_needs_enumeration(port)) {
            xhci_clear_port_needs_enumeration(port);

            const bool connected = xhci_port_connected(port);

            int existing_idx = -1;
            for (int i = 0; i < g_usb_device_count; i++) {
                if (g_usb_devices[i].port == port && g_usb_devices[i].parent_hub_slot == 0 &&
                    g_usb_devices[i].configured) {
                    existing_idx = i;
                    break;
                }
            }

            if (connected) {
                if (existing_idx != -1) {
                    // Force re-enumeration on change
                    KLOG(LogModule::Usb, LogLevel::Info, "USB Port %d status changed while connected, re-enumerating",
                         port);
                    usb_hid_device_disconnected(&g_usb_devices[existing_idx]);
                    usb_msc_device_disconnected(&g_usb_devices[existing_idx]);
                    xhci_disable_slot(g_usb_devices[existing_idx].slot_id);
                    g_usb_devices[existing_idx].configured = false;
                }
                usb_enumerate_device(port);
            } else {
                if (existing_idx != -1) {
                    KLOG(LogModule::Usb, LogLevel::Info, "USB Device on port %d disconnected", port);
                    usb_hid_device_disconnected(&g_usb_devices[existing_idx]);
                    usb_msc_device_disconnected(&g_usb_devices[existing_idx]);
                    xhci_disable_slot(g_usb_devices[existing_idx].slot_id);
                    g_usb_devices[existing_idx].configured = false;
                }
            }
        }
    }
}
