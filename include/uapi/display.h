#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DISPLAY_PIXEL_FORMAT_XRGB8888 = 1
} DisplayPixelFormat;

enum
{
    DISPLAY_FLAG_HAS_VBLANK = 1u << 0,
    DISPLAY_FLAG_HAS_PAGE_FLIP = 1u << 1,
    DISPLAY_FLAG_USES_COPY_PATH = 1u << 2,
    DISPLAY_FLAG_STRICT_SYNC_ONLY = 1u << 3,
    DISPLAY_FLAG_HAS_CURSOR_PLANE = 1u << 4,
    DISPLAY_FLAG_HAS_OVERLAY = 1u << 5,
    DISPLAY_FLAG_HAS_COMPOSITOR = 1u << 6
};

enum
{
    DISPLAY_PRESENT_VBLANK = 1u << 0,
    DISPLAY_PRESENT_ASYNC = 1u << 1
};

enum
{
    DISPLAY_PRESENT_RESULT_OK = 0,
    DISPLAY_PRESENT_RESULT_DROPPED = 1,
    DISPLAY_PRESENT_RESULT_UNSUPPORTED = 2
};

enum
{
    DISPLAY_MODE_FLAG_PREFERRED = 1u << 0,
    DISPLAY_MODE_FLAG_CURRENT = 1u << 1,
    DISPLAY_MODE_FLAG_INTERLACED = 1u << 2,
    DISPLAY_MODE_FLAG_EXACT_TIMING = 1u << 3
};

enum
{
    DISPLAY_CONNECTOR_TYPE_UNKNOWN = 0,
    DISPLAY_CONNECTOR_TYPE_VGA = 1,
    DISPLAY_CONNECTOR_TYPE_DVI = 2,
    DISPLAY_CONNECTOR_TYPE_HDMI = 3,
    DISPLAY_CONNECTOR_TYPE_DP = 4
};

enum
{
    DISPLAY_CONNECTOR_STATUS_DISCONNECTED = 0,
    DISPLAY_CONNECTOR_STATUS_CONNECTED = 1
};

enum
{
    DISPLAY_PLANE_TYPE_PRIMARY = 0,
    DISPLAY_PLANE_TYPE_CURSOR = 1,
    DISPLAY_PLANE_TYPE_OVERLAY = 2
};

enum
{
    DISPLAY_BUFFER_FLAG_CPU_VISIBLE = 1u << 0,
    DISPLAY_BUFFER_FLAG_LINEAR = 1u << 1,
    DISPLAY_BUFFER_FLAG_SCANOUT = 1u << 2,
    DISPLAY_BUFFER_FLAG_CURSOR = 1u << 3,
    DISPLAY_BUFFER_FLAG_RENDER_TARGET = 1u << 4
};

enum
{
    DISPLAY_COMPOSE_LAYER_OPAQUE = 1u << 0,
    DISPLAY_COMPOSE_LAYER_PREMULTIPLIED = 1u << 1,
    DISPLAY_COMPOSE_LAYER_CURSOR = 1u << 2
};

enum
{
    DISPLAY_SURFACE_BACKING_NONE = 0,
    DISPLAY_SURFACE_BACKING_SHM = 1,
    DISPLAY_SURFACE_BACKING_DISPLAY_BUFFER = 2,
    DISPLAY_SURFACE_BACKING_PRIVATE = 3
};

enum
{
    DISPLAY_EVENT_NONE = 0,
    DISPLAY_EVENT_VBLANK = 1,
    DISPLAY_EVENT_FLIP_COMPLETE = 2,
    DISPLAY_EVENT_HOTPLUG = 3
};

enum
{
    DISPLAY_ATOMIC_MODESET = 1u << 0,
    DISPLAY_ATOMIC_PAGE_FLIP = 1u << 1
};

typedef struct Rect
{
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} Rect;

typedef struct DisplayCaps
{
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t pixel_format;
    uint32_t refresh_hz;
    uint32_t refresh_millihz;
    uint32_t nominal_refresh_millihz;
    uint32_t measured_refresh_millihz;
    uint32_t flags;
} DisplayCaps;

typedef uint32_t DisplayBufferHandle;

typedef struct DisplayMode
{
    uint32_t mode_id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t nominal_refresh_millihz;
    uint32_t measured_refresh_millihz;
    uint32_t pixel_clock_khz;
    uint32_t htotal;
    uint32_t vtotal;
    uint32_t hblank_start;
    uint32_t hblank_end;
    uint32_t hsync_start;
    uint32_t hsync_end;
    uint32_t vblank_start;
    uint32_t vblank_end;
    uint32_t vsync_start;
    uint32_t vsync_end;
    uint32_t flags;
} DisplayMode;

typedef struct DisplayConnectorInfo
{
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t status;
    uint32_t active_mode_id;
    char name[32];
} DisplayConnectorInfo;

typedef struct DisplayPlaneInfo
{
    uint32_t plane_id;
    uint32_t plane_type;
    uint32_t flags;
} DisplayPlaneInfo;

typedef struct DisplayBufferCreate
{
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t stride;
    DisplayBufferHandle handle;
    uint64_t size_bytes;
} DisplayBufferCreate;

typedef struct DisplayBufferMap
{
    DisplayBufferHandle handle;
    uint64_t address;
    uint64_t size_bytes;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
} DisplayBufferMap;

typedef struct DisplayPresentRequest
{
    const uint32_t *buffer;
    uint32_t stride;
    const Rect *rects;
    uint32_t rect_count;
    uint32_t frame_sequence;
    uint32_t flags;
    int32_t source_origin_x;
    int32_t source_origin_y;
} DisplayPresentRequest;

typedef struct DisplayStatus
{
    uint32_t completed_sequence;
    uint32_t queued_sequence;
    uint64_t vblank_count;
    uint64_t last_vblank_ticks;
    uint64_t last_present_ticks;
    uint32_t last_present_result;
    uint32_t flags;
} DisplayStatus;

typedef struct DisplaySyncPoint
{
    uint32_t sequence;
    uint32_t flags;
    uint64_t completed_ticks;
} DisplaySyncPoint;

typedef struct DisplaySurface
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t backing_kind;
    uint64_t cpu_map_address;
    uint64_t cpu_map_size;
    uint32_t cpu_map_stride;
    uint32_t cpu_map_flags;
    uint64_t backend_handle;
    uint64_t backend_private_handle;
    uint32_t dirty_generation;
    uint32_t resize_generation;
    uint32_t reserved;
} DisplaySurface;

typedef struct DisplaySurfaceImport
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t backing_kind;
    int32_t shm_id;
    DisplayBufferHandle display_handle;
    uint32_t dirty_generation;
    uint32_t resize_generation;
    uint32_t flags;
} DisplaySurfaceImport;

typedef struct DisplayRenderTarget
{
    DisplaySurface surface;
    DisplaySyncPoint last_sync;
    uint32_t flags;
    uint32_t reserved;
} DisplayRenderTarget;

typedef struct CompositorLayerSurface
{
    DisplaySurface surface;
    Rect src_rect;
    Rect dst_rect;
    uint32_t flags;
    uint32_t alpha;
} CompositorLayerSurface;

typedef struct DisplayComposeLayer
{
    DisplayBufferHandle buffer_handle;
    Rect src_rect;
    Rect dst_rect;
    uint32_t flags;
    uint32_t alpha;
} DisplayComposeLayer;

typedef struct DisplayComposeRequest
{
    const DisplayComposeLayer *layers;
    uint32_t layer_count;
    const Rect *damage_rects;
    uint32_t damage_rect_count;
    uint32_t frame_sequence;
    uint32_t flags;
    DisplayBufferHandle cursor_buffer_handle;
    int32_t cursor_x;
    int32_t cursor_y;
    uint32_t reserved;
} DisplayComposeRequest;

typedef struct DisplayEvent
{
    uint32_t type;
    uint32_t connector_id;
    uint32_t sequence;
    uint32_t reserved;
    uint64_t timestamp_ticks;
    uint64_t vblank_count;
} DisplayEvent;

typedef struct DisplayAtomicRequest
{
    uint32_t connector_id;
    uint32_t mode_id;
    uint32_t flags;
    uint32_t reserved;
    Rect damage_bounds;
} DisplayAtomicRequest;

static inline Rect gui_rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    Rect r = {x, y, w, h};
    return r;
}

static inline bool gui_rect_is_empty(Rect r)
{
    return r.w <= 0 || r.h <= 0;
}

static inline Rect gui_rect_union(Rect a, Rect b)
{
    if (gui_rect_is_empty(a))
        return b;
    if (gui_rect_is_empty(b))
        return a;
    int32_t x1 = (a.x < b.x) ? a.x : b.x;
    int32_t y1 = (a.y < b.y) ? a.y : b.y;
    int32_t x2 = (a.x + a.w > b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y2 = (a.y + a.h > b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return gui_rect_make(x1, y1, x2 - x1, y2 - y1);
}

#ifdef __cplusplus
}
#endif
