#pragma once
#include <stdbool.h>
#include <stdint.h>

struct UsbHub
{
    uint8_t root_port;
    uint8_t num_ports;
    uint8_t device_slot;
    uint8_t intr_endpoint;
    volatile bool status_changed;
};

// Initialize the USB Hub driver
void usb_hub_init();

// Register a newly found Hub device. intr_endpoint is the DCI of the hub's
// status-change interrupt-IN endpoint (0 if it could not be configured).
bool usb_hub_register(uint8_t slot_id, uint8_t root_port, uint8_t speed, uint8_t intr_endpoint);

// Poll all registered hubs for status changes
void usb_hub_poll();
