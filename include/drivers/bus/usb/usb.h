#pragma once
#include <kernel/debug.h>
#include <stdint.h>

// USB Descriptor Types
#define USB_DESC_DEVICE 1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_STRING 3
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT 5
#define USB_DESC_HID 0x21
#define USB_DESC_HID_REPORT 0x22
#define USB_DESC_SS_ENDPOINT_COMPANION 0x30

// USB Request Types
#define USB_REQ_HOST_TO_DEVICE 0x00
#define USB_REQ_DEVICE_TO_HOST 0x80
#define USB_REQ_TYPE_STANDARD 0x00
#define USB_REQ_TYPE_CLASS 0x20
#define USB_REQ_TYPE_VENDOR 0x40
#define USB_REQ_RECIPIENT_DEVICE 0x00
#define USB_REQ_RECIPIENT_INTERFACE 0x01
#define USB_REQ_RECIPIENT_ENDPOINT 0x02
#define USB_REQ_RECIPIENT_OTHER 0x03

// USB Standard Requests
#define USB_REQ_GET_STATUS 0
#define USB_REQ_CLEAR_FEATURE 1
#define USB_REQ_SET_FEATURE 3
#define USB_REQ_SET_ADDRESS 5
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_DESCRIPTOR 7
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_GET_INTERFACE 10
#define USB_REQ_SET_INTERFACE 11

// USB Class Codes
#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE 0x02
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_MSC_SUBCLASS_SCSI 0x06
#define USB_MSC_PROTOCOL_BULK_ONLY 0x50

// Endpoint direction/type
#define USB_ENDPOINT_DIR_MASK 0x80
#define USB_ENDPOINT_DIR_IN 0x80
#define USB_ENDPOINT_DIR_OUT 0x00
#define USB_ENDPOINT_TYPE_MASK 0x03
#define USB_ENDPOINT_TYPE_CONTROL 0
#define USB_ENDPOINT_TYPE_ISOCH 1
#define USB_ENDPOINT_TYPE_BULK 2
#define USB_ENDPOINT_TYPE_INTERRUPT 3

// USB Device Descriptor
struct UsbDeviceDescriptor
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

// USB Configuration Descriptor
struct UsbConfigDescriptor
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed));

// USB Interface Descriptor
struct UsbInterfaceDescriptor
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

// USB Endpoint Descriptor
struct UsbEndpointDescriptor
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

// USB HID Descriptor
struct UsbHidDescriptor
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));

struct UsbSsEpCompDescriptor
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bMaxBurst;
    uint8_t bmAttributes;
    uint16_t wBytesPerInterval;
} __attribute__((packed));

// USB Device (internal representation)
struct UsbDeviceInfo
{
    uint8_t slot_id;
    uint8_t port;
    uint8_t parent_hub_slot; // 0 if root port
    uint8_t parent_hub_port; // 0 if root port
    uint8_t speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t config_value;
    uint8_t num_interfaces;
    bool configured;

    // HID Keyboard support
    bool has_keyboard;
    uint8_t kbd_interface;
    uint8_t kbd_alt_setting;
    uint8_t kbd_endpoint;
    uint16_t kbd_max_packet;
    uint8_t kbd_interval;
    uint16_t kbd_report_desc_length;
    uint8_t kbd_max_burst;
    uint8_t kbd_mult;
    uint16_t kbd_max_esit_payload;
    bool kbd_is_boot;

    // HID Mouse support
    bool has_mouse;
    uint8_t mouse_interface;
    uint8_t mouse_alt_setting;
    uint8_t mouse_endpoint;
    uint16_t mouse_max_packet;
    uint8_t mouse_interval;
    uint16_t mouse_report_desc_length;
    uint8_t mouse_max_burst;
    uint8_t mouse_mult;
    uint16_t mouse_max_esit_payload;
    bool mouse_is_boot;

    // Mass Storage (Bulk-Only Transport) support
    bool has_mass_storage;
    uint8_t msc_interface;
    uint8_t msc_alt_setting;
    uint8_t msc_bulk_in_endpoint;
    uint8_t msc_bulk_out_endpoint;
    uint16_t msc_bulk_in_max_packet;
    uint16_t msc_bulk_out_max_packet;
    uint8_t msc_bulk_in_max_burst;
    uint8_t msc_bulk_out_max_burst;

    uint8_t root_hub_port; // The port on the ROOT hub (motherboard)
};

// Maximum devices
#define USB_MAX_DEVICES 16

// USB subsystem functions
void usb_init();
void usb_poll();
int usb_enumerate_device(uint8_t port, uint8_t parent_hub_slot = 0, uint8_t parent_hub_port = 0, uint8_t speed = 0);
bool usb_get_device_descriptor(uint8_t slot_id, UsbDeviceDescriptor *desc, uint16_t size = sizeof(UsbDeviceDescriptor));
bool usb_get_config_descriptor(uint8_t slot_id, uint8_t index, void *buffer, uint16_t size);
bool usb_set_configuration(uint8_t slot_id, uint8_t config_value);
bool usb_set_interface(uint8_t slot_id, uint8_t interface_number, uint8_t alt_setting);
bool usb_get_hid_report_descriptor(uint8_t slot_id, uint8_t interface_number, void *buffer, uint16_t size,
                                   uint16_t *transferred = nullptr);
void usb_remove_device(uint8_t parent_hub_slot, uint8_t parent_hub_port);

// Device access
int usb_get_device_count();
UsbDeviceInfo *usb_get_device(int index);
UsbDeviceInfo *usb_find_keyboard();
UsbDeviceInfo *usb_find_mouse();

// HID hotplug support
void usb_hid_device_connected(UsbDeviceInfo *dev);
void usb_hid_device_disconnected(const UsbDeviceInfo *dev);

// Debug logging
void usb_set_debug(bool enabled);
