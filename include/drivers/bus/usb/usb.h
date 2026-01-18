#pragma once
#include <stdint.h>
#include <kernel/debug.h>

// USB Descriptor Types
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIGURATION  2
#define USB_DESC_STRING         3
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22

// USB Request Types
#define USB_REQ_HOST_TO_DEVICE  0x00
#define USB_REQ_DEVICE_TO_HOST  0x80
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40
#define USB_REQ_RECIPIENT_DEVICE    0x00
#define USB_REQ_RECIPIENT_INTERFACE 0x01
#define USB_REQ_RECIPIENT_ENDPOINT  0x02

// USB Standard Requests
#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_DESCRIPTOR    7
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_GET_INTERFACE     10
#define USB_REQ_SET_INTERFACE     11

// USB Class Codes
#define USB_CLASS_HID           0x03
#define USB_SUBCLASS_BOOT       0x01
#define USB_PROTOCOL_KEYBOARD   0x01
#define USB_PROTOCOL_MOUSE      0x02

// Endpoint direction/type
#define USB_ENDPOINT_DIR_MASK   0x80
#define USB_ENDPOINT_DIR_IN     0x80
#define USB_ENDPOINT_DIR_OUT    0x00
#define USB_ENDPOINT_TYPE_MASK  0x03
#define USB_ENDPOINT_TYPE_CONTROL   0
#define USB_ENDPOINT_TYPE_ISOCH     1
#define USB_ENDPOINT_TYPE_BULK      2
#define USB_ENDPOINT_TYPE_INTERRUPT 3

// USB Device Descriptor
struct UsbDeviceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

// USB Configuration Descriptor
struct UsbConfigDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

// USB Interface Descriptor
struct UsbInterfaceDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

// USB Endpoint Descriptor
struct UsbEndpointDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

// USB HID Descriptor
struct UsbHidDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    uint8_t  bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));

// USB Device (internal representation)
struct UsbDeviceInfo {
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
    bool configured;
    
    // For HID devices - primary interface (keyboard)
    bool is_keyboard;
    bool is_mouse;
    bool is_boot_interface;  // True if Subclass 1 (Boot Interface)
    uint8_t hid_interface;   // Primary HID interface number
    uint8_t hid_endpoint;    // Primary HID endpoint (keyboard)
    uint16_t hid_max_packet;
    uint8_t hid_interval;
    
    // For composite devices with secondary HID interface (mouse)
    uint8_t hid_interface2;  // Secondary HID interface number
    uint8_t hid_endpoint2;   // Secondary HID endpoint (mouse)
    uint16_t hid_max_packet2;
    uint8_t hid_interval2;
};

// Maximum devices
#define USB_MAX_DEVICES 16

// USB subsystem functions
void usb_init();
int usb_enumerate_device(uint8_t port);
bool usb_get_device_descriptor(uint8_t slot_id, UsbDeviceDescriptor* desc);
bool usb_get_config_descriptor(uint8_t slot_id, uint8_t index, void* buffer, uint16_t size);
bool usb_set_configuration(uint8_t slot_id, uint8_t config_value);

// Device access
int usb_get_device_count();
UsbDeviceInfo* usb_get_device(int index);
UsbDeviceInfo* usb_find_keyboard();
UsbDeviceInfo* usb_find_mouse();

// Polling
void usb_poll();

// Debug logging
void usb_set_debug(bool enabled);
