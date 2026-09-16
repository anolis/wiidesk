// SPDX-License-Identifier: GPL-2.0-only
/* Lightweight ordinary X11 clients for the WiiDesk desktop. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include "x11_preferences.h"
#include "x11_controls.h"

#define ENTRIES 512
#define ROW 22
static Display *display;
static Window window;
static GC gc;
static XFontStruct *font;
static Atom wm_delete, wm_protocols;
static int width = 460, height = 330, visible = 1, dirty = 1;
static volatile sig_atomic_t stopping;
static unsigned long foreground, muted, surface, accent;
static enum { FILES, SYSTEM, SETTINGS, EDITOR, PROCESSES } app;
static struct ui shared_ui;
static struct ui_text editor_text;
static struct ui_list process_list;
static char editor_path[PATH_MAX];
static char process_lines[128][96];
static int process_count, editor_top;
static struct preferences prefs;
static int setting_row;
static char status[160];
static struct entry { char name[NAME_MAX + 1]; int directory; off_t size; } entries[ENTRIES];
static int entry_count, selected, scroll, last_click = -1;
static Time last_click_time;
static char directory[PATH_MAX];
static char system_lines[7][128];
static uint64_t previous_total, previous_idle;

static void stop_handler(int s) { (void)s; stopping = 1; }
static uint64_t now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static unsigned long color(const char *name)
{
    XColor c, exact;
    if (!XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)), name, &c, &exact))
        return BlackPixel(display, DefaultScreen(display));
    return c.pixel;
}
static void text(int x, int y, const char *s, unsigned long pixel)
{
    char clean[512];
    int limit = (width - x - 8) / 8, n = 0;
    while (s[n] && n < limit && n < (int)sizeof(clean)) {
        unsigned char c = s[n]; clean[n++] = c >= 32 && c <= 126 ? c : '?';
    }
    XSetForeground(display, gc, pixel);
    XDrawString(display, window, gc, x, y, clean, n);
}
static void rectangle(int x, int y, int w, int h, unsigned long pixel)
{ XSetForeground(display, gc, pixel); XFillRectangle(display, window, gc, x, y, w, h); }
static int page_rows(void) { int rows = (height - 104) / ROW; return rows > 0 ? rows : 1; }
static void reveal_selection(void)
{
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + page_rows()) scroll = selected - page_rows() + 1;
}
static int entry_compare(const void *left, const void *right)
{
    const struct entry *a = left, *b = right;
    if (a->directory != b->directory) return b->directory - a->directory;
    return strcmp(a->name, b->name);
}
static void load_directory(const char *path)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) { snprintf(status, sizeof(status), "Open failed: %s", strerror(errno)); return; }
    DIR *d = opendir(resolved);
    if (!d) { snprintf(status, sizeof(status), "Open failed: %s", strerror(errno)); return; }
    entry_count = 0;
    int truncated = 0, read_error = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { read_error = errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (entry_count == ENTRIES) { truncated = 1; break; }
        struct entry *item = &entries[entry_count++];
        snprintf(item->name, sizeof(item->name), "%s", e->d_name);
        struct stat st;
        item->directory = 0; item->size = -1;
        if (!fstatat(dirfd(d), e->d_name, &st, 0)) {
            item->directory = S_ISDIR(st.st_mode); item->size = st.st_size;
        }
    }
    closedir(d);
    qsort(entries, entry_count, sizeof(entries[0]), entry_compare);
    snprintf(directory, sizeof(directory), "%s", resolved);
    selected = scroll = 0; last_click = -1;
    if (read_error) snprintf(status, sizeof(status), "Read incomplete: %s", strerror(read_error));
    else snprintf(status, sizeof(status), "%d%s entries | Enter opens folders", entry_count, truncated ? "+ (limit)" : "");
    char title[160]; snprintf(title, sizeof(title), "WiiDesk Files - %.130s", directory);
    XStoreName(display, window, title);
}
static void parent_directory(void)
{
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s", directory);
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) strcpy(path, "/"); else *slash = 0;
    load_directory(path);
}
static void open_selected(void)
{
    if (!entry_count) return;
    struct entry *e = &entries[selected];
    if (!e->directory) {
        if (e->size < 0) snprintf(status, sizeof(status), "Metadata unavailable: %.110s", e->name);
        else snprintf(status, sizeof(status), "%lld bytes | %.110s", (long long)e->size, e->name);
        return;
    }
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", directory, e->name) >= (int)sizeof(path))
        snprintf(status, sizeof(status), "Path too long");
    else load_directory(path);
}
static void system_refresh(void)
{
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long u = 0, n = 0, k = 0, idle = 0, wait = 0, irq = 0, soft = 0, steal = 0;
    strcpy(system_lines[0], "CPU         Unavailable");
    if (f) {
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &k, &idle, &wait, &irq, &soft, &steal) >= 4) {
            uint64_t total = u + n + k + idle + wait + irq + soft + steal, free_cpu = idle + wait;
            if (previous_total && total > previous_total && free_cpu >= previous_idle) {
                uint64_t dt = total - previous_total, di = free_cpu - previous_idle;
                snprintf(system_lines[0], sizeof(system_lines[0]), "CPU         %llu%%", (unsigned long long)((dt - (di < dt ? di : dt)) * 100 / dt));
            } else strcpy(system_lines[0], "CPU         Sampling...");
            previous_total = total; previous_idle = free_cpu;
        }
        fclose(f);
    }
    unsigned long total = 0, available = 0, swap = 0, swap_free = 0, v;
    char line[256];
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %lu", &v) == 1) total = v;
            if (sscanf(line, "MemAvailable: %lu", &v) == 1) available = v;
            if (sscanf(line, "SwapTotal: %lu", &v) == 1) swap = v;
            if (sscanf(line, "SwapFree: %lu", &v) == 1) swap_free = v;
        }
        fclose(f);
    }
    if (total) snprintf(system_lines[1], sizeof(system_lines[1]), "Memory      %lu / %lu MiB used", (total > available ? total - available : 0) / 1024, total / 1024);
    else strcpy(system_lines[1], "Memory      Unavailable");
    snprintf(system_lines[2], sizeof(system_lines[2]), "Swap        %lu / %lu MiB used", (swap > swap_free ? swap - swap_free : 0) / 1024, swap / 1024);
    double uptime;
    f = fopen("/proc/uptime", "r");
    strcpy(system_lines[3], "Uptime      Unavailable");
    if (f) {
        if (fscanf(f, "%lf", &uptime) == 1) snprintf(system_lines[3], sizeof(system_lines[3]), "Uptime      %luh %02lum", (unsigned long)uptime / 3600, (unsigned long)uptime / 60 % 60);
        fclose(f);
    }
    unsigned long long rx = 0, tx = 0, r, t;
    f = fopen("/proc/net/dev", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char name[32];
            if (sscanf(line, " %31[^:]: %llu %*s %*s %*s %*s %*s %*s %*s %llu", name, &r, &t) == 3 && strcmp(name, "lo")) { rx += r; tx += t; }
        }
        fclose(f);
        snprintf(system_lines[4], sizeof(system_lines[4]), "Network     RX %llu / TX %llu MiB", rx >> 20, tx >> 20);
    } else strcpy(system_lines[4], "Network     Unavailable");
    snprintf(system_lines[5], sizeof(system_lines[5]), "Graphics    GX %s | DRM %s", access("/sys/module/gcn_gx", F_OK) ? "absent" : "loaded", access("/sys/class/drm/card0", F_OK) ? "absent" : "present");
    snprintf(system_lines[6], sizeof(system_lines[6]), "Session     X11 | user ID %lu", (unsigned long)getuid());
}
static void process_refresh(void)
{
    DIR *d = opendir("/proc"); process_count = 0;
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && process_count < 128) {
        char path[64], name[64];
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        snprintf(path, sizeof(path), "/proc/%.48s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f || fscanf(f, "%*s (%63[^)])", name) != 1) { if (f) fclose(f); continue; }
        fclose(f); snprintf(process_lines[process_count], sizeof(process_lines[0]), "%.15s  %.78s", e->d_name, name); process_count++;
    }
    closedir(d); process_list.count = process_count; ui_list_reveal(&process_list, 10);
}
static void editor_load(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) { snprintf(status, sizeof(status), "Open failed: %s", strerror(errno)); return; }
    char *data = malloc(262145); if (!data) { fclose(f); return; }
    size_t n = fread(data, 1, 262144, f); int bad = ferror(f); fclose(f);
    if (bad || ui_text_set(&editor_text, data, n)) snprintf(status, sizeof(status), "File too large or binary");
    else { snprintf(editor_path, sizeof(editor_path), "%s", path); snprintf(status, sizeof(status), "Loaded %zu bytes", n); }
    free(data);
}
static void editor_save(void)
{
    if (!editor_path[0]) { snprintf(status, sizeof(status), "Set WIIDESK_EDITOR_FILE first"); return; }
    char temp[PATH_MAX]; if (strlen(editor_path) + 7 >= sizeof(temp)) { snprintf(status, sizeof(status), "Path too long"); return; } snprintf(temp, sizeof(temp), "%s.XXXXXX", editor_path); int fd = mkstemp(temp);
    if (fd < 0) { snprintf(status, sizeof(status), "Save failed: %s", strerror(errno)); return; }
    ssize_t n = write(fd, editor_text.data, editor_text.length); int bad = n != (ssize_t)editor_text.length || fsync(fd) || close(fd) || rename(temp, editor_path);
    if (bad) { unlink(temp); snprintf(status, sizeof(status), "Save failed: %s", strerror(errno)); } else { editor_text.modified = 0; snprintf(status, sizeof(status), "Saved %zu bytes", editor_text.length); }
}
static void draw(void)
{
    XClearWindow(display, window);
    if (app == FILES) {
        rectangle(8, 8, width - 16, 24, surface); text(16, 25, "Up       Home       Refresh", foreground);
        text(12, 53, directory, muted);
        reveal_selection();
        for (int row = 0; row < page_rows() && scroll + row < entry_count; row++) {
            int index = scroll + row, y = 64 + row * ROW;
            if (index == selected) rectangle(8, y, width - 16, ROW, accent);
            char label[NAME_MAX + 8]; snprintf(label, sizeof(label), "%s %s", entries[index].directory ? "[+]" : "   ", entries[index].name);
            text(12, y + 16, label, foreground);
        }
        if (!entry_count) text(12, 82, "This folder is empty", muted);
        text(12, height - 13, status, muted);
    } else if (app == SYSTEM) {
        text(16, 28, "System monitor", foreground);
        for (int i = 0; i < 7; i++) text(16, 60 + i * 30, system_lines[i], i % 2 ? muted : foreground);
        text(16, height - 14, "Updates once per second while visible", muted);
    } else if (app == SETTINGS) {
        text(16, 28, "Desktop appearance", foreground);
        for (int i = 0; i < 2; i++) {
            rectangle(12, 48 + i * 38, width - 24, 30, setting_row == i ? accent : surface);
            char label[128];
            snprintf(label, sizeof(label), "%s:  < %s >", i ? "Accent    " : "Background", i ? accent_names[prefs.accent] : background_names[prefs.background]);
            text(22, 68 + i * 38, label, foreground);
        }
        rectangle(12, 140, 140, 28, setting_row == 2 ? accent : surface); text(24, 159, "Save and apply", foreground);
        text(16, 198, "Arrows change colors; Tab selects Save", muted);
        text(16, 220, "Enter saves; Escape closes", muted);
        text(16, height - 16, status, muted);
    } else if (app == EDITOR) {
        text(16, 28, editor_path[0] ? editor_path : "WiiDesk Editor", foreground);
        shared_ui.window = window; shared_ui.width = width; shared_ui.height = height;
        ui_text_draw(&shared_ui, &editor_text, 12, 46, width - 24, height - 74, &editor_top, 1);
        text(16, height - 14, "Ctrl+O open default | Ctrl+S save | Ctrl+Z undo", muted);
    } else {
        text(16, 28, "Process manager", foreground);
        for (int i = 0; i < page_rows() && process_list.scroll + i < process_count; i++) text(16, 60 + i * ROW, process_lines[process_list.scroll + i], foreground);
        text(16, height - 14, "Up/Down select | Delete sends TERM | F5 refresh", muted);
    }
    XFlush(display); dirty = 0;
}
static void save_settings(void)
{
    if (preferences_save(&prefs)) snprintf(status, sizeof(status), "Save failed: %s", strerror(errno));
    else {
        XEvent e = {0}; e.xclient.type = ClientMessage;
        e.xclient.window = DefaultRootWindow(display);
        e.xclient.message_type = XInternAtom(display, SETTINGS_MESSAGE, False); e.xclient.format = 32;
        XSendEvent(display, e.xclient.window, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
        snprintf(status, sizeof(status), "Saved for this user");
    }
}
static void change_setting(int direction)
{
    unsigned int *v = setting_row ? &prefs.accent : &prefs.background;
    if (setting_row < 2) { *v = (*v + (direction < 0 ? 2 : 1)) % 3; strcpy(status, "Unsaved changes"); }
}
static void key(XKeyEvent *e)
{
    KeySym k = XLookupKeysym(e, 0);
    if (k == XK_Escape) { stopping = 1; return; }
    if (app == EDITOR) {
        if ((e->state & ControlMask) && (k == XK_s || k == XK_S)) { editor_save(); dirty = 1; return; }
        if ((e->state & ControlMask) && (k == XK_o || k == XK_O)) { editor_load(getenv("WIIDESK_EDITOR_FILE") ? getenv("WIIDESK_EDITOR_FILE") : "/tmp/wiidesk-note.txt"); dirty = 1; return; }
        char bytes[8]; int n = XLookupString(e, bytes, sizeof(bytes), NULL, NULL);
        ui_text_key(&editor_text, k, e->state, bytes, n, 1); dirty = 1; return;
    } else if (app == PROCESSES) {
        if (ui_list_key(&process_list, k, page_rows())) dirty = 1;
        if (k == XK_F5) { process_refresh(); dirty = 1; }
        if (k == XK_Delete && process_list.selected < process_count) { char pid[16]; sscanf(process_lines[process_list.selected], "%15s", pid); kill((pid_t)strtol(pid, NULL, 10), SIGTERM); process_refresh(); dirty = 1; }
        return;
    } else if (app == FILES) {
        if (k == XK_Up && selected > 0) selected--;
        if (k == XK_Down && selected + 1 < entry_count) selected++;
        if (k == XK_Page_Down) { selected += page_rows(); if (selected >= entry_count) selected = entry_count ? entry_count - 1 : 0; }
        if (k == XK_Page_Up) { selected -= page_rows(); if (selected < 0) selected = 0; }
        if (k == XK_Return) open_selected();
        if (k == XK_BackSpace || (k == XK_Up && (e->state & Mod1Mask))) parent_directory();
        if (k == XK_Home) load_directory(getenv("HOME") ? getenv("HOME") : "/");
        if (k == XK_F5) load_directory(directory);
        last_click = -1;
    } else if (app == SETTINGS) {
        if (k == XK_Tab || k == XK_Down) setting_row = (setting_row + 1) % 3;
        if (k == XK_Up) setting_row = (setting_row + 2) % 3;
        if (k == XK_Left || k == XK_Right || k == XK_space) change_setting(k == XK_Left ? -1 : 1);
        if (k == XK_Return) save_settings();
    }
    dirty = 1;
}
static void button(XButtonEvent *e)
{
    if (app == FILES) {
        if (e->button == Button4 && selected > 0) selected--;
        if (e->button == Button5 && selected + 1 < entry_count) selected++;
        if (e->button == Button1) {
            if (e->y >= 8 && e->y < 32) {
                if (e->x < 72) parent_directory();
                else if (e->x < 160) load_directory(getenv("HOME") ? getenv("HOME") : "/");
                else if (e->x < 256) load_directory(directory);
            } else if (e->y >= 64 && e->y < 64 + page_rows() * ROW) {
                int i = scroll + (e->y - 64) / ROW;
                if (i < entry_count) {
                    selected = i;
                    if (last_click == i && (Time)(e->time - last_click_time) < 400) { open_selected(); last_click = -1; }
                    else { last_click = i; last_click_time = e->time; }
                }
            }
        }
    } else if (app == SETTINGS && e->button == Button1) {
        if (e->y >= 48 && e->y < 116) { setting_row = (e->y - 48) / 38; change_setting(1); }
        else if (e->x >= 12 && e->x < 152 && e->y >= 140 && e->y < 168) save_settings();
    }
    dirty = 1;
}
int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "files") && strcmp(argv[1], "system") && strcmp(argv[1], "settings") && strcmp(argv[1], "editor") && strcmp(argv[1], "processes"))) {
        fprintf(stderr, "Usage: %s files|system|settings|editor|processes\n", argv[0]); return 1;
    }
    app = !strcmp(argv[1], "files") ? FILES : !strcmp(argv[1], "system") ? SYSTEM : !strcmp(argv[1], "settings") ? SETTINGS : !strcmp(argv[1], "editor") ? EDITOR : PROCESSES;
    display = XOpenDisplay(NULL);
    if (!display) { fprintf(stderr, "wiidesk-x11-app: cannot open DISPLAY\n"); return 1; }
    preferences_load(&prefs);
    foreground = color("#eef2f3"); muted = color("#aabdc3"); surface = color("#333b40"); accent = color(accent_colors[prefs.accent]);
    window = XCreateSimpleWindow(display, DefaultRootWindow(display), 48, 48, width, height, 0, 0, color("#242b2f"));
    const char *title = app == FILES ? "WiiDesk Files" : app == SYSTEM ? "WiiDesk System" : app == SETTINGS ? "WiiDesk Settings" : app == EDITOR ? "WiiDesk Editor" : "WiiDesk Processes";
    XStoreName(display, window, title);
    XClassHint class_hint = { .res_name = argv[1], .res_class = "WiiDesk" }; XSetClassHint(display, window, &class_hint);
    XSizeHints hints = { .flags = PMinSize, .min_width = 360, .min_height = 300 }; XSetWMNormalHints(display, window, &hints);
    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False); wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    XSetWMProtocols(display, window, &wm_delete, 1);
    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask | VisibilityChangeMask);
    gc = XCreateGC(display, window, 0, NULL); font = XLoadQueryFont(display, "8x13");
    if (!font) font = XLoadQueryFont(display, "fixed");
    if (font) XSetFont(display, gc, font->fid);
    shared_ui = (struct ui){.display=display, .window=window, .gc=gc, .width=width, .height=height, .text=foreground, .muted=muted, .surface=surface, .accent=accent, .background=color("#242b2f")};
    if (app == EDITOR) { if (ui_text_init(&editor_text, 262144)) return 1; if (getenv("WIIDESK_EDITOR_FILE")) editor_load(getenv("WIIDESK_EDITOR_FILE")); }
    if (app == PROCESSES) process_refresh();
    if (app == FILES) load_directory(getenv("HOME") ? getenv("HOME") : "/");
    if (app == SYSTEM) system_refresh();
    if (app == SETTINGS) strcpy(status, "Choose colors, then Save and apply");
    XMapWindow(display, window);
    signal(SIGTERM, stop_handler); signal(SIGINT, stop_handler);
    uint64_t next_sample = now_ms() + 1000;
    while (!stopping) {
        while (XPending(display) && !stopping) {
            XEvent e; XNextEvent(display, &e);
            switch (e.type) {
            case Expose: if (!e.xexpose.count) dirty = 1; break;
            case ConfigureNotify: width = e.xconfigure.width; height = e.xconfigure.height; dirty = 1; break;
            case VisibilityNotify: visible = e.xvisibility.state != VisibilityFullyObscured; break;
            case MapNotify: visible = 1; dirty = 1; break;
            case UnmapNotify: visible = 0; break;
            case KeyPress: key(&e.xkey); break;
            case ButtonPress: button(&e.xbutton); break;
            case ClientMessage: if (e.xclient.message_type == wm_protocols && (Atom)e.xclient.data.l[0] == wm_delete) stopping = 1; break;
            case DestroyNotify: stopping = 1; break;
            default: break;
            }
        }
        uint64_t now = now_ms();
        if (app == SYSTEM && visible && now >= next_sample) { system_refresh(); next_sample = now + 1000; dirty = 1; }
        if (dirty && visible) draw();
        if (stopping) break;
        int timeout = app == SYSTEM && visible ? (int)(next_sample > now ? next_sample - now : 0) : -1;
        struct pollfd fd = { .fd = ConnectionNumber(display), .events = POLLIN };
        if (poll(&fd, 1, timeout) < 0 && errno != EINTR) break;
        if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    }
    if (font) XFreeFont(display, font);
    XFreeGC(display, gc); XCloseDisplay(display); return 0;
}
