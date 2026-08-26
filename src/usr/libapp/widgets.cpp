#include "widgets.h"

#include <string.h>

bool widget_contains(Rect rect, int x, int y)
{
    return rect.w > 0 && rect.h > 0 && x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

int widget_hit_rects(const Rect *rects, int count, int x, int y)
{
    if (!rects)
        return -1;
    for (int i = 0; i < count; i++) {
        if (widget_contains(rects[i], x, y))
            return i;
    }
    return -1;
}

static bool event_left_down(const Event *ev)
{
    return ev->type == EVT_MOUSE_DOWN && ev->mouse.button == 1;
}

static bool event_left_up(const Event *ev)
{
    return ev->type == EVT_MOUSE_UP && ev->mouse.button == 1;
}

static bool event_clears_hover(const Event *ev)
{
    return ev->type == EVT_UNFOCUS || ev->type == EVT_MOUSE_LEAVE;
}

// --- Button -----------------------------------------------------------------

void widget_button_reset(WidgetButton *b)
{
    if (!b)
        return;
    b->hovered = false;
    b->pressed = false;
}

int widget_button_event(WidgetButton *b, const Event *ev)
{
    if (!b || !ev || gui_rect_is_empty(b->rect))
        return WIDGET_NONE;

    if (event_clears_hover(ev)) {
        if (!b->hovered && !b->pressed)
            return WIDGET_NONE;
        widget_button_reset(b);
        return WIDGET_CHANGED;
    }

    if (ev->type == EVT_MOUSE_MOVE) {
        bool over = widget_contains(b->rect, ev->mouse.x, ev->mouse.y);
        int changed = WIDGET_NONE;
        if (over != b->hovered) {
            b->hovered = over;
            changed = WIDGET_CHANGED;
        }
        // Moving off a pressed button cancels the press visually.
        if (b->pressed && !over) {
            b->pressed = false;
            changed = WIDGET_CHANGED;
        }
        return changed;
    }

    if (event_left_down(ev) && widget_contains(b->rect, ev->mouse.x, ev->mouse.y)) {
        b->pressed = true;
        return b->fire_on_down ? (WIDGET_CHANGED | WIDGET_CLICKED) : WIDGET_CHANGED;
    }

    if (event_left_up(ev) && b->pressed) {
        b->pressed = false;
        int result = WIDGET_CHANGED;
        if (widget_contains(b->rect, ev->mouse.x, ev->mouse.y))
            result |= WIDGET_CLICKED;
        return result;
    }

    return WIDGET_NONE;
}

void widget_button_draw(Surface *s, const WidgetButton *b, const char *label, bool primary, bool armed)
{
    if (!s || !b || gui_rect_is_empty(b->rect))
        return;
    gui_app_draw_button_ex(s, b->rect.x, b->rect.y, b->rect.w, b->rect.h, label, primary, armed, b->hovered,
                           b->pressed);
}

// --- Toggle row ---------------------------------------------------------------

void widget_toggle_reset(WidgetToggle *t)
{
    if (t)
        t->hovered = false;
}

int widget_toggle_event(WidgetToggle *t, const Event *ev)
{
    if (!t || !ev || gui_rect_is_empty(t->rect))
        return WIDGET_NONE;

    if (event_clears_hover(ev)) {
        if (!t->hovered)
            return WIDGET_NONE;
        t->hovered = false;
        return WIDGET_CHANGED;
    }

    if (ev->type == EVT_MOUSE_MOVE) {
        bool over = widget_contains(t->rect, ev->mouse.x, ev->mouse.y);
        if (over == t->hovered)
            return WIDGET_NONE;
        t->hovered = over;
        return WIDGET_CHANGED;
    }

    if (event_left_down(ev) && widget_contains(t->rect, ev->mouse.x, ev->mouse.y))
        return WIDGET_CLICKED;

    return WIDGET_NONE;
}

void widget_toggle_draw(Surface *s, const WidgetToggle *t, const char *label, const char *detail, bool on)
{
    if (!s || !t || gui_rect_is_empty(t->rect))
        return;
    gui_app_draw_toggle_row(s, t->rect.x, t->rect.y, t->rect.w, t->rect.h, label, detail, on, false, t->hovered);
}

// --- Slider ------------------------------------------------------------------

void widget_slider_reset(WidgetSlider *slider)
{
    if (!slider)
        return;
    slider->hovered = false;
    slider->dragging = false;
}

int widget_slider_event(WidgetSlider *slider, const Event *ev, uint32_t max_value)
{
    if (!slider || !ev || gui_rect_is_empty(slider->rect))
        return WIDGET_NONE;

    if (event_clears_hover(ev)) {
        int changed = WIDGET_NONE;
        if (slider->hovered)
            changed = WIDGET_CHANGED;
        slider->hovered = false;
        slider->dragging = false;
        return changed;
    }

    if (slider->dragging) {
        if (ev->type == EVT_MOUSE_MOVE || event_left_down(ev)) {
            Rect track = gui_app_slider_track_rect(slider->rect.x, slider->rect.y, slider->rect.w, slider->rect.h);
            uint32_t next = gui_app_slider_value_from_x(ev->mouse.x, &track, max_value);
            if (next == slider->value)
                return WIDGET_NONE;
            slider->value = next;
            return WIDGET_CHANGED;
        }
        if (event_left_up(ev)) {
            slider->dragging = false;
            return WIDGET_CHANGED | WIDGET_CLICKED;
        }
        return WIDGET_NONE;
    }

    if (ev->type == EVT_MOUSE_MOVE) {
        bool over = widget_contains(slider->rect, ev->mouse.x, ev->mouse.y);
        if (over == slider->hovered)
            return WIDGET_NONE;
        slider->hovered = over;
        return WIDGET_CHANGED;
    }

    if (event_left_down(ev) && widget_contains(slider->rect, ev->mouse.x, ev->mouse.y)) {
        slider->dragging = true;
        Rect track = gui_app_slider_track_rect(slider->rect.x, slider->rect.y, slider->rect.w, slider->rect.h);
        slider->value = gui_app_slider_value_from_x(ev->mouse.x, &track, max_value);
        return WIDGET_CHANGED;
    }

    return WIDGET_NONE;
}

void widget_slider_draw(Surface *s, const WidgetSlider *slider, const char *label, uint32_t max_value)
{
    if (!s || !slider || gui_rect_is_empty(slider->rect))
        return;
    gui_app_draw_slider(s, slider->rect.x, slider->rect.y, slider->rect.w, slider->rect.h, label, slider->value,
                        max_value, slider->hovered || slider->dragging);
}

// --- Segmented control ----------------------------------------------------------

void widget_segment_reset(WidgetSegment *seg)
{
    if (seg)
        seg->hovered_index = -1;
}

static int widget_segment_index_at(const WidgetSegment *seg, int count, int x, int y)
{
    if (!seg || count <= 0 || !widget_contains(seg->rect, x, y))
        return -1;
    int index = (x - seg->rect.x) * count / seg->rect.w;
    if (index < 0)
        index = 0;
    if (index >= count)
        index = count - 1;
    return index;
}

int widget_segment_event(WidgetSegment *seg, const Event *ev, int count, int *out_index)
{
    if (out_index)
        *out_index = -1;
    if (!seg || !ev || count <= 0 || gui_rect_is_empty(seg->rect))
        return WIDGET_NONE;

    if (event_clears_hover(ev)) {
        if (seg->hovered_index < 0)
            return WIDGET_NONE;
        seg->hovered_index = -1;
        return WIDGET_CHANGED;
    }

    if (ev->type == EVT_MOUSE_MOVE) {
        int index = widget_segment_index_at(seg, count, ev->mouse.x, ev->mouse.y);
        if (index == seg->hovered_index)
            return WIDGET_NONE;
        seg->hovered_index = index;
        return WIDGET_CHANGED;
    }

    if (event_left_down(ev)) {
        int index = widget_segment_index_at(seg, count, ev->mouse.x, ev->mouse.y);
        if (index < 0)
            return WIDGET_NONE;
        if (out_index)
            *out_index = index;
        return WIDGET_CLICKED;
    }

    return WIDGET_NONE;
}

void widget_segment_draw(Surface *s, const WidgetSegment *seg, const char *const *labels, int count, int selected)
{
    if (!s || !seg || count <= 0 || gui_rect_is_empty(seg->rect))
        return;
    gui_app_draw_segmented_choice(s, seg->rect.x, seg->rect.y, seg->rect.w, seg->rect.h, labels, count, selected,
                                  seg->hovered_index);
}

// --- Text field -----------------------------------------------------------------

void widget_field_init(WidgetField *f, char *buf, size_t capacity)
{
    if (!f)
        return;
    f->text = buf;
    f->capacity = capacity;
    f->focused = false;
    f->hovered = false;
    if (buf && capacity > 0)
        buf[0] = '\0';
}

void widget_field_set(WidgetField *f, const char *value)
{
    if (!f || !f->text || f->capacity == 0)
        return;
    if (!value)
        value = "";
    size_t i = 0;
    for (; i + 1 < f->capacity && value[i]; i++)
        f->text[i] = value[i];
    f->text[i] = '\0';
}

size_t widget_field_len(const WidgetField *f)
{
    return (f && f->text) ? strlen(f->text) : 0;
}

int widget_field_event(WidgetField *f, Rect rect, const Event *ev)
{
    if (!f || !ev || !f->text || f->capacity == 0)
        return WIDGET_NONE;

    if (event_clears_hover(ev)) {
        int changed = WIDGET_NONE;
        if (f->hovered)
            changed = WIDGET_CHANGED;
        if (f->focused) {
            f->focused = false;
            changed |= WIDGET_CHANGED | WIDGET_FIELD_BLUR;
        }
        f->hovered = false;
        return changed;
    }

    if (ev->type == EVT_MOUSE_MOVE && !gui_rect_is_empty(rect)) {
        bool over = widget_contains(rect, ev->mouse.x, ev->mouse.y);
        if (over == f->hovered)
            return WIDGET_NONE;
        f->hovered = over;
        return WIDGET_CHANGED;
    }

    if (event_left_down(ev) && !gui_rect_is_empty(rect)) {
        if (widget_contains(rect, ev->mouse.x, ev->mouse.y)) {
            if (f->focused)
                return WIDGET_NONE;
            f->focused = true;
            return WIDGET_CHANGED;
        }
        if (f->focused) {
            f->focused = false;
            return WIDGET_CHANGED | WIDGET_FIELD_BLUR;
        }
        return WIDGET_NONE;
    }

    if (ev->type == EVT_KEY_DOWN && f->focused && ev->key.c != 0) {
        char c = ev->key.c;
        size_t len = strlen(f->text);
        if (c == '\n' || c == '\r')
            return WIDGET_FIELD_ENTER;
        if ((uint8_t)c == 27) {
            f->focused = false;
            return WIDGET_CHANGED | WIDGET_FIELD_BLUR;
        }
        if ((c == '\b' || c == 127)) {
            if (len == 0)
                return WIDGET_NONE;
            f->text[len - 1] = '\0';
            return WIDGET_CHANGED;
        }
        if (c >= 32 && c <= 126) {
            if (len + 1 >= f->capacity)
                return WIDGET_NONE;
            f->text[len] = c;
            f->text[len + 1] = '\0';
            return WIDGET_CHANGED;
        }
    }

    return WIDGET_NONE;
}

void widget_field_draw(Surface *s, const WidgetField *f, int x, int y, int w, int h)
{
    if (!s || !f)
        return;
    gui_app_draw_text_field(s, x, y, w, h, f->text ? f->text : "", f->focused, f->hovered);
}

// --- Popup menu --------------------------------------------------------------------

void widget_popup_reset(WidgetPopup *p)
{
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->hovered = -1;
}

void widget_popup_open(WidgetPopup *p, const GuiMenuItem *items, int count, int at_x, int at_y, int canvas_w,
                       int view_y, int view_h, int min_width)
{
    if (!p)
        return;
    widget_popup_reset(p);
    if (!items || count <= 0)
        return;

    p->items = items;
    p->count = count;
    p->w = gui_popup_menu_width(items, count, min_width);
    p->h = gui_popup_menu_height(items, count);
    p->x = at_x;
    p->y = at_y;
    if (p->x + p->w > canvas_w)
        p->x = canvas_w - p->w - gui_space_1();
    if (p->y + p->h > view_y + view_h)
        p->y = view_y + view_h - p->h - gui_space_1();
    if (p->x < 0)
        p->x = 0;
    if (p->y < view_y)
        p->y = view_y;
    p->hovered = gui_popup_menu_hit_test(items, count, p->x, p->y, p->w, at_x, at_y);
    p->open = true;
}

void widget_popup_close(WidgetPopup *p)
{
    if (p)
        p->open = false;
}

int widget_popup_event(WidgetPopup *p, const Event *ev)
{
    if (!p || !p->open || !ev)
        return WIDGET_POPUP_NONE;

    if (event_clears_hover(ev)) {
        if (p->hovered < 0)
            return WIDGET_POPUP_NONE;
        p->hovered = -1;
        return WIDGET_POPUP_HOVERED;
    }

    if (ev->type == EVT_MOUSE_MOVE) {
        int hovered = gui_popup_menu_hit_test(p->items, p->count, p->x, p->y, p->w, ev->mouse.x, ev->mouse.y);
        if (hovered == p->hovered)
            return WIDGET_POPUP_NONE;
        p->hovered = hovered;
        return WIDGET_POPUP_HOVERED;
    }

    if (event_left_down(ev)) {
        int index = gui_popup_menu_hit_test(p->items, p->count, p->x, p->y, p->w, ev->mouse.x, ev->mouse.y);
        p->open = false;
        if (index >= 0 && p->items[index].enabled)
            return index;
        return WIDGET_POPUP_DISMISSED;
    }

    return WIDGET_POPUP_NONE;
}

void widget_popup_draw(Surface *s, const WidgetPopup *p)
{
    if (!s || !p || !p->open || !p->items || p->count <= 0)
        return;
    gui_draw_popup_menu(s, p->x, p->y, p->w, p->items, p->count, p->hovered);
}

// --- Modal dialog ---------------------------------------------------------------------

void widget_dialog_open(WidgetDialog *d, bool has_field, bool has_cancel, const char *prefill)
{
    if (!d)
        return;
    memset(d, 0, sizeof(*d));
    d->open = true;
    d->has_field = has_field;
    d->has_cancel = has_cancel;
    widget_field_init(&d->field, d->input, sizeof(d->input));
    if (has_field)
        widget_field_set(&d->field, prefill);
}

void widget_dialog_close(WidgetDialog *d)
{
    if (d)
        d->open = false;
}

const char *widget_dialog_input(const WidgetDialog *d)
{
    return (d && d->has_field) ? d->input : "";
}

int widget_dialog_event(WidgetDialog *d, const Event *ev)
{
    if (!d || !d->open || !ev)
        return WIDGET_NONE;

    if (ev->type == EVT_KEY_DOWN && ev->key.c != 0) {
        char c = ev->key.c;
        if (c == '\n' || c == '\r')
            return WIDGET_DIALOG_CONFIRM;
        if ((uint8_t)c == 27)
            return d->has_cancel ? WIDGET_DIALOG_CANCEL : WIDGET_DIALOG_CONFIRM;
        if (d->has_field) {
            int rc = widget_field_event(&d->field, d->layout.field, ev);
            if (rc & WIDGET_FIELD_ENTER)
                return WIDGET_DIALOG_CONFIRM;
            if (rc & WIDGET_FIELD_BLUR)
                return WIDGET_DIALOG_CANCEL;
            return rc & WIDGET_CHANGED ? WIDGET_CHANGED : WIDGET_NONE;
        }
        return WIDGET_NONE;
    }

    if (ev->type == EVT_MOUSE_MOVE) {
        int hover = 0;
        if (widget_contains(d->layout.confirm, ev->mouse.x, ev->mouse.y))
            hover = 1;
        else if (d->has_cancel && widget_contains(d->layout.cancel, ev->mouse.x, ev->mouse.y))
            hover = 2;
        if (hover == d->hover)
            return WIDGET_NONE;
        d->hover = hover;
        return WIDGET_CHANGED;
    }

    if (event_left_down(ev)) {
        if (widget_contains(d->layout.confirm, ev->mouse.x, ev->mouse.y)) {
            d->pressed = 1;
            return WIDGET_CHANGED;
        }
        if (d->has_cancel && widget_contains(d->layout.cancel, ev->mouse.x, ev->mouse.y)) {
            d->pressed = 2;
            return WIDGET_CHANGED;
        }
        if (d->has_field && widget_contains(d->layout.field, ev->mouse.x, ev->mouse.y)) {
            d->field.focused = true;
            return WIDGET_CHANGED;
        }
        d->open = false;
        return WIDGET_DIALOG_DISMISS;
    }

    if (event_left_up(ev) && d->pressed != 0) {
        int pressed = d->pressed;
        d->pressed = 0;
        bool over_confirm = widget_contains(d->layout.confirm, ev->mouse.x, ev->mouse.y);
        bool over_cancel = d->has_cancel && widget_contains(d->layout.cancel, ev->mouse.x, ev->mouse.y);
        if (pressed == 1 && over_confirm)
            return WIDGET_CHANGED | WIDGET_DIALOG_CONFIRM;
        if (pressed == 2 && over_cancel)
            return WIDGET_CHANGED | WIDGET_DIALOG_CANCEL;
        return WIDGET_CHANGED;
    }

    return WIDGET_NONE;
}

void widget_dialog_draw(Surface *s, WidgetDialog *d, int view_w, int view_h, int scroll_y, const char *title,
                        const char *const *lines, int line_count, const char *confirm_label, const char *cancel_label)
{
    if (!s || !d || !d->open)
        return;

    d->layout = gui_dialog_layout(view_w, view_h, scroll_y, lines, line_count, d->has_field);
    gui_draw_dialog(s, view_w, view_h, scroll_y, &d->layout, title, lines, line_count,
                    d->has_field ? d->input : nullptr, confirm_label, d->hover == 1, d->pressed == 1,
                    d->has_cancel ? cancel_label : nullptr, d->hover == 2, d->pressed == 2);
}

// --- Help overlay ------------------------------------------------------------------------

bool widget_help_event(WidgetHelp *h, const Event *ev)
{
    if (!h || !h->open || !ev)
        return false;
    if (ev->type == EVT_KEY_DOWN && ((uint8_t)ev->key.c == 27 || ev->key.c == '\n' || ev->key.c == '\r')) {
        h->open = false;
        return true;
    }
    if (event_left_down(ev)) {
        h->open = false;
        return true;
    }
    return false;
}

void widget_help_draw(Surface *s, int view_w, int view_h, int scroll_y, const char *title, const char *const *lines,
                      int line_count)
{
    if (!s || line_count <= 0)
        return;
    GuiDialogLayout layout = gui_dialog_layout(view_w, view_h, scroll_y, lines, line_count, false);
    gui_draw_dialog(s, view_w, view_h, scroll_y, &layout, title, lines, line_count, nullptr, "Close", false, false,
                    nullptr, false, false);
}
