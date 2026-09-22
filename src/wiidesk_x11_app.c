// SPDX-License-Identifier: GPL-2.0-only
/* Lightweight ordinary X11 clients for the WiiDesk desktop. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "x11_preferences.h"
#include "x11_controls.h"
#include "x11_app_io.h"
#include "x11_session.h"
#include "x11_utilities.h"
#include "x11_network.h"
#include <pwd.h>

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
static enum { FILES, SYSTEM, SETTINGS, EDITOR, PROCESSES, SESSION } app;
static unsigned long session_caps[4];
static int session_row;
static char session_user[64];
static struct ui shared_ui;
static struct ui_text editor_text;
static struct ui_list process_list;
static char editor_path[PATH_MAX];
static char process_lines[512][96];
static struct process { pid_t pid; unsigned long long start, ticks; unsigned long rss; unsigned int cpu; char name[64]; } processes[512];
static int process_count, editor_top;
static pid_t process_filter;
static struct document_stamp editor_stamp;
static struct ui_text dialog_text;
static int dialog_top;
static enum { D_NONE, D_OPEN, D_SAVE, D_FIND, D_CLOSE, D_DISCARD_OPEN, D_MKDIR, D_RENAME, D_COPY, D_MOVE, D_TRASH, D_TERM, D_FOLDER, D_LOGOUT, D_FORCE_LOGOUT, D_PID } dialog;
static char dialog_label[160], op_source[PATH_MAX];
static struct stat op_stamp;
static int process_fd = -1;
static struct file_copy copy_job = { .input = -1, .output = -1 };
static char last_trash[PATH_MAX], last_trash_info[PATH_MAX], last_trash_original[PATH_MAX];
static uint64_t paste_deadline;
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
    shared_ui.width = width; shared_ui.height = height;
    ui_label(&shared_ui, x, y, s, pixel);
}
static void rectangle(int x, int y, int w, int h, unsigned long pixel)
{ ui_fill(&shared_ui, x, y, w, h, pixel); }
static int page_rows(void) { int rows = (height - 112) / ROW; return rows > 0 ? rows : 1; }
static void reveal_selection(void)
{
    struct ui_list list = {entry_count, selected, scroll};
    ui_list_reveal(&list, page_rows()); selected = list.selected; scroll = list.scroll;
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
static int process_read(pid_t pid, struct process *p)
{
    char path[64], line[2048]; snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "r"); if (!f) return -1;
    int ok = fgets(line, sizeof(line), f) != NULL; fclose(f); if (!ok) return -1;
    char *a = strchr(line, '('), *b = strrchr(line, ')'); if (!a || !b || b <= a) return -1;
    *p = (struct process){ .pid = pid }; *b = 0; snprintf(p->name, sizeof(p->name), "%.63s", a + 1);
    char *save = NULL, *token = strtok_r(b + 1, " ", &save); int field = 3;
    for (; token; token = strtok_r(NULL, " ", &save), field++) {
        if (field == 14 || field == 15) p->ticks += strtoull(token, NULL, 10);
        if (field == 22) p->start = strtoull(token, NULL, 10);
        if (field == 24) { long pages = strtol(token, NULL, 10); p->rss = pages > 0 ? (unsigned long)pages * (sysconf(_SC_PAGESIZE) / 1024) : 0; }
    }
    return field > 24 ? 0 : -1;
}
static int process_compare(const void *a, const void *b)
{ pid_t x = ((const struct process *)a)->pid, y = ((const struct process *)b)->pid; return x < y ? -1 : x > y; }
static void process_refresh(void)
{
    static struct process previous[512]; static uint64_t sampled;
    uint64_t now = now_ms(); int old_count = process_count;
    struct process chosen = {0};
    if (process_count && process_list.selected < process_count) chosen = processes[process_list.selected];
    memcpy(previous, processes, sizeof(previous));
    DIR *d = opendir("/proc"); if (!d) { snprintf(status, sizeof(status), "Cannot read processes"); return; }
    process_count = 0; struct dirent *e; int capped = 0;
    while ((e = readdir(d))) {
        char *end; long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0 || pid > INT_MAX) continue;
        if (process_filter && pid != process_filter) continue;
        if (process_count == 512) { capped = 1; break; }
        struct process p; if (process_read((pid_t)pid, &p)) continue;
        for (int i = 0; i < old_count && sampled && now > sampled; i++) {
            if (p.pid == previous[i].pid && p.start == previous[i].start && p.ticks >= previous[i].ticks) {
                unsigned long long percent = (p.ticks - previous[i].ticks) * 100000 / (sysconf(_SC_CLK_TCK) * (now - sampled));
                p.cpu = percent > 9999 ? 9999 : percent; break;
            }
        }
        processes[process_count++] = p;
    }
    closedir(d); sampled = now;
    qsort(processes, process_count, sizeof(processes[0]), process_compare);
    for (int i = 0; i < process_count; i++) {
        struct process *p = &processes[i];
        if (p->pid == chosen.pid && p->start == chosen.start) process_list.selected = i;
        snprintf(process_lines[i], sizeof(process_lines[i]), "%6ld %4u%% %6lu KiB  %.36s", (long)p->pid, p->cpu, p->rss, p->name);
    }
    process_list.count = process_count; ui_list_reveal(&process_list, page_rows());
    if (capped) snprintf(status, sizeof(status), "Showing first 512 processes");
    else if (process_filter) snprintf(status, sizeof(status), "PID %ld: %s | Ctrl+L shows all", (long)process_filter, process_count ? "filtered" : "not running or unavailable");
}
static void editor_load(const char *path)
{
    char *data = malloc(262145); if (!data) { strcpy(status, "Not enough memory"); return; }
    struct document_stamp stamp; size_t n;
    if (document_read(path, data, 262144, &n, &stamp)) snprintf(status, sizeof(status), "Open failed: %s", strerror(errno));
    else if (ui_text_set(&editor_text, data, n)) strcpy(status, "Only ASCII text is supported; file not loaded");
    else { editor_stamp = stamp; editor_top = 0; snprintf(editor_path, sizeof(editor_path), "%s", path); snprintf(status, sizeof(status), "Loaded %zu bytes", n); }
    free(data);
}
static int editor_save(const char *path)
{
    editor_text.typing = 0;
    struct document_stamp expected = {0}, after;
    if (!strcmp(path, editor_path)) expected = editor_stamp;
    if (document_write(path, editor_text.data, editor_text.length, &expected, &after)) {
        snprintf(status, sizeof(status), "Save failed: %s", strerror(errno)); return -1;
    }
    editor_stamp = after; memmove(editor_path, path, strlen(path) + 1);
    editor_text.modified = 0; snprintf(status, sizeof(status), "Saved %zu bytes", editor_text.length); return 0;
}
static void session_refresh(void)
{
    Atom type; int format; unsigned long n, left; unsigned char *data = NULL;
    memset(session_caps, 0, sizeof(session_caps));
    if (XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, SESSION_CAPS, False), 0, 4, False, XA_CARDINAL, &type, &format, &n, &left, &data) == Success && format == 32 && n == 4)
        memcpy(session_caps, data, sizeof(session_caps));
    if (data) XFree(data);
    data = NULL;
    if (XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, SESSION_STATUS, False), 0, 40, False, AnyPropertyType, &type, &format, &n, &left, &data) == Success && format == 8 && n) {
        size_t length = n < sizeof(status) - 1 ? n : sizeof(status) - 1;
        memcpy(status, data, length); status[length] = 0;
    }
    if (data) XFree(data);
}
static void session_send(int action)
{
    XEvent e = {0}; e.xclient.type = ClientMessage; e.xclient.window = window;
    e.xclient.message_type = XInternAtom(display, SESSION_ACTION, False); e.xclient.format = 32;
    e.xclient.data.l[0] = action;
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(display);
}
static int confirmation(void) { return dialog == D_CLOSE || dialog == D_DISCARD_OPEN || dialog == D_TRASH || dialog == D_TERM || dialog == D_LOGOUT || dialog == D_FORCE_LOGOUT; }
static void begin_dialog(int action, const char *label, const char *value)
{
    ui_clipboard_cancel(&shared_ui); dialog = action; dialog_top = 0;
    editor_text.typing = 0;
    snprintf(dialog_label, sizeof(dialog_label), "%s", label);
    if (ui_text_set(&dialog_text, value, strlen(value))) {
        ui_text_set(&dialog_text, "", 0); strcpy(status, "Path entry supports ASCII text only");
    }
    dialog_text.cursor = dialog_text.length;
    dirty = 1;
}
static void end_dialog(void)
{
    ui_clipboard_cancel(&shared_ui); dialog = D_NONE;
    if (process_fd >= 0) close(process_fd);
    process_fd = -1; dirty = 1;
}
static void session_activate(int row)
{
    if (row == 0) {
        if (session_caps[1]) session_send(SESSION_LOCK);
        else strcpy(status, "Lock is unavailable: XSecureLock is missing");
    } else if (row == 1) {
        if (session_caps[0]) begin_dialog(D_LOGOUT, "Close applications and log out?", "");
        else strcpy(status, "Sign in through WiiDesk to enable logout");
    } else if (row == 2 && session_caps[2]) session_send(SESSION_CANCEL_LOGOUT);
    else if (row == 3 && session_caps[2]) begin_dialog(D_FORCE_LOGOUT, "Discard unsaved work and force logout?", "");
    else if (row >= 4) strcpy(status, access("/sys/power/state", F_OK) ? "This kernel has no suspend/hibernate interface" : "Sleep support is not enabled for this Wii session");
    dirty = 1;
}
static void request_close(void)
{
    if (copy_job.input >= 0) { copy_cancel(&copy_job); strcpy(status, "Copy cancelled"); dirty = 1; return; }
    if (app == EDITOR && editor_text.modified) begin_dialog(D_CLOSE, "Discard unsaved changes and close?", "");
    else stopping = 1;
}
static int target_path(const char *input, char *out)
{
    if (!*input || strchr(input, '\n') || strchr(input, '\r')) { errno = EINVAL; return -1; }
    int n;
    if (*input == '/') n = snprintf(out, PATH_MAX, "%s", input);
    else if (input[0] == '~' && input[1] == '/') n = snprintf(out, PATH_MAX, "%s/%s", getenv("HOME") ? getenv("HOME") : "/", input + 2);
    else n = snprintf(out, PATH_MAX, "%s/%s", app == FILES ? directory : getenv("HOME") ? getenv("HOME") : "/", input);
    if (n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; } return 0;
}
static void file_action(int action)
{
    if (copy_job.input >= 0) return;
    if (action == D_MKDIR) { begin_dialog(action, "New folder name", ""); return; }
    if (!entry_count) return;
    if (snprintf(op_source, sizeof(op_source), "%s/%s", directory, entries[selected].name) >= (int)sizeof(op_source) || lstat(op_source, &op_stamp)) {
        strcpy(status, "Selected entry is unavailable"); return;
    }
    begin_dialog(action, action == D_RENAME ? "Rename to (no overwrite)" : action == D_COPY ? "Copy file to (no overwrite)" : action == D_MOVE ? "Move to (no overwrite)" : "Move selected entry to Trash?", action == D_RENAME ? entries[selected].name : "");
}
static void process_action(void)
{
    if (!process_count) return;
    struct process *chosen = &processes[process_list.selected], check;
    if (chosen->pid <= 1 || chosen->pid == getpid()) { strcpy(status, "This process cannot be terminated here"); return; }
    int fd = syscall(SYS_pidfd_open, chosen->pid, 0);
    if (fd < 0) { snprintf(status, sizeof(status), "Cannot select process: %s", strerror(errno)); return; }
    if (process_read(chosen->pid, &check) || check.start != chosen->start) { close(fd); strcpy(status, "Process changed; refresh and select again"); return; }
    char label[160]; snprintf(label, sizeof(label), "Send TERM to %ld (%.60s)?", (long)chosen->pid, chosen->name);
    begin_dialog(D_TERM, label, ""); process_fd = fd;
}
static void accept_dialog(void)
{
    int action = dialog; char target[PATH_MAX];
    if (action == D_PID) {
        char *end; errno = 0; long pid = strtol(dialog_text.data, &end, 10);
        if (errno || !dialog_text.length || *end || pid <= 0 || pid > INT_MAX) {
            end_dialog(); strcpy(status, "Enter a positive numeric process ID"); return;
        }
        process_filter = (pid_t)pid; process_list.selected = process_list.scroll = 0;
        end_dialog(); process_refresh(); return;
    }
    if (action == D_LOGOUT || action == D_FORCE_LOGOUT) {
        end_dialog(); session_send(action == D_LOGOUT ? SESSION_LOGOUT : SESSION_FORCE_LOGOUT); return;
    }
    if (action == D_CLOSE) { end_dialog(); stopping = 1; return; }
    if (action == D_DISCARD_OPEN) { begin_dialog(D_OPEN, "Open text file (up to 256 KiB)", editor_path); return; }
    if (action == D_TERM) {
        if (syscall(SYS_pidfd_send_signal, process_fd, SIGTERM, NULL, 0)) snprintf(status, sizeof(status), "TERM failed: %s", strerror(errno));
        else strcpy(status, "Termination requested");
        end_dialog(); process_refresh(); return;
    }
    if (action == D_FIND) {
        if (dialog_text.length) {
            char *found = strstr(editor_text.data + editor_text.cursor, dialog_text.data);
            if (!found) found = strstr(editor_text.data, dialog_text.data);
            if (found) { editor_text.anchor = found - editor_text.data; editor_text.cursor = editor_text.anchor + dialog_text.length; strcpy(status, "Match selected"); }
            else strcpy(status, "Text not found");
        }
        end_dialog(); return;
    }
    if (action != D_TRASH && target_path(dialog_text.data, target)) { snprintf(status, sizeof(status), "Invalid path: %s", strerror(errno)); end_dialog(); return; }
    if (action == D_OPEN) { end_dialog(); editor_load(target); return; }
    if (action == D_SAVE) { end_dialog(); editor_save(target); return; }
    if (action == D_FOLDER) { end_dialog(); load_directory(target); return; }
    struct stat st;
    if (action != D_MKDIR && (lstat(op_source, &st) || st.st_dev != op_stamp.st_dev || st.st_ino != op_stamp.st_ino)) {
        strcpy(status, "Selected entry changed; refresh first"); end_dialog(); return;
    }
    int result = -1;
    if (action == D_MKDIR) result = mkdir(target, 0700);
    if (action == D_RENAME || action == D_MOVE) result = move_new(op_source, target);
    if (action == D_COPY) result = copy_start(&copy_job, op_source, target);
    if (action == D_TRASH) {
        char saved_path[PATH_MAX], saved_info[PATH_MAX];
        result = trash_move(op_source, saved_path, sizeof(saved_path), saved_info, sizeof(saved_info));
        if (!result) {
            snprintf(last_trash, sizeof(last_trash), "%s", saved_path);
            snprintf(last_trash_info, sizeof(last_trash_info), "%s", saved_info);
            snprintf(last_trash_original, sizeof(last_trash_original), "%s", op_source);
        }
    }
    int saved = errno; end_dialog();
    if (result) snprintf(status, sizeof(status), "Operation failed: %s", strerror(saved));
    else { load_directory(directory); strcpy(status, action == D_COPY ? "Copying... Escape cancels" : action == D_TRASH ? "Moved to Trash; Ctrl+Z restores last entry" : "Done"); }
}
static void draw(void)
{
    shared_ui.width = width; shared_ui.height = height;
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
        text(12, height - 32, "F2 Rename F6 Move F7 Folder F8 Copy Del Trash", muted);
        text(12, height - 13, status, muted);
    } else if (app == SYSTEM) {
        text(16, 28, "System monitor", foreground);
        for (int i = 0; i < 7; i++) text(16, 60 + i * 30, system_lines[i], i % 2 ? muted : foreground);
        text(16, height - 14, "Updates once per second while visible", muted);
    } else if (app == SETTINGS) {
        text(16, 28, "Appearance and session", foreground);
        for (int i = 0; i < 3; i++) {
            rectangle(12, 48 + i * 38, width - 24, 30, setting_row == i ? accent : surface);
            char label[128];
            if (i < 2) snprintf(label, sizeof(label), "%s:  < %s >", i ? "Accent    " : "Background", i ? accent_names[prefs.accent] : background_names[prefs.background]);
            else if (!prefs.idle_lock_seconds) strcpy(label, "Idle lock :  < Off >");
            else snprintf(label, sizeof(label), "Idle lock :  < %u seconds >", prefs.idle_lock_seconds);
            text(22, 68 + i * 38, label, foreground);
        }
        ui_button(&shared_ui, 12, 178, 140, "Save and apply", setting_row == 3);
        text(16, 228, "Arrows change; Tab selects; Enter saves", muted);
        text(16, 250, "Idle lock applies to managed X11 sessions", muted);
        text(16, height - 16, status, muted);
    } else if (app == EDITOR) {
        ui_button(&shared_ui, 8, 8, 64, "Open", 0);
        ui_button(&shared_ui, 78, 8, 64, "Save", 0);
        ui_button(&shared_ui, 148, 8, 80, "Save as", 0);
        ui_button(&shared_ui, 234, 8, 64, "Find", 0);
        text(12, 51, editor_path[0] ? editor_path : "Untitled", muted);
        ui_text_draw(&shared_ui, &editor_text, 12, 64, width - 24, height - 110, &editor_top, 1);
        text(12, height - 30, editor_text.modified ? "Unsaved | Ctrl+A/C/X/V/Z; Shift+arrows select" : "Ctrl+O/S/F | Ctrl+A/C/X/V/Z; Shift selects", muted);
        text(12, height - 12, status, muted);
    } else if (app == SESSION) {
        char account[100]; snprintf(account, sizeof(account), "WiiDesk session: %s", session_user);
        text(16, 28, account, foreground);
        ui_button(&shared_ui, 12, 46, 212, session_caps[1] ? "Lock now" : "Lock unavailable", session_row == 0);
        ui_button(&shared_ui, 12, 80, 212, session_caps[0] ? "Log out..." : "Log out unavailable", session_row == 1);
        if (session_caps[2]) {
            ui_button(&shared_ui, 12, 114, 168, "Cancel logout", session_row == 2);
            ui_button(&shared_ui, 190, 114, 184, "Force logout...", session_row == 3);
        }
        ui_button(&shared_ui, 12, 158, 292, "Suspend unavailable", session_row == 4);
        ui_button(&shared_ui, 12, 192, 292, "Hibernate unavailable", session_row == 5);
        text(16, 242, "Ctrl+Alt+L locks; password unlocks", muted);
        text(16, height - 36, "Save your work before logging out", muted);
        text(16, height - 14, status, muted);
    } else {
        unsigned long pid = process_count ? (unsigned long)processes[process_list.selected].pid : 0;
        XChangeProperty(display, window, XInternAtom(display, "_WIIDESK_SELECTED_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
        ui_button(&shared_ui, 8, 8, 90, "Refresh", 0);
        ui_button(&shared_ui, 104, 8, 110, "End process", 0);
        ui_button(&shared_ui, 220, 8, 80, "PID...", 0);
        ui_button(&shared_ui, 306, 8, 44, "All", 0);
        text(16, 51, "   PID   CPU      Memory   Name", muted);
        for (int i = 0; i < page_rows() && process_list.scroll + i < process_count; i++) {
            if (process_list.scroll + i == process_list.selected) rectangle(8, 64 + i * ROW, width - 16, ROW, accent);
            text(16, 80 + i * ROW, process_lines[process_list.scroll + i], foreground);
        }
        text(16, height - 32, "Ctrl+F PID | Ctrl+L all | Delete TERM", muted);
        text(16, height - 14, status, muted);
    }
    if (dialog) {
        int y = (height - 132) / 2;
        rectangle(8, y, width - 16, 132, surface);
        text(20, y + 24, dialog_label, foreground);
        if (!confirmation()) {
            rectangle(16, y + 38, width - 32, 28, shared_ui.background);
            ui_text_draw(&shared_ui, &dialog_text, 20, y + 44, width - 40, 16, &dialog_top, 0);
        }
        ui_button(&shared_ui, 20, y + 88, 100, confirmation() ? "Confirm" : "Accept", 1);
        ui_button(&shared_ui, 132, y + 88, 100, "Cancel", 0);
        text(244, y + 105, "Enter / Escape", muted);
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
    else if (setting_row == 2) {
        int i = 0, count = sizeof(idle_lock_choices) / sizeof(idle_lock_choices[0]);
        while (i < count - 1 && idle_lock_choices[i] < prefs.idle_lock_seconds) i++;
        prefs.idle_lock_seconds = idle_lock_choices[(i + (direction < 0 ? count - 1 : 1)) % count];
        strcpy(status, "Unsaved changes");
    }
}
static void key(XKeyEvent *e)
{
    KeySym k = XLookupKeysym(e, 0);
    char bytes[16]; int n = XLookupString(e, bytes, sizeof(bytes), NULL, NULL);
    if (dialog) {
        if (k == XK_Escape) end_dialog();
        else if (k == XK_Return) accept_dialog();
        else if (!confirmation()) {
            int clipboard = ui_clipboard_key(&shared_ui, &dialog_text, k, e->state, e->time);
            if (!clipboard) ui_text_key(&dialog_text, k, e->state, bytes, n, 0);
            if (shared_ui.paste_target) paste_deadline = now_ms() + 3000;
        }
        dirty = 1; return;
    }
    if (k == XK_Escape) { request_close(); return; }
    if (app == SESSION) {
        if (k == XK_Up || k == XK_Down || k == XK_Tab) {
            int step = k == XK_Up ? 5 : 1;
            do { session_row = (session_row + step) % 6; }
            while (!session_caps[2] && (session_row == 2 || session_row == 3));
        }
        if (k == XK_Return || k == XK_space) session_activate(session_row);
        dirty = 1; return;
    } else if (app == EDITOR) {
        if ((e->state & ControlMask) && (k == XK_s || k == XK_S)) {
            if (!editor_path[0] || (e->state & ShiftMask)) begin_dialog(D_SAVE, "Save to new file (no overwrite)", editor_path);
            else editor_save(editor_path);
        } else if ((e->state & ControlMask) && (k == XK_o || k == XK_O)) {
            if (editor_text.modified) begin_dialog(D_DISCARD_OPEN, "Discard unsaved changes and open another file?", "");
            else begin_dialog(D_OPEN, "Open text file (up to 256 KiB)", editor_path);
        } else if ((e->state & ControlMask) && (k == XK_f || k == XK_F)) begin_dialog(D_FIND, "Find text", "");
        else {
            int result = ui_clipboard_key(&shared_ui, &editor_text, k, e->state, e->time);
            if (!result) result = ui_text_key(&editor_text, k, e->state, bytes, n, 1);
            if (result < 0) strcpy(status, "Text rejected: size limit or unsupported text");
            if (shared_ui.paste_target) paste_deadline = now_ms() + 3000;
        }
        dirty = 1; return;
    } else if (app == PROCESSES) {
        if ((e->state & ControlMask) && k == XK_f) begin_dialog(D_PID, "Find process by PID", "");
        if ((e->state & ControlMask) && k == XK_l) { process_filter = 0; strcpy(status, "Showing all processes"); process_refresh(); }
        if (ui_list_key(&process_list, k, page_rows())) dirty = 1;
        if (k == XK_F5) { process_refresh(); dirty = 1; }
        if (k == XK_Delete) process_action();
        dirty = 1;
        return;
    } else if (app == FILES) {
        if (copy_job.input >= 0) return;
        struct ui_list list = { entry_count, selected, scroll };
        if (k != XK_Home) ui_list_key(&list, k, page_rows());
        selected = list.selected; scroll = list.scroll;
        if (k == XK_Return) open_selected();
        if (k == XK_BackSpace || (k == XK_Up && (e->state & Mod1Mask))) parent_directory();
        if (k == XK_Home) load_directory(getenv("HOME") ? getenv("HOME") : "/");
        if (k == XK_F5) load_directory(directory);
        if ((e->state & ControlMask) && k == XK_l) begin_dialog(D_FOLDER, "Go to folder", directory);
        if (k == XK_F2) file_action(D_RENAME);
        if (k == XK_F6) file_action(D_MOVE);
        if (k == XK_F7) file_action(D_MKDIR);
        if (k == XK_F8) file_action(D_COPY);
        if (k == XK_Delete) file_action(D_TRASH);
        if ((e->state & ControlMask) && k == XK_z && last_trash_original[0]) {
            if (move_new(last_trash, last_trash_original)) snprintf(status, sizeof(status), "Restore failed: %s", strerror(errno));
            else { unlink(last_trash_info); last_trash_original[0] = 0; load_directory(directory); strcpy(status, "Restored from Trash"); }
        }
        last_click = -1;
    } else if (app == SETTINGS) {
        if (k == XK_Tab || k == XK_Down) setting_row = (setting_row + 1) % 4;
        if (k == XK_Up) setting_row = (setting_row + 3) % 4;
        if (k == XK_Left || k == XK_Right || k == XK_space) change_setting(k == XK_Left ? -1 : 1);
        if (k == XK_Return) save_settings();
    }
    dirty = 1;
}
static void button(XButtonEvent *e)
{
    if (dialog) {
        int y = (height - 132) / 2;
        if (e->button == Button1) {
            if (ui_hit(e->x, e->y, 20, y + 88, 100, 24)) accept_dialog();
            else if (ui_hit(e->x, e->y, 132, y + 88, 100, 24)) end_dialog();
            else if (!confirmation() && ui_hit(e->x, e->y, 20, y + 44, width - 40, 16)) ui_text_click(&dialog_text, e->x - 20, e->y - y - 44, width - 40, dialog_top, 0, e->state & ShiftMask);
        }
        dirty = 1; return;
    }
    if (app == SESSION && e->button == Button1) {
        if (ui_hit(e->x, e->y, 12, 46, 212, 24)) session_activate(0);
        else if (ui_hit(e->x, e->y, 12, 80, 212, 24)) session_activate(1);
        else if (ui_hit(e->x, e->y, 12, 114, 168, 24)) session_activate(2);
        else if (ui_hit(e->x, e->y, 190, 114, 184, 24)) session_activate(3);
        else if (ui_hit(e->x, e->y, 12, 158, 292, 24)) session_activate(4);
        else if (ui_hit(e->x, e->y, 12, 192, 292, 24)) session_activate(5);
    }
    if (app == EDITOR && e->button == Button1) {
        if (ui_hit(e->x, e->y, 8, 8, 64, 24)) {
            if (editor_text.modified) begin_dialog(D_DISCARD_OPEN, "Discard unsaved changes and open another file?", "");
            else begin_dialog(D_OPEN, "Open text file (up to 256 KiB)", editor_path);
        } else if (ui_hit(e->x, e->y, 78, 8, 64, 24)) {
            if (editor_path[0]) editor_save(editor_path); else begin_dialog(D_SAVE, "Save to new file (no overwrite)", "");
        } else if (ui_hit(e->x, e->y, 148, 8, 80, 24)) begin_dialog(D_SAVE, "Save to new file (no overwrite)", editor_path);
        else if (ui_hit(e->x, e->y, 234, 8, 64, 24)) begin_dialog(D_FIND, "Find text", "");
        else if (ui_hit(e->x, e->y, 12, 64, width - 24, height - 110)) ui_text_click(&editor_text, e->x - 12, e->y - 64, width - 24, editor_top, 1, e->state & ShiftMask);
    }
    if (app == PROCESSES) {
        if (e->button == Button4) ui_list_key(&process_list, XK_Up, page_rows());
        if (e->button == Button5) ui_list_key(&process_list, XK_Down, page_rows());
        if (e->button == Button1) {
            if (ui_hit(e->x, e->y, 8, 8, 90, 24)) process_refresh();
            else if (ui_hit(e->x, e->y, 104, 8, 110, 24)) process_action();
            else if (ui_hit(e->x, e->y, 220, 8, 80, 24)) begin_dialog(D_PID, "Find process by PID", "");
            else if (ui_hit(e->x, e->y, 306, 8, 44, 24)) { process_filter = 0; strcpy(status, "Showing all processes"); process_refresh(); }
            else if (e->y >= 64 && e->y < 64 + page_rows() * ROW) { process_list.selected = process_list.scroll + (e->y - 64) / ROW; ui_list_reveal(&process_list, page_rows()); }
        }
    }
    if (app == FILES) {
        if (copy_job.input >= 0) return;
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
        if (e->y >= 48 && e->y < 154) { setting_row = (e->y - 48) / 38; change_setting(1); }
        else if (e->x >= 12 && e->x < 152 && e->y >= 178 && e->y < 206) save_settings();
    }
    dirty = 1;
}
int main(int argc, char **argv)
{
    if ((argc == 2 || argc == 3) && (!strcmp(argv[1], "images") || !strcmp(argv[1], "archives") || !strcmp(argv[1], "packages") || !strcmp(argv[1], "audio"))) {
        const char *program=!strcmp(argv[1], "images")?"wiidesk-x11-image":!strcmp(argv[1], "archives")?"wiidesk-x11-archive":!strcmp(argv[1], "packages")?"wiidesk-x11-packages":"wiidesk-x11-audio";
        char executable[PATH_MAX];
        ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1);
        if(n<0 || n>=(ssize_t)sizeof(executable)-1)return 1;
        executable[n]=0; char *base=strrchr(executable,'/');
        if(!base || (size_t)(base+1-executable)+strlen(program)>=sizeof(executable))return 1;
        strcpy(base+1,program);
        execl(executable,executable,argc==3?argv[2]:NULL,(char *)NULL);
        perror(program); return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "network")) return x11_network_main();
    if (argc > 1 && x11_utility_supported(argv[1])) return x11_utility_main(argc, argv);
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[1], "editor")) || (strcmp(argv[1], "files") && strcmp(argv[1], "system") && strcmp(argv[1], "settings") && strcmp(argv[1], "editor") && strcmp(argv[1], "processes") && strcmp(argv[1], "session"))) {
        fprintf(stderr, "Usage: %s files|system|settings|processes|session|network|calculator|editor [FILE]|logs [FILE]\n", argv[0]); return 1;
    }
    app = !strcmp(argv[1], "files") ? FILES : !strcmp(argv[1], "system") ? SYSTEM : !strcmp(argv[1], "settings") ? SETTINGS : !strcmp(argv[1], "editor") ? EDITOR : !strcmp(argv[1], "session") ? SESSION : PROCESSES;
    display = XOpenDisplay(NULL);
    if (!display) { fprintf(stderr, "wiidesk-x11-app: cannot open DISPLAY\n"); return 1; }
    preferences_load(&prefs);
    foreground = color("#eef2f3"); muted = color("#aabdc3"); surface = color("#333b40"); accent = color(accent_colors[prefs.accent]);
    window = XCreateSimpleWindow(display, DefaultRootWindow(display), 48, 48, width, height, 0, 0, color("#242b2f"));
    const char *title = app == FILES ? "WiiDesk Files" : app == SYSTEM ? "WiiDesk System" : app == SETTINGS ? "WiiDesk Settings" : app == EDITOR ? "WiiDesk Editor" : app == SESSION ? "WiiDesk Session" : "WiiDesk Processes";
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
    ui_clipboard_init(&shared_ui);
    if (ui_text_init(&dialog_text, PATH_MAX - 1)) return 1;
    if (app == EDITOR) {
        if (ui_text_init(&editor_text, 262144)) return 1;
        if (argc == 3) editor_load(argv[2]);
        else if (getenv("WIIDESK_EDITOR_FILE")) editor_load(getenv("WIIDESK_EDITOR_FILE"));
    }
    if (app == PROCESSES) process_refresh();
    if (app == SESSION) {
        struct passwd *pw = getpwuid(getuid()); snprintf(session_user, sizeof(session_user), "%s", pw ? pw->pw_name : "unknown");
        XSelectInput(display, DefaultRootWindow(display), PropertyChangeMask); session_refresh();
    }
    if (app == FILES) load_directory(getenv("HOME") ? getenv("HOME") : "/");
    if (app == SYSTEM) system_refresh();
    if (app == SETTINGS) strcpy(status, "Choose colors, then Save and apply");
    XMapWindow(display, window);
    signal(SIGTERM, stop_handler); signal(SIGINT, stop_handler);
    uint64_t next_sample = now_ms() + 1000;
    while (!stopping) {
        while (XPending(display) && !stopping) {
            XEvent e; XNextEvent(display, &e);
            int clip = ui_clipboard_event(&shared_ui, &e);
            if (clip) { if (clip < 0) strcpy(status, "Paste failed: supports ASCII text up to 64 KiB"); dirty = 1; continue; }
            switch (e.type) {
            case PropertyNotify: if (app == SESSION && e.xproperty.window == DefaultRootWindow(display)) { session_refresh(); dirty = 1; } break;
            case Expose: if (!e.xexpose.count) dirty = 1; break;
            case ConfigureNotify: width = e.xconfigure.width; height = e.xconfigure.height; dirty = 1; break;
            case VisibilityNotify: visible = e.xvisibility.state != VisibilityFullyObscured; break;
            case MapNotify: visible = 1; dirty = 1; break;
            case UnmapNotify: visible = 0; break;
            case KeyPress: key(&e.xkey); break;
            case ButtonPress: button(&e.xbutton); break;
            case ClientMessage: if (e.xclient.message_type == wm_protocols && (Atom)e.xclient.data.l[0] == wm_delete) request_close(); break;
            case DestroyNotify: stopping = 1; break;
            default: break;
            }
        }
        uint64_t now = now_ms();
        if (app == SYSTEM && visible && now >= next_sample) { system_refresh(); next_sample = now + 1000; dirty = 1; }
        if (app == PROCESSES && visible && !dialog && now >= next_sample) { process_refresh(); next_sample = now + 2000; dirty = 1; }
        if (shared_ui.paste_target && now >= paste_deadline) { ui_clipboard_cancel(&shared_ui); strcpy(status, "Paste timed out"); dirty = 1; }
        if (copy_job.input >= 0) {
            int result = copy_step(&copy_job);
            if (result < 0) snprintf(status, sizeof(status), "Copy failed: %s", strerror(errno));
            else if (result) { load_directory(directory); strcpy(status, "Copy complete"); }
            else snprintf(status, sizeof(status), "Copy %lld / %lld KiB | Escape cancels", (long long)copy_job.done / 1024, (long long)copy_job.total / 1024);
            dirty = 1;
        }
        if (dirty && visible) draw();
        if (stopping) break;
        int timeout = ((app == SYSTEM || app == PROCESSES) && visible && !dialog) ? (int)(next_sample > now ? next_sample - now : 0) : -1;
        if (shared_ui.paste_target && (timeout < 0 || timeout > 100)) timeout = 100;
        if (copy_job.input >= 0) timeout = 0;
        struct pollfd fd = { .fd = ConnectionNumber(display), .events = POLLIN };
        if (poll(&fd, 1, timeout) < 0 && errno != EINTR) break;
        if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    }
    copy_cancel(&copy_job);
    if (process_fd >= 0) close(process_fd);
    ui_clipboard_free(&shared_ui); ui_text_free(&dialog_text); ui_text_free(&editor_text);
    if (font) XFreeFont(display, font);
    XFreeGC(display, gc); XCloseDisplay(display); return 0;
}
