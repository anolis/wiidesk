// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Display *d;
static Window root;
static void pause_events(void) { struct timespec t = {0, 20000000}; XSync(d, False); nanosleep(&t, NULL); }
static Atom a(const char *name) { return XInternAtom(d, name, False); }
static unsigned long value(Window w, const char *name)
{
    Atom type; int format; unsigned long count, remaining; unsigned char *data = NULL;
    unsigned long result = 0;
    if (XGetWindowProperty(d, w, a(name), 0, 1, False, AnyPropertyType,
        &type, &format, &count, &remaining, &data) == Success && count && format == 32)
        result = *(unsigned long *)data;
    if (data) XFree(data);
    return result;
}
static int count_clients(void)
{
    Atom type; int format; unsigned long count = 0, remaining; unsigned char *data = NULL;
    XGetWindowProperty(d, root, a("_NET_CLIENT_LIST"), 0, 128, False, XA_WINDOW,
                      &type, &format, &count, &remaining, &data);
    if (data) XFree(data);
    return (int)count;
}
static void require(int condition, const char *what)
{ if (!condition) { fprintf(stderr, "FAIL: %s\n", what); exit(1); } }
static void wait_count(int expected)
{
    for (int i = 0; i < 250 && count_clients() != expected; i++) pause_events();
    require(count_clients() == expected, "client-list lifecycle");
}
static void wait_value(Window w, const char *name, unsigned long expected)
{
    for (int i = 0; i < 250 && value(w, name) != expected; i++) pause_events();
    require(value(w, name) == expected, name);
}
static Window parent_of(Window w)
{
    Window r, parent, *children; unsigned int count;
    XQueryTree(d, w, &r, &parent, &children, &count);
    if (children) XFree(children);
    return parent;
}
static Window create(const char *name)
{
    Window w = XCreateSimpleWindow(d, root, 30, 30, 300, 150, 1, 0, 0xffffff);
    XSizeHints h = { .flags = PMinSize | PBaseSize | PResizeInc,
        .min_width = 80, .min_height = 40, .base_width = 0, .base_height = 0,
        .width_inc = 10, .height_inc = 5 };
    Atom protocols[] = {a("WM_DELETE_WINDOW")};
    XSetWMNormalHints(d, w, &h); XSetWMProtocols(d, w, protocols, 1);
    XStoreName(d, w, name); XSelectInput(d, w, StructureNotifyMask);
    XMapWindow(d, w); XFlush(d); return w;
}
static void message(Window w, const char *name, long v0, long v1, long v2)
{
    XEvent e = {0}; e.xclient.type = ClientMessage; e.xclient.window = w;
    e.xclient.message_type = a(name); e.xclient.format = 32;
    e.xclient.data.l[0] = v0; e.xclient.data.l[1] = v1; e.xclient.data.l[2] = v2;
    XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e); XFlush(d);
}
int main(void)
{
    d = XOpenDisplay(NULL); require(d != NULL, "open test display");
    root = DefaultRootWindow(d);
    require(value(root, "_NET_SUPPORTING_WM_CHECK") != 0, "WM advertised");
    int baseline = count_clients();
    Window first = create("WiiDesk test A"); wait_count(baseline + 1);
    wait_value(first, "WM_STATE", NormalState); wait_value(root, "_NET_ACTIVE_WINDOW", first);
    require(parent_of(first) != root, "client reparented into decoration");
    XMoveResizeWindow(d, first, 102, 104, 333, 177); XFlush(d);
    XWindowAttributes attr;
    for (int i = 0; i < 250; i++) { pause_events(); XGetWindowAttributes(d, first, &attr); if (attr.width == 330 && attr.height == 175) break; }
    require(attr.width == 330 && attr.height == 175, "resize increments honored");
    int x, y; Window child;
    XTranslateCoordinates(d, first, root, 0, 0, &x, &y, &child);
    require(x == 102 && y == 104, "root-relative client configure position");
    Window second = create("WiiDesk test B"); wait_count(baseline + 2);
    wait_value(root, "_NET_ACTIVE_WINDOW", second);
    message(first, "_NET_ACTIVE_WINDOW", 1, CurrentTime, 0);
    wait_value(root, "_NET_ACTIVE_WINDOW", first);
    XIconifyWindow(d, first, DefaultScreen(d)); XFlush(d);
    wait_value(first, "WM_STATE", IconicState);
    wait_value(root, "_NET_ACTIVE_WINDOW", second);
    XGetWindowAttributes(d, parent_of(first), &attr); require(attr.map_state == IsUnmapped, "minimized frame hidden");
    message(first, "_NET_ACTIVE_WINDOW", 1, CurrentTime, 0);
    wait_value(first, "WM_STATE", NormalState);
    message(first, "_NET_WM_STATE", 1, a("_NET_WM_STATE_MAXIMIZED_HORZ"), a("_NET_WM_STATE_MAXIMIZED_VERT"));
    for (int i = 0; i < 250; i++) { pause_events(); XGetWindowAttributes(d, first, &attr); if (attr.width > 500) break; }
    require(attr.width > 500 && attr.height > 350, "maximize");
    message(first, "_NET_WM_STATE", 0, a("_NET_WM_STATE_MAXIMIZED_HORZ"), a("_NET_WM_STATE_MAXIMIZED_VERT"));
    for (int i = 0; i < 250; i++) { pause_events(); XGetWindowAttributes(d, first, &attr); if (attr.width == 330) break; }
    require(attr.width == 330 && attr.height == 175, "maximize restore geometry");
    XUnmapWindow(d, second); XFlush(d); wait_count(baseline + 1);
    require(parent_of(second) == root, "withdraw reparents safely");
    XMapWindow(d, second); XFlush(d); wait_count(baseline + 2);
    message(first, "_NET_CLOSE_WINDOW", CurrentTime, 1, 0);
    XEvent event; int deleted = 0;
    for (int i = 0; i < 250 && !deleted; i++) {
        pause_events();
        while (XCheckTypedWindowEvent(d, first, ClientMessage, &event))
            if ((Atom)event.xclient.data.l[0] == a("WM_DELETE_WINDOW")) deleted = 1;
    }
    require(deleted, "polite close protocol");
    XDestroyWindow(d, first); XDestroyWindow(d, second); XFlush(d); wait_count(baseline);
    for (int i = 0; i < 20; i++) {
        Window w = create("lifecycle"); wait_count(baseline + 1);
        if (i % 2) { XIconifyWindow(d, w, DefaultScreen(d)); XFlush(d); wait_value(w, "WM_STATE", IconicState); }
        XDestroyWindow(d, w); XFlush(d); wait_count(baseline);
    }
    puts("PASS: manage, focus, configure, size hints, minimize, restore, maximize, withdraw/remap, close, 20 lifecycles");
    XCloseDisplay(d); return 0;
}
