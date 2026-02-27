#pragma once
#include <stdint.h>
#include <stdbool.h>

struct UsbHub {
    uint8_t port;
    uint8_t num_ports;
    uint8_t device_address;
};

// Initialize the USB Hub driver
void usb_hub_init();

// Register a newly found Hub device
bool usb_hub_register(uint8_t address, uint8_t port, uint8_t speed);
