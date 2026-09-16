/* SPDX-License-Identifier: GPL-2.0-only */
/* A bounded display-manager health probe, not an authentication check. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
static int ignore_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
int main(void)
{
    alarm(5);
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    XSetErrorHandler(ignore_error);
    Window root = DefaultRootWindow(d), parent, returned, *children = NULL;
    Atom type; int format, ready = 0; unsigned long n, after; unsigned char *data = NULL;
    Atom caps = XInternAtom(d, "_WIIDESK_SESSION_CAPS", True);
    if (caps && XGetWindowProperty(d, root, caps, 0, 4, False, XA_CARDINAL,
            &type, &format, &n, &after, &data) == Success && type == XA_CARDINAL && format == 32 && n == 4)
        ready = ((unsigned long *)data)[0] == 1;
    if (data) XFree(data);
    unsigned int count;
    if (!ready && XQueryTree(d, root, &returned, &parent, &children, &count)) {
        for (unsigned int i = 0; i < count && !ready; i++) {
            XClassHint hint = {0}; XWindowAttributes attr;
            if (XGetClassHint(d, children[i], &hint)) {
                if (hint.res_class && !strcmp(hint.res_class, "Xlogin") &&
                    XGetWindowAttributes(d, children[i], &attr) && attr.map_state == IsViewable) ready = 1;
                if (hint.res_name) XFree(hint.res_name);
                if (hint.res_class) XFree(hint.res_class);
            }
        }
        if (children) XFree(children);
    }
    XCloseDisplay(d);
    return ready ? 0 : 1;
}
