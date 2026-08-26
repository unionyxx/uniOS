#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/event.h>

#include "../libgui/gui.h"

#ifdef __cplusplus
extern "C" {
#endif

// libapp widgets: stateful controls on top of libgui's immediate-mode
// drawing. Each widget owns its interaction state (hover/press/focus/value);
// feed events to the *_event functions and draw with the *_draw functions.
// *_event return values are WIDGET_* bitmasks; WIDGET_CHANGED means the
// visual state changed and the widget rect should be invalidated.

enum
{
    WIDGET_NONE = 0,
    WIDGET_CHANGED = 1 << 0,
    WIDGET_CLICKED = 1 << 1,
    WIDGET_FIELD_BLUR = 1 << 2,    // text field lost focus
    WIDGET_FIELD_ENTER = 1 << 3,   // Enter pressed in a focused text field
    WIDGET_DIALOG_CONFIRM = 1 << 4,
    WIDGET_DIALOG_CANCEL = 1 << 5,
    WIDGET_DIALOG_DISMISS = 1 << 6, // click outside the dialog panel
};

bool widget_contains(Rect rect, int x, int y);
// First rect containing the point, or -1.
int widget_hit_rects(const Rect *rects, int count, int x, int y);

// --- Button ----------------------------------------------------------------
// Press/release tracking with release-to-apply: the click fires only when the
// pointer is released over the button, so dragging away cancels it. Set
// fire_on_down for controls that act immediately (with a pressed flash).

typedef struct WidgetButton
{
    Rect rect;
    bool hovered;
    bool pressed;
    bool fire_on_down;
} WidgetButton;

void widget_button_reset(WidgetButton *b);
int widget_button_event(WidgetButton *b, const Event *ev);
void widget_button_draw(Surface *s, const WidgetButton *b, const char *label, bool primary, bool armed);

// --- Toggle row --------------------------------------------------------------
// Settings row (label + optional detail + switch). Fires WIDGET_CLICKED on
// pointer-down inside the row; the app flips its own value.

typedef struct WidgetToggle
{
    Rect rect;
    bool hovered;
} WidgetToggle;

void widget_toggle_reset(WidgetToggle *t);
int widget_toggle_event(WidgetToggle *t, const Event *ev);
void widget_toggle_draw(Surface *s, const WidgetToggle *t, const char *label, const char *detail, bool on);

// --- Slider ------------------------------------------------------------------
// Label + percent value + draggable track. The current value lives in the
// widget; WIDGET_CHANGED fires on hover/value changes, WIDGET_CLICKED when a
// drag ends (persist the value then).

typedef struct WidgetSlider
{
    Rect rect;
    uint32_t value;
    bool hovered;
    bool dragging;
} WidgetSlider;

void widget_slider_reset(WidgetSlider *s);
int widget_slider_event(WidgetSlider *s, const Event *ev, uint32_t max_value);
void widget_slider_draw(Surface *s, const WidgetSlider *slider, const char *label, uint32_t max_value);

// --- Segmented control ---------------------------------------------------------

typedef struct WidgetSegment
{
    Rect rect;
    int hovered_index; // -1 when not hovered
} WidgetSegment;

void widget_segment_reset(WidgetSegment *seg);
// WIDGET_CHANGED on hover changes; WIDGET_CLICKED on a click, with the
// clicked segment stored in *out_index (when non-NULL).
int widget_segment_event(WidgetSegment *seg, const Event *ev, int count, int *out_index);
void widget_segment_draw(Surface *s, const WidgetSegment *seg, const char *const *labels, int count, int selected);

// --- Text field -----------------------------------------------------------------
// Edits a caller-owned NUL-terminated buffer. Enter reports
// WIDGET_FIELD_ENTER (focus kept), Escape blurs and reports WIDGET_FIELD_BLUR;
// clicking outside a focused field also reports WIDGET_FIELD_BLUR after
// unfocusing, and the app may still dispatch that click elsewhere.

typedef struct WidgetField
{
    char *text;
    size_t capacity; // includes the NUL byte
    bool focused;
    bool hovered;
} WidgetField;

void widget_field_init(WidgetField *f, char *buf, size_t capacity);
void widget_field_set(WidgetField *f, const char *value);
size_t widget_field_len(const WidgetField *f);
int widget_field_event(WidgetField *f, Rect rect, const Event *ev);
void widget_field_draw(Surface *s, const WidgetField *f, int x, int y, int w, int h);

// --- Popup menu --------------------------------------------------------------------

enum
{
    WIDGET_POPUP_NONE = -1,
    WIDGET_POPUP_DISMISSED = -2,
    WIDGET_POPUP_HOVERED = -3,
};

typedef struct WidgetPopup
{
    bool open;
    int x, y, w, h;
    int hovered;
    int count;
    const GuiMenuItem *items;
} WidgetPopup;

void widget_popup_reset(WidgetPopup *p);
// Opens at (at_x, at_y), clamped to the canvas width and the visible
// viewport (view_y .. view_y + view_h) so it never opens below the fold.
void widget_popup_open(WidgetPopup *p, const GuiMenuItem *items, int count, int at_x, int at_y, int canvas_w,
                       int view_y, int view_h, int min_width);
void widget_popup_close(WidgetPopup *p);
// >= 0: selected item index (popup closed). WIDGET_POPUP_* otherwise.
int widget_popup_event(WidgetPopup *p, const Event *ev);
void widget_popup_draw(Surface *s, const WidgetPopup *p);

// --- Modal dialog ---------------------------------------------------------------------
// Scrim + panel + optional text field + footer buttons, laid out by
// gui_dialog_layout. Feed events first, draw every frame while open.

typedef struct WidgetDialog
{
    bool open;
    bool has_field;
    bool has_cancel;
    int pressed; // 1 = confirm held, 2 = cancel held
    int hover;   // 0 none, 1 confirm, 2 cancel
    char input[256];
    GuiDialogLayout layout; // valid after the most recent draw
    WidgetField field;      // bound to input while open
} WidgetDialog;

void widget_dialog_open(WidgetDialog *d, bool has_field, bool has_cancel, const char *prefill);
void widget_dialog_close(WidgetDialog *d);
const char *widget_dialog_input(const WidgetDialog *d);
// Returns WIDGET_CHANGED possibly OR'd with one of WIDGET_DIALOG_CONFIRM /
// CANCEL / DISMISS. Enter confirms, Escape cancels (or confirms when there is
// no cancel button). Typing edits the field when has_field is set.
int widget_dialog_event(WidgetDialog *d, const Event *ev);
void widget_dialog_draw(Surface *s, WidgetDialog *d, int view_w, int view_h, int scroll_y, const char *title,
                        const char *const *lines, int line_count, const char *confirm_label, const char *cancel_label);

// --- Help overlay ------------------------------------------------------------------------
// Minimal modal: closes on Escape/Enter or a left click anywhere.

typedef struct WidgetHelp
{
    bool open;
} WidgetHelp;

// Returns true when the event closed the overlay (invalidate the window).
bool widget_help_event(WidgetHelp *h, const Event *ev);
void widget_help_draw(Surface *s, int view_w, int view_h, int scroll_y, const char *title, const char *const *lines,
                      int line_count);

#ifdef __cplusplus
}
#endif
