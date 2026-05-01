/**
 * @file usb_hid.cpp
 * @brief USB HID (Human Interface Device) driver for uniOS
 */

#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <drivers/class/hid/hid_report_parser.h>
#include <drivers/class/hid/usb_hid.h>
#include <kernel/debug.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <libk/kstd.h>
#include <libk/kstring.h>
#include <stddef.h>

// Keyboard state
static bool keyboard_available = false;
static bool keyboard_preferred = false;
static UsbDeviceInfo *keyboard_device = nullptr;
static HidKeyboardReport last_kbd_report = {0, 0, {0}};
static HidKeyboardReportLayout keyboard_report_layout = {};
static HidDecodedKeyboardReport last_decoded_report = {};
static bool keyboard_boot_protocol = false;
static bool keyboard_warned_unsupported = false;
static uint64_t keyboard_last_valid_report_tick = 0;
static bool caps_lock = false;

// Keyboard buffer
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static volatile uint8_t kb_head = 0;
static volatile uint8_t kb_tail = 0;

// Mouse state
static bool mouse_available = false;
static bool mouse_data_received = false;
static UsbDeviceInfo *mouse_device = nullptr;
static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static bool mouse_left = false;
static bool mouse_right = false;
static bool mouse_middle = false;
static int8_t mouse_scroll = 0;

// Screen dimensions
static int32_t screen_width = 1024;
static int32_t screen_height = 768;

// Key repeat
static uint8_t repeat_key = 0;
static uint64_t repeat_start = 0;
static uint64_t repeat_last = 0;
static bool repeat_shift = false;
static const uint32_t REPEAT_DELAY = 500;
static const uint32_t REPEAT_RATE = 33;

static Spinlock g_kb_lock = SPINLOCK_INIT;
static Spinlock g_mouse_lock = SPINLOCK_INIT;

// Special key codes (matching PS/2)
#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_HOME 0x84
#define KEY_END 0x85
#define KEY_DELETE 0x86
#define KEY_PAGEUP 0x87
#define KEY_PAGEDOWN 0x88

// HID keycode to ASCII (lowercase)
static const char hid_to_ascii[128] = {0,
                                       0,
                                       0,
                                       0,
                                       'a',
                                       'b',
                                       'c',
                                       'd',
                                       'e',
                                       'f',
                                       'g',
                                       'h',
                                       'i',
                                       'j',
                                       'k',
                                       'l',
                                       'm',
                                       'n',
                                       'o',
                                       'p',
                                       'q',
                                       'r',
                                       's',
                                       't',
                                       'u',
                                       'v',
                                       'w',
                                       'x',
                                       'y',
                                       'z',
                                       '1',
                                       '2',
                                       '3',
                                       '4',
                                       '5',
                                       '6',
                                       '7',
                                       '8',
                                       '9',
                                       '0',
                                       '\n',
                                       27,
                                       '\b',
                                       '\t',
                                       ' ',
                                       '-',
                                       '=',
                                       '[',
                                       ']',
                                       '\\',
                                       '#',
                                       ';',
                                       '\'',
                                       '`',
                                       ',',
                                       '.',
                                       '/',
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       (char)KEY_HOME,
                                       (char)KEY_PAGEUP,
                                       (char)KEY_DELETE,
                                       (char)KEY_END,
                                       (char)KEY_PAGEDOWN,
                                       (char)KEY_RIGHT,
                                       (char)KEY_LEFT,
                                       (char)KEY_DOWN,
                                       (char)KEY_UP,
                                       0,
                                       '/',
                                       '*',
                                       '-',
                                       '+',
                                       '\n',
                                       '1',
                                       '2',
                                       '3',
                                       '4',
                                       '5',
                                       '6',
                                       '7',
                                       '8',
                                       '9',
                                       '0',
                                       '.',
                                       0,
                                       0,
                                       0,
                                       '=',
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0};

// HID keycode to ASCII (shifted)
static const char hid_to_ascii_shift[128] = {
    0,    0,   0,   0,   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',  'P', 'Q',  'R',
    'S',  'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '\n', 27,  '\b', '\t',
    ' ',  '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?', 0,   0,   0,   0,   0,   0,    0,   0,    0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '/',  '*', '-',  '+',
    '\n', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.', 0,   0,   0,   '=', 0,   0,   0,    0,   0,    0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0};

static bool key_was_pressed(uint8_t keycode)
{
    for (int i = 0; i < 6; i++) {
        if (last_kbd_report.keys[i] == keycode)
            return true;
    }
    return false;
}

static void kb_push(char c)
{
    spinlock_acquire(&g_kb_lock);
    const uint8_t next = static_cast<uint8_t>((kb_tail + 1) % KB_BUFFER_SIZE);
    if (next != kb_head) {
        kb_buffer[kb_tail] = c;
        kb_tail = next;
    }
    spinlock_release(&g_kb_lock);
}

static void clear_repeat_state()
{
    repeat_key = 0;
    repeat_shift = false;
    repeat_start = 0;
    repeat_last = 0;
}

static void reset_keyboard_decoder_state()
{
    spinlock_acquire(&g_kb_lock);
    kb_head = 0;
    kb_tail = 0;
    spinlock_release(&g_kb_lock);

    last_kbd_report = {0, 0, {0}};
    hid_reset_keyboard_report_layout(&keyboard_report_layout);
    hid_reset_decoded_keyboard_report(&last_decoded_report);
    keyboard_boot_protocol = false;
    keyboard_warned_unsupported = false;
    keyboard_preferred = false;
    keyboard_last_valid_report_tick = 0;
    caps_lock = false;
    clear_repeat_state();
}

static void handle_key_repeat()
{
    if (repeat_key == 0)
        return;
    const uint64_t now = timer_get_ticks();
    if (now - repeat_start < REPEAT_DELAY)
        return;
    if (now - repeat_last < REPEAT_RATE)
        return;

    const char c = repeat_shift ? hid_to_ascii_shift[repeat_key] : hid_to_ascii[repeat_key];
    if (c)
        kb_push(c);
    repeat_last = now;
}

static void queue_key_press(uint8_t key, bool shift, bool ctrl)
{
    if (key == 0 || key == 1 || key == 2 || key == 3 || key >= 128)
        return;

    if (key == 0x39) {
        caps_lock = !caps_lock;
        return;
    }

    if (key == 0x50) {
        kb_push((char)KEY_LEFT);
        return;
    }
    if (key == 0x4F) {
        kb_push((char)KEY_RIGHT);
        return;
    }

    bool use_shift = shift;
    const char base = hid_to_ascii[key];
    if (caps_lock && base >= 'a' && base <= 'z') {
        use_shift = !use_shift;
    }

    const char c = use_shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
    if (ctrl && c != 0) {
        if (c >= 'a' && c <= 'z') {
            kb_push(c - 'a' + 1);
            return;
        }
        if (c >= 'A' && c <= 'Z') {
            kb_push(c - 'A' + 1);
            return;
        }
        if (c == '[' || c == '{') {
            kb_push(27);
            return;
        }
    }

    if (c) {
        kb_push(c);
        repeat_key = key;
        repeat_shift = use_shift;
        repeat_start = timer_get_ticks();
        repeat_last = repeat_start;
    }
}

static void process_boot_keyboard_report(const HidKeyboardReport *report)
{
    if (!report)
        return;

    bool has_rollover = false;
    for (int i = 0; i < 6; i++) {
        if (report->keys[i] == 1 || report->keys[i] == 2 || report->keys[i] == 3) {
            has_rollover = true;
            break;
        }
    }

    const bool shift = (report->modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
    const bool ctrl = (report->modifiers & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL)) != 0;

    if (!has_rollover) {
        for (int i = 0; i < 6; i++) {
            const uint8_t key = report->keys[i];
            if (key == 0 || key >= 128 || key <= 3)
                continue;
            if (key_was_pressed(key))
                continue;
            queue_key_press(key, shift, ctrl);
        }
    } else {
        clear_repeat_state();
    }

    bool repeat_key_held = false;
    if (repeat_key != 0) {
        for (int i = 0; i < 6; i++) {
            if (report->keys[i] == repeat_key) {
                repeat_key_held = true;
                break;
            }
        }
    }
    if (!repeat_key_held)
        repeat_key = 0;

    last_kbd_report = *report;
    keyboard_preferred = true;
    keyboard_last_valid_report_tick = timer_get_ticks();
}

static void process_decoded_keyboard_report(const HidDecodedKeyboardReport *report)
{
    if (!report)
        return;

    const bool shift = (report->modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
    const bool ctrl = (report->modifiers & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL)) != 0;

    for (uint8_t i = 0; i < report->usage_count; i++) {
        const uint8_t usage = report->usages[i];
        if (usage <= 3 || usage >= 128)
            continue;
        if (hid_keyboard_report_has_usage(&last_decoded_report, usage))
            continue;
        queue_key_press(usage, shift, ctrl);
    }

    if (repeat_key != 0 && !hid_keyboard_report_has_usage(report, repeat_key)) {
        repeat_key = 0;
    }

    last_decoded_report = *report;
    keyboard_preferred = true;
    keyboard_last_valid_report_tick = timer_get_ticks();
}

static bool hid_set_protocol(UsbDeviceInfo *dev, uint8_t iface, uint8_t protocol)
{
    if (!dev)
        return false;
    uint16_t transferred = 0;
    return xhci_control_transfer(dev->slot_id, 0x21, HID_REQ_SET_PROTOCOL, protocol, iface, 0, nullptr, &transferred,
                                 false);
}

static bool hid_set_idle(UsbDeviceInfo *dev, uint8_t iface, uint8_t duration)
{
    if (!dev)
        return false;
    uint16_t transferred = 0;
    return xhci_control_transfer(dev->slot_id, 0x21, HID_REQ_SET_IDLE, static_cast<uint16_t>(duration) << 8, iface, 0,
                                 nullptr, &transferred, false);
}

static bool load_keyboard_report_layout(UsbDeviceInfo *dev)
{
    if (!dev || dev->kbd_report_desc_length == 0 || dev->kbd_report_desc_length > 1024)
        return false;

    kstd::unique_ptr<uint8_t[]> report_desc(new uint8_t[dev->kbd_report_desc_length]);
    if (!report_desc)
        return false;

    uint16_t transferred = 0;
    if (!usb_get_hid_report_descriptor(dev->slot_id, dev->kbd_interface, report_desc.get(), dev->kbd_report_desc_length,
                                       &transferred)) {
        return false;
    }
    if (transferred == 0)
        return false;

    hid_reset_keyboard_report_layout(&keyboard_report_layout);
    if (!hid_parse_keyboard_report_descriptor(report_desc.get(), transferred, &keyboard_report_layout)) {
        hid_reset_keyboard_report_layout(&keyboard_report_layout);
        return false;
    }

    return true;
}

static void keyboard_interrupt_cb(uint8_t slot_id, uint8_t ep_num, void *data, uint16_t length)
{
    (void)slot_id;
    (void)ep_num;

    if (!data || length == 0) {
        clear_repeat_state();
        scheduler_notify_input_waiters();
        return;
    }

    if (keyboard_boot_protocol) {
        if (length >= sizeof(HidKeyboardReport)) {
            process_boot_keyboard_report(reinterpret_cast<const HidKeyboardReport *>(data));
        } else {
            clear_repeat_state();
        }
        scheduler_notify_input_waiters();
        return;
    }

    if (!keyboard_report_layout.valid) {
        if (!keyboard_warned_unsupported) {
            KLOG(LogModule::Usb, LogLevel::Warn, "HID: Unsupported keyboard report format, ignoring input safely");
            keyboard_warned_unsupported = true;
        }
        clear_repeat_state();
        scheduler_notify_input_waiters();
        return;
    }

    HidDecodedKeyboardReport decoded = {};
    switch (hid_decode_keyboard_report(&keyboard_report_layout, reinterpret_cast<const uint8_t *>(data), length,
                                       &decoded)) {
        case HidKeyboardDecodeStatus::Match:
            process_decoded_keyboard_report(&decoded);
            break;
        case HidKeyboardDecodeStatus::Ignore:
            break;
        case HidKeyboardDecodeStatus::Invalid:
        default:
            clear_repeat_state();
            break;
    }

    scheduler_notify_input_waiters();
}

static void process_mouse_report(uint8_t *data, uint16_t length)
{
    if (length < 3)
        return;

    uint8_t btn = 0;
    int16_t dx = 0;
    int16_t dy = 0;
    int8_t wheel = 0;

    const bool has_report_id = length >= 5;
    if (has_report_id) {
        btn = data[1];
        if (length >= 6) {
            dx = static_cast<int16_t>(data[2] | (data[3] << 8));
            dy = static_cast<int16_t>(data[4] | (data[5] << 8));
            if (length >= 7)
                wheel = static_cast<int8_t>(data[6]);
        } else {
            dx = static_cast<int8_t>(data[2]);
            dy = static_cast<int8_t>(data[3]);
            if (length >= 5)
                wheel = static_cast<int8_t>(data[4]);
        }
    } else {
        btn = data[0];
        dx = static_cast<int8_t>(data[1]);
        dy = static_cast<int8_t>(data[2]);
        if (length >= 4)
            wheel = static_cast<int8_t>(data[3]);
    }

    spinlock_acquire(&g_mouse_lock);
    mouse_left = (btn & HID_MOUSE_LEFT) != 0;
    mouse_right = (btn & HID_MOUSE_RIGHT) != 0;
    mouse_middle = (btn & HID_MOUSE_MIDDLE) != 0;
    mouse_scroll += wheel;
    mouse_x += dx;
    mouse_y += dy;

    if (mouse_x < 0)
        mouse_x = 0;
    if (mouse_y < 0)
        mouse_y = 0;
    if (mouse_x >= screen_width)
        mouse_x = screen_width - 1;
    if (mouse_y >= screen_height)
        mouse_y = screen_height - 1;

    mouse_available = true;
    mouse_data_received = true;
    spinlock_release(&g_mouse_lock);
}

static void mouse_interrupt_cb(uint8_t slot_id, uint8_t ep_num, void *data, uint16_t length)
{
    (void)slot_id;
    (void)ep_num;
    if (length >= 3)
        process_mouse_report(reinterpret_cast<uint8_t *>(data), length);
    scheduler_notify_input_waiters();
}

static void attach_keyboard(UsbDeviceInfo *dev)
{
    if (!dev || !dev->configured || !dev->has_keyboard || dev->kbd_endpoint == 0)
        return;

    keyboard_available = true;
    keyboard_device = dev;
    reset_keyboard_decoder_state();

    if (dev->kbd_is_boot) {
        keyboard_boot_protocol = hid_set_protocol(dev, dev->kbd_interface, HID_PROTOCOL_BOOT);
        if (!keyboard_boot_protocol) {
            KLOG(LogModule::Usb, LogLevel::Info,
                 "HID: Keyboard boot protocol unavailable, falling back to report descriptor decoding");
        }
    }

    if (!keyboard_boot_protocol) {
        if (!load_keyboard_report_layout(dev)) {
            KLOG(LogModule::Usb, LogLevel::Warn,
                 "HID: Failed to parse keyboard report descriptor, ignoring unsupported input safely");
        }
    }

    hid_set_idle(dev, dev->kbd_interface, 0);
    xhci_register_interrupt_callback(dev->slot_id, dev->kbd_endpoint, keyboard_interrupt_cb);

    uint16_t packet_size = dev->kbd_max_packet ? dev->kbd_max_packet : sizeof(HidKeyboardReport);
    if (keyboard_report_layout.valid && keyboard_report_layout.report_bytes != 0) {
        packet_size = static_cast<uint16_t>(keyboard_report_layout.report_bytes +
                                            (keyboard_report_layout.uses_report_id ? 1u : 0u));
    }
    if (packet_size > 64)
        packet_size = 64;
    if (!xhci_submit_interrupt_transfer(dev->slot_id, dev->kbd_endpoint, packet_size)) {
        KLOG(LogModule::Usb, LogLevel::Error, "HID: Failed to arm keyboard interrupt endpoint (Slot %d, EP %d)",
             dev->slot_id, dev->kbd_endpoint);
        keyboard_available = false;
        keyboard_device = nullptr;
        reset_keyboard_decoder_state();
        return;
    }
    KLOG(LogModule::Usb, LogLevel::Success, "HID: Keyboard initialized (Slot %d, EP %d)", dev->slot_id,
         dev->kbd_endpoint);
}

static void attach_mouse(UsbDeviceInfo *dev)
{
    if (!dev || !dev->configured || !dev->has_mouse || dev->mouse_endpoint == 0)
        return;

    mouse_available = true;
    mouse_device = dev;
    mouse_data_received = false;

    if (dev->mouse_is_boot) {
        if (hid_set_protocol(dev, dev->mouse_interface, HID_PROTOCOL_BOOT)) {
            KLOG(LogModule::Usb, LogLevel::Info, "HID: Set Mouse Boot Protocol OK");
        } else {
            KLOG(LogModule::Usb, LogLevel::Info,
                 "HID: Mouse boot protocol unavailable, continuing with device default protocol");
        }
    }

    hid_set_idle(dev, dev->mouse_interface, 0);
    mouse_x = screen_width / 2;
    mouse_y = screen_height / 2;

    xhci_register_interrupt_callback(dev->slot_id, dev->mouse_endpoint, mouse_interrupt_cb);
    uint16_t packet_size = dev->mouse_max_packet ? dev->mouse_max_packet : 64;
    if (packet_size > 64)
        packet_size = 64;
    if (!xhci_submit_interrupt_transfer(dev->slot_id, dev->mouse_endpoint, packet_size)) {
        KLOG(LogModule::Usb, LogLevel::Error, "HID: Failed to arm mouse interrupt endpoint (Slot %d, EP %d)",
             dev->slot_id, dev->mouse_endpoint);
        mouse_available = false;
        mouse_data_received = false;
        mouse_device = nullptr;
        return;
    }
    KLOG(LogModule::Usb, LogLevel::Success, "HID: Mouse initialized (Slot %d, EP %d)", dev->slot_id,
         dev->mouse_endpoint);
}

void usb_hid_device_connected(UsbDeviceInfo *dev)
{
    if (!dev || !dev->configured)
        return;
    if (dev->has_keyboard && dev->kbd_endpoint != 0)
        attach_keyboard(dev);
    if (dev->has_mouse && dev->mouse_endpoint != 0)
        attach_mouse(dev);
}

void usb_hid_device_disconnected(const UsbDeviceInfo *dev)
{
    if (dev && keyboard_device && keyboard_device->slot_id == dev->slot_id) {
        keyboard_available = false;
        keyboard_preferred = false;
        keyboard_device = nullptr;
        reset_keyboard_decoder_state();
    }

    if (dev && mouse_device && mouse_device->slot_id == dev->slot_id) {
        mouse_available = false;
        mouse_data_received = false;
        mouse_device = nullptr;
    }
}

void usb_hid_init()
{
    if (!keyboard_device)
        keyboard_device = usb_find_keyboard();
    if (!mouse_device)
        mouse_device = usb_find_mouse();
    keyboard_available = keyboard_device != nullptr;
    mouse_available = mouse_device != nullptr;

    KLOG(LogModule::Usb, LogLevel::Info, "hid: keyboard=%s mouse=%s", keyboard_available ? "YES" : "NO",
         mouse_available ? "YES" : "NO");
}

void usb_hid_update()
{
    usb_poll();

    if (keyboard_preferred && keyboard_device && repeat_key != 0 &&
        !xhci_interrupt_transfer_pending(keyboard_device->slot_id, keyboard_device->kbd_endpoint) &&
        timer_get_ticks() - keyboard_last_valid_report_tick > REPEAT_DELAY) {
        clear_repeat_state();
    }

    handle_key_repeat();
}

bool usb_hid_keyboard_available()
{
    return keyboard_available;
}
bool usb_hid_keyboard_preferred()
{
    return keyboard_preferred;
}
bool usb_hid_keyboard_has_char()
{
    return kb_head != kb_tail;
}

char usb_hid_keyboard_get_char()
{
    spinlock_acquire(&g_kb_lock);
    if (kb_head == kb_tail) {
        spinlock_release(&g_kb_lock);
        return 0;
    }
    const char c = kb_buffer[kb_head];
    kb_head = static_cast<uint8_t>((kb_head + 1) % KB_BUFFER_SIZE);
    spinlock_release(&g_kb_lock);
    return c;
}

bool usb_hid_mouse_available()
{
    return mouse_available && mouse_data_received;
}

void usb_hid_mouse_get_state(int32_t *x, int32_t *y, bool *left, bool *right, bool *middle)
{
    spinlock_acquire(&g_mouse_lock);
    if (x)
        *x = mouse_x;
    if (y)
        *y = mouse_y;
    if (left)
        *left = mouse_left;
    if (right)
        *right = mouse_right;
    if (middle)
        *middle = mouse_middle;
    spinlock_release(&g_mouse_lock);
}

int8_t usb_hid_mouse_get_scroll()
{
    const int8_t delta = mouse_scroll;
    mouse_scroll = 0;
    return delta;
}

void usb_hid_set_screen_size(int32_t width, int32_t height)
{
    screen_width = width;
    screen_height = height;
    mouse_x = width / 2;
    mouse_y = height / 2;
}

void usb_hid_set_debug(bool enabled)
{
    (void)enabled;
}
