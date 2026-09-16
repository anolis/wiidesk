// SPDX-License-Identifier: GPL-2.0-only
#include "x11_controls.h"
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <string.h>

void ui_label(struct ui *u, int x, int y, const char *s, unsigned long pixel)
{
    char clean[512]; int n = 0, limit = (u->width - x - 8) / 8;
    while (s[n] && n < limit && n < (int)sizeof(clean)) {
        unsigned char c = s[n]; clean[n++] = c >= 32 && c <= 126 ? c : '?';
    }
    XSetForeground(u->display, u->gc, pixel);
    XDrawString(u->display, u->window, u->gc, x, y, clean, n);
}
void ui_fill(struct ui *u, int x, int y, int w, int h, unsigned long pixel)
{
    if (w <= 0 || h <= 0) return;
    XSetForeground(u->display, u->gc, pixel);
    XFillRectangle(u->display, u->window, u->gc, x, y, w, h);
}
void ui_button(struct ui *u, int x, int y, int w, const char *s, int selected)
{
    ui_fill(u, x, y, w, 24, selected ? u->accent : u->surface);
    XRectangle clip = {x, y, w, 24};
    XSetClipRectangles(u->display, u->gc, 0, 0, &clip, 1, Unsorted);
    ui_label(u, x + 6, y + 17, s, u->text);
    XSetClipMask(u->display, u->gc, None);
}
int ui_hit(int x, int y, int bx, int by, int bw, int bh)
{ return x >= bx && y >= by && x < bx + bw && y < by + bh; }
void ui_list_reveal(struct ui_list *l, int rows)
{
    if (rows < 1) rows = 1;
    if (l->selected >= l->count) l->selected = l->count - 1;
    if (l->selected < 0) l->selected = 0;
    if (l->scroll > l->selected) l->scroll = l->selected;
    if (l->selected >= l->scroll + rows) l->scroll = l->selected - rows + 1;
}
int ui_list_key(struct ui_list *l, KeySym key, int rows)
{
    switch (key) {
    case XK_Up: l->selected--; break;
    case XK_Down: l->selected++; break;
    case XK_Page_Up: l->selected -= rows; break;
    case XK_Page_Down: l->selected += rows; break;
    case XK_Home: l->selected = 0; break;
    case XK_End: l->selected = l->count - 1; break;
    default: return 0;
    }
    ui_list_reveal(l, rows); return 1;
}
int ui_text_init(struct ui_text *t, size_t capacity)
{
    memset(t, 0, sizeof(*t));
    t->data = calloc(capacity + 1, 1); t->undo = calloc(capacity + 1, 1);
    if (!t->data || !t->undo) { ui_text_free(t); return -1; }
    t->capacity = capacity; return 0;
}
void ui_text_free(struct ui_text *t)
{ free(t->data); free(t->undo); memset(t, 0, sizeof(*t)); }
static int valid_text(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c > 126) return 0;
    }
    return 1;
}
int ui_text_set(struct ui_text *t, const char *s, size_t n)
{
    if (n > t->capacity || !valid_text(s, n)) return -1;
    memmove(t->data, s, n); t->data[n] = 0;
    t->length = n; t->cursor = t->anchor = 0; t->has_undo = t->modified = 0; return 0;
}
static void snapshot(struct ui_text *t)
{
    memcpy(t->undo, t->data, t->length + 1);
    t->undo_length = t->length; t->undo_cursor = t->cursor; t->has_undo = 1;
}
int ui_text_insert(struct ui_text *t, const char *s, size_t n)
{
    size_t a = t->cursor < t->anchor ? t->cursor : t->anchor;
    size_t b = t->cursor > t->anchor ? t->cursor : t->anchor;
    if (n > t->capacity - (t->length - (b - a)) || !valid_text(s, n)) return -1;
    if (a == b && !n) return 0;
    snapshot(t);
    memmove(t->data + a + n, t->data + b, t->length - b + 1);
    if (n) memcpy(t->data + a, s, n);
    t->length += n; t->length -= b - a;
    t->cursor = t->anchor = a + n; t->modified = 1; return 0;
}
static size_t line_start(struct ui_text *t, size_t p)
{ while (p && t->data[p - 1] != '\n') p--; return p; }
static size_t line_end(struct ui_text *t, size_t p)
{ while (p < t->length && t->data[p] != '\n') p++; return p; }
int ui_text_key(struct ui_text *t, KeySym k, unsigned state, const char *bytes, int n, int multiline)
{
    int control = state & ControlMask, shift = state & ShiftMask;
    if (control && (k == XK_a || k == XK_A)) { t->anchor = 0; t->cursor = t->length; return 1; }
    if (control && (k == XK_z || k == XK_Z) && t->has_undo) {
        char *swap = t->data; t->data = t->undo; t->undo = swap;
        size_t length = t->length, cursor = t->cursor;
        t->length = t->undo_length; t->cursor = t->anchor = t->undo_cursor;
        t->undo_length = length; t->undo_cursor = cursor; t->modified = 1; return 1;
    }
    size_t p = t->cursor;
    if (k == XK_Left) { if (p) p--; }
    else if (k == XK_Right) { if (p < t->length) p++; }
    else if (k == XK_Home) p = control ? 0 : line_start(t, p);
    else if (k == XK_End) p = control ? t->length : line_end(t, p);
    else if ((k == XK_Up || k == XK_Down) && multiline) {
        size_t start = line_start(t, p), col = p - start;
        if (k == XK_Up && start) { size_t a = line_start(t, start - 1); p = a + col < start ? a + col : start - 1; }
        if (k == XK_Down) { size_t a = line_end(t, p); if (a < t->length) { a++; size_t end = line_end(t, a); p = a + col < end ? a + col : end; } }
    } else if (k == XK_BackSpace || k == XK_Delete) {
        if (t->anchor == t->cursor) {
            if (k == XK_BackSpace && t->anchor) t->anchor--;
            if (k == XK_Delete && t->anchor < t->length) t->anchor++;
        }
        return ui_text_insert(t, "", 0) ? -1 : 1;
    } else if (k == XK_Return && multiline) return ui_text_insert(t, "\n", 1) ? -1 : 1;
    else if (k == XK_Tab && multiline) return ui_text_insert(t, "\t", 1) ? -1 : 1;
    else if (!control && !(state & Mod1Mask) && n > 0 && (unsigned char)bytes[0] >= 32) return ui_text_insert(t, bytes, n) ? -1 : 1;
    else return 0;
    t->cursor = p; if (!shift) t->anchor = p; return 1;
}
static void position(struct ui_text *t, size_t end, int columns, int multiline, int *row, int *col)
{
    *row = *col = 0;
    for (size_t i = 0; i < end; i++) {
        if (multiline && t->data[i] == '\n') { (*row)++; *col = 0; }
        else if (++*col == columns && multiline) { (*row)++; *col = 0; }
    }
}
void ui_text_draw(struct ui *u, struct ui_text *t, int x, int y, int w, int h, int *top, int multiline)
{
    int cols = w / 8, rows = h / 16, cr, cc;
    if (cols < 1 || rows < 1) return;
    position(t, t->cursor, cols, multiline, &cr, &cc);
    if (multiline) {
        if (cr < *top) *top = cr;
        if (cr >= *top + rows) *top = cr - rows + 1;
    } else {
        if (cc < *top) *top = cc;
        if (cc >= *top + cols) *top = cc - cols + 1;
    }
    XRectangle clip = {x, y, w, h}; XSetClipRectangles(u->display, u->gc, 0, 0, &clip, 1, Unsorted);
    int row = 0, col = 0;
    size_t a = t->cursor < t->anchor ? t->cursor : t->anchor, b = t->cursor > t->anchor ? t->cursor : t->anchor;
    for (size_t i = 0; i <= t->length; i++) {
        int dx = x + (col - (multiline ? 0 : *top)) * 8;
        int dy = y + (row - (multiline ? *top : 0)) * 16;
        if (dy >= y && dy < y + h && dx >= x && dx < x + w) {
            if (i >= a && i < b) ui_fill(u, dx, dy, 8, 16, u->accent);
            if (i < t->length && t->data[i] >= 32) {
                XSetForeground(u->display, u->gc, u->text);
                XDrawString(u->display, u->window, u->gc, dx, dy + 12, t->data + i, 1);
            }
            if (i == t->cursor) ui_fill(u, dx, dy + 14, 8, 2, u->text);
        }
        if (multiline && t->data[i] == '\n') { row++; col = 0; }
        else if (++col == cols && multiline) { row++; col = 0; }
        if (multiline && row >= *top + rows) break;
    }
    XSetClipMask(u->display, u->gc, None);
}
void ui_text_click(struct ui_text *t, int x, int y, int width, int top, int multiline, int extend)
{
    int cols = width / 8, row = 0, col = 0, target_row = multiline ? y / 16 + top : 0;
    int target_col = x / 8 + (multiline ? 0 : top);
    if (cols < 1 || x < 0 || y < 0) return;
    size_t p;
    for (p = 0; p < t->length; p++) {
        if (row == target_row && (col >= target_col || (multiline && t->data[p] == '\n'))) break;
        if (multiline && t->data[p] == '\n') { row++; col = 0; }
        else if (++col == cols && multiline) { row++; col = 0; }
        if (row > target_row) break;
    }
    t->cursor = p; if (!extend) t->anchor = p;
}
void ui_clipboard_init(struct ui *u)
{
    u->clipboard = XInternAtom(u->display, "CLIPBOARD", False);
    u->utf8 = XInternAtom(u->display, "UTF8_STRING", False);
    u->targets = XInternAtom(u->display, "TARGETS", False);
    u->transfer = XInternAtom(u->display, "_WIIDESK_PASTE", False);
}
int ui_clipboard_key(struct ui *u, struct ui_text *t, KeySym k, unsigned state, Time time)
{
    if (!(state & ControlMask)) return 0;
    if (k == XK_c || k == XK_C || k == XK_x || k == XK_X) {
        size_t a = t->cursor < t->anchor ? t->cursor : t->anchor, b = t->cursor > t->anchor ? t->cursor : t->anchor;
        if (a == b) return 1;
        char *copy = malloc(b - a + 1); if (!copy) return -1;
        memcpy(copy, t->data + a, b - a); copy[b - a] = 0;
        free(u->selection); u->selection = copy; u->selection_length = b - a;
        XSetSelectionOwner(u->display, u->clipboard, u->window, time);
        if (k == XK_x || k == XK_X) ui_text_insert(t, "", 0);
        return 1;
    }
    if (k == XK_v || k == XK_V) {
        if (u->paste_target) return -1;
        u->paste_target = t;
        XConvertSelection(u->display, u->clipboard, u->utf8, u->transfer, u->window, time);
        return 1;
    }
    return 0;
}
int ui_clipboard_event(struct ui *u, XEvent *e)
{
    if (e->type == SelectionClear) { free(u->selection); u->selection = NULL; return 1; }
    if (e->type == SelectionRequest) {
        XSelectionRequestEvent *r = &e->xselectionrequest;
        XEvent reply = {0}; reply.xselection.type = SelectionNotify;
        reply.xselection.display = r->display; reply.xselection.requestor = r->requestor;
        reply.xselection.selection = r->selection; reply.xselection.target = r->target;
        reply.xselection.time = r->time; reply.xselection.property = None;
        Atom prop = r->property ? r->property : r->target;
        if (r->selection == u->clipboard && u->selection) {
            if (r->target == u->targets) {
                Atom values[] = {u->targets, u->utf8, XA_STRING};
                XChangeProperty(u->display, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)values, 3);
                reply.xselection.property = prop;
            } else if ((r->target == u->utf8 || r->target == XA_STRING) && u->selection_length <= 65536) {
                XChangeProperty(u->display, r->requestor, prop, r->target, 8, PropModeReplace, (unsigned char *)u->selection, u->selection_length);
                reply.xselection.property = prop;
            }
        }
        XSendEvent(u->display, r->requestor, False, 0, &reply); return 1;
    }
    if (e->type == SelectionNotify && u->paste_target && e->xselection.selection == u->clipboard) {
        struct ui_text *t = u->paste_target; u->paste_target = NULL;
        if (e->xselection.property == None) return -1;
        Atom type; int format; unsigned long count, left; unsigned char *data = NULL;
        int ok = XGetWindowProperty(u->display, u->window, u->transfer, 0, 16385, True, AnyPropertyType,
                                   &type, &format, &count, &left, &data) == Success &&
                 (type == u->utf8 || type == XA_STRING) && format == 8 && !left && count <= 65536;
        if (ok) ok = !ui_text_insert(t, (char *)data, count);
        if (data) XFree(data);
        XDeleteProperty(u->display, u->window, u->transfer);
        return ok ? 1 : -1;
    }
    return 0;
}
void ui_clipboard_cancel(struct ui *u) { u->paste_target = NULL; }
void ui_clipboard_free(struct ui *u) { free(u->selection); u->selection = NULL; ui_clipboard_cancel(u); }
