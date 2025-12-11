#pragma once

#include <stdint.h>

// Standard COM port base addresses
#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8
#define COM3_PORT 0x3E8
#define COM4_PORT 0x2E8

// Initialize serial port (default: COM1, 115200 baud)
void serial_init();
void serial_init_port(uint16_t port, uint32_t baud);

// Write a single character
void serial_putc(char c);

// Write a null-terminated string
void serial_puts(const char* str);

// Write formatted output (simple printf-like)
void serial_printf(const char* fmt, ...);

// Check if serial is ready to transmit
bool serial_is_ready();
