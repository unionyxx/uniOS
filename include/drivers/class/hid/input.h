#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/input.h>

void input_poll();
bool input_keyboard_has_char();
char input_keyboard_get_char();
// True when Ctrl+C should synthesize SIGINT (terminal/desktop context);
// false when a GUI app is focused and the control char should be delivered
// instead (apps bind it, e.g. Edit > Copy).
bool input_ctrl_c_to_signal(void);
struct InputMouseState
{
    int32_t x;
    int32_t y;
    bool left;
    bool right;
    bool middle;
    int8_t scroll_delta;
};

void input_mouse_get_state(InputMouseState *state);
void input_set_screen_size(int32_t width, int32_t height);

// --- Input device settings (kernel-internal; driven by SYS_INPUT_* syscalls) ---
//
// Pointer speed is a Q8 multiplier (units of 1/256): 256 == 1.0x. It scales raw
// mouse deltas before position accumulation, orthogonal to the per-driver
// acceleration curve. Repeat delay/rate drive the USB HID software key-repeat
// path. Per-device enable flags gate whether a source contributes events; a
// disabled device is enumerated but its movement/keys are dropped at the merge
// layer.
int32_t input_scale_pointer(int32_t raw);
uint32_t input_pointer_speed(void);
uint32_t input_repeat_delay_ms(void);
uint32_t input_repeat_rate_ms(void);
bool input_usb_slot_enabled(uint8_t slot);
bool input_device_enabled(uint32_t id);
bool input_is_valid_device_id(uint32_t id);
void input_set_pointer_speed(uint32_t multiplier);
void input_set_repeat_rate(uint32_t delay_ms, uint32_t rate_ms);
void input_set_device_enabled(uint32_t id, bool enabled);
size_t input_enum_devices(InputDeviceInfo *out, size_t max_count);
