#include <stdint.h>
#include <stddef.h>
#include "limine.h"

// Limine base revision
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "ps2_keyboard.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "scheduler.h"
#include "unifs.h"
#include "shell.h"
#include "ps2_mouse.h"
#include "debug.h"
#include "pci.h"
#include "usb.h"
#include "usb_hid.h"
#include "xhci.h"
#include "graphics.h"
#include "input.h"
#include "acpi.h"
#include "rtc.h"
#include "serial.h"
#include "net.h"
#include "version.h"

// Global framebuffer pointer
struct limine_framebuffer* g_framebuffer = nullptr;

// Global bootloader info (for version command)
const char* g_bootloader_name = nullptr;
const char* g_bootloader_version = nullptr;

#include "panic.h"

// IRQ handler
extern "C" void irq_handler(void* stack_frame) {
    uint64_t* regs = (uint64_t*)stack_frame;
    uint64_t int_no = regs[15];
    uint8_t irq = int_no - 32;
    
    pic_send_eoi(irq);

    if (irq == 0) {
        timer_handler();
        scheduler_schedule();
    } else if (irq == 1) {
        ps2_keyboard_handler();
    } else if (irq == 12) {
        ps2_mouse_handler();
    }
}


// User mode test program
static void user_program() __attribute__((section(".user_code")));
static void user_program() {
    const char* msg = "Hello from User Mode!\n";
    asm volatile(
        "mov $1, %%rax\n"
        "mov %0, %%rbx\n"
        "mov $22, %%rcx\n"
        "int $0x80\n"
        : : "r"(msg) : "rax", "rbx", "rcx"
    );
    asm volatile("mov $60, %%rax\n" "int $0x80\n" : : : "rax");
    for(;;);
}

static uint8_t user_stack[4096] __attribute__((aligned(16)));
extern "C" void jump_to_user_mode(uint64_t code_sel, uint64_t stack, uint64_t entry);

void run_user_test() {
    user_program();
}

// GUI Mode

static uint32_t cursor_backup[12 * 19];
static int32_t backup_x = -1, backup_y = -1;

static void save_cursor_area(int32_t x, int32_t y) {
    uint32_t* fb = (uint32_t*)g_framebuffer->address;
    uint32_t pitch = g_framebuffer->pitch / 4;
    int idx = 0;
    for (int row = 0; row < 19; row++) {
        for (int col = 0; col < 12; col++) {
            int32_t px = x + col, py = y + row;
            if (px >= 0 && py >= 0 && px < (int32_t)g_framebuffer->width && py < (int32_t)g_framebuffer->height) {
                cursor_backup[idx] = fb[py * pitch + px];
            }
            idx++;
        }
    }
    backup_x = x; backup_y = y;
}

static void restore_cursor_area() {
    if (backup_x < 0) return;
    uint32_t* fb = (uint32_t*)g_framebuffer->address;
    uint32_t pitch = g_framebuffer->pitch / 4;
    int idx = 0;
    for (int row = 0; row < 19; row++) {
        for (int col = 0; col < 12; col++) {
            int32_t px = backup_x + col, py = backup_y + row;
            if (px >= 0 && py >= 0 && px < (int32_t)g_framebuffer->width && py < (int32_t)g_framebuffer->height) {
                fb[py * pitch + px] = cursor_backup[idx];
            }
            idx++;
        }
    }
}

void gui_start() {
    // Note: gfx and mouse are already initialized during boot
    // No need to reinitialize - would cause memory leaks and issues
    
    uint32_t screen_width = g_framebuffer->width;
    uint32_t screen_height = g_framebuffer->height;
    uint32_t taskbar_height = 40;
    
    // Draw gradient desktop background
    gfx_draw_gradient_v(0, 0, screen_width, screen_height - taskbar_height, 
                        COLOR_DESKTOP_TOP, COLOR_DESKTOP_BOTTOM);
    
    // Draw taskbar
    gfx_fill_rect(0, screen_height - taskbar_height, screen_width, taskbar_height, COLOR_TASKBAR);
    gfx_fill_rect(0, screen_height - taskbar_height, screen_width, 1, 0x333350);  // Top border
    
    // Draw start button
    gfx_fill_rect(8, screen_height - taskbar_height + 8, 80, 24, COLOR_ACCENT);
    gfx_draw_string(20, screen_height - taskbar_height + 14, "uniOS", COLOR_WHITE);
    
    // Draw desktop icons
    int icon_y = 30;
    int icon_spacing = 80;
    
    // Terminal icon
    gfx_fill_rect(30, icon_y, 48, 48, 0x2a2a4a);
    gfx_draw_rect(30, icon_y, 48, 48, COLOR_ACCENT);
    gfx_draw_string(36, icon_y + 18, ">_", COLOR_WHITE);
    gfx_draw_string(30, icon_y + 54, "Shell", COLOR_WHITE);
    
    // Info icon
    gfx_fill_rect(30, icon_y + icon_spacing, 48, 48, 0x2a2a4a);
    gfx_draw_rect(30, icon_y + icon_spacing, 48, 48, COLOR_SUCCESS);
    gfx_draw_string(48, icon_y + icon_spacing + 18, "i", COLOR_SUCCESS);
    gfx_draw_string(30, icon_y + icon_spacing + 54, "About", COLOR_WHITE);
    
    bool running = true;
    backup_x = -1;
    uint64_t last_update = 0;
    
    while (running) {
        input_poll();
        uint64_t now = timer_get_ticks();
        bool status_updated = false;
        
        // Update status bar every 100 ticks (~1 second)
        if (now - last_update > 100) {
            last_update = now;
            status_updated = true;
            
            // Clear status area
            gfx_fill_rect(screen_width - 250, screen_height - taskbar_height + 8, 240, 24, COLOR_TASKBAR);
            
            // Display time
            RTCTime time;
            rtc_get_time(&time);
            char time_str[32];
            int ti = 0;
            time_str[ti++] = '0' + (time.hour / 10);
            time_str[ti++] = '0' + (time.hour % 10);
            time_str[ti++] = ':';
            time_str[ti++] = '0' + (time.minute / 10);
            time_str[ti++] = '0' + (time.minute % 10);
            time_str[ti++] = ':';
            time_str[ti++] = '0' + (time.second / 10);
            time_str[ti++] = '0' + (time.second % 10);
            time_str[ti] = 0;
            gfx_draw_string(screen_width - 80, screen_height - taskbar_height + 14, time_str, COLOR_WHITE);
            
            // Display memory
            extern uint64_t pmm_get_free_memory();
            uint64_t free_mb = pmm_get_free_memory() / (1024 * 1024);
            char mem_str[32] = "RAM: ";
            int mi = 5;
            if (free_mb >= 1000) mem_str[mi++] = '0' + (free_mb / 1000) % 10;
            if (free_mb >= 100) mem_str[mi++] = '0' + (free_mb / 100) % 10;
            if (free_mb >= 10) mem_str[mi++] = '0' + (free_mb / 10) % 10;
            mem_str[mi++] = '0' + (free_mb % 10);
            mem_str[mi++] = 'M';
            mem_str[mi++] = 'B';
            mem_str[mi] = 0;
            gfx_draw_string(screen_width - 180, screen_height - taskbar_height + 14, mem_str, COLOR_GRAY);
        }
        
        // Mouse handling - using unified input layer (supports both PS/2 and USB)
        InputMouseState mouse_state;
        input_mouse_get_state(&mouse_state);
        int32_t mx = mouse_state.x;
        int32_t my = mouse_state.y;
        
        // Status bar region: x from (screen_width - 250) to screen_width
        bool cursor_in_status = (mx >= (int32_t)(screen_width - 260) && 
                                 my >= (int32_t)(screen_height - taskbar_height));
        
        if (mx != backup_x || my != backup_y || status_updated) {
            // If status was updated and cursor was/is in that region, don't restore
            // (the backup data is stale from before the status update)
            if (status_updated && cursor_in_status) {
                // Skip restore - status bar content changed, backup is invalid
            } else {
                restore_cursor_area();
            }
            
            save_cursor_area(mx, my);
            gfx_draw_cursor(mx, my);
            backup_x = mx;
            backup_y = my;
        }
        
        // Keyboard input
        char c = 0;
        if (input_keyboard_has_char()) {
            c = input_keyboard_get_char();
        }
        if (c == 'q' || c == 'Q' || c == 27) running = false;
        
        for (volatile int i = 0; i < 1000; i++);  // Small delay for CPU
    }
    
    // Restore shell screen
    gfx_clear(COLOR_BLACK);
    gfx_draw_string(50, 50, "uniOS Shell (uniSH)", COLOR_WHITE);
}

// Kernel entry point
extern "C" void _start(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count < 1) hcf();

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    g_framebuffer = fb;
    
    // Initialize graphics subsystem
    gfx_init(fb);
    debug_init(fb);

    // Clear screen
    gfx_clear(COLOR_BLACK);
    
    // Initialize serial console for debug output
    serial_init();
    serial_puts("\r\n=== uniOS Kernel v");
    serial_puts(UNIOS_VERSION_STRING);
    serial_puts(" ===\r\n");
    
    // Get bootloader info if available
    if (bootloader_info_request.response) {
        g_bootloader_name = bootloader_info_request.response->name;
        g_bootloader_version = bootloader_info_request.response->version;
        serial_printf("Bootloader: %s %s\r\n", g_bootloader_name, g_bootloader_version);
    }
    
    DEBUG_INFO("uniOS Kernel v%s Starting...", UNIOS_VERSION_STRING);
    DEBUG_INFO("Framebuffer: %dx%d bpp=%d", fb->width, fb->height, fb->bpp);

    // Initialize core systems
    gdt_init();
    DEBUG_INFO("GDT Initialized");
    
    idt_init();
    DEBUG_INFO("IDT Initialized");
    
    pic_remap(32, 40);
    for (int i = 0; i < 16; i++) pic_set_mask(i);
    DEBUG_INFO("PIC Remapped and Masked");
    
    ps2_keyboard_init();
    DEBUG_INFO("PS/2 Keyboard Initialized");
    
    ps2_mouse_init();
    DEBUG_INFO("PS/2 Mouse Initialized");
    
    timer_init(100);
    DEBUG_INFO("Timer Initialized (100Hz)");
    
    pmm_init();
    DEBUG_INFO("PMM Initialized");
    
    vmm_init();
    DEBUG_INFO("VMM Initialized");
    
    // Initialize heap
    heap_init(nullptr, 0);
    DEBUG_INFO("Heap Initialized (Bucket Allocator)");
    
    scheduler_init();
    DEBUG_INFO("Scheduler Initialized");
    
    // Initialize USB subsystem via unified input layer
    pci_init();
    DEBUG_INFO("PCI Subsystem Initialized");
    
    acpi_init();  // Initialize ACPI for poweroff support
    
    rtc_init();  // Initialize RTC for date/time
    DEBUG_INFO("RTC Initialized");
    
    usb_init();
    // usb_init logs its own status
    
    usb_hid_init();
    // usb_hid_init logs its own status
    
    input_set_screen_size(fb->width, fb->height);
    
    // Initialize network stack
    net_init();
    // net_init logs its own status
    
    // Enable interrupts
    asm("sti");
    DEBUG_INFO("Interrupts Enabled");
    
    // Initialize filesystem
    if (module_request.response && module_request.response->module_count > 0) {
        unifs_init(module_request.response->modules[0]->address);
        DEBUG_INFO("Filesystem Ready");
    } else {
        DEBUG_WARN("Filesystem: No modules");
    }
    
#ifdef DEBUG
    // Debug build: show boot log and wait for keypress
    DEBUG_INFO("Boot complete!");
    gfx_draw_string(50, fb->height - 40, "Press any key to continue...", 0x00AAAAAA);
    
    while (!input_keyboard_has_char()) {
        input_poll();
        for (volatile int i = 0; i < 10000; i++);
    }
    input_keyboard_get_char();  // Consume keypress
#endif
    
    // Splash screen
    gfx_clear(COLOR_BLACK);
    gfx_draw_centered_text("uniOS", COLOR_WHITE);
    // Wait ~0.5 seconds but keep polling input to avoid buffer overflows
    uint64_t splash_start = timer_get_ticks();
    while (timer_get_ticks() - splash_start < 50) {
        input_poll();
        asm volatile("hlt");
    }
    
    // Clear screen again
    gfx_clear(COLOR_BLACK);
    
    // Initialize shell
    shell_init(fb);

    // Main loop using unified input
    while (true) {
        // Poll all input sources
        input_poll();
        
        // Poll network
        net_poll();
        
        // Update shell cursor blink
        shell_tick();
        
        // Handle shell input via unified API
        if (input_keyboard_has_char()) {
            char c = input_keyboard_get_char();
            shell_process_char(c);
        }
    }
}
