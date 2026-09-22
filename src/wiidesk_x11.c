// SPDX-License-Identifier: GPL-2.0-only
/* Small non-compositing WiiDesk window manager for an existing X11 session. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/scrnsaver.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include "x11_preferences.h"
#include "x11_session.h"

#define CLIENTS 64
#define TITLE 24
#define EDGE 2
#define PANEL 26
#define BUTTON 22

struct client {
    Window window, frame;
    int x, y, width, height, old_border, hidden, maximized, ignore_unmap;
    int saved_x, saved_y, saved_width, saved_height;
    XSizeHints hints;
    char title[128];
};
static Display *display;
static Window root, panel, check, menu;
static int menu_open, menu_selected;
static char app_program[PATH_MAX];
static const char *const menu_labels[] = { "Terminal", "Files", "System", "Settings", "Editor", "Processes", "Session", "Log Viewer", "Calculator", "Network Settings", "Image Viewer", "Archive Manager", "Package Installer", "Audio Player" };
static const char *const app_arguments[] = { NULL, "files", "system", "settings", "editor", "processes", "session", "logs", "calculator", "network", "images", "archives", "packages", "audio" };
#define MENU_ITEMS ((int)(sizeof(menu_labels) / sizeof(menu_labels[0])))
static int screen, screen_width, screen_height, ownership_error;
static GC gc, outline_gc;
static XFontStruct *font;
static unsigned long background, foreground, muted, accent, border;
static struct client clients[CLIENTS], *active, *drag;
static int drag_resize, drag_x, drag_y, drag_width, drag_height, pointer_x, pointer_y;
static volatile sig_atomic_t stopping;
static Atom wm_protocols, wm_delete, wm_take_focus, wm_state, wm_change_state;
static Atom net_active, net_clients, net_supported, net_check, net_name, utf8;
static Atom net_state, net_hidden, net_max_h, net_max_v, net_close, net_extents;
static Atom net_workarea, net_desktops, net_current, net_wm_desktop, reload_settings;
static const char *terminal = "xterm";
static Atom session_action_atom, session_caps_atom, session_status_atom;
static int managed_session, logout_pending;
static Window logout_requester;
static pid_t locker_pid;
static const char *locker = "/usr/bin/xsecurelock";
static char session_status[160];
static unsigned int idle_lock_seconds;
static XScreenSaverInfo *idle_info;
static uint64_t idle_next, idle_retry_after;
static void close_menu(void);
static void cancel_drag(void);
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void stop_handler(int signal_number) { (void)signal_number; stopping = 1; }
static int xerror(Display *d, XErrorEvent *e)
{
    char message[128];
    if (e->error_code == BadAccess) ownership_error = 1;
    /* Clients may disappear between their event and our request. */
    if (e->error_code == BadWindow || e->error_code == BadDrawable || e->error_code == BadMatch)
        return 0;
    XGetErrorText(d, e->error_code, message, sizeof(message));
    fprintf(stderr, "wiidesk-x11: X error %s request=%u resource=0x%lx\n",
            message, e->request_code, e->resourceid);
    return 0;
}
static Atom atom(const char *name) { return XInternAtom(display, name, False); }
static void property(Window w, Atom name, Atom type, const unsigned long *data, int n)
{ XChangeProperty(display, w, name, type, 32, PropModeReplace, (const unsigned char *)data, n); }
static unsigned long color(const char *name)
{
    XColor value, exact;
    if (!XAllocNamedColor(display, DefaultColormap(display, screen), name, &value, &exact))
        return BlackPixel(display, screen);
    return value.pixel;
}
static struct client *find(Window w)
{
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].window && (clients[i].window == w || clients[i].frame == w)) return &clients[i];
    return NULL;
}
static int supports(struct client *c, Atom protocol)
{
    Atom *list = NULL;
    int count = 0, found = 0;
    if (XGetWMProtocols(display, c->window, &list, &count)) {
        for (int i = 0; i < count; i++) if (list[i] == protocol) found = 1;
        XFree(list);
    }
    return found;
}
static void protocol(struct client *c, Atom message, Time time)
{
    XEvent e = {0};
    e.xclient.type = ClientMessage; e.xclient.window = c->window;
    e.xclient.message_type = wm_protocols; e.xclient.format = 32;
    e.xclient.data.l[0] = message; e.xclient.data.l[1] = time;
    XSendEvent(display, c->window, False, NoEventMask, &e);
}
static void state(struct client *c)
{
    unsigned long values[3], classic[2] = { c->hidden ? IconicState : NormalState, None };
    int n = 0;
    if (c->hidden) values[n++] = net_hidden;
    if (c->maximized) { values[n++] = net_max_h; values[n++] = net_max_v; }
    property(c->window, wm_state, wm_state, classic, 2);
    property(c->window, net_state, XA_ATOM, values, n);
}
static void client_list(void)
{
    unsigned long list[CLIENTS]; int n = 0;
    for (int i = 0; i < CLIENTS; i++) if (clients[i].window) list[n++] = clients[i].window;
    property(root, net_clients, XA_WINDOW, list, n);
}
static void text(Window w, int x, int y, const char *s, unsigned long pixel, int limit)
{
    int n = (int)strlen(s); if (n > limit) n = limit;
    XSetForeground(display, gc, pixel); XDrawString(display, w, gc, x, y, s, n);
}
static void draw_frame(struct client *c)
{
    int width = c->width + EDGE * 2;
    XSetForeground(display, gc, c == active ? accent : border);
    XFillRectangle(display, c->frame, gc, 0, 0, width, TITLE);
    text(c->frame, 7, 16, c->title, foreground, (width - 3 * BUTTON - 14) / 8);
    text(c->frame, width - 3 * BUTTON + 6, 16, "_", foreground, 1);
    text(c->frame, width - 2 * BUTTON + 6, 16, c->maximized ? "o" : "+", foreground, 1);
    text(c->frame, width - BUTTON + 6, 16, "x", foreground, 1);
    XSetForeground(display, gc, muted);
    XDrawLine(display, c->frame, gc, width - 12, c->height + TITLE + EDGE - 1,
              width - 1, c->height + TITLE + EDGE - 12);
}
static void draw_panel(void)
{
    int n = 0;
    for (int i = 0; i < CLIENTS; i++) if (clients[i].window) n++;
    XClearWindow(display, panel);
    text(panel, 8, 17, "WiiDesk +", foreground, 10);
    int width = n ? (screen_width - 94) / n : 0;
    if (width > 150) width = 150;
    if (width < 1) return;
    n = 0;
    for (int i = 0; i < CLIENTS; i++) {
        struct client *c = &clients[i]; if (!c->window) continue;
        int x = 94 + n++ * width;
        XSetForeground(display, gc, c == active ? accent : border);
        XFillRectangle(display, panel, gc, x, 3, width > 2 ? width - 2 : 1, PANEL - 6);
        text(panel, x + 4, 17, c->title, c->hidden ? muted : foreground,
             width > 8 ? (width - 8) / 8 : 0);
    }
}
static void configure_notify(struct client *c)
{
    XEvent e = {0};
    e.xconfigure.type = ConfigureNotify; e.xconfigure.display = display;
    e.xconfigure.event = e.xconfigure.window = c->window;
    e.xconfigure.x = c->x + EDGE; e.xconfigure.y = c->y + TITLE;
    e.xconfigure.width = c->width; e.xconfigure.height = c->height;
    e.xconfigure.border_width = 0; e.xconfigure.above = None;
    XSendEvent(display, c->window, False, StructureNotifyMask, &e);
}
static int bounded(int v, int low, int high) { return v < low ? low : v > high ? high : v; }
static void geometry(struct client *c, int x, int y, int width, int height)
{
    XSizeHints *h = &c->hints;
    int maxw = screen_width - EDGE * 2, maxh = screen_height - PANEL - TITLE - EDGE;
    int minw = 80, minh = 32;
    if (h->flags & PMinSize) { if (h->min_width > minw) minw = h->min_width; if (h->min_height > minh) minh = h->min_height; }
    if (h->flags & PMaxSize) { if (h->max_width > 0 && h->max_width < maxw) maxw = h->max_width; if (h->max_height > 0 && h->max_height < maxh) maxh = h->max_height; }
    if (minw > maxw) minw = maxw;
    if (minh > maxh) minh = maxh;
    width = bounded(width, minw, maxw); height = bounded(height, minh, maxh);
    if (h->flags & PResizeInc) {
        int basew = (h->flags & PBaseSize) ? h->base_width : (h->flags & PMinSize) ? h->min_width : 0;
        int baseh = (h->flags & PBaseSize) ? h->base_height : (h->flags & PMinSize) ? h->min_height : 0;
        if (h->width_inc > 0 && width >= basew) width -= (width - basew) % h->width_inc;
        if (h->height_inc > 0 && height >= baseh) height -= (height - baseh) % h->height_inc;
    }
    c->width = bounded(width, 1, maxw); c->height = bounded(height, 1, maxh);
    c->x = bounded(x, 0, screen_width - c->width - EDGE * 2);
    c->y = bounded(y, 0, screen_height - PANEL - c->height - TITLE - EDGE);
    XMoveResizeWindow(display, c->frame, c->x, c->y, c->width + EDGE * 2, c->height + TITLE + EDGE);
    XMoveResizeWindow(display, c->window, EDGE, TITLE, c->width, c->height);
    configure_notify(c); draw_frame(c);
}
static void focus(struct client *c, Time time)
{
    struct client *previous = active;
    if (!c) { active = NULL; XSetInputFocus(display, root, RevertToPointerRoot, time); XDeleteProperty(display, root, net_active); }
    else {
        XWMHints *h;
        c->hidden = 0; state(c); XMapWindow(display, c->window); XMapRaised(display, c->frame);
        active = c;
        h = XGetWMHints(display, c->window);
        if (!h || !(h->flags & InputHint) || h->input) XSetInputFocus(display, c->window, RevertToPointerRoot, time);
        if (h) XFree(h);
        if (supports(c, wm_take_focus)) protocol(c, wm_take_focus, time);
        unsigned long value = c->window; property(root, net_active, XA_WINDOW, &value, 1);
        draw_frame(c);
    }
    if (previous && previous != c && previous->window) draw_frame(previous);
    XRaiseWindow(display, panel); draw_panel();
}
static void focus_next(Time time)
{
    int start = active ? (int)(active - clients) + 1 : 0;
    for (int j = 0; j < CLIENTS; j++) {
        struct client *c = &clients[(start + j) % CLIENTS];
        if (c->window && !c->hidden) { focus(c, time); return; }
    }
    focus(NULL, time);
}
static void minimize(struct client *c)
{
    if (!c) return;
    c->hidden = 1; state(c); XUnmapWindow(display, c->frame);
    if (active == c) focus_next(CurrentTime);
    draw_panel();
}
static void maximize(struct client *c, int enabled)
{
    if (!c || c->maximized == enabled) return;
    c->maximized = enabled;
    if (enabled) {
        c->saved_x = c->x; c->saved_y = c->y; c->saved_width = c->width; c->saved_height = c->height;
        geometry(c, 0, 0, screen_width, screen_height);
    } else geometry(c, c->saved_x, c->saved_y, c->saved_width, c->saved_height);
    state(c);
}
static void close_client(struct client *c, Time time)
{
    if (!c) return;
    if (supports(c, wm_delete)) protocol(c, wm_delete, time);
    else XKillClient(display, c->window);
}
static void session_publish(const char *status)
{
    unsigned long caps[] = {managed_session, !access(locker, X_OK), logout_pending, locker_pid > 0};
    property(root, session_caps_atom, XA_CARDINAL, caps, 4);
    if (status && strcmp(status, session_status)) {
        snprintf(session_status, sizeof(session_status), "%s", status);
        XChangeProperty(display, root, session_status_atom, utf8, 8, PropModeReplace,
                        (unsigned char *)session_status, strlen(session_status));
    }
}
static void session_lock(void)
{
    if (locker_pid > 0) return;
    close_menu(); cancel_drag(); XSync(display, False);
    idle_retry_after = monotonic_ms() + (uint64_t)idle_lock_seconds * 1000;
    if (access(locker, X_OK)) { session_publish("Lock unavailable: install XSecureLock"); return; }
    locker_pid = fork();
    if (!locker_pid) {
        close(ConnectionNumber(display));
        /* Authentication and screen/input protection belong to the packaged locker. */
        setenv("XSECURELOCK_SAVER", "saver_blank", 1);
        setenv("XSECURELOCK_AUTH_BACKGROUND_COLOR", "#244b59", 1);
        setenv("XSECURELOCK_AUTH_FOREGROUND_COLOR", "#eef2f3", 1);
        setenv("XSECURELOCK_FONT", "fixed", 1);
        setenv("XSECURELOCK_AUTH_CURSOR_BLINK", "0", 1);
        setenv("XSECURELOCK_PAM_SERVICE", "wiidesk-lock", 1);
        setenv("XSECURELOCK_SHOW_KEYBOARD_LAYOUT", "1", 1);
        execl(locker, locker, (char *)NULL); _exit(127);
    }
    if (locker_pid < 0) { locker_pid = 0; session_publish("Lock could not start"); if (managed_session) stopping = 1; }
    else session_publish("Lock requested; enter your password to unlock");
}
static void session_request(int action, Window requester)
{
    if (action == SESSION_LOCK) { session_lock(); return; }
    if (!managed_session) { session_publish("Log out is available after signing in through WiiDesk"); return; }
    if (locker_pid > 0) return;
    if (action == SESSION_CANCEL_LOGOUT) { logout_pending = 0; session_publish("Logout cancelled"); }
    else if (action == SESSION_FORCE_LOGOUT && logout_pending && requester == logout_requester) stopping = 1;
    else if (action == SESSION_LOGOUT && !logout_pending) {
        logout_pending = 1; logout_requester = requester;
        session_publish("Close or save your other applications to finish logout");
        for (int i = 0; i < CLIENTS; i++) {
            struct client *c = &clients[i];
            if (c->window && c->window != requester && supports(c, wm_delete)) protocol(c, wm_delete, CurrentTime);
        }
    }
}
static void session_tick(void)
{
    int status; pid_t child;
    while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
        if (child != locker_pid) continue;
        locker_pid = 0;
        idle_retry_after = monotonic_ms() + (uint64_t)idle_lock_seconds * 1000;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) session_publish("Unlocked");
        else {
            session_publish(managed_session ? "Lock failed; ending session" : "Lock failed; session is NOT locked");
            /* XDM resets the authenticated session if the locker fails. */
            if (managed_session) stopping = 1;
        }
    }
    if (!logout_pending) return;
    if (!find(logout_requester)) { logout_pending = 0; session_publish("Logout cancelled"); return; }
    int remaining = 0;
    for (int i = 0; i < CLIENTS; i++) if (clients[i].window && clients[i].window != logout_requester) remaining++;
    if (!remaining) stopping = 1;
}
static void idle_tick(void)
{
    uint64_t now = monotonic_ms();
    if (!managed_session || !idle_info || !idle_lock_seconds || locker_pid || stopping || now < idle_next) return;
    idle_next = now + 1000;
    if (!XScreenSaverQueryInfo(display, root, idle_info)) {
        session_publish("Automatic lock unavailable: cannot read X11 idle time");
        XFree(idle_info); idle_info = NULL; return;
    }
    if (now >= idle_retry_after && idle_info->idle >= (unsigned long)idle_lock_seconds * 1000)
        session_lock();
}
static void update_title(struct client *c)
{
    char *name = NULL;
    if (XFetchName(display, c->window, &name) && name) {
        snprintf(c->title, sizeof(c->title), "%s", name); XFree(name);
    } else snprintf(c->title, sizeof(c->title), "Application");
    for (char *p = c->title; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
}
static void manage(Window w)
{
    XWindowAttributes a; struct client *c = find(w); long supplied;
    if (c) { focus(c, CurrentTime); return; }
    if (!XGetWindowAttributes(display, w, &a) || a.override_redirect || a.class == InputOnly) return;
    for (int i = 0; i < CLIENTS; i++) if (!clients[i].window) { c = &clients[i]; break; }
    if (!c) { fprintf(stderr, "wiidesk-x11: window limit reached\n"); return; }
    memset(c, 0, sizeof(*c)); c->window = w; c->old_border = a.border_width;
    c->ignore_unmap = a.map_state != IsUnmapped;
    update_title(c); XGetWMNormalHints(display, w, &c->hints, &supplied);
    c->frame = XCreateSimpleWindow(display, root, a.x, a.y, a.width + EDGE * 2,
                                  a.height + TITLE + EDGE, 0, border, border);
    XSelectInput(display, c->frame, ExposureMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask | SubstructureRedirectMask | SubstructureNotifyMask);
    XSelectInput(display, w, PropertyChangeMask);
    XAddToSaveSet(display, w); XSetWindowBorderWidth(display, w, 0);
    XReparentWindow(display, w, c->frame, EDGE, TITLE);
    XGrabButton(display, Button1, AnyModifier, w, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    unsigned long extents[] = { EDGE, EDGE, TITLE, EDGE }, desktop = 0;
    property(w, net_extents, XA_CARDINAL, extents, 4);
    property(w, net_wm_desktop, XA_CARDINAL, &desktop, 1);
    geometry(c, a.x, a.y, a.width, a.height);
    XWMHints *h = XGetWMHints(display, w);
    int iconic = h && (h->flags & StateHint) && h->initial_state == IconicState;
    if (h) XFree(h);
    client_list();
    if (iconic) { XMapWindow(display, w); minimize(c); } else focus(c, CurrentTime);
    fprintf(stdout, "wiidesk-x11: managed 0x%lx %s\n", w, c->title); fflush(stdout);
}
static void outline(void)
{
    XDrawRectangle(display, root, outline_gc, drag_x, drag_y,
                   drag_width + EDGE * 2 - 1, drag_height + TITLE + EDGE - 1);
}
static void cancel_drag(void)
{
    if (!drag) return;
    outline(); XUngrabPointer(display, CurrentTime); drag = NULL;
}
static void unmanage(struct client *c, int destroyed)
{
    if (drag == c) cancel_drag();
    Window frame = c->frame, w = c->window;
    if (!destroyed) {
        XUngrabButton(display, AnyButton, AnyModifier, w);
        XSelectInput(display, w, NoEventMask);
        XReparentWindow(display, w, root, c->x + EDGE, c->y + TITLE);
        XRemoveFromSaveSet(display, w); XSetWindowBorderWidth(display, w, c->old_border);
        XDeleteProperty(display, w, wm_state); XDeleteProperty(display, w, net_state);
        XDeleteProperty(display, w, net_extents);
    }
    c->window = None; XDestroyWindow(display, frame);
    if (active == c) { active = NULL; focus_next(CurrentTime); }
    client_list(); draw_panel();
}
static void launch_app(int index)
{
    pid_t child = fork();
    if (child == 0) {
        close(ConnectionNumber(display)); setsid();
        if (!index) execlp(terminal, terminal, (char *)NULL);
        else execl(app_program, app_program, app_arguments[index], (char *)NULL);
        perror("wiidesk-x11: launch"); _exit(127);
    }
    if (child < 0) perror("fork application");
}
static void draw_menu(void)
{
    XClearWindow(display, menu);
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (i == menu_selected) {
            XSetForeground(display, gc, accent);
            XFillRectangle(display, menu, gc, 4, 4 + i * 30, 172, 28);
        }
        text(menu, 14, 23 + i * 30, menu_labels[i], foreground, 20);
    }
}
static void close_menu(void)
{
    if (!menu_open) return;
    XUngrabKeyboard(display, CurrentTime); XUngrabPointer(display, CurrentTime);
    XUnmapWindow(display, menu); menu_open = 0;
}
static void open_menu(Time time)
{
    if (menu_open) { close_menu(); return; }
    XMapRaised(display, menu);
    if (XGrabPointer(display, menu, False, ButtonPressMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, None, time) != GrabSuccess) {
        XUnmapWindow(display, menu); return;
    }
    if (XGrabKeyboard(display, menu, False, GrabModeAsync, GrabModeAsync, time) != GrabSuccess) {
        XUngrabPointer(display, time); XUnmapWindow(display, menu); return;
    }
    menu_open = 1; menu_selected = 0; draw_menu();
}
static void apply_preferences(void)
{
    struct preferences p; preferences_load(&p);
    if (idle_lock_seconds != p.idle_lock_seconds)
        idle_retry_after = monotonic_ms() + (uint64_t)p.idle_lock_seconds * 1000;
    idle_lock_seconds = p.idle_lock_seconds;
    unsigned long old[] = { background, accent };
    background = color(background_colors[p.background]); accent = color(accent_colors[p.accent]);
    XSetWindowBackground(display, root, background); XClearWindow(display, root);
    for (int i = 0; i < CLIENTS; i++) if (clients[i].window) draw_frame(&clients[i]);
    draw_panel(); if (menu_open) draw_menu();
    XFreeColors(display, DefaultColormap(display, screen), old, 2, 0);
}
static void button(XButtonEvent *e)
{
    if (menu_open) {
        int selected = (e->y - 4) / 30;
        int inside = e->x >= 4 && e->x < 176 && e->y >= 4 && e->y < 4 + MENU_ITEMS * 30;
        close_menu();
        if (inside && e->button == Button1) launch_app(selected);
        return;
    }
    if (e->window == panel) {
        if (e->button != Button1) return;
        if (e->x < 94) { open_menu(e->time); return; }
        int n = 0;
        for (int i = 0; i < CLIENTS; i++) if (clients[i].window) n++;
        int width = n ? (screen_width - 94) / n : 0;
        if (width > 150) width = 150;
        if (width < 1) return;
        int index = (e->x - 94) / width;
        for (int i = 0; i < CLIENTS; i++) if (clients[i].window && index-- == 0) {
            if (active == &clients[i] && !clients[i].hidden) minimize(&clients[i]);
            else focus(&clients[i], e->time);
            return;
        }
        return;
    }
    struct client *c = find(e->window);
    if (!c) return;
    focus(c, e->time);
    if (e->window == c->window) { XAllowEvents(display, ReplayPointer, e->time); return; }
    if (e->button != Button1) return;
    int width = c->width + EDGE * 2;
    if (e->y < TITLE && e->x >= width - 3 * BUTTON) {
        int action = (e->x - (width - 3 * BUTTON)) / BUTTON;
        if (action == 0) minimize(c);
        else if (action == 1) maximize(c, !c->maximized);
        else close_client(c, e->time);
        return;
    }
    if (e->y >= TITLE && !(e->x >= width - 16 && e->y >= c->height + TITLE - 16)) return;
    if (XGrabPointer(display, root, False, PointerMotionMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, e->time) != GrabSuccess) return;
    drag = c; drag_resize = e->y >= TITLE;
    pointer_x = e->x_root; pointer_y = e->y_root;
    drag_x = c->x; drag_y = c->y; drag_width = c->width; drag_height = c->height;
    outline();
}
static void key(XKeyEvent *e)
{
    KeySym sym = XLookupKeysym(e, 0);
    if ((e->state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask) && sym == XK_l) {
        session_lock(); return;
    }
    if (menu_open) {
        if (sym == XK_Escape) close_menu();
        else if (sym == XK_Up || sym == XK_Down) {
            menu_selected = (menu_selected + (sym == XK_Up ? MENU_ITEMS - 1 : 1)) % MENU_ITEMS; draw_menu();
        } else if (sym == XK_Return || sym == XK_space) {
            int selected = menu_selected; close_menu(); launch_app(selected);
        }
        return;
    }
    if (sym == XK_F1 && (e->state & Mod1Mask)) { open_menu(e->time); return; }
    if ((e->state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask) && sym == XK_Return) launch_app(0);
    else if (e->state & Mod1Mask) {
        if (sym == XK_Tab) focus_next(e->time);
        else if (sym == XK_F4) close_client(active, e->time);
        else if (sym == XK_F9) minimize(active);
        else if (sym == XK_F10 && active) maximize(active, !active->maximized);
    }
}
static void configure_request(XConfigureRequestEvent *e)
{
    struct client *c = find(e->window);
    if (!c) {
        XWindowChanges changes = { e->x, e->y, e->width, e->height, e->border_width, e->above, e->detail };
        XConfigureWindow(display, e->window, e->value_mask, &changes); return;
    }
    if (!c->maximized) geometry(c,
        (e->value_mask & CWX) ? e->x - EDGE : c->x,
        (e->value_mask & CWY) ? e->y - TITLE : c->y,
        (e->value_mask & CWWidth) ? e->width : c->width,
        (e->value_mask & CWHeight) ? e->height : c->height);
    else configure_notify(c);
    if ((e->value_mask & CWStackMode) && e->detail == Above) XRaiseWindow(display, c->frame);
    XRaiseWindow(display, panel);
}
static void message(XClientMessageEvent *e)
{
    if (e->message_type == session_action_atom && e->format == 32 && find(e->window)) {
        session_request(e->data.l[0], e->window); return;
    }
    if (e->message_type == reload_settings && e->window == root && e->format == 32) {
        apply_preferences(); return;
    }
    struct client *c = find(e->window); if (!c || e->format != 32) return;
    if (e->message_type == net_active) focus(c, e->data.l[1]);
    else if (e->message_type == net_close) close_client(c, e->data.l[0]);
    else if (e->message_type == wm_change_state && e->data.l[0] == IconicState) minimize(c);
    else if (e->message_type == net_state &&
             ((Atom)e->data.l[1] == net_max_h || (Atom)e->data.l[1] == net_max_v ||
              (Atom)e->data.l[2] == net_max_h || (Atom)e->data.l[2] == net_max_v)) {
        if (e->data.l[0] >= 0 && e->data.l[0] <= 2)
            maximize(c, e->data.l[0] == 2 ? !c->maximized : e->data.l[0] == 1);
    }
}
static void event(XEvent *e)
{
    struct client *c = find(e->xany.window);
    switch (e->type) {
    case MapRequest: manage(e->xmaprequest.window); break;
    case ConfigureRequest: configure_request(&e->xconfigurerequest); break;
    case DestroyNotify:
        c = find(e->xdestroywindow.window);
        if (c && c->window == e->xdestroywindow.window) unmanage(c, 1);
        break;
    case UnmapNotify:
        c = find(e->xunmap.window);
        if (c && c->window == e->xunmap.window) {
            if (c->ignore_unmap) c->ignore_unmap--;
            else unmanage(c, 0);
        }
        break;
    case PropertyNotify:
        if (c && (e->xproperty.atom == XA_WM_NAME || e->xproperty.atom == net_name)) { update_title(c); draw_frame(c); draw_panel(); }
        if (c && e->xproperty.atom == XA_WM_NORMAL_HINTS) { long supplied; memset(&c->hints, 0, sizeof(c->hints)); XGetWMNormalHints(display, c->window, &c->hints, &supplied); }
        break;
    case Expose: if (!e->xexpose.count) { if (e->xexpose.window == panel) draw_panel(); else if (e->xexpose.window == menu) draw_menu(); else if (c) draw_frame(c); } break;
    case ButtonPress: button(&e->xbutton); break;
    case KeyPress: key(&e->xkey); break;
    case ClientMessage: message(&e->xclient); break;
    case MotionNotify:
        if (menu_open && e->xmotion.x >= 4 && e->xmotion.x < 176 && e->xmotion.y >= 4 && e->xmotion.y < 4 + MENU_ITEMS * 30) {
            int selected = (e->xmotion.y - 4) / 30;
            if (selected != menu_selected) { menu_selected = selected; draw_menu(); }
        }
        if (drag) {
            outline();
            int dx = e->xmotion.x_root - pointer_x, dy = e->xmotion.y_root - pointer_y;
            if (drag_resize) { drag_width = bounded(drag->width + dx, 80, screen_width - EDGE * 2); drag_height = bounded(drag->height + dy, 32, screen_height - PANEL - TITLE - EDGE); }
            else { drag_x = bounded(drag->x + dx, 0, screen_width - drag_width - EDGE * 2); drag_y = bounded(drag->y + dy, 0, screen_height - PANEL - drag_height - TITLE - EDGE); }
            outline();
        }
        break;
    case ButtonRelease:
        if (drag) { struct client *moved = drag; cancel_drag(); moved->maximized = 0; geometry(moved, drag_x, drag_y, drag_width, drag_height); state(moved); }
        break;
    default: break;
    }
}
static void setup_atoms(void)
{
    session_action_atom = atom(SESSION_ACTION); session_caps_atom = atom(SESSION_CAPS); session_status_atom = atom(SESSION_STATUS);
    reload_settings = atom(SETTINGS_MESSAGE);
    wm_protocols = atom("WM_PROTOCOLS"); wm_delete = atom("WM_DELETE_WINDOW"); wm_take_focus = atom("WM_TAKE_FOCUS");
    wm_state = atom("WM_STATE"); wm_change_state = atom("WM_CHANGE_STATE");
    net_active = atom("_NET_ACTIVE_WINDOW"); net_clients = atom("_NET_CLIENT_LIST");
    net_supported = atom("_NET_SUPPORTED"); net_check = atom("_NET_SUPPORTING_WM_CHECK");
    net_name = atom("_NET_WM_NAME"); utf8 = atom("UTF8_STRING");
    net_state = atom("_NET_WM_STATE"); net_hidden = atom("_NET_WM_STATE_HIDDEN");
    net_max_h = atom("_NET_WM_STATE_MAXIMIZED_HORZ"); net_max_v = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    net_close = atom("_NET_CLOSE_WINDOW"); net_extents = atom("_NET_FRAME_EXTENTS");
    net_workarea = atom("_NET_WORKAREA"); net_desktops = atom("_NET_NUMBER_OF_DESKTOPS");
    net_current = atom("_NET_CURRENT_DESKTOP"); net_wm_desktop = atom("_NET_WM_DESKTOP");
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--terminal")) terminal = argv[2];
    else if (argc != 1) { fprintf(stderr, "Usage: %s [--terminal PROGRAM]\n", argv[0]); return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 1; }
    ssize_t executable_length = readlink("/proc/self/exe", app_program, sizeof(app_program) - 1);
    if (executable_length < 0 || executable_length >= (ssize_t)sizeof(app_program) - 1) {
        fprintf(stderr, "wiidesk-x11: cannot locate companion applications\n"); return 1;
    }
    app_program[executable_length] = 0;
    char *name = strrchr(app_program, '/');
    if (!name || (size_t)(name + 1 - app_program) + strlen("wiidesk-x11-app") >= sizeof(app_program)) return 1;
    strcpy(name + 1, "wiidesk-x11-app");
    display = XOpenDisplay(NULL);
    if (!display) { fprintf(stderr, "wiidesk-x11: cannot open DISPLAY\n"); return 1; }
    screen = DefaultScreen(display); root = RootWindow(display, screen);
    screen_width = DisplayWidth(display, screen); screen_height = DisplayHeight(display, screen);
    if (screen_width < 320 || screen_height < 240) { fprintf(stderr, "wiidesk-x11: display too small\n"); XCloseDisplay(display); return 1; }
    XSetErrorHandler(xerror);
    XSelectInput(display, root, SubstructureRedirectMask | SubstructureNotifyMask | KeyPressMask);
    XSync(display, False);
    if (ownership_error) { fprintf(stderr, "wiidesk-x11: another window manager is running\n"); XCloseDisplay(display); return 1; }
    setup_atoms();
    managed_session = getenv("WIIDESK_MANAGED_SESSION") && !strcmp(getenv("WIIDESK_MANAGED_SESSION"), "1");
    if (getenv("WIIDESK_LOCKER") && getenv("WIIDESK_LOCKER")[0] == '/') locker = getenv("WIIDESK_LOCKER");
    session_publish(managed_session ? "Signed in" : "Manual X11 session");
    struct preferences p; preferences_load(&p);
    idle_lock_seconds = p.idle_lock_seconds;
    if (managed_session) {
        int event_base, error_base;
        if (XScreenSaverQueryExtension(display, &event_base, &error_base)) idle_info = XScreenSaverAllocInfo();
        if (!idle_info) session_publish("Automatic lock unavailable: XScreenSaver extension missing");
        idle_retry_after = monotonic_ms() + (uint64_t)idle_lock_seconds * 1000;
    }
    background = color(background_colors[p.background]); foreground = color("#f0f4f5"); muted = color("#aabdc3");
    accent = color(accent_colors[p.accent]); border = color("#263137");
    XSetWindowBackground(display, root, background); XClearWindow(display, root);
    Cursor cursor = XCreateFontCursor(display, XC_left_ptr); XDefineCursor(display, root, cursor); XFreeCursor(display, cursor);
    gc = XCreateGC(display, root, 0, NULL); font = XLoadQueryFont(display, "8x13");
    if (!font) font = XLoadQueryFont(display, "fixed");
    if (font) XSetFont(display, gc, font->fid);
    XGCValues values = { .function = GXxor, .foreground = WhitePixel(display, screen) ^ BlackPixel(display, screen), .subwindow_mode = IncludeInferiors };
    outline_gc = XCreateGC(display, root, GCFunction | GCForeground | GCSubwindowMode, &values);
    XSetWindowAttributes a = { .override_redirect = True, .background_pixel = border, .event_mask = ExposureMask | ButtonPressMask };
    panel = XCreateWindow(display, root, 0, screen_height - PANEL, screen_width, PANEL,
                          0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    XStoreName(display, panel, "WiiDesk Panel"); XMapRaised(display, panel);
    a.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | KeyPressMask;
    int menu_y = screen_height - PANEL - (MENU_ITEMS * 30 + 8);
    if (menu_y < 0) menu_y = 0;
    menu = XCreateWindow(display, root, 4, menu_y, 180, MENU_ITEMS * 30 + 4,
                         0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    XStoreName(display, menu, "WiiDesk Launcher");
    check = XCreateSimpleWindow(display, root, -1, -1, 1, 1, 0, 0, 0);
    unsigned long self = check, one = 1, zero = 0, area[] = {0, 0, screen_width, screen_height - PANEL};
    property(root, net_check, XA_WINDOW, &self, 1); property(check, net_check, XA_WINDOW, &self, 1);
    XChangeProperty(display, check, net_name, utf8, 8, PropModeReplace, (const unsigned char *)"WiiDesk", 7);
    unsigned long supported[] = {net_active, net_clients, net_check, net_name, net_state, net_hidden,
        net_max_h, net_max_v, net_close, net_extents, net_workarea, net_desktops, net_current, net_wm_desktop};
    property(root, net_supported, XA_ATOM, supported, sizeof(supported) / sizeof(supported[0]));
    property(root, net_desktops, XA_CARDINAL, &one, 1); property(root, net_current, XA_CARDINAL, &zero, 1);
    property(root, net_workarea, XA_CARDINAL, area, 4); client_list();
    const KeySym keys[] = { XK_Tab, XK_F1, XK_F4, XK_F9, XK_F10, XK_Return, XK_l };
    for (unsigned int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        unsigned int modifiers = Mod1Mask | (keys[i] == XK_Return || keys[i] == XK_l ? ControlMask : 0);
        for (unsigned int j = 0; j < 4; j++) XGrabKey(display, XKeysymToKeycode(display, keys[i]),
            modifiers | ((j & 1) ? LockMask : 0) | ((j & 2) ? Mod2Mask : 0), root, True, GrabModeAsync, GrabModeAsync);
    }
    Window root_return, parent, *children = NULL; unsigned int count;
    if (XQueryTree(display, root, &root_return, &parent, &children, &count)) {
        for (unsigned int i = 0; i < count; i++) {
            XWindowAttributes attr;
            if (children[i] != panel && children[i] != check && XGetWindowAttributes(display, children[i], &attr) && attr.map_state == IsViewable) manage(children[i]);
        }
        if (children) XFree(children);
    }
    draw_panel(); XSync(display, False);
    signal(SIGTERM, stop_handler); signal(SIGINT, stop_handler);
    puts("wiidesk-x11: ready; Alt+Tab focus, Alt+F4 close, Alt+F9 minimize, Alt+F10 maximize, Ctrl+Alt+Return terminal"); fflush(stdout);
    while (!stopping) {
        while (XPending(display) && !stopping) { XEvent e; XNextEvent(display, &e); event(&e); }
        session_tick();
        idle_tick();
        XFlush(display);
        struct pollfd p = { .fd = ConnectionNumber(display), .events = POLLIN };
        if (poll(&p, 1, 200) < 0 && errno != EINTR) break;
        if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    }
    close_menu(); cancel_drag();
    if (idle_info) XFree(idle_info);
    for (int i = 0; i < CLIENTS; i++) if (clients[i].window) {
        Window w = clients[i].window; unmanage(&clients[i], 0); XMapWindow(display, w);
    }
    XUngrabKey(display, AnyKey, AnyModifier, root);
    XDeleteProperty(display, root, net_check); XDeleteProperty(display, root, net_supported);
    XDeleteProperty(display, root, net_clients); XDeleteProperty(display, root, net_active);
    XDeleteProperty(display, root, net_workarea); XDeleteProperty(display, root, net_desktops); XDeleteProperty(display, root, net_current);
    XDeleteProperty(display, root, session_caps_atom); XDeleteProperty(display, root, session_status_atom);
    XDestroyWindow(display, panel); XDestroyWindow(display, menu); XDestroyWindow(display, check);
    if (font) XFreeFont(display, font);
    XFreeGC(display, gc); XFreeGC(display, outline_gc); XCloseDisplay(display);
    return 0;
}
