#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    EVT_NONE = 0,
    EVT_MOUSE_MOVE,
    EVT_MOUSE_DOWN,
    EVT_MOUSE_UP,
    EVT_MOUSE_SCROLL,
    EVT_KEY_DOWN,
    EVT_KEY_UP,
    EVT_WINDOW_RESIZE,
    EVT_WINDOW_CLOSE,
} EventType;

typedef struct EventMouseData
{
    int32_t x, y;
    uint8_t button;
    int8_t scroll_x;
    int8_t scroll_y;
    uint8_t reserved;
} EventMouseData;

typedef struct EventKeyData
{
    char c;
    uint8_t scancode;
} EventKeyData;

typedef struct EventResizeData
{
    int32_t width, height;
    uint32_t serial;
} EventResizeData;

typedef struct EventWindowData
{
    int window_id;
} EventWindowData;

struct Event
{
    EventType type;
    union
    {
        EventMouseData mouse;
        EventKeyData key;
        EventResizeData resize;
        EventWindowData window;
    };
};

#ifdef __cplusplus
}
#endif
