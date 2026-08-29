#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_DEVICE_NAME_MAX 40
/* Upper bound on enumerated entries: 2 fixed PS/2 devices plus up to two
 * streams (mouse + keyboard) per USB device. Usersize buffers this large. */
#define INPUT_MAX_DEVICES 32

/* Stable device ids: USB devices reuse their xHCI slot_id (1..15); the
 * fixed PS/2 controllers use the sentinel ids below so a settings UI can
 * address them across boots even though they have no slot. */
#define INPUT_DEVICE_ID_PS2_MOUSE 0xFEu
#define INPUT_DEVICE_ID_PS2_KEYBOARD 0xFFu

typedef enum
{
    INPUT_DEVICE_PS2_MOUSE = 1,
    INPUT_DEVICE_PS2_KEYBOARD = 2,
    INPUT_DEVICE_USB_MOUSE = 3,
    INPUT_DEVICE_USB_KEYBOARD = 4,
} InputDeviceKind;

typedef struct
{
    uint32_t id; /* stable id (USB slot or a PS/2 sentinel) */
    InputDeviceKind kind;
    uint16_t vendor_id;  /* 0 for PS/2 */
    uint16_t product_id; /* 0 for PS/2 */
    char name[INPUT_DEVICE_NAME_MAX];
    bool present; /* currently connected */
    bool enabled; /* contributing input */
} InputDeviceInfo;

#ifdef __cplusplus
}
#endif
