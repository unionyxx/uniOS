#pragma once
#include <stdint.h>

// Port I/O - 8-bit
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Port I/O - 16-bit
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

// Port I/O - 32-bit
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

// I/O wait (for PIC and other slow devices)
static inline void io_wait() {
    outb(0x80, 0);
}

// Memory-mapped I/O with barriers
static inline uint32_t mmio_read32(volatile void* addr) {
    uint32_t val = *(volatile uint32_t*)addr;
    asm volatile("mfence" ::: "memory");
    return val;
}

static inline void mmio_write32(volatile void* addr, uint32_t val) {
    asm volatile("mfence" ::: "memory");
    *(volatile uint32_t*)addr = val;
    asm volatile("mfence" ::: "memory");
}

static inline uint64_t mmio_read64(volatile void* addr) {
    uint64_t val = *(volatile uint64_t*)addr;
    asm volatile("mfence" ::: "memory");
    return val;
}

static inline void mmio_write64(volatile void* addr, uint64_t val) {
    asm volatile("mfence" ::: "memory");
    *(volatile uint64_t*)addr = val;
    asm volatile("mfence" ::: "memory");
}
