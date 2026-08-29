#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <drivers/class/hid/input.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <drivers/class/hid/usb_hid.h>
#include <drivers/video/framebuffer.h>

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
