#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <drivers/class/hid/input.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <drivers/class/hid/usb_hid.h>
#include <drivers/video/framebuffer.h>

// Input device settings (driven by SYS_INPUT_* syscalls). Reads happen from
// IRQ context, so these are volatile and setters issue a full barrier. USB
// device enable is a 16-bit bitmask (one bit per xHCI slot) defaulting to all
// enabled without a per-element initializer.
static volatile uint32_t g_pointer_speed = 256; // Q8: 256 == 1.0x
static volatile uint32_t g_repeat_delay_ms = 500;
static volatile uint32_t g_repeat_rate_ms = 33;
static volatile bool g_ps2_mouse_enabled = true;
static volatile bool g_ps2_keyboard_enabled = true;
static volatile uint16_t g_usb_enabled_mask = 0xFFFFu;

void input_poll()
{
    static bool in_poll = false;
    if (in_poll)
        return;
    in_poll = true;

    // Full USB polling can sleep and enumerate devices, so it must not run
    // from timer/IRQ-driven input paths.
    usb_hid_update();

    in_poll = false;
}

bool input_keyboard_has_char()
{
    if (usb_hid_keyboard_preferred()) {
        return usb_hid_keyboard_has_char();
    }
    if (usb_hid_keyboard_has_char())
        return true;
    return ps2_keyboard_has_char();
}

char input_keyboard_get_char()
{
    if (usb_hid_keyboard_preferred()) {
        return usb_hid_keyboard_get_char();
    }
    if (usb_hid_keyboard_has_char())
        return usb_hid_keyboard_get_char();
    return ps2_keyboard_get_char();
}

void input_mouse_get_state(InputMouseState *state)
{
    if (!state)
        return;
    state->scroll_delta = 0;

    MouseState ps2_mouse = ps2_mouse_get_state();
    state->x = ps2_mouse.x;
    state->y = ps2_mouse.y;
    state->left = ps2_mouse.left_button;
    state->right = ps2_mouse.right_button;
    state->middle = ps2_mouse.middle_button;
    state->scroll_delta = ps2_mouse_get_scroll();

    if (usb_hid_mouse_available()) {
        int32_t usb_x, usb_y;
        bool left, right, middle;
        usb_hid_mouse_get_state(&usb_x, &usb_y, &left, &right, &middle);
        state->x = usb_x;
        state->y = usb_y;
        state->left = left;
        state->right = right;
        state->middle = middle;
        int8_t usb_scroll = usb_hid_mouse_get_scroll();
        if (usb_scroll != 0)
            state->scroll_delta = usb_scroll;
    }
}

void input_set_screen_size(int32_t width, int32_t height)
{
    usb_hid_set_screen_size(width, height);
}

// --- Input device settings (driven by SYS_INPUT_* syscalls) -----------------
// Globals live at the top of the file so the merge functions can gate PS/2
// sources. Setters clamp + fence; getters return the volatile scalar.

int32_t input_scale_pointer(int32_t raw)
{
    int64_t scaled = (int64_t)raw * (int64_t)g_pointer_speed / 256;
    if (scaled > 0x7fffffffLL)
        scaled = 0x7fffffffLL;
    if (scaled < (-0x7fffffffLL - 1))
        scaled = -0x7fffffffLL - 1;
    return (int32_t)scaled;
}

uint32_t input_pointer_speed(void)
{
    return g_pointer_speed;
}

uint32_t input_repeat_delay_ms(void)
{
    return g_repeat_delay_ms;
}

uint32_t input_repeat_rate_ms(void)
{
    return g_repeat_rate_ms;
}

bool input_usb_slot_enabled(uint8_t slot)
{
    if (slot == 0 || slot >= USB_MAX_DEVICES)
        return true; // unknown slot: never block input
    return (g_usb_enabled_mask >> slot) & 1u;
}

bool input_device_enabled(uint32_t id)
{
    if (id == INPUT_DEVICE_ID_PS2_MOUSE)
        return g_ps2_mouse_enabled;
    if (id == INPUT_DEVICE_ID_PS2_KEYBOARD)
        return g_ps2_keyboard_enabled;
    return input_usb_slot_enabled((uint8_t)id);
}

bool input_is_valid_device_id(uint32_t id)
{
    if (id == INPUT_DEVICE_ID_PS2_MOUSE || id == INPUT_DEVICE_ID_PS2_KEYBOARD)
        return true;
    return id >= 1 && id < USB_MAX_DEVICES;
}

void input_set_pointer_speed(uint32_t multiplier)
{
    if (multiplier < 16)
        multiplier = 16;
    if (multiplier > 1024)
        multiplier = 1024;
    g_pointer_speed = multiplier;
    __sync_synchronize();
}

void input_set_repeat_rate(uint32_t delay_ms, uint32_t rate_ms)
{
    if (delay_ms < 50)
        delay_ms = 50;
    if (delay_ms > 2000)
        delay_ms = 2000;
    if (rate_ms < 10)
        rate_ms = 10;
    if (rate_ms > 500)
        rate_ms = 500;
    g_repeat_delay_ms = delay_ms;
    g_repeat_rate_ms = rate_ms;
    __sync_synchronize();
}

void input_set_device_enabled(uint32_t id, bool enabled)
{
    if (id == INPUT_DEVICE_ID_PS2_MOUSE) {
        g_ps2_mouse_enabled = enabled;
    } else if (id == INPUT_DEVICE_ID_PS2_KEYBOARD) {
        g_ps2_keyboard_enabled = enabled;
    } else if (id >= 1 && id < USB_MAX_DEVICES) {
        if (enabled)
            g_usb_enabled_mask |= (uint16_t)(1u << id);
        else
            g_usb_enabled_mask &= (uint16_t)~(1u << id);
    }
    __sync_synchronize();
}

static void set_input_device_name(InputDeviceInfo &info, const char *name)
{
    size_t i = 0;
    if (name) {
        for (; i + 1 < INPUT_DEVICE_NAME_MAX && name[i]; i++)
            info.name[i] = name[i];
    }
    info.name[i] = '\0';
}

static size_t push_input_device(InputDeviceInfo *out, size_t max_count, size_t count, InputDeviceKind kind, uint32_t id,
                                uint16_t vendor, uint16_t product, const char *name, bool present, bool enabled)
{
    if (count < max_count && out) {
        InputDeviceInfo &info = out[count];
        info.id = id;
        info.kind = kind;
        info.vendor_id = vendor;
        info.product_id = product;
        info.present = present;
        info.enabled = enabled;
        set_input_device_name(info, name);
    }
    return count + 1;
}

size_t input_enum_devices(InputDeviceInfo *out, size_t max_count)
{
    size_t count = 0;
    count = push_input_device(out, max_count, count, INPUT_DEVICE_PS2_MOUSE, INPUT_DEVICE_ID_PS2_MOUSE, 0, 0,
                              "PS/2 Mouse", true, g_ps2_mouse_enabled);
    count = push_input_device(out, max_count, count, INPUT_DEVICE_PS2_KEYBOARD, INPUT_DEVICE_ID_PS2_KEYBOARD, 0, 0,
                              "PS/2 Keyboard", true, g_ps2_keyboard_enabled);

    int usb_count = usb_get_device_count();
    for (int i = 0; i < usb_count; i++) {
        UsbDeviceInfo *dev = usb_get_device(i);
        if (!dev)
            continue;
        bool present = dev->configured;
        if (dev->has_mouse)
            count = push_input_device(out, max_count, count, INPUT_DEVICE_USB_MOUSE, dev->slot_id, dev->vendor_id,
                                      dev->product_id, "USB Mouse", present, input_usb_slot_enabled(dev->slot_id));
        if (dev->has_keyboard)
            count = push_input_device(out, max_count, count, INPUT_DEVICE_USB_KEYBOARD, dev->slot_id, dev->vendor_id,
                                      dev->product_id, "USB Keyboard", present, input_usb_slot_enabled(dev->slot_id));
    }
    return count;
}
