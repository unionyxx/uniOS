#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>

static bool g_pic_disabled = false;

static inline bool pic_valid_irq(uint8_t irq)
{
    return irq < 16;
}

void pic_disable()
{
    outb(PIC1_DATA, 0xFF);
    io_wait();
    outb(PIC2_DATA, 0xFF);
    io_wait();
    g_pic_disabled = true;
}

void pic_remap(uint8_t offset1, uint8_t offset2)
{
    const uint8_t a1 = inb(PIC1_DATA);
    const uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, offset1);
    io_wait();
    outb(PIC2_DATA, offset2);
    io_wait();

    // Master has slave on IRQ2.
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, a1);
    io_wait();
    outb(PIC2_DATA, a2);
    io_wait();

    g_pic_disabled = false;
}

void pic_send_eoi(uint8_t irq)
{
    if (g_pic_disabled || !pic_valid_irq(irq))
        return;

    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq)
{
    if (g_pic_disabled || !pic_valid_irq(irq))
        return;

    uint16_t port;
    uint8_t bit;

    if (irq < 8) {
        port = PIC1_DATA;
        bit = irq;
    } else {
        port = PIC2_DATA;
        bit = static_cast<uint8_t>(irq - 8);
    }

    outb(port, static_cast<uint8_t>(inb(port) | (1u << bit)));
}

void pic_clear_mask(uint8_t irq)
{
    if (g_pic_disabled || !pic_valid_irq(irq))
        return;

    if (irq < 8) {
        outb(PIC1_DATA, static_cast<uint8_t>(inb(PIC1_DATA) & ~(1u << irq)));
        return;
    }

    const uint8_t slave_irq = static_cast<uint8_t>(irq - 8);
    outb(PIC2_DATA, static_cast<uint8_t>(inb(PIC2_DATA) & ~(1u << slave_irq)));

    // Ensure the slave cascade line is unmasked on the master.
    outb(PIC1_DATA, static_cast<uint8_t>(inb(PIC1_DATA) & ~(1u << 2)));
}