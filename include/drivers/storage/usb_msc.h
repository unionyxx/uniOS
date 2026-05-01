#pragma once

struct UsbDeviceInfo;

void usb_msc_init();
void usb_msc_device_connected(UsbDeviceInfo *dev);
void usb_msc_device_disconnected(const UsbDeviceInfo *dev);
