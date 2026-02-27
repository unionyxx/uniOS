#include <stdint.h>
#include <stddef.h>
#include <boot/limine.h>

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id       = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id       = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/time/timer.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/arch/x86_64/pat.h>
#include <kernel/mm/heap.h>
#include <kernel/scheduler.h>
#include <kernel/fs/unifs.h>
#include <kernel/shell.h>
#include <kernel/debug.h>
#include <drivers/bus/pci/pci.h>
#include <drivers/bus/usb/usb.h>
#include <drivers/class/hid/usb_hid.h>
#include <drivers/video/framebuffer.h>
#include <drivers/class/hid/input.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <drivers/acpi/acpi.h>
#include <drivers/rtc/rtc.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/net/net.h>
#include <drivers/sound/sound.h>
#include <kernel/panic.h>

struct limine_framebuffer* g_framebuffer = nullptr;

static char  g_bootloader_name_buf[64];
static char  g_bootloader_version_buf[64];
const  char* g_bootloader_name    = nullptr;
const  char* g_bootloader_version = nullptr;

extern void call_global_constructors();
extern void cpu_enable_sse();

static void idle_task_entry() {
    while (true) {
        asm volatile("hlt");
    }
}

extern "C" void _start(void) {
    call_global_constructors();
    cpu_enable_sse();

    if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
    if (!framebuffer_request.response ||
         framebuffer_request.response->framebuffer_count < 1) hcf();

    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    g_framebuffer = fb;

    gfx_init(fb);
    debug_init(fb);
    g_boot_quiet = false; // Show boot logs during development
    gfx_clear(COLOR_BLACK);

    serial_init();
    serial_printf("\r\n=== uniOS Kernel @ %s ===\r\n", GIT_COMMIT);
    DEBUG_INFO("uniOS Kernel @ %s starting...", GIT_COMMIT);

    if (bootloader_info_request.response) {
        const char* src_name = bootloader_info_request.response->name;
        const char* src_ver  = bootloader_info_request.response->version;

        int i = 0;
        while (src_name && src_name[i] && i < 63) {
            g_bootloader_name_buf[i] = src_name[i];
            i++;
        }
        g_bootloader_name_buf[i] = '\0';
        g_bootloader_name = g_bootloader_name_buf;

        i = 0;
        while (src_ver && src_ver[i] && i < 63) {
            g_bootloader_version_buf[i] = src_ver[i];
            i++;
        }
        g_bootloader_version_buf[i] = '\0';
        g_bootloader_version = g_bootloader_version_buf;

        serial_printf("Bootloader: %s %s\r\n", g_bootloader_name, g_bootloader_version);
    }

    gdt_init();
    idt_init();

    pic_remap(32, 40);
    for (int i = 0; i < 16; i++) pic_set_mask(i);

    vmm_init();
    pmm_init();
    pat_init();

    if (fb) {
        uint64_t fb_size = fb->pitch * fb->height;
        vmm_remap_framebuffer((uint64_t)fb->address, fb_size);
    }

    heap_init(nullptr, 0);
    gfx_enable_double_buffering();

    scheduler_init();
    scheduler_create_task(idle_task_entry, "Idle");

    ps2_keyboard_init();
    ps2_mouse_init();

    timer_init(1000);

    pci_init();
    acpi_init();
    rtc_init();
    usb_init();
    usb_hid_init();

    input_set_screen_size(fb->width, fb->height);
    net_init();
    sound_init();

    asm("sti");
    DEBUG_SUCCESS("Boot Sequence Complete");

    if (module_request.response && module_request.response->module_count > 0) {
        unifs_init(module_request.response->modules[0]->address);
    }

#ifdef DEBUG
    serial_puts("[DEBUG] Boot complete!\r\n");
    DEBUG_INFO("Boot complete! Press any key to continue...");
    gfx_swap_buffers();

    while (!input_keyboard_has_char()) {
        input_poll();
        scheduler_yield();
    }
    input_keyboard_get_char();
#endif

    gfx_clear(COLOR_BLACK);
    gfx_draw_centered_text("uniOS", COLOR_WHITE);
    gfx_swap_buffers();

    uint64_t splash_start = timer_get_ticks();
    while (timer_get_ticks() - splash_start < 500) {
        input_poll();
        asm volatile("hlt");
    }

    gfx_clear(COLOR_BLACK);
    gfx_swap_buffers();

    shell_init(fb);

    uint64_t last_frame_time = 0;
    const uint64_t frame_delay = 16;

    while (true) {
        input_poll();
        net_poll();
        sound_poll();

        uint64_t now = timer_get_ticks();
        if (now - last_frame_time >= frame_delay) {
            shell_tick();
            while (input_keyboard_has_char()) {
                shell_process_char(input_keyboard_get_char());
            }
            gfx_swap_buffers();
            last_frame_time = now;
        } else {
            asm volatile("hlt");
        }
    }
}
