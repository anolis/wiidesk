/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_X11_CONTROLS_H
#define WIIDESK_X11_CONTROLS_H
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stddef.h>

/* Bounded controls; applications own data and decide when to redraw. */
struct ui {
    Display *display;
    Window window;
    GC gc;
    int width, height;
    unsigned long text, muted, surface, accent, background;
    Atom clipboard, utf8, targets, transfer;
    char *selection;
    size_t selection_length;
    struct ui_text *paste_target;
    Time paste_time;
};
struct ui_list { int count, selected, scroll; };
struct ui_text {
    char *data, *undo;
    size_t capacity, length, cursor, anchor, undo_length, undo_cursor;
    int has_undo, modified, typing;
};
void ui_label(struct ui *, int x, int y, const char *, unsigned long);
void ui_fill(struct ui *, int x, int y, int w, int h, unsigned long);
void ui_button(struct ui *, int x, int y, int w, const char *, int selected);
int ui_hit(int x, int y, int bx, int by, int bw, int bh);
int ui_list_key(struct ui_list *, KeySym, int rows);
void ui_list_reveal(struct ui_list *, int rows);
int ui_text_init(struct ui_text *, size_t capacity);
void ui_text_free(struct ui_text *);
int ui_text_set(struct ui_text *, const char *, size_t length);
int ui_text_insert(struct ui_text *, const char *, size_t length);
int ui_text_key(struct ui_text *, KeySym, unsigned int state, const char *, int length, int multiline);
void ui_text_draw(struct ui *, struct ui_text *, int x, int y, int w, int h, int *top, int multiline);
void ui_text_click(struct ui_text *, int x, int y, int width, int top, int multiline, int extend);
void ui_clipboard_init(struct ui *);
/* Return -1 on rejected paste, 1 when consumed, 0 for unrelated input. */
int ui_clipboard_key(struct ui *, struct ui_text *, KeySym, unsigned state, Time);
int ui_clipboard_event(struct ui *, XEvent *);
void ui_clipboard_cancel(struct ui *);
void ui_clipboard_free(struct ui *);
#endif
