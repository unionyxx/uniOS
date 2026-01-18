#include <kernel/arch/x86_64/pic.h>
#include <kernel/arch/x86_64/io.h>

void pic_remap(uint8_t offset1, uint8_t offset2) {
    uint8_t a1, a2;
    
    // Save masks
    a1 = inb(PIC1_DATA);
    a2 = inb(PIC2_DATA);
    
    // Start initialization sequence (ICW1)
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    
    // ICW2: Set vector offsets
    outb(PIC1_DATA, offset1);
    io_wait();
    outb(PIC2_DATA, offset2);
    io_wait();
    
    // ICW3: Tell Master PIC there is a slave PIC at IRQ2
    outb(PIC1_DATA, 4);
    io_wait();
    // ICW3: Tell Slave PIC its cascade identity
    outb(PIC2_DATA, 2);
    io_wait();
    
    // ICW4: 8086 mode
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();
    
    // Restore saved masks
    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) | (1 << irq);
    outb(port, value);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}
