#pragma once

#include <boot/boot_info.h>
#include <drivers/bus/pci/pci.h>
#include <stdint.h>
#include <uapi/display.h>

enum class DisplayBackendKind : uint32_t
{
    None = 0,
    FirmwareFramebuffer
};

struct DisplayBuffer
{
    uint32_t *cpu_mapping;
    uint64_t phys_addr;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    bool full_frame_valid;
};

struct DisplayPlane
{
    DisplayPlaneInfo info;
    DisplayBuffer buffer;
    Rect src_rect;
    Rect dst_rect;
};

struct DisplayCrtc
{
    uint32_t crtc_id;
    DisplayMode current_mode;
    uint64_t last_vblank_ticks;
};

struct DisplayEncoder
{
    uint32_t encoder_id;
    uint32_t flags;
};

struct DisplayConnector
{
    DisplayConnectorInfo info;
    DisplayMode modes[32];
    uint32_t mode_count;
    uint32_t encoder_index;
};

struct DisplayAtomicState
{
    uint32_t connector_id;
    uint32_t mode_id;
    bool modeset;
    bool page_flip;
    Rect damage_bounds;
};

enum class PresentBackendResult : uint32_t
{
    PRESENT_DIRECT_FLIP = 0,
    PRESENT_PLANE_ONLY = 1,
    PRESENT_BACKEND_COMPOSED = 2,
    PRESENT_CPU_FALLBACK = 3
};

struct DisplayComposeFrameContext
{
    uint64_t owner_pid;
    uint32_t frame_sequence;
    uint32_t flags;
    uint32_t layer_count;
    DisplayBuffer target_buffer;
    const Rect *damage_rects;
    uint32_t damage_rect_count;
    DisplayBufferHandle cursor_buffer_handle;
    int32_t cursor_x;
    int32_t cursor_y;
};

struct DisplayHead
{
    DisplayCaps caps;
    DisplayStatus status;
    DisplayBuffer buffers[3];
    uint32_t buffer_count;
    uint32_t front_buffer_index;
    uint32_t submitted_sequence;
    uint32_t buffer_in_flight_sequence[3];
    uint32_t pending_buffer_index;
    uint32_t pending_flip_sequence;
    bool pending_flip;
    uint64_t next_deadline_tick;
    uint32_t deadline_remainder;
    uint64_t last_present_pixels;
    DisplayMode current_mode;
    DisplayMode preferred_mode;
    uint32_t last_submitted_layers;
    uint32_t last_submitted_dirty_rects;
    uint64_t last_submitted_dirty_area;
    uint32_t last_present_path;
    uint64_t last_dma_wait_ticks;
    uint64_t last_vblank_wait_ticks;
    uint64_t total_dma_wait_ticks;
    uint64_t total_vblank_wait_ticks;
    uint64_t missed_target_vblank_count;
    uint64_t flip_path_count;
    uint64_t copy_path_count;
    uint64_t backend_compose_attempt_count;
    uint64_t backend_compose_path_count;
    uint64_t backend_compose_failure_count;
    uint64_t cpu_compose_path_count;
};

struct DisplayDevice;

struct DisplayBackendOps
{
    const char *name;
    bool (*get_caps)(DisplayDevice *device, DisplayCaps *out_caps);
    bool (*get_status)(DisplayDevice *device, DisplayStatus *out_status);
    uint32_t (*present)(DisplayDevice *device, const DisplayPresentRequest &request);
    uint32_t (*present_buffer)(DisplayDevice *device, const DisplayBuffer &buffer, const Rect *damage_rects,
                               uint32_t damage_count, uint32_t frame_sequence, uint32_t flags);
    uint32_t (*wait)(DisplayDevice *device);
    bool (*set_mode)(DisplayDevice *device, const DisplayMode &mode);
    bool (*atomic_commit)(DisplayDevice *device, const DisplayAtomicState &state, const DisplayMode &mode);
    bool (*backend_compose_begin)(DisplayDevice *device, const DisplayBuffer *target,
                                  const DisplayComposeFrameContext &frame_ctx);
    bool (*backend_compose_clear)(DisplayDevice *device, uint32_t color, const Rect &rect);
    bool (*backend_compose_layer)(DisplayDevice *device, const DisplayComposeLayer &layer);
    bool (*backend_compose_cursor)(DisplayDevice *device, DisplayBufferHandle cursor_buffer_handle, int32_t cursor_x,
                                   int32_t cursor_y);
    bool (*backend_compose_end)(DisplayDevice *device);
    uint32_t (*backend_compose_submit)(DisplayDevice *device, PresentBackendResult *out_result);
};

struct DisplayDevice
{
    const DisplayBackendOps *ops;
    const BootFramebuffer *boot_framebuffer;
    PciDevice pci_device;
    bool has_pci_device;
    uint16_t detected_vendor_id;
    uint16_t detected_device_id;
    DisplayBackendKind backend_kind;
    DisplayHead primary_head;
    DisplayConnector connectors[4];
    uint32_t connector_count;
    DisplayPlane planes[3];
    uint32_t plane_count;
    DisplayCrtc crtcs[1];
    uint32_t crtc_count;
    DisplayEncoder encoders[4];
    uint32_t encoder_count;
};

DisplayDevice *display_device_instance(void);
DisplayHead *display_primary_head(void);
bool display_clip_rect(Rect &rect, const DisplayCaps &caps);
uint32_t display_resolve_present_sequence(DisplayHead *head, const DisplayPresentRequest &request);
bool display_import_surface(uint64_t owner_pid, const DisplaySurfaceImport &request, DisplaySurface *out_surface);
bool display_lookup_present_buffer(uint64_t owner_pid, DisplayBufferHandle handle, DisplayBuffer *out_buffer);
void display_note_present_submitted(DisplayHead *head, uint32_t sequence, uint32_t path, uint32_t layers,
                                    const Rect *damage_rects, uint32_t damage_count);
void display_complete_present(DisplayHead *head, uint32_t sequence, uint32_t result, uint64_t present_pixels);
void display_copy_present(DisplayDevice *device, const DisplayPresentRequest &request);
uint32_t display_fallback_wait_until_next_frame(DisplayHead *head);

void display_init(const BootFramebuffer *fb);
void display_late_init(void);
uint32_t display_detect_refresh_millihz_from_edid(const uint8_t *edid, uint64_t edid_size, uint32_t width,
                                                  uint32_t height);
uint32_t display_detect_refresh_hz_from_edid(const uint8_t *edid, uint64_t edid_size, uint32_t width, uint32_t height);
bool display_detect_exact_refresh_millihz_from_edid_hint(const uint8_t *edid, uint64_t edid_size, uint32_t width,
                                                         uint32_t height, uint32_t refresh_hint_millihz,
                                                         uint32_t *out_refresh_millihz);
int display_detect_modes_from_edid(const uint8_t *edid, uint64_t edid_size, DisplayMode *out_modes, uint32_t max_modes,
                                   const DisplayMode *current_mode, uint32_t measured_refresh_millihz);
bool display_get_caps(DisplayCaps *out_caps);
bool display_get_status(DisplayStatus *out_status);
int display_query_connectors(DisplayConnectorInfo *out, uint32_t max_count);
int display_get_modes(uint32_t connector_id, DisplayMode *out, uint32_t max_count);
bool display_atomic_commit(const DisplayAtomicRequest &request);
bool display_set_mode(const DisplayMode &mode);
int display_buffer_create(DisplayBufferCreate *request);
int display_buffer_map(DisplayBufferMap *request);
bool display_buffer_destroy(DisplayBufferHandle handle);
uint32_t display_present(const DisplayPresentRequest &request);
uint32_t display_compose_submit(const DisplayComposeRequest &request);
bool display_event_wait(DisplayEvent *out_event, bool block);
uint32_t display_wait(void);
uint32_t display_get_completed_sequence(void);
