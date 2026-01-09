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
#include "pat.h"
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

// New
#include "sound.h"

// Global framebuffer pointer
struct limine_framebuffer* g_framebuffer = nullptr;

// Global bootloader info (for version command)
// Stored in kernel-owned buffers to survive bootloader memory reclamation
static char g_bootloader_name_buf[64];
static char g_bootloader_version_buf[64];
const char* g_bootloader_name = nullptr;
const char* g_bootloader_version = nullptr;

#include "panic.h"

// Enable SSE/FPU in control registers (required for fxsave/fxrstor)
static void cpu_enable_sse() {
    // Enable SSE in CR4
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);   // OSFXSR - Enable fxsave/fxrstor
    cr4 |= (1 << 10);  // OSXMMEXCPT - Enable SSE exceptions
    asm volatile("mov %0, %%cr4" :: "r"(cr4));
    
    // Enable FPU in CR0
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);  // Clear EM (Emulation) - don't trap FPU instructions
    cr0 |= (1 << 1);   // Set MP (Monitor Coprocessor) - monitor FPU
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
}

// Idle task - runs when no other task is ready
// This prevents CPU starvation when all tasks are sleeping/waiting
static void idle_task_entry() {
    while (true) {
        asm volatile("hlt");  // Halt until next interrupt
    }
}

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

// ============================================================================
// GUI Mode & Window Management
// ============================================================================

static uint32_t cursor_backup[12 * 19];
static int32_t backup_x = -1, backup_y = -1;

// Basic Window Structure
struct Window {
    int32_t x, y;
    int32_t width, height;
    const char* title;
    uint32_t color;
    bool dragging;
    int32_t drag_offset_x;
    int32_t drag_offset_y;
    bool visible;
};

// Helper: Restore background behind cursor (optimized row-based)
static void restore_cursor_area() {
    if (backup_x < 0) return;
    uint32_t* fb = gfx_get_buffer();
    uint32_t pitch = g_framebuffer->pitch / 4;
    int32_t width = (int32_t)g_framebuffer->width;
    int32_t height = (int32_t)g_framebuffer->height;
    
    for (int row = 0; row < 19; row++) {
        int32_t py = backup_y + row;
        if (py >= 0 && py < height) {
            uint32_t* fb_row = &fb[py * pitch];
            uint32_t* backup_row = &cursor_backup[row * 12];
            for (int col = 0; col < 12; col++) {
                int32_t px = backup_x + col;
                if (px >= 0 && px < width) {
                    fb_row[px] = backup_row[col];
                }
            }
        }
    }
    gfx_mark_dirty(backup_x, backup_y, 12, 19);
}

// Helper: Save background behind cursor (optimized row-based)
static void save_cursor_area(int32_t x, int32_t y) {
    uint32_t* fb = gfx_get_buffer();
    uint32_t pitch = g_framebuffer->pitch / 4;
    int32_t width = (int32_t)g_framebuffer->width;
    int32_t height = (int32_t)g_framebuffer->height;
    
    for (int row = 0; row < 19; row++) {
        int32_t py = y + row;
        if (py >= 0 && py < height) {
            uint32_t* fb_row = &fb[py * pitch];
            uint32_t* backup_row = &cursor_backup[row * 12];
            for (int col = 0; col < 12; col++) {
                int32_t px = x + col;
                if (px >= 0 && px < width) {
                    backup_row[col] = fb_row[px];
                }
            }
        }
    }
    backup_x = x; backup_y = y;
}

// Helper: Draw a single window
static void draw_window(Window* win, bool active) {
    if (!win->visible) return;

    // 1. Draw Body
    gfx_fill_rect(win->x, win->y, win->width, win->height, win->color);
    gfx_draw_rect(win->x, win->y, win->width, win->height, 0x444444);

    // 2. Draw Title Bar
    uint32_t title_color = active ? COLOR_ACCENT : 0x333333;
    gfx_fill_rect(win->x, win->y, win->width, 24, title_color);
    
    // 3. Draw Title Text
    gfx_draw_string(win->x + 8, win->y + 7, win->title, COLOR_WHITE);

    // 4. Draw Close Button
    gfx_fill_rect(win->x + win->width - 20, win->y + 4, 16, 16, 0xcc3333);
    gfx_draw_char(win->x + win->width - 15, win->y + 7, 'x', COLOR_WHITE);
}

// Helper: Draw the entire desktop scene
static void draw_desktop(uint32_t width, uint32_t height, Window* windows, int win_count, int active_idx) {
    uint32_t taskbar_height = 40;

    // 1. Wallpaper (Gradient)
    gfx_draw_gradient_v(0, 0, width, height - taskbar_height, 
                        COLOR_DESKTOP_TOP, COLOR_DESKTOP_BOTTOM);

    // 2. Desktop Icons
    int icon_y = 30;
    int icon_spacing = 80;
    
    // Terminal Icon
    gfx_fill_rect(30, icon_y, 48, 48, 0x2a2a4a);
    gfx_draw_rect(30, icon_y, 48, 48, COLOR_ACCENT);
    gfx_draw_string(36, icon_y + 18, ">_", COLOR_WHITE);
    gfx_draw_string(30, icon_y + 54, "Shell", COLOR_WHITE);
    
    // Info Icon
    gfx_fill_rect(30, icon_y + icon_spacing, 48, 48, 0x2a2a4a);
    gfx_draw_rect(30, icon_y + icon_spacing, 48, 48, COLOR_SUCCESS);
    gfx_draw_string(48, icon_y + icon_spacing + 18, "i", COLOR_SUCCESS);
    gfx_draw_string(30, icon_y + icon_spacing + 54, "About", COLOR_WHITE);

    // 3. Windows (Painter's Algorithm: Draw inactive first, active last)
    for (int i = 0; i < win_count; i++) {
        if (i != active_idx) draw_window(&windows[i], false);
    }
    if (active_idx >= 0 && active_idx < win_count) {
        draw_window(&windows[active_idx], true);
    }

    // 4. Taskbar
    gfx_fill_rect(0, height - taskbar_height, width, taskbar_height, COLOR_TASKBAR);
    gfx_fill_rect(0, height - taskbar_height, width, 1, 0x333350);
    
    // Start Button
    gfx_fill_rect(8, height - taskbar_height + 8, 80, 24, COLOR_ACCENT);
    gfx_draw_string(20, height - taskbar_height + 14, "uniOS", COLOR_WHITE);
}

void gui_start() {
    uint32_t screen_width = g_framebuffer->width;
    uint32_t screen_height = g_framebuffer->height;
    uint32_t taskbar_height = 40;

    // Define Windows
    const int WINDOW_COUNT = 3;
    Window windows[3];
    
    // Window 1: Welcome
    windows[0].x = 150; windows[0].y = 100;
    windows[0].width = 300; windows[0].height = 200;
    windows[0].title = "Welcome";
    windows[0].color = 0x222222;
    windows[0].dragging = false;
    windows[0].drag_offset_x = 0; windows[0].drag_offset_y = 0;
    windows[0].visible = true;
    
    // Window 2: System Info
    windows[1].x = 500; windows[1].y = 150;
    windows[1].width = 250; windows[1].height = 300;
    windows[1].title = "System Info";
    windows[1].color = 0x1a1a2e;
    windows[1].dragging = false;
    windows[1].drag_offset_x = 0; windows[1].drag_offset_y = 0;
    windows[1].visible = true;
    
    // Window 3: Notepad
    windows[2].x = 200; windows[2].y = 350;
    windows[2].width = 400; windows[2].height = 250;
    windows[2].title = "Notepad";
    windows[2].color = 0x2d2d2d;
    windows[2].dragging = false;
    windows[2].drag_offset_x = 0; windows[2].drag_offset_y = 0;
    windows[2].visible = true;

    int active_window_idx = 2;

    // Initial Draw
    draw_desktop(screen_width, screen_height, windows, WINDOW_COUNT, active_window_idx);

    bool running = true;
    backup_x = -1;
    uint64_t last_time_update = 0;
    bool prev_mouse_left = false;

    while (running) {
        input_poll();
        
        InputMouseState mouse_state;
        input_mouse_get_state(&mouse_state);
        
        int32_t mx = mouse_state.x;
        int32_t my = mouse_state.y;
        
        uint64_t now = timer_get_ticks();
        bool need_full_redraw = false;

        // --- Logic: Window Dragging & Selection ---
        
        // 1. Mouse Click (Start Drag or Select)
        if (mouse_state.left && !prev_mouse_left) {
            int click_target = -1;
            
            // Check active window first
            if (active_window_idx >= 0 && active_window_idx < WINDOW_COUNT) {
                Window* win = &windows[active_window_idx];
                if (win->visible &&
                    mx >= win->x && mx < win->x + win->width &&
                    my >= win->y && my < win->y + win->height) {
                    click_target = active_window_idx;
                }
            }
            
            // If not clicked active, check others
            if (click_target == -1) {
                for (int i = WINDOW_COUNT - 1; i >= 0; i--) {
                    if (i == active_window_idx) continue;
                    
                    Window* win = &windows[i];
                    if (win->visible && 
                        mx >= win->x && mx < win->x + win->width &&
                        my >= win->y && my < win->y + win->height) {
                        click_target = i;
                        break;
                    }
                }
            }

            if (click_target != -1) {
                active_window_idx = click_target;
                
                // Check if clicked Title Bar (top 24 pixels)
                if (my < windows[click_target].y + 24) {
                    // Check for Close Button
                    if (mx > windows[click_target].x + windows[click_target].width - 20) {
                        windows[click_target].visible = false;
                    } else {
                        // Start Dragging
                        windows[click_target].dragging = true;
                        windows[click_target].drag_offset_x = mx - windows[click_target].x;
                        windows[click_target].drag_offset_y = my - windows[click_target].y;
                    }
                }
                need_full_redraw = true;
            }
            
            // Check desktop icons (only if no window was clicked)
            if (click_target == -1) {
                int icon_y = 30;
                int icon_spacing = 80;
                
                // Shell icon (30, 30) to (78, 78) - opens Welcome window
                if (mx >= 30 && mx < 78 && my >= icon_y && my < icon_y + 48) {
                    windows[0].visible = true;
                    windows[0].x = 150; windows[0].y = 100;  // Reset position
                    active_window_idx = 0;
                    need_full_redraw = true;
                }
                
                // About icon (30, 110) to (78, 158) - opens System Info window
                if (mx >= 30 && mx < 78 && my >= icon_y + icon_spacing && my < icon_y + icon_spacing + 48) {
                    windows[1].visible = true;
                    windows[1].x = 500; windows[1].y = 150;  // Reset position
                    active_window_idx = 1;
                    need_full_redraw = true;
                }
            }
        }

        // 2. Mouse Release (Stop Drag)
        if (!mouse_state.left && prev_mouse_left) {
            for (int i = 0; i < WINDOW_COUNT; i++) {
                windows[i].dragging = false;
            }
        }

        // 3. Mouse Move (Perform Drag)
        if (mouse_state.left) {
            for (int i = 0; i < WINDOW_COUNT; i++) {
                if (windows[i].dragging) {
                    int32_t new_x = mx - windows[i].drag_offset_x;
                    int32_t new_y = my - windows[i].drag_offset_y;
                    
                    if (new_x != windows[i].x || new_y != windows[i].y) {
                        windows[i].x = new_x;
                        windows[i].y = new_y;
                        need_full_redraw = true;
                    }
                }
            }
        }

        prev_mouse_left = mouse_state.left;

        // --- Logic: Time Update ---
        if (now - last_time_update > 1000) {
            last_time_update = now;
            
            if (!need_full_redraw) {
                restore_cursor_area();
                
                gfx_fill_rect(screen_width - 250, screen_height - taskbar_height + 8, 240, 24, COLOR_TASKBAR);
                
                RTCTime time;
                rtc_get_time(&time);
                char time_str[32];
                int ti = 0;
                time_str[ti++] = '0' + (time.hour / 10); time_str[ti++] = '0' + (time.hour % 10);
                time_str[ti++] = ':';
                time_str[ti++] = '0' + (time.minute / 10); time_str[ti++] = '0' + (time.minute % 10);
                time_str[ti++] = ':';
                time_str[ti++] = '0' + (time.second / 10); time_str[ti++] = '0' + (time.second % 10);
                time_str[ti] = 0;
                gfx_draw_string(screen_width - 80, screen_height - taskbar_height + 14, time_str, COLOR_WHITE);
                
                save_cursor_area(mx, my);
                gfx_draw_cursor(mx, my);
            }
        }

        // --- Rendering ---
        if (need_full_redraw) {
            draw_desktop(screen_width, screen_height, windows, WINDOW_COUNT, active_window_idx);
            
            // Draw clock after redraw
            RTCTime time;
            rtc_get_time(&time);
            char time_str[32];
            int ti = 0;
            time_str[ti++] = '0' + (time.hour / 10); time_str[ti++] = '0' + (time.hour % 10);
            time_str[ti++] = ':';
            time_str[ti++] = '0' + (time.minute / 10); time_str[ti++] = '0' + (time.minute % 10);
            time_str[ti++] = ':';
            time_str[ti++] = '0' + (time.second / 10); time_str[ti++] = '0' + (time.second % 10);
            time_str[ti] = 0;
            gfx_draw_string(screen_width - 80, screen_height - taskbar_height + 14, time_str, COLOR_WHITE);

            save_cursor_area(mx, my);
            gfx_draw_cursor(mx, my);
            backup_x = mx;
            backup_y = my;
        } 
        else if (mx != backup_x || my != backup_y) {
            restore_cursor_area();
            save_cursor_area(mx, my);
            gfx_draw_cursor(mx, my);
            backup_x = mx;
            backup_y = my;
        }

        // Keyboard Exit
        if (input_keyboard_has_char()) {
            char c = input_keyboard_get_char();
            if (c == 'q' || c == 'Q' || c == 27) running = false;
        }
        
        // Copy backbuffer to screen (double buffering)
        gfx_swap_buffers();
        
        scheduler_yield();
    }
    
    // Restore shell screen
    gfx_clear(COLOR_BLACK);
    gfx_draw_string(50, 50, "uniOS Shell", COLOR_WHITE);
}
// ============================================================================
// C++ Global Constructors
// ============================================================================
// The linker collects function pointers for global object constructors in
// the .init_array section. We must call these before using any C++ globals.

extern "C" {
    typedef void (*ctor_func)();
    extern ctor_func __init_array_start[];
    extern ctor_func __init_array_end[];
    
    // C++ runtime stubs required for freestanding environment
    // __dso_handle is used by __cxa_atexit to identify shared objects
    void* __dso_handle = nullptr;
    
    // __cxa_atexit registers destructors for static objects
    // In a kernel, we never exit, so we just ignore these registrations
    int __cxa_atexit(void (*)(void*), void*, void*) {
        return 0;  // Success - just ignore destructor registration
    }
    
    // __cxa_pure_virtual is called if a pure virtual function is invoked
    void __cxa_pure_virtual() {
        // This should never happen in correct code
        for (;;);  // Hang
    }
}

static void call_global_constructors() {
    for (ctor_func* p = __init_array_start; p < __init_array_end; p++) {
        (*p)();
    }
}

// Kernel entry point
extern "C" void _start(void) {
    // Call C++ global constructors first (before any C++ code runs)
    call_global_constructors();
    
    // Enable SSE/FPU early - required before any SSE instructions in graphics code
    cpu_enable_sse();
    
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
    // IMPORTANT: Copy strings to kernel-owned buffers!
    // Limine response memory may be reclaimed and overwritten later.
    if (bootloader_info_request.response) {
        const char* src_name = bootloader_info_request.response->name;
        const char* src_ver = bootloader_info_request.response->version;
        
        // Copy name
        int i = 0;
        while (src_name && src_name[i] && i < 63) {
            g_bootloader_name_buf[i] = src_name[i];
            i++;
        }
        g_bootloader_name_buf[i] = '\0';
        g_bootloader_name = g_bootloader_name_buf;
        
        // Copy version
        i = 0;
        while (src_ver && src_ver[i] && i < 63) {
            g_bootloader_version_buf[i] = src_ver[i];
            i++;
        }
        g_bootloader_version_buf[i] = '\0';
        g_bootloader_version = g_bootloader_version_buf;
        
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
    
    timer_init(1000);  // 1000Hz = 1ms granularity (better for UI and network)
    DEBUG_INFO("Timer Initialized (1000Hz)");
    
    pmm_init();
    DEBUG_INFO("PMM Initialized");
    
    vmm_init();
    DEBUG_INFO("VMM Initialized");
    
    // Initialize PAT for Write-Combining support (improves graphics performance on AMD)
    pat_init();
    
    // Remap framebuffer with Write-Combining for faster graphics
    // The VMM now properly handles 2MB huge pages by splitting them into 4KB pages
    if (fb) {
        uint64_t fb_size = fb->pitch * fb->height;
        vmm_remap_framebuffer((uint64_t)fb->address, fb_size);
        DEBUG_INFO("Framebuffer remapped with Write-Combining");
    }
    
    // Initialize heap
    heap_init(nullptr, 0);
    DEBUG_INFO("Heap Initialized (Bucket Allocator)");
    
    // Enable double buffering now that heap is ready (allocates backbuffer from heap)
    gfx_enable_double_buffering();
    DEBUG_INFO("Double Buffering Enabled");
    
    // NOTE: SSE/FPU is enabled early in _start() before graphics init
    
    scheduler_init();
    DEBUG_INFO("Scheduler Initialized");
    
    // Create dedicated idle task (always runnable, prevents deadlock)
    scheduler_create_task(idle_task_entry, "Idle");
    DEBUG_INFO("Idle Task Created");
    
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

    // Initialize sound drivers
    sound_init();
    // sound_init logs its own status
    
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
    gfx_swap_buffers();  // Show debug text
    
    while (!input_keyboard_has_char()) {
        input_poll();
        scheduler_yield();  // Yield CPU instead of busy-wait
    }
    input_keyboard_get_char();  // Consume keypress
#endif
    
    // Splash screen
    gfx_clear(COLOR_BLACK);
    gfx_draw_centered_text("uniOS", COLOR_WHITE);
    gfx_swap_buffers();  // Show splash text
    // Wait ~0.5 seconds but keep polling input to avoid buffer overflows
    uint64_t splash_start = timer_get_ticks();
    while (timer_get_ticks() - splash_start < 500) {  // 500 ticks at 1000Hz = 500ms
        input_poll();
        asm volatile("hlt");
    }
    
    // Clear screen again
    gfx_clear(COLOR_BLACK);
    gfx_swap_buffers();  // Show cleared screen;
    
    // Initialize shell
    shell_init(fb);

    // Variables for FPS limiting
    uint64_t last_frame_time = 0;
    const uint64_t frame_delay = 16;  // 16ms per frame ≈ 60 FPS

    // Main loop using unified input
    while (true) {
        // 1. Always poll hardware (USB needs frequent polling)
        input_poll();
        net_poll();

        // Poll sound
        sound_poll();

        uint64_t now = timer_get_ticks();
        
        // 2. Only process shell/UI updates if we're about to draw
        // This reduces input latency by ensuring fresh input is displayed immediately
        if (now - last_frame_time >= frame_delay) {
            // Update logic RIGHT NOW based on the freshest input
            shell_tick();
            
            // Process ALL pending keys, not just one per frame
            while (input_keyboard_has_char()) {
                char c = input_keyboard_get_char();
                shell_process_char(c);
            }
            
            // Draw immediately after processing input
            gfx_swap_buffers();
            last_frame_time = now;
        } else {
            // Sleep briefly to save power, but not too long to miss USB packets
            asm volatile("hlt");
        }
    }
}
