#include <stdint.h>
#include <stddef.h>
#include <boot/limine.h>

// Limine base revision
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id       = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id       = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/pic.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <kernel/time/timer.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/arch/x86_64/pat.h>
#include <kernel/mm/heap.h>
#include <kernel/scheduler.h>
#include <kernel/fs/unifs.h>
#include <kernel/shell.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <kernel/debug.h>
#include <drivers/bus/pci/pci.h>
#include <drivers/bus/usb/usb.h>
#include <drivers/class/hid/usb_hid.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <drivers/video/framebuffer.h>
#include <drivers/class/hid/input.h>
#include <drivers/acpi/acpi.h>
#include <drivers/rtc/rtc.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/net/net.h>
#include <kernel/version.h>
#include <drivers/sound/sound.h>

// Global framebuffer pointer
struct limine_framebuffer* g_framebuffer = nullptr;

// Global bootloader info (stored in kernel-owned buffers to survive
// bootloader memory reclamation)
static char  g_bootloader_name_buf[64];
static char  g_bootloader_version_buf[64];
const  char* g_bootloader_name    = nullptr;
const  char* g_bootloader_version = nullptr;

#include <kernel/panic.h>

// ============================================================================
// GUI / window-manager constants
// FIXED: replaced all scattered magic numbers with named constexpr values
// ============================================================================
constexpr int     WINDOW_COUNT      = 3;
constexpr int32_t TASKBAR_HEIGHT    = 40;
constexpr int32_t CURSOR_W          = 12;
constexpr int32_t CURSOR_H          = 19;
constexpr int32_t TITLE_BAR_HEIGHT  = 24;
constexpr int32_t CLOSE_BTN_SIZE    = 16;
constexpr int32_t CLOSE_BTN_MARGIN  = 4;
constexpr int32_t DESKTOP_ICON_X    = 30;
constexpr int32_t DESKTOP_ICON_SIZE = 48;
constexpr int32_t DESKTOP_ICON_STEP = 80;
constexpr int32_t CLOCK_CLEAR_W     = 250;

// ============================================================================
// CPU feature initialisation
// ============================================================================

// Enable SSE/FPU in control registers (required for fxsave/fxrstor)
static void cpu_enable_sse() {
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);   // OSFXSR     – enable fxsave/fxrstor
    cr4 |= (1 << 10);  // OSXMMEXCPT – enable SSE exceptions
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);  // Clear EM – don't trap FPU instructions
    cr0 |=  (1 << 1);  // Set MP   – monitor coprocessor
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
}

// ============================================================================
// Idle task
// ============================================================================

// Runs when no other task is ready; prevents CPU starvation
static void idle_task_entry() {
    while (true) {
        asm volatile("hlt");
    }
}

// ============================================================================
// IRQ handler
// ============================================================================

extern "C" void irq_handler(void* stack_frame) {
    uint64_t* regs   = (uint64_t*)stack_frame;
    uint64_t  int_no = regs[15];
    uint8_t   irq    = (uint8_t)(int_no - 32);

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

// ============================================================================
// User mode test stub
// (kept for reference / future use; not called from the boot path)
// ============================================================================

static void user_program() __attribute__((section(".user_code")));
static void user_program() {
    const char* msg = "Hello from User Mode!\n";
    asm volatile(
        "mov $1,  %%rax\n"
        "mov %0,  %%rbx\n"
        "mov $22, %%rcx\n"
        "int $0x80\n"
        : : "r"(msg) : "rax", "rbx", "rcx"
    );
    asm volatile("mov $60, %%rax\n" "int $0x80\n" : : : "rax");
    for (;;);
}

extern "C" void jump_to_user_mode(uint64_t code_sel, uint64_t stack, uint64_t entry);

// FIXED: removed dead declarations:
//   - static uint8_t user_stack[4096]  (allocated but never used)
//   - void run_user_test()              (defined but never called)

// ============================================================================
// GUI Mode & Window Management
// ============================================================================

// FIXED: array dimensions now use constexpr CURSOR_W / CURSOR_H
static uint32_t cursor_backup[CURSOR_W * CURSOR_H];
static int32_t  backup_x = -1, backup_y = -1;

// Basic Window Structure
struct Window {
    int32_t     x, y;
    int32_t     width, height;
    const char* title;
    uint32_t    color;
    bool        dragging;
    int32_t     drag_offset_x;
    int32_t     drag_offset_y;
    bool        visible;
};

// Helper: restore the framebuffer pixels behind the cursor
static void restore_cursor_area() {
    if (backup_x < 0) return;
    uint32_t* fb     = gfx_get_buffer();
    uint32_t  pitch  = g_framebuffer->pitch / 4;
    int32_t   width  = (int32_t)g_framebuffer->width;
    int32_t   height = (int32_t)g_framebuffer->height;

    for (int row = 0; row < CURSOR_H; row++) {
        int32_t py = backup_y + row;
        if (py >= 0 && py < height) {
            uint32_t* fb_row     = &fb[py * pitch];
            uint32_t* backup_row = &cursor_backup[row * CURSOR_W];
            for (int col = 0; col < CURSOR_W; col++) {
                int32_t px = backup_x + col;
                if (px >= 0 && px < width) {
                    fb_row[px] = backup_row[col];
                }
            }
        }
    }
    gfx_mark_dirty(backup_x, backup_y, CURSOR_W, CURSOR_H);
}

// Helper: save the framebuffer pixels behind the cursor
static void save_cursor_area(int32_t x, int32_t y) {
    uint32_t* fb     = gfx_get_buffer();
    uint32_t  pitch  = g_framebuffer->pitch / 4;
    int32_t   width  = (int32_t)g_framebuffer->width;
    int32_t   height = (int32_t)g_framebuffer->height;

    for (int row = 0; row < CURSOR_H; row++) {
        int32_t py = y + row;
        if (py >= 0 && py < height) {
            uint32_t* fb_row     = &fb[py * pitch];
            uint32_t* backup_row = &cursor_backup[row * CURSOR_W];
            for (int col = 0; col < CURSOR_W; col++) {
                int32_t px = x + col;
                if (px >= 0 && px < width) {
                    backup_row[col] = fb_row[px];
                }
            }
        }
    }
    backup_x = x;
    backup_y = y;
}

// Helper: draw one window onto the backbuffer
static void draw_window(Window* win, bool active) {
    if (!win->visible) return;

    // Body + border
    gfx_fill_rect(win->x, win->y, win->width, win->height, win->color);
    gfx_draw_rect(win->x, win->y, win->width, win->height, 0x444444);

    // Title bar
    uint32_t title_color = active ? COLOR_ACCENT : 0x333333;
    gfx_fill_rect(win->x, win->y, win->width, TITLE_BAR_HEIGHT, title_color);
    gfx_draw_string(win->x + 8, win->y + 7, win->title, COLOR_WHITE);

    // Close button
    int32_t close_x = win->x + win->width - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN;
    gfx_fill_rect(close_x, win->y + CLOSE_BTN_MARGIN,
                  CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, 0xcc3333);
    gfx_draw_char(close_x + 5, win->y + 7, 'x', COLOR_WHITE);
}

// FIXED: extracted the duplicated clock rendering into a single helper.
// Previously the same RTC read + digit format + gfx_draw_string block was
// copy-pasted verbatim in two separate branches of the gui_start() loop.
static void draw_clock(uint32_t screen_width, uint32_t screen_height) {
    // Erase the previous clock digits before drawing new ones
    gfx_fill_rect((int32_t)(screen_width - CLOCK_CLEAR_W),
                  (int32_t)(screen_height - TASKBAR_HEIGHT + 8),
                  CLOCK_CLEAR_W - 10,
                  TASKBAR_HEIGHT - 16,
                  COLOR_TASKBAR);

    RTCTime time;
    rtc_get_time(&time);

    char time_str[9]; // "HH:MM:SS\0"
    int  ti = 0;
    time_str[ti++] = '0' + (time.hour   / 10);
    time_str[ti++] = '0' + (time.hour   % 10);
    time_str[ti++] = ':';
    time_str[ti++] = '0' + (time.minute / 10);
    time_str[ti++] = '0' + (time.minute % 10);
    time_str[ti++] = ':';
    time_str[ti++] = '0' + (time.second / 10);
    time_str[ti++] = '0' + (time.second % 10);
    time_str[ti]   = '\0';

    gfx_draw_string((int32_t)(screen_width - 80),
                    (int32_t)(screen_height - TASKBAR_HEIGHT + 14),
                    time_str, COLOR_WHITE);
}

// Helper: draw the entire desktop scene (wallpaper + icons + windows + taskbar)
static void draw_desktop(uint32_t width, uint32_t height,
                          Window* windows, int win_count, int active_idx) {
    // 1. Wallpaper – vertical gradient
    gfx_draw_gradient_v(0, 0, width, height - TASKBAR_HEIGHT,
                        COLOR_DESKTOP_TOP, COLOR_DESKTOP_BOTTOM);

    // 2. Desktop icons
    const int32_t icon_y   = DESKTOP_ICON_X;
    const int32_t icon_gap = DESKTOP_ICON_STEP;

    // Terminal icon
    gfx_fill_rect(DESKTOP_ICON_X, icon_y,
                  DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, 0x2a2a4a);
    gfx_draw_rect(DESKTOP_ICON_X, icon_y,
                  DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, COLOR_ACCENT);
    gfx_draw_string(DESKTOP_ICON_X + 6, icon_y + 18, ">_", COLOR_WHITE);
    gfx_draw_string(DESKTOP_ICON_X,     icon_y + 54, "Shell", COLOR_WHITE);

    // Info / About icon
    gfx_fill_rect(DESKTOP_ICON_X, icon_y + icon_gap,
                  DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, 0x2a2a4a);
    gfx_draw_rect(DESKTOP_ICON_X, icon_y + icon_gap,
                  DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, COLOR_SUCCESS);
    gfx_draw_string(DESKTOP_ICON_X + 18, icon_y + icon_gap + 18, "i", COLOR_SUCCESS);
    gfx_draw_string(DESKTOP_ICON_X,      icon_y + icon_gap + 54, "About", COLOR_WHITE);

    // 3. Windows – painter's algorithm: inactive first, active on top
    for (int i = 0; i < win_count; i++) {
        if (i != active_idx) draw_window(&windows[i], false);
    }
    if (active_idx >= 0 && active_idx < win_count) {
        draw_window(&windows[active_idx], true);
    }

    // 4. Taskbar
    gfx_fill_rect(0, (int32_t)(height - TASKBAR_HEIGHT),
                  width, TASKBAR_HEIGHT, COLOR_TASKBAR);
    gfx_fill_rect(0, (int32_t)(height - TASKBAR_HEIGHT), width, 1, 0x333350);

    // Start button
    gfx_fill_rect(8,  (int32_t)(height - TASKBAR_HEIGHT + 8), 80, 24, COLOR_ACCENT);
    gfx_draw_string(20, (int32_t)(height - TASKBAR_HEIGHT + 14), "uniOS", COLOR_WHITE);
}

void gui_start() {
    uint32_t screen_width  = g_framebuffer->width;
    uint32_t screen_height = g_framebuffer->height;

    // FIXED: array uses constexpr WINDOW_COUNT – no raw literal duplication
    Window windows[WINDOW_COUNT];

    // Window 0: Welcome
    windows[0].x             = 150; windows[0].y      = 100;
    windows[0].width         = 300; windows[0].height  = 200;
    windows[0].title         = "Welcome";
    windows[0].color         = 0x222222;
    windows[0].dragging      = false;
    windows[0].drag_offset_x = 0;   windows[0].drag_offset_y = 0;
    windows[0].visible       = true;

    // Window 1: System Info
    windows[1].x             = 500; windows[1].y      = 150;
    windows[1].width         = 250; windows[1].height  = 300;
    windows[1].title         = "System Info";
    windows[1].color         = 0x1a1a2e;
    windows[1].dragging      = false;
    windows[1].drag_offset_x = 0;   windows[1].drag_offset_y = 0;
    windows[1].visible       = true;

    // Window 2: Notepad
    windows[2].x             = 200; windows[2].y      = 350;
    windows[2].width         = 400; windows[2].height  = 250;
    windows[2].title         = "Notepad";
    windows[2].color         = 0x2d2d2d;
    windows[2].dragging      = false;
    windows[2].drag_offset_x = 0;   windows[2].drag_offset_y = 0;
    windows[2].visible       = true;

    int active_window_idx = 2;

    // Initial draw
    draw_desktop(screen_width, screen_height, windows, WINDOW_COUNT, active_window_idx);

    bool     running          = true;
    backup_x                  = -1;
    uint64_t last_time_update = 0;
    bool     prev_mouse_left  = false;

    while (running) {
        input_poll();

        InputMouseState mouse_state;
        input_mouse_get_state(&mouse_state);

        int32_t  mx              = mouse_state.x;
        int32_t  my              = mouse_state.y;
        uint64_t now             = timer_get_ticks();
        bool     need_full_redraw = false;

        // ── Mouse click: select or begin dragging a window ──────────────────
        if (mouse_state.left && !prev_mouse_left) {
            int click_target = -1;

            // Check the active (topmost) window first
            if (active_window_idx >= 0 && active_window_idx < WINDOW_COUNT) {
                Window* win = &windows[active_window_idx];
                if (win->visible &&
                    mx >= win->x && mx < win->x + win->width &&
                    my >= win->y && my < win->y + win->height) {
                    click_target = active_window_idx;
                }
            }

            // If the active window wasn't hit, check the rest back-to-front
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

                // Did we hit the title bar?
                if (my < windows[click_target].y + TITLE_BAR_HEIGHT) {
                    int32_t close_x = windows[click_target].x
                                      + windows[click_target].width
                                      - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN;
                    if (mx >= close_x) {
                        // Close button hit
                        windows[click_target].visible = false;
                    } else {
                        // Start dragging
                        windows[click_target].dragging      = true;
                        windows[click_target].drag_offset_x = mx - windows[click_target].x;
                        windows[click_target].drag_offset_y = my - windows[click_target].y;
                    }
                }
                need_full_redraw = true;
            }

            // Desktop icon hits (only when no window was clicked)
            if (click_target == -1) {
                const int32_t icon_y   = DESKTOP_ICON_X;
                const int32_t icon_gap = DESKTOP_ICON_STEP;

                // Shell icon → show Welcome window
                if (mx >= DESKTOP_ICON_X &&
                    mx <  DESKTOP_ICON_X + DESKTOP_ICON_SIZE &&
                    my >= icon_y &&
                    my <  icon_y + DESKTOP_ICON_SIZE) {
                    windows[0].visible = true;
                    windows[0].x = 150; windows[0].y = 100;
                    active_window_idx = 0;
                    need_full_redraw  = true;
                }

                // About icon → show System Info window
                if (mx >= DESKTOP_ICON_X &&
                    mx <  DESKTOP_ICON_X + DESKTOP_ICON_SIZE &&
                    my >= icon_y + icon_gap &&
                    my <  icon_y + icon_gap + DESKTOP_ICON_SIZE) {
                    windows[1].visible = true;
                    windows[1].x = 500; windows[1].y = 150;
                    active_window_idx = 1;
                    need_full_redraw  = true;
                }
            }
        }

        // ── Mouse release: stop all drags ────────────────────────────────────
        if (!mouse_state.left && prev_mouse_left) {
            for (int i = 0; i < WINDOW_COUNT; i++) {
                windows[i].dragging = false;
            }
        }

        // ── Mouse move while held: drag the active window ────────────────────
        if (mouse_state.left) {
            for (int i = 0; i < WINDOW_COUNT; i++) {
                if (windows[i].dragging) {
                    int32_t new_x = mx - windows[i].drag_offset_x;
                    int32_t new_y = my - windows[i].drag_offset_y;
                    if (new_x != windows[i].x || new_y != windows[i].y) {
                        windows[i].x     = new_x;
                        windows[i].y     = new_y;
                        need_full_redraw = true;
                    }
                }
            }
        }

        prev_mouse_left = mouse_state.left;

        // ── Periodic clock update (once per second) ──────────────────────────
        if (now - last_time_update > 1000) {
            last_time_update = now;

            if (!need_full_redraw) {
                restore_cursor_area();
                // FIXED: single call to draw_clock() instead of duplicated block
                draw_clock(screen_width, screen_height);
                save_cursor_area(mx, my);
                gfx_draw_cursor(mx, my);
            }
        }

        // ── Rendering ────────────────────────────────────────────────────────
        if (need_full_redraw) {
            draw_desktop(screen_width, screen_height,
                         windows, WINDOW_COUNT, active_window_idx);
            // FIXED: single call to draw_clock() instead of duplicated block
            draw_clock(screen_width, screen_height);
            save_cursor_area(mx, my);
            gfx_draw_cursor(mx, my);
            backup_x = mx;
            backup_y = my;
        } else if (mx != backup_x || my != backup_y) {
            restore_cursor_area();
            save_cursor_area(mx, my);
            gfx_draw_cursor(mx, my);
            backup_x = mx;
            backup_y = my;
        }

        // ── Keyboard exit (Q or ESC leaves GUI, returns to shell) ────────────
        if (input_keyboard_has_char()) {
            char c = input_keyboard_get_char();
            if (c == 'q' || c == 'Q' || c == 27) running = false;
        }

        // Blit backbuffer to screen (double buffering)
        gfx_swap_buffers();

        scheduler_yield();
    }

    // Return to shell
    gfx_clear(COLOR_BLACK);
    gfx_draw_string(50, 50, "uniOS Shell", COLOR_WHITE);
}

// ============================================================================
// C++ Global Constructors
// ============================================================================
// The linker collects function pointers for global object constructors in the
// .init_array section.  We must call them before any C++ globals are used.

extern "C" {
    typedef void (*ctor_func)();
    extern ctor_func __init_array_start[];
    extern ctor_func __init_array_end[];

    // Used by __cxa_atexit to identify the owning shared object
    void* __dso_handle = nullptr;

    // The kernel never exits, so destructor registration is a no-op
    int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

    // Called on a pure-virtual dispatch – should never happen in correct code
    void __cxa_pure_virtual() { for (;;); }
}

static void call_global_constructors() {
    for (ctor_func* p = __init_array_start; p < __init_array_end; p++) {
        (*p)();
    }
}

// ============================================================================
// Kernel entry point
// ============================================================================

extern "C" void _start(void) {
    // Must run before any C++ globals are used
    call_global_constructors();

    // Enable SSE/FPU early – required before any SSE instructions in gfx code
    cpu_enable_sse();

    if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
    if (!framebuffer_request.response ||
         framebuffer_request.response->framebuffer_count < 1) hcf();

    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    g_framebuffer = fb;

    // Initialise graphics + debug overlay
    gfx_init(fb);
    debug_init(fb);
    gfx_clear(COLOR_BLACK);

    // Serial console for early debug output
    serial_init();
    serial_puts("\r\n=== uniOS Kernel v");
    serial_puts(UNIOS_VERSION_STRING);
    serial_puts(" ===\r\n");

    // Copy bootloader info strings into kernel-owned buffers.
    // IMPORTANT: Limine response memory may be reclaimed later; do not keep
    // raw pointers into it past this point.
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

    DEBUG_INFO("uniOS Kernel v%s Starting...", UNIOS_VERSION_STRING);
    DEBUG_INFO("Framebuffer: %dx%d bpp=%d", fb->width, fb->height, fb->bpp);

    // ── Core architecture ────────────────────────────────────────────────────
    gdt_init();
    DEBUG_INFO("GDT Initialized");

    idt_init();
    DEBUG_INFO("IDT Initialized");

    pic_remap(32, 40);
    for (int i = 0; i < 16; i++) pic_set_mask(i);
    DEBUG_INFO("PIC Remapped and Masked");

    // ── Input devices ────────────────────────────────────────────────────────
    ps2_keyboard_init();
    DEBUG_INFO("PS/2 Keyboard Initialized");

    ps2_mouse_init();
    DEBUG_INFO("PS/2 Mouse Initialized");

    // ── Timer ────────────────────────────────────────────────────────────────
    timer_init(1000);   // 1000 Hz = 1 ms granularity (better for UI and network)
    DEBUG_INFO("Timer Initialized (1000Hz)");

    // ── Memory management ────────────────────────────────────────────────────
    pmm_init();
    DEBUG_INFO("PMM Initialized");

    vmm_init();
    DEBUG_INFO("VMM Initialized");

    // PAT: Write-Combining improves graphics performance (especially on AMD)
    pat_init();

    if (fb) {
        uint64_t fb_size = fb->pitch * fb->height;
        vmm_remap_framebuffer((uint64_t)fb->address, fb_size);
        DEBUG_INFO("Framebuffer remapped with Write-Combining");
    }

    heap_init(nullptr, 0);
    DEBUG_INFO("Heap Initialized (Bucket Allocator)");

    // Double buffering requires the heap (allocates the backbuffer)
    gfx_enable_double_buffering();
    DEBUG_INFO("Double Buffering Enabled");

    // ── Scheduler ────────────────────────────────────────────────────────────
    scheduler_init();
    DEBUG_INFO("Scheduler Initialized");

    scheduler_create_task(idle_task_entry, "Idle");
    DEBUG_INFO("Idle Task Created");

    // ── Bus / device layer ───────────────────────────────────────────────────
    pci_init();
    DEBUG_INFO("PCI Subsystem Initialized");

    acpi_init();    // Required for power-off support

    rtc_init();
    DEBUG_INFO("RTC Initialized");

    usb_init();     // usb_init logs its own status
    usb_hid_init(); // usb_hid_init logs its own status

    input_set_screen_size(fb->width, fb->height);

    // ── Network stack ────────────────────────────────────────────────────────
    net_init();     // net_init logs its own status

    // ── Sound ────────────────────────────────────────────────────────────────
    sound_init();   // sound_init logs its own status

    // ── Enable interrupts ────────────────────────────────────────────────────
    asm("sti");
    DEBUG_INFO("Interrupts Enabled");

    // ── Filesystem ──────────────────────────────────────────────────────────
    if (module_request.response && module_request.response->module_count > 0) {
        unifs_init(module_request.response->modules[0]->address);
        DEBUG_INFO("Filesystem Ready");
    } else {
        DEBUG_WARN("Filesystem: No modules");
    }

#ifdef DEBUG
    // Debug build: show boot log and wait for a keypress before continuing
    DEBUG_INFO("Boot complete!");
    gfx_draw_string(50, (int32_t)(fb->height - 40),
                    "Press any key to continue...", 0x00AAAAAA);
    gfx_swap_buffers();

    while (!input_keyboard_has_char()) {
        input_poll();
        scheduler_yield();
    }
    input_keyboard_get_char();  // consume the keypress
#endif

    // ── Splash screen ────────────────────────────────────────────────────────
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

    // ── Shell ────────────────────────────────────────────────────────────────
    shell_init(fb);

    // ── Main loop ────────────────────────────────────────────────────────────
    uint64_t       last_frame_time = 0;
    const uint64_t frame_delay     = 16;

    while (true) {
        input_poll();
        net_poll();
        sound_poll();

        uint64_t now = timer_get_ticks();

        if (now - last_frame_time >= frame_delay) {
            shell_tick();

            while (input_keyboard_has_char()) {
                char c = input_keyboard_get_char();
                shell_process_char(c);
            }

            gfx_swap_buffers();
            last_frame_time = now;
        } else {
            asm volatile("hlt");
        }
    }
}
