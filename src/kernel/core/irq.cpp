#include <stdint.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/time/timer.h>
#include <kernel/scheduler.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <drivers/bus/usb/xhci/xhci.h>

extern "C" void irq_handler(void* stack_frame) {
    const uint64_t int_no = static_cast<uint64_t*>(stack_frame)[15];
    const uint8_t irq = static_cast<uint8_t>(int_no - 32);

    pic_send_eoi(irq);

    if (irq == 0) {
        timer_handler();
        scheduler_schedule();
    } else if (irq == 1) {
        ps2_keyboard_handler();
    } else if (irq == 12) {
        ps2_mouse_handler();
    } else if (xhci_is_initialized() && irq == xhci_get_irq()) {
        xhci_poll_events();
    }
}
