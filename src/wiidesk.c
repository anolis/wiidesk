// SPDX-License-Identifier: GPL-2.0-only
/* WiiDesk desktop shell for the Wii VI DRM/KMS driver. */

#define main wii_drm_test_main
#include "drm_backend.c"
#undef main

#include <dirent.h>
#include <grp.h>
#include <limits.h>
#include <linux/input.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>

#ifdef WII_HAVE_VNC
#include <arpa/inet.h>
#include <rfb/rfb.h>
#include <rfb/keysym.h>
#endif

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define BIT(bit) (1U << (bit))

/* Reuse the kernel's built-in GPL VGA font without kernel-only headers. */
#define _VIDEO_FONT_H
#define _LINUX_MODULE_H
#define __packed
#define VGA8x16_IDX 1
#define EXPORT_SYMBOL(symbol)
struct font_desc {
	int idx;
	const char *name;
	unsigned int width;
	unsigned int height;
	unsigned int charcount;
	const void *data;
	int pref;
};

struct font_data {
	unsigned int extra[4];
	const unsigned char data[];
} __packed;
#include "font_8x16.c"

#define SHELL_BUFFER_COUNT 3
#define SHELL_INPUT_COUNT 16
#define SHELL_POLL_MS 10
#define SHELL_FONT_WIDTH 8
#define SHELL_FONT_HEIGHT 16
#define SHELL_WORKSPACE_LEFT 8
#define SHELL_WORKSPACE_TOP 32
#define SHELL_WORKSPACE_BOTTOM (TEST_HEIGHT - 24)
#define SHELL_TITLE_HEIGHT 28
#define SHELL_MENU_X 8
#define SHELL_MENU_WIDTH 176
#define SHELL_MENU_HEADER_HEIGHT 34
#define SHELL_MENU_ROW_HEIGHT 40
#define SHELL_MENU_PADDING 8
#define SHELL_START_X 8
#define SHELL_START_WIDTH 72
#define SHELL_TASK_X 88
#define SHELL_TASK_WIDTH 128
#define SHELL_TASK_HEIGHT 20
#define SHELL_CONTROL_SIZE 12
#define SHELL_MENU_LOCK SHELL_APP_COUNT
#define SHELL_MENU_LOGOUT (SHELL_APP_COUNT + 1)
#define SHELL_MENU_ITEM_COUNT (SHELL_APP_COUNT + 2)
#define SHELL_DESKTOP_ICON_X 18
#define SHELL_DESKTOP_ICON_Y 54
#define SHELL_DESKTOP_ICON_WIDTH 74
#define SHELL_DESKTOP_ICON_HEIGHT 70
#define SETTINGS_ROW_HEIGHT 38
#define SETTINGS_ROW_COUNT 5
#define SETTINGS_WALLPAPER_COUNT 3
#define SETTINGS_ACCENT_COUNT 3
#define SETTINGS_LOCK_COUNT 4
#define TERMINAL_COLUMNS 50
#define TERMINAL_ROWS 14
#define TERMINAL_CSI_PARAMS 4
#define FILES_ENTRY_COUNT 64
#define FILES_VISIBLE_ROWS 10
#define FILES_PATH_SIZE 512
#define SYSTEM_NETWORK_NAME 16
#define LOGIN_USERNAME_SIZE 32
#define LOGIN_PASSWORD_SIZE 64
#define LOGIN_STATUS_SIZE 64
#define LOGIN_SPLASH_MS 1500
#define LOGIN_TIMEOUT_MS 10000
#define LOGIN_PANEL_X 160
#define LOGIN_PANEL_Y 78
#define LOGIN_PANEL_WIDTH 320
#define LOGIN_PANEL_HEIGHT 316
#define LOGIN_FIELD_X 196
#define LOGIN_FIELD_WIDTH 248
#define LOGIN_USERNAME_Y 180
#define LOGIN_PASSWORD_Y 246
#define LOGIN_BUTTON_X 250
#define LOGIN_BUTTON_Y 308
#define LOGIN_BUTTON_WIDTH 140
#define LOGIN_BUTTON_HEIGHT 36

enum shell_app {
	SHELL_APP_TERMINAL,
	SHELL_APP_FILES,
	SHELL_APP_SYSTEM,
	SHELL_APP_SETTINGS,
	SHELL_APP_COUNT,
};

enum shell_view {
	SHELL_VIEW_SPLASH,
	SHELL_VIEW_LOGIN,
	SHELL_VIEW_LOCK,
	SHELL_VIEW_DESKTOP,
};

enum login_field {
	LOGIN_FIELD_USERNAME,
	LOGIN_FIELD_PASSWORD,
};

enum login_auth_state {
	LOGIN_AUTH_IDLE,
	LOGIN_AUTH_QUEUED,
	LOGIN_AUTH_RUNNING,
};

struct shell_input {
	int fd;
	char path[32];
};

struct shell_window {
	enum shell_app app;
	int x;
	int y;
	int width;
	int height;
	int visible;
	int minimized;
	int maximized;
	int restore_x;
	int restore_y;
	int restore_width;
	int restore_height;
};

struct terminal_cell {
	uint8_t character;
	uint8_t color;
};

enum terminal_parser_state {
	TERMINAL_NORMAL,
	TERMINAL_ESCAPE,
	TERMINAL_CSI,
};

struct shell_terminal {
	struct terminal_cell cells[TERMINAL_ROWS][TERMINAL_COLUMNS];
	pid_t child_pid;
	int master_fd;
	unsigned int cursor_x;
	unsigned int cursor_y;
	unsigned int saved_x;
	unsigned int saved_y;
	unsigned int csi_params[TERMINAL_CSI_PARAMS];
	unsigned int csi_count;
	enum terminal_parser_state parser_state;
	uint8_t color;
	int csi_private;
	int child_exited;
};

struct shell_file_entry {
	char name[NAME_MAX + 1];
	int directory;
};

struct shell_files {
	struct shell_file_entry entries[FILES_ENTRY_COUNT];
	char path[FILES_PATH_SIZE];
	char status[64];
	unsigned int count;
	unsigned int selected;
	unsigned int scroll;
	uint64_t last_click_ms;
	int last_clicked;
	int loaded;
};

struct shell_system {
	uint64_t cpu_total;
	uint64_t cpu_idle;
	uint64_t uptime_seconds;
	uint64_t network_rx_bytes;
	uint64_t network_tx_bytes;
	unsigned long memory_total_kb;
	unsigned long memory_available_kb;
	unsigned int cpu_percent;
	char network_name[SYSTEM_NETWORK_NAME];
	int network_up;
	int gx_loaded;
	int drm_present;
	int valid;
};

struct shell_login {
	char username[LOGIN_USERNAME_SIZE];
	char password[LOGIN_PASSWORD_SIZE];
	char status[LOGIN_STATUS_SIZE];
	size_t username_length;
	size_t password_length;
	uint64_t splash_until_ms;
	enum login_field field;
	enum login_auth_state auth_state;
};

struct shell_settings {
	unsigned int wallpaper;
	unsigned int accent;
	unsigned int pointer_scale;
	unsigned int lock_option;
	unsigned int selected;
	int clock_24h;
	char path[PATH_MAX];
	char status[64];
	uid_t owner;
	gid_t group;
};

#ifdef WII_HAVE_VNC
struct shell_vnc {
	rfbScreenInfoPtr screen;
	int buttons;
	int changed;
	int shift_down;
	int control_down;
};
#endif

struct shell_state {
	struct test_buffer buffers[SHELL_BUFFER_COUNT];
	struct shell_input inputs[SHELL_INPUT_COUNT];
	struct shell_window windows[SHELL_APP_COUNT];
	struct shell_terminal terminal;
	struct shell_files files;
	struct shell_system system;
	struct shell_login login;
	struct shell_settings settings;
#ifdef WII_HAVE_VNC
	struct shell_vnc vnc;
#endif
	unsigned int z_order[SHELL_APP_COUNT];
	unsigned int input_count;
	unsigned int visible;
	unsigned int selected;
	__u64 serial;
	enum shell_view view;
	int focused;
	int dragging;
	int drag_offset_x;
	int drag_offset_y;
	int shift_down;
	int control_down;
	int caps_lock;
	int menu_open;
	int cursor_visible;
	int desktop_selected;
	int desktop_last_clicked;
	int pointer_x;
	int pointer_y;
	uint64_t desktop_last_click_ms;
	uint64_t last_input_ms;
};

static const char *const app_titles[SHELL_APP_COUNT] = {
	"Terminal", "Files", "System", "Settings",
};

static const uint32_t shell_colors[] = {
	0x00171c1f, /* desktop */
	0x00242b2f, /* panel */
	0x00333b40, /* border */
	0x00eef2f3, /* text */
	0x0099a5aa, /* muted */
	0x002fc4b2, /* teal */
	0x00e35d4f, /* red */
	0x00d9a441, /* gold */
	0x008e71c7, /* violet */
	0x000d1113, /* terminal */
	0x004b7bec, /* blue */
	0x0078bfe5, /* sky */
	0x004aaa5b, /* green */
};

enum shell_color {
	COLOR_DESKTOP,
	COLOR_PANEL,
	COLOR_BORDER,
	COLOR_TEXT,
	COLOR_MUTED,
	COLOR_TEAL,
	COLOR_RED,
	COLOR_GOLD,
	COLOR_VIOLET,
	COLOR_TERMINAL,
	COLOR_BLUE,
	COLOR_SKY,
	COLOR_GREEN,
};

static const enum shell_color app_accents[SHELL_APP_COUNT] = {
	COLOR_TEAL, COLOR_GOLD, COLOR_VIOLET, COLOR_SKY,
};

static const enum shell_color setting_accents[SETTINGS_ACCENT_COUNT] = {
	COLOR_TEAL, COLOR_SKY, COLOR_VIOLET,
};

static uint16_t rgb565(enum shell_color color)
{
	return xrgb8888_to_rgb565(shell_colors[color]);
}

static uint64_t shell_monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static enum shell_color shell_accent(const struct shell_state *shell)
{
	return setting_accents[shell->settings.accent % SETTINGS_ACCENT_COUNT];
}

static unsigned int settings_lock_minutes(const struct shell_settings *settings)
{
	static const unsigned int minutes[SETTINGS_LOCK_COUNT] = { 0, 5, 15, 30 };

	return minutes[settings->lock_option % SETTINGS_LOCK_COUNT];
}

static void settings_defaults(struct shell_settings *settings)
{
	memset(settings, 0, sizeof(*settings));
	settings->pointer_scale = 1;
	settings->clock_24h = 1;
}

static int settings_make_directory(const char *path, uid_t owner, gid_t group)
{
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return -1;
	if (chown(path, owner, group) < 0 && errno != EPERM)
		return -1;
	return 0;
}

static int settings_load(struct shell_settings *settings, const char *username)
{
	struct passwd *account;
	FILE *passwd;
	char config[PATH_MAX];
	char directory[PATH_MAX];
	char line[128];
	FILE *file;

	settings_defaults(settings);
	passwd = fopen("/etc/passwd", "r");
	account = passwd ? fgetpwent(passwd) : NULL;
	while (account && strcmp(account->pw_name, username))
		account = fgetpwent(passwd);
	if (!account || !account->pw_dir || account->pw_dir[0] != '/') {
		if (passwd)
			fclose(passwd);
		snprintf(settings->status, sizeof(settings->status),
			 "Settings unavailable");
		return -1;
	}
	settings->owner = account->pw_uid;
	settings->group = account->pw_gid;
	if (snprintf(config, sizeof(config), "%s/.config", account->pw_dir) >=
	    (int)sizeof(config) ||
	    snprintf(directory, sizeof(directory), "%s/wiidesk", config) >=
	    (int)sizeof(directory) ||
	    snprintf(settings->path, sizeof(settings->path), "%s/settings.conf",
		     directory) >= (int)sizeof(settings->path)) {
		settings->path[0] = '\0';
		fclose(passwd);
		return -1;
	}
	fclose(passwd);
	if (settings_make_directory(config, settings->owner, settings->group) < 0 ||
	    settings_make_directory(directory, settings->owner,
				    settings->group) < 0) {
		settings->path[0] = '\0';
		snprintf(settings->status, sizeof(settings->status),
			 "Settings are read-only");
		return -1;
	}
	file = fopen(settings->path, "r");
	if (!file) {
		if (errno != ENOENT)
			snprintf(settings->status, sizeof(settings->status),
				 "Settings are read-only");
		return errno == ENOENT ? 0 : -1;
	}
	while (fgets(line, sizeof(line), file)) {
		unsigned int value;

		if (sscanf(line, "wallpaper=%u", &value) == 1)
			settings->wallpaper = value % SETTINGS_WALLPAPER_COUNT;
		else if (sscanf(line, "accent=%u", &value) == 1)
			settings->accent = value % SETTINGS_ACCENT_COUNT;
		else if (sscanf(line, "clock_24h=%u", &value) == 1)
			settings->clock_24h = value != 0;
		else if (sscanf(line, "pointer_scale=%u", &value) == 1)
			settings->pointer_scale = value >= 1 && value <= 3 ? value : 1;
		else if (sscanf(line, "lock_option=%u", &value) == 1)
			settings->lock_option = value % SETTINGS_LOCK_COUNT;
	}
	fclose(file);
	return 0;
}

static int settings_save(struct shell_settings *settings)
{
	char temporary[PATH_MAX];
	FILE *file;
	int failed;
	int result = -1;

	if (!settings->path[0] ||
	    snprintf(temporary, sizeof(temporary), "%s.tmp", settings->path) >=
	    (int)sizeof(temporary))
		goto out;
	file = fopen(temporary, "w");
	if (!file)
		goto out;
	fprintf(file, "wallpaper=%u\naccent=%u\nclock_24h=%u\n",
		settings->wallpaper, settings->accent, settings->clock_24h);
	fprintf(file, "pointer_scale=%u\nlock_option=%u\n",
		settings->pointer_scale, settings->lock_option);
	failed = fflush(file) || fsync(fileno(file)) ||
		fchown(fileno(file), settings->owner, settings->group);
	if (fclose(file))
		failed = 1;
	if (failed) {
		unlink(temporary);
		goto out;
	}
	if (rename(temporary, settings->path) < 0) {
		unlink(temporary);
		goto out;
	}
	result = 0;
out:
	snprintf(settings->status, sizeof(settings->status), "%s",
		 result ? "Could not save settings" : "Saved");
	return result;
}

static void terminal_clear_row(struct shell_terminal *terminal,
			       unsigned int row, unsigned int first)
{
	unsigned int column;

	if (row >= TERMINAL_ROWS)
		return;
	for (column = first; column < TERMINAL_COLUMNS; column++) {
		terminal->cells[row][column].character = ' ';
		terminal->cells[row][column].color = COLOR_TEXT;
	}
}

static void terminal_clear(struct shell_terminal *terminal)
{
	unsigned int row;

	for (row = 0; row < TERMINAL_ROWS; row++)
		terminal_clear_row(terminal, row, 0);
	terminal->cursor_x = 0;
	terminal->cursor_y = 0;
}

static void terminal_scroll(struct shell_terminal *terminal)
{
	size_t move_size = sizeof(terminal->cells) -
		sizeof(terminal->cells[0]);

	memmove(terminal->cells[0], terminal->cells[1], move_size);
	terminal_clear_row(terminal, TERMINAL_ROWS - 1, 0);
}

static void terminal_newline(struct shell_terminal *terminal)
{
	terminal->cursor_y++;
	if (terminal->cursor_y >= TERMINAL_ROWS) {
		terminal_scroll(terminal);
		terminal->cursor_y = TERMINAL_ROWS - 1;
	}
}

static void terminal_put_character(struct shell_terminal *terminal,
				   unsigned char character)
{
	terminal->cells[terminal->cursor_y][terminal->cursor_x].character =
		character;
	terminal->cells[terminal->cursor_y][terminal->cursor_x].color =
		terminal->color;
	terminal->cursor_x++;
	if (terminal->cursor_x >= TERMINAL_COLUMNS) {
		terminal->cursor_x = 0;
		terminal_newline(terminal);
	}
}

static void terminal_set_color(struct shell_terminal *terminal,
			       unsigned int parameter)
{
	static const enum shell_color ansi_colors[8] = {
		COLOR_TEXT, COLOR_RED, COLOR_TEAL, COLOR_GOLD,
		COLOR_BLUE, COLOR_VIOLET, COLOR_TEAL, COLOR_TEXT,
	};

	if (!parameter || parameter == 39)
		terminal->color = COLOR_TEXT;
	else if (parameter >= 30 && parameter <= 37)
		terminal->color = ansi_colors[parameter - 30];
	else if (parameter >= 90 && parameter <= 97)
		terminal->color = ansi_colors[parameter - 90];
}

static void terminal_handle_csi(struct shell_terminal *terminal,
				unsigned char command)
{
	unsigned int first = terminal->csi_params[0];
	unsigned int second = terminal->csi_count > 1 ?
		terminal->csi_params[1] : 0;
	unsigned int amount = first ? first : 1;
	unsigned int i;

	switch (command) {
	case 'A':
		terminal->cursor_y = amount > terminal->cursor_y ?
			0 : terminal->cursor_y - amount;
		break;
	case 'B':
		terminal->cursor_y += amount;
		if (terminal->cursor_y >= TERMINAL_ROWS)
			terminal->cursor_y = TERMINAL_ROWS - 1;
		break;
	case 'C':
		terminal->cursor_x += amount;
		if (terminal->cursor_x >= TERMINAL_COLUMNS)
			terminal->cursor_x = TERMINAL_COLUMNS - 1;
		break;
	case 'D':
		terminal->cursor_x = amount > terminal->cursor_x ?
			0 : terminal->cursor_x - amount;
		break;
	case 'H':
	case 'f':
		terminal->cursor_y = first ? first - 1 : 0;
		terminal->cursor_x = second ? second - 1 : 0;
		if (terminal->cursor_y >= TERMINAL_ROWS)
			terminal->cursor_y = TERMINAL_ROWS - 1;
		if (terminal->cursor_x >= TERMINAL_COLUMNS)
			terminal->cursor_x = TERMINAL_COLUMNS - 1;
		break;
	case 'J':
		if (first == 2) {
			terminal_clear(terminal);
		} else {
			terminal_clear_row(terminal, terminal->cursor_y,
					   terminal->cursor_x);
			for (i = terminal->cursor_y + 1; i < TERMINAL_ROWS; i++)
				terminal_clear_row(terminal, i, 0);
		}
		break;
	case 'K':
		terminal_clear_row(terminal, terminal->cursor_y,
				   first == 2 ? 0 : terminal->cursor_x);
		break;
	case 'm':
		for (i = 0; i < terminal->csi_count; i++)
			terminal_set_color(terminal, terminal->csi_params[i]);
		break;
	case 's':
		terminal->saved_x = terminal->cursor_x;
		terminal->saved_y = terminal->cursor_y;
		break;
	case 'u':
		terminal->cursor_x = terminal->saved_x;
		terminal->cursor_y = terminal->saved_y;
		break;
	default:
		break;
	}
}

static void terminal_feed_byte(struct shell_terminal *terminal,
			       unsigned char byte)
{
	if (terminal->parser_state == TERMINAL_ESCAPE) {
		terminal->parser_state = TERMINAL_NORMAL;
		if (byte == '[') {
			memset(terminal->csi_params, 0,
			       sizeof(terminal->csi_params));
			terminal->csi_count = 1;
			terminal->csi_private = 0;
			terminal->parser_state = TERMINAL_CSI;
		} else if (byte == '7') {
			terminal->saved_x = terminal->cursor_x;
			terminal->saved_y = terminal->cursor_y;
		} else if (byte == '8') {
			terminal->cursor_x = terminal->saved_x;
			terminal->cursor_y = terminal->saved_y;
		} else if (byte == 'c') {
			terminal_clear(terminal);
		}
		return;
	}
	if (terminal->parser_state == TERMINAL_CSI) {
		unsigned int *parameter =
			&terminal->csi_params[terminal->csi_count - 1];

		if (byte >= '0' && byte <= '9') {
			*parameter = *parameter * 10 + byte - '0';
			return;
		}
		if ((byte == '?' || byte == '>') &&
		    terminal->csi_count == 1 && !*parameter) {
			terminal->csi_private = 1;
			return;
		}
		if (byte == ';' && terminal->csi_count < TERMINAL_CSI_PARAMS) {
			terminal->csi_count++;
			return;
		}
		if (!terminal->csi_private)
			terminal_handle_csi(terminal, byte);
		terminal->parser_state = TERMINAL_NORMAL;
		return;
	}

	switch (byte) {
	case '\033':
		terminal->parser_state = TERMINAL_ESCAPE;
		break;
	case '\r':
		terminal->cursor_x = 0;
		break;
	case '\n':
		terminal_newline(terminal);
		break;
	case '\b':
		if (terminal->cursor_x)
			terminal->cursor_x--;
		break;
	case '\t':
		do {
			terminal_put_character(terminal, ' ');
		} while (terminal->cursor_x % 8);
		break;
	default:
		if (byte >= 32)
			terminal_put_character(terminal,
					       byte < 127 ? byte : '?');
		break;
	}
}

static void terminal_feed_text(struct shell_terminal *terminal,
			       const char *text)
{
	while (*text)
		terminal_feed_byte(terminal, *text++);
}

static int terminal_write(struct shell_terminal *terminal,
			  const void *data, size_t length)
{
	const uint8_t *bytes = data;

	while (length) {
		ssize_t written = write(terminal->master_fd, bytes, length);

		if (written > 0) {
			bytes += written;
			length -= written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return -1;
	}
	return 0;
}

static void wipe_secret(char *secret, size_t length)
{
	memset(secret, 0, length);
	__asm__("" : : "r"(secret) : "memory");
}

static int write_login_line(int master, const char *line)
{
	size_t length = strlen(line);
	size_t offset = 0;

	while (offset <= length) {
		char byte = offset == length ? '\n' : line[offset];
		ssize_t written = write(master, &byte, 1);

		if (written == 1) {
			offset++;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			(void)poll(NULL, 0, 10);
			continue;
		}
		return -1;
	}
	return 0;
}

static void stop_login_child(pid_t child)
{
	int status;
	int i;

	if (child <= 0)
		return;
	(void)kill(-child, SIGHUP);
	for (i = 0; i < 50; i++) {
		pid_t result = waitpid(child, &status, WNOHANG);

		if (result == child || (result < 0 && errno == ECHILD))
			return;
		(void)poll(NULL, 0, 10);
	}
	(void)kill(-child, SIGKILL);
	(void)waitpid(child, &status, 0);
}

static int authenticate_login(const char *username, char *password,
			      struct shell_terminal *terminal)
{
	static const char auth_command[] =
		"printf '\\036WIIDESK_AUTH_OK\\037\\n'";
	static const char auth_marker[] = "\036WIIDESK_AUTH_OK\037";
	struct winsize size = {
		.ws_row = 24,
		.ws_col = 80,
	};
	char output[256] = { };
	char slave_path[32];
	uint64_t deadline = shell_monotonic_ms() + LOGIN_TIMEOUT_MS;
	size_t output_length = 0;
	unsigned int number;
	int password_sent = 0;
	int authenticated = 0;
	int unlock = 0;
	int status;
	int flags;
	pid_t child;
	int master;

	master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || ioctl(master, TIOCSPTLCK, &unlock) < 0 ||
	    ioctl(master, TIOCGPTN, &number) < 0) {
		if (master >= 0)
			close(master);
		wipe_secret(password, LOGIN_PASSWORD_SIZE);
		return -1;
	}
	snprintf(slave_path, sizeof(slave_path), "/dev/pts/%u", number);
	(void)ioctl(master, TIOCSWINSZ, &size);
	child = fork();
	if (child < 0) {
		close(master);
		wipe_secret(password, LOGIN_PASSWORD_SIZE);
		return -1;
	}
	if (!child) {
		int slave;

		if (setsid() < 0)
			_exit(126);
		slave = open(slave_path, O_RDWR);
		if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) < 0)
			_exit(126);
		if (dup2(slave, STDIN_FILENO) < 0 ||
		    dup2(slave, STDOUT_FILENO) < 0 ||
		    dup2(slave, STDERR_FILENO) < 0)
			_exit(126);
		if (slave > STDERR_FILENO)
			close(slave);
		close(master);
		if (setgroups(0, NULL) < 0 || setgid(65534) < 0 ||
		    setuid(65534) < 0)
			_exit(126);
		setenv("LC_ALL", "C", 1);
		setenv("TERM", "vt100", 1);
		execl("/bin/su", "su", "-", username, (char *)NULL);
		_exit(127);
	}
	flags = fcntl(master, F_GETFL);
	if (flags >= 0)
		(void)fcntl(master, F_SETFL, flags | O_NONBLOCK);

	while (shell_monotonic_ms() < deadline) {
		char bytes[128];
		ssize_t length;
		pid_t result;

		while ((length = read(master, bytes, sizeof(bytes))) > 0) {
			size_t copy = (size_t)length;

			if (copy > sizeof(bytes))
				copy = sizeof(bytes);
			if (output_length + copy >= sizeof(output)) {
				size_t discard = output_length + copy -
					sizeof(output) + 1;

				memmove(output, output + discard,
					output_length - discard);
				output_length -= discard;
			}
			memcpy(output + output_length, bytes, copy);
			output_length += copy;
			output[output_length] = '\0';
		}
		if (!password_sent && strstr(output, "Password:")) {
			if (write_login_line(master, password) < 0 ||
			    write_login_line(master, auth_command) < 0)
				break;
			wipe_secret(password, LOGIN_PASSWORD_SIZE);
			password_sent = 1;
		}
		if (password_sent && strstr(output, auth_marker)) {
			authenticated = 1;
			break;
		}
		result = waitpid(child, &status, WNOHANG);
		if (result == child || (result < 0 && errno == ECHILD)) {
			child = 0;
			break;
		}
		(void)poll(NULL, 0, 20);
	}
	wipe_secret(password, LOGIN_PASSWORD_SIZE);
	if (authenticated) {
		if (terminal) {
			memset(terminal, 0, sizeof(*terminal));
			terminal->master_fd = master;
			terminal->child_pid = child;
			terminal_clear(terminal);
			terminal->color = COLOR_TEXT;
			(void)terminal_write(terminal, "\n", 1);
			printf("wiidesk: authenticated session pid=%d pty=%s\n",
			       child, slave_path);
		} else {
			close(master);
			stop_login_child(child);
		}
		return 1;
	}
	close(master);
	stop_login_child(child);
	return 0;
}

static void stop_terminal(struct shell_terminal *terminal)
{
	int status;
	int i;

	if (terminal->master_fd >= 0) {
		close(terminal->master_fd);
		terminal->master_fd = -1;
	}
	if (terminal->child_pid <= 0)
		return;
	(void)kill(-terminal->child_pid, SIGHUP);
	for (i = 0; i < 50; i++) {
		pid_t result = waitpid(terminal->child_pid, &status, WNOHANG);

		if (result == terminal->child_pid ||
		    (result < 0 && errno == ECHILD)) {
			terminal->child_pid = 0;
			return;
		}
		(void)poll(NULL, 0, 10);
	}
	(void)kill(-terminal->child_pid, SIGKILL);
	(void)waitpid(terminal->child_pid, &status, 0);
	terminal->child_pid = 0;
}

static int drain_terminal(struct shell_terminal *terminal)
{
	uint8_t bytes[512];
	int changed = 0;
	ssize_t length;

	while ((length = read(terminal->master_fd, bytes, sizeof(bytes))) > 0) {
		ssize_t i;

		for (i = 0; i < length; i++)
			terminal_feed_byte(terminal, bytes[i]);
		changed = 1;
	}
	if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
	    errno != EIO)
		return -1;
	if (terminal->child_pid > 0) {
		int status;
		pid_t result = waitpid(terminal->child_pid, &status, WNOHANG);

		if (result == terminal->child_pid) {
			terminal->child_pid = 0;
			terminal->child_exited = 1;
			terminal_feed_text(terminal,
					   "\r\n[process exited]\r\n");
			changed = 1;
		}
	}
	if (terminal->child_exited && terminal->master_fd >= 0) {
		close(terminal->master_fd);
		terminal->master_fd = -1;
	}
	return changed;
}

static int file_entry_compare(const void *left, const void *right)
{
	const struct shell_file_entry *a = left;
	const struct shell_file_entry *b = right;

	if (!strcmp(a->name, ".."))
		return -1;
	if (!strcmp(b->name, ".."))
		return 1;
	if (a->directory != b->directory)
		return b->directory - a->directory;
	return strcmp(a->name, b->name);
}

static int files_load(struct shell_files *files, const char *path)
{
	struct shell_file_entry entries[FILES_ENTRY_COUNT];
	struct dirent *directory_entry;
	unsigned int count = 0;
	int truncated = 0;
	DIR *directory;

	directory = opendir(path);
	if (!directory) {
		snprintf(files->status, sizeof(files->status),
			 "Open failed: %s", strerror(errno));
		return -1;
	}
	if (strcmp(path, "/")) {
		strcpy(entries[count].name, "..");
		entries[count++].directory = 1;
	}
	while ((directory_entry = readdir(directory))) {
		struct shell_file_entry *entry;
		char full_path[FILES_PATH_SIZE + NAME_MAX + 2];
		struct stat status;
		size_t name_length;

		if (!strcmp(directory_entry->d_name, ".") ||
		    !strcmp(directory_entry->d_name, ".."))
			continue;
		if (count == FILES_ENTRY_COUNT) {
			truncated = 1;
			break;
		}
		entry = &entries[count++];
		name_length = strnlen(directory_entry->d_name, NAME_MAX);
		memcpy(entry->name, directory_entry->d_name, name_length);
		entry->name[name_length] = '\0';
		entry->directory = directory_entry->d_type == DT_DIR;
		if (directory_entry->d_type != DT_UNKNOWN &&
		    directory_entry->d_type != DT_LNK)
			continue;
		if (!strcmp(path, "/"))
			snprintf(full_path, sizeof(full_path), "/%s", entry->name);
		else
			snprintf(full_path, sizeof(full_path), "%s/%s", path,
				 entry->name);
		if (!stat(full_path, &status))
			entry->directory = S_ISDIR(status.st_mode);
	}
	closedir(directory);
	qsort(entries, count, sizeof(entries[0]), file_entry_compare);
	memcpy(files->entries, entries, count * sizeof(entries[0]));
	strncpy(files->path, path, sizeof(files->path) - 1);
	files->path[sizeof(files->path) - 1] = '\0';
	files->count = count;
	files->selected = 0;
	files->scroll = 0;
	files->last_clicked = -1;
	files->loaded = 1;
	if (truncated)
		snprintf(files->status, sizeof(files->status),
			 "%u+ entries", count);
	else
		snprintf(files->status, sizeof(files->status),
			 "%u entr%s", count, count == 1 ? "y" : "ies");
	return 0;
}

static void files_parent_path(const char *path, char *parent, size_t size)
{
	char *separator;

	strncpy(parent, path, size - 1);
	parent[size - 1] = '\0';
	separator = strrchr(parent, '/');
	if (!separator || separator == parent)
		strcpy(parent, "/");
	else
		*separator = '\0';
}

static int files_open_selected(struct shell_files *files)
{
	const struct shell_file_entry *entry;
	char target[FILES_PATH_SIZE];

	if (!files->count || files->selected >= files->count)
		return 0;
	entry = &files->entries[files->selected];
	if (!entry->directory) {
		snprintf(files->status, sizeof(files->status), "File: %.38s",
			 entry->name);
		return 1;
	}
	if (!strcmp(entry->name, "..")) {
		files_parent_path(files->path, target, sizeof(target));
	} else if (!strcmp(files->path, "/")) {
		snprintf(target, sizeof(target), "/%s", entry->name);
	} else if (snprintf(target, sizeof(target), "%s/%s", files->path,
			    entry->name) >= (int)sizeof(target)) {
		snprintf(files->status, sizeof(files->status), "Path too long");
		return 1;
	}
	(void)files_load(files, target);
	return 1;
}

static int files_move_selection(struct shell_files *files, int movement)
{
	int selected;

	if (!files->count)
		return 0;
	selected = files->selected + movement;
	if (selected < 0)
		selected = 0;
	if (selected >= (int)files->count)
		selected = files->count - 1;
	if (selected == (int)files->selected)
		return 0;
	files->selected = selected;
	if (files->selected < files->scroll)
		files->scroll = files->selected;
	else if (files->selected >= files->scroll + FILES_VISIBLE_ROWS)
		files->scroll = files->selected - FILES_VISIBLE_ROWS + 1;
	return 1;
}

static int read_u64_file(const char *path, uint64_t *value)
{
	unsigned long long parsed;
	FILE *file;
	int result;

	file = fopen(path, "r");
	if (!file)
		return -1;
	result = fscanf(file, "%llu", &parsed);
	fclose(file);
	if (result != 1)
		return -1;
	*value = parsed;
	return 0;
}

static void system_read_cpu(struct shell_system *system)
{
	unsigned long long user = 0;
	unsigned long long nice = 0;
	unsigned long long kernel = 0;
	unsigned long long idle = 0;
	unsigned long long iowait = 0;
	unsigned long long irq = 0;
	unsigned long long softirq = 0;
	unsigned long long steal = 0;
	uint64_t total;
	uint64_t idle_total;
	FILE *file;

	file = fopen("/proc/stat", "r");
	if (!file)
		return;
	if (fscanf(file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &user, &nice, &kernel, &idle, &iowait, &irq, &softirq,
		   &steal) < 4) {
		fclose(file);
		return;
	}
	fclose(file);
	total = user + nice + kernel + idle + iowait + irq + softirq + steal;
	idle_total = idle + iowait;
	if (system->cpu_total && total > system->cpu_total) {
		uint64_t total_delta = total - system->cpu_total;
		uint64_t idle_delta = idle_total - system->cpu_idle;

		if (idle_delta > total_delta)
			idle_delta = total_delta;
		system->cpu_percent =
			(total_delta - idle_delta) * 100 / total_delta;
	}
	system->cpu_total = total;
	system->cpu_idle = idle_total;
}

static void system_read_memory(struct shell_system *system)
{
	char line[128];
	FILE *file;

	system->memory_total_kb = 0;
	system->memory_available_kb = 0;
	file = fopen("/proc/meminfo", "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file)) {
		unsigned long value;

		if (sscanf(line, "MemTotal: %lu kB", &value) == 1)
			system->memory_total_kb = value;
		else if (sscanf(line, "MemAvailable: %lu kB", &value) == 1)
			system->memory_available_kb = value;
	}
	fclose(file);
}

static int system_read_operstate(const char *name)
{
	char path[128];
	char state[16];
	FILE *file;

	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name);
	file = fopen(path, "r");
	if (!file)
		return 0;
	state[0] = '\0';
	if (fscanf(file, "%15s", state) != 1)
		state[0] = '\0';
	fclose(file);
	return !strcmp(state, "up");
}

static void system_copy_network_name(char *destination, const char *source)
{
	size_t length = strnlen(source, SYSTEM_NETWORK_NAME - 1);

	memcpy(destination, source, length);
	destination[length] = '\0';
}

static void system_read_network(struct shell_system *system)
{
	struct dirent *entry;
	char fallback[SYSTEM_NETWORK_NAME] = "";
	DIR *directory;

	system->network_name[0] = '\0';
	system->network_up = 0;
	system->network_rx_bytes = 0;
	system->network_tx_bytes = 0;
	directory = opendir("/sys/class/net");
	if (!directory)
		return;
	while ((entry = readdir(directory))) {
		char candidate[SYSTEM_NETWORK_NAME];

		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "lo"))
			continue;
		system_copy_network_name(candidate, entry->d_name);
		if (!fallback[0])
			system_copy_network_name(fallback, candidate);
		if (system_read_operstate(candidate)) {
			system_copy_network_name(system->network_name, candidate);
			system->network_up = 1;
			break;
		}
	}
	closedir(directory);
	if (!system->network_name[0])
		strcpy(system->network_name, fallback);
	if (system->network_name[0]) {
		char path[128];

		snprintf(path, sizeof(path),
			 "/sys/class/net/%s/statistics/rx_bytes",
			 system->network_name);
		(void)read_u64_file(path, &system->network_rx_bytes);
		snprintf(path, sizeof(path),
			 "/sys/class/net/%s/statistics/tx_bytes",
			 system->network_name);
		(void)read_u64_file(path, &system->network_tx_bytes);
	}
}

static void refresh_system(struct shell_system *system)
{
	unsigned long long uptime;
	FILE *file;

	system_read_cpu(system);
	system_read_memory(system);
	system_read_network(system);
	file = fopen("/proc/uptime", "r");
	if (file) {
		if (fscanf(file, "%llu", &uptime) == 1)
			system->uptime_seconds = uptime;
		fclose(file);
	}
	system->gx_loaded = !access("/sys/module/gcn_gx", F_OK);
	system->drm_present = !access("/sys/class/drm/card0", F_OK);
	system->valid = 1;
}

#ifdef WII_HAVE_VNC
static int handle_files_key(struct shell_files *files, unsigned int key);
static int handle_key(struct shell_state *shell, unsigned int key);
static int handle_key_event(struct shell_state *shell, unsigned int key,
			    int value);
static int update_pointer(struct shell_state *shell, int delta_x, int delta_y);
static int handle_pointer_button(struct shell_state *shell, int pressed);

static unsigned int vnc_keysym_key(rfbKeySym keysym)
{
	static const unsigned int letter_keys[] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
		KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
		KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
		KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	static const unsigned int digit_keys[] = {
		KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
		KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	};
	static const unsigned int function_keys[] = {
		KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
		KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
	};

	if (keysym >= XK_A && keysym <= XK_Z)
		keysym += XK_a - XK_A;
	if (keysym >= XK_a && keysym <= XK_z)
		return letter_keys[keysym - XK_a];
	if (keysym >= XK_0 && keysym <= XK_9)
		return digit_keys[keysym - XK_0];
	if (keysym >= XK_F1 && keysym <= XK_F10)
		return function_keys[keysym - XK_F1];
	if (keysym == XK_F11)
		return KEY_F11;
	if (keysym == XK_F12)
		return KEY_F12;

	switch (keysym) {
	case XK_exclam: return KEY_1;
	case XK_at: return KEY_2;
	case XK_numbersign: return KEY_3;
	case XK_dollar: return KEY_4;
	case XK_percent: return KEY_5;
	case XK_asciicircum: return KEY_6;
	case XK_ampersand: return KEY_7;
	case XK_asterisk: return KEY_8;
	case XK_parenleft: return KEY_9;
	case XK_parenright: return KEY_0;
	case XK_BackSpace: return KEY_BACKSPACE;
	case XK_Tab: return KEY_TAB;
	case XK_Return: return KEY_ENTER;
	case XK_Escape: return KEY_ESC;
	case XK_Delete: return KEY_DELETE;
	case XK_Home: return KEY_HOME;
	case XK_Left: return KEY_LEFT;
	case XK_Up: return KEY_UP;
	case XK_Right: return KEY_RIGHT;
	case XK_Down: return KEY_DOWN;
	case XK_Page_Up: return KEY_PAGEUP;
	case XK_Page_Down: return KEY_PAGEDOWN;
	case XK_End: return KEY_END;
	case XK_KP_Enter: return KEY_KPENTER;
	case XK_Shift_L: return KEY_LEFTSHIFT;
	case XK_Shift_R: return KEY_RIGHTSHIFT;
	case XK_Control_L: return KEY_LEFTCTRL;
	case XK_Control_R: return KEY_RIGHTCTRL;
	case XK_Caps_Lock: return KEY_CAPSLOCK;
	case XK_space: return KEY_SPACE;
	case XK_minus: return KEY_MINUS;
	case XK_underscore: return KEY_MINUS;
	case XK_equal: return KEY_EQUAL;
	case XK_plus: return KEY_EQUAL;
	case XK_bracketleft: return KEY_LEFTBRACE;
	case XK_braceleft: return KEY_LEFTBRACE;
	case XK_bracketright: return KEY_RIGHTBRACE;
	case XK_braceright: return KEY_RIGHTBRACE;
	case XK_backslash: return KEY_BACKSLASH;
	case XK_bar: return KEY_BACKSLASH;
	case XK_semicolon: return KEY_SEMICOLON;
	case XK_colon: return KEY_SEMICOLON;
	case XK_apostrophe: return KEY_APOSTROPHE;
	case XK_quotedbl: return KEY_APOSTROPHE;
	case XK_grave: return KEY_GRAVE;
	case XK_asciitilde: return KEY_GRAVE;
	case XK_comma: return KEY_COMMA;
	case XK_less: return KEY_COMMA;
	case XK_period: return KEY_DOT;
	case XK_greater: return KEY_DOT;
	case XK_slash: return KEY_SLASH;
	case XK_question: return KEY_SLASH;
	default: return KEY_RESERVED;
	}
}

static int vnc_keysym_needs_shift(rfbKeySym keysym)
{
	return (keysym >= XK_A && keysym <= XK_Z) || keysym == XK_exclam ||
		keysym == XK_at || keysym == XK_numbersign || keysym == XK_dollar ||
		keysym == XK_percent || keysym == XK_asciicircum ||
		keysym == XK_ampersand || keysym == XK_asterisk ||
		keysym == XK_parenleft || keysym == XK_parenright ||
		keysym == XK_underscore || keysym == XK_plus ||
		keysym == XK_braceleft || keysym == XK_braceright ||
		keysym == XK_bar || keysym == XK_colon ||
		keysym == XK_quotedbl || keysym == XK_asciitilde ||
		keysym == XK_less || keysym == XK_greater ||
		keysym == XK_question;
}

static void vnc_key_event(rfbBool down, rfbKeySym key, rfbClientPtr client)
{
	struct shell_state *shell = client->screen->screenData;
	unsigned int input_key = vnc_keysym_key(key);
	int force_shift = down && vnc_keysym_needs_shift(key) &&
		!shell->shift_down;

	if (input_key == KEY_RESERVED)
		return;
	if (force_shift)
		(void)handle_key_event(shell, KEY_LEFTSHIFT, 1);
	if (input_key == KEY_LEFTSHIFT || input_key == KEY_RIGHTSHIFT)
		shell->vnc.shift_down = down;
	if (input_key == KEY_LEFTCTRL || input_key == KEY_RIGHTCTRL)
		shell->vnc.control_down = down;
	shell->vnc.changed |= handle_key_event(shell, input_key, down ? 1 : 0);
	if (force_shift)
		(void)handle_key_event(shell, KEY_LEFTSHIFT, 0);
}

static void vnc_pointer_event(int buttons, int x, int y,
			      rfbClientPtr client)
{
	struct shell_state *shell = client->screen->screenData;
	int previous = shell->vnc.buttons;

	if (buttons != previous)
		shell->last_input_ms = shell_monotonic_ms();
	shell->vnc.changed |= update_pointer(shell, x - shell->pointer_x,
					      y - shell->pointer_y);
	if ((buttons ^ previous) & 1)
		shell->vnc.changed |= handle_pointer_button(shell, buttons & 1);
	if ((buttons & 8) && !(previous & 8)) {
		if (shell->focused == SHELL_APP_FILES)
			shell->vnc.changed |= handle_files_key(&shell->files,
							    KEY_UP);
		else
			shell->vnc.changed |= handle_key(shell, KEY_UP);
	}
	if ((buttons & 16) && !(previous & 16)) {
		if (shell->focused == SHELL_APP_FILES)
			shell->vnc.changed |= handle_files_key(&shell->files,
							    KEY_DOWN);
		else
			shell->vnc.changed |= handle_key(shell, KEY_DOWN);
	}
	shell->vnc.buttons = buttons;
}

static void vnc_client_gone(rfbClientPtr client)
{
	struct shell_state *shell = client->screen->screenData;

	if (shell->vnc.buttons & 1)
		shell->vnc.changed |= handle_pointer_button(shell, 0);
	if (shell->vnc.shift_down)
		(void)handle_key_event(shell, KEY_LEFTSHIFT, 0);
	if (shell->vnc.control_down)
		(void)handle_key_event(shell, KEY_LEFTCTRL, 0);
	shell->vnc.buttons = 0;
	shell->vnc.shift_down = 0;
	shell->vnc.control_down = 0;
}

static enum rfbNewClientAction vnc_new_client(rfbClientPtr client)
{
	client->clientGoneHook = vnc_client_gone;
	return RFB_CLIENT_ACCEPT;
}

static int start_vnc(struct shell_state *shell, struct test_buffer *buffer)
{
	char program[] = "wiidesk";
	char *arguments[2];
	int argument_count = 1;
	rfbScreenInfoPtr screen;

	if (buffer->create.pitch != TEST_WIDTH * sizeof(uint16_t))
		return -1;
	arguments[0] = program;
	arguments[1] = NULL;
	screen = rfbGetScreen(&argument_count, arguments, TEST_WIDTH,
			      TEST_HEIGHT, 5, 3, 2);
	if (!screen)
		return -1;
	screen->desktopName = "WiiDesk";
	screen->frameBuffer = buffer->map;
	screen->screenData = shell;
	screen->listenInterface = htonl(INADDR_LOOPBACK);
	screen->port = 5900;
	screen->alwaysShared = TRUE;
	screen->deferPtrUpdateTime = 16;
	screen->kbdAddEvent = vnc_key_event;
	screen->ptrAddEvent = vnc_pointer_event;
	screen->newClientHook = vnc_new_client;
	screen->serverFormat.bitsPerPixel = 16;
	screen->serverFormat.depth = 16;
	screen->serverFormat.bigEndian = TRUE;
	screen->serverFormat.trueColour = TRUE;
	screen->serverFormat.redMax = 31;
	screen->serverFormat.greenMax = 63;
	screen->serverFormat.blueMax = 31;
	screen->serverFormat.redShift = 11;
	screen->serverFormat.greenShift = 5;
	screen->serverFormat.blueShift = 0;
	rfbInitServer(screen);
	if (screen->listenSock == RFB_INVALID_SOCKET) {
		rfbScreenCleanup(screen);
		return -1;
	}
	shell->vnc.screen = screen;
	printf("wiidesk: interactive VNC on 127.0.0.1:5900\n");
	return 0;
}

static int process_vnc(struct shell_state *shell)
{
	int changed;

	if (!shell->vnc.screen)
		return 0;
	shell->vnc.changed = 0;
	(void)rfbProcessEvents(shell->vnc.screen, 0);
	changed = shell->vnc.changed;
	shell->vnc.changed = 0;
	return changed;
}

static void update_vnc(struct shell_state *shell, struct test_buffer *buffer)
{
	const uint16_t *previous;
	const uint16_t *current;
	unsigned int stride;
	int minimum_x = TEST_WIDTH;
	int minimum_y = TEST_HEIGHT;
	int maximum_x = -1;
	int maximum_y = -1;
	int y;

	if (!shell->vnc.screen)
		return;
	previous = (const uint16_t *)shell->vnc.screen->frameBuffer;
	current = buffer->map;
	stride = buffer->create.pitch / sizeof(*current);
	for (y = 0; y < TEST_HEIGHT; y++) {
		const uint16_t *old_row = previous + y * stride;
		const uint16_t *new_row = current + y * stride;
		int first;
		int last;

		if (!memcmp(old_row, new_row,
			    TEST_WIDTH * sizeof(*new_row)))
			continue;
		for (first = 0; first < TEST_WIDTH; first++)
			if (old_row[first] != new_row[first])
				break;
		for (last = TEST_WIDTH - 1; last > first; last--)
			if (old_row[last] != new_row[last])
				break;
		if (first < minimum_x)
			minimum_x = first;
		if (last > maximum_x)
			maximum_x = last;
		if (minimum_y == TEST_HEIGHT)
			minimum_y = y;
		maximum_y = y;
	}
	shell->vnc.screen->frameBuffer = buffer->map;
	if (maximum_x >= minimum_x)
		rfbMarkRectAsModified(shell->vnc.screen, minimum_x, minimum_y,
				      maximum_x + 1, maximum_y + 1);
}

static void stop_vnc(struct shell_state *shell)
{
	if (!shell->vnc.screen)
		return;
	rfbShutdownServer(shell->vnc.screen, TRUE);
	rfbScreenCleanup(shell->vnc.screen);
	shell->vnc.screen = NULL;
}
#else
static int start_vnc(struct shell_state *shell, struct test_buffer *buffer)
{
	(void)shell;
	(void)buffer;
	return 0;
}

static int process_vnc(struct shell_state *shell)
{
	(void)shell;
	return 0;
}

static void update_vnc(struct shell_state *shell, struct test_buffer *buffer)
{
	(void)shell;
	(void)buffer;
}

static void stop_vnc(struct shell_state *shell)
{
	(void)shell;
}
#endif

static void fill_rect(struct test_buffer *buffer, int x, int y,
		      int width, int height, uint16_t color)
{
	int row;
	int column;

	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > TEST_WIDTH)
		width = TEST_WIDTH - x;
	if (y + height > TEST_HEIGHT)
		height = TEST_HEIGHT - y;
	if (width <= 0 || height <= 0)
		return;

	for (row = 0; row < height; row++) {
		uint16_t *pixels = (uint16_t *)((uint8_t *)buffer->map +
			(size_t)(y + row) * buffer->create.pitch) + x;

		for (column = 0; column < width; column++)
			pixels[column] = color;
	}
}

static void stroke_rect(struct test_buffer *buffer, int x, int y,
			int width, int height, uint16_t color)
{
	fill_rect(buffer, x, y, width, 1, color);
	fill_rect(buffer, x, y + height - 1, width, 1, color);
	fill_rect(buffer, x, y, 1, height, color);
	fill_rect(buffer, x + width - 1, y, 1, height, color);
}

static void draw_character(struct test_buffer *buffer, int x, int y,
			   unsigned char character, uint16_t foreground)
{
	const uint8_t *glyph = font_vga_8x16.data +
		character * SHELL_FONT_HEIGHT;
	unsigned int glyph_y;

	for (glyph_y = 0; glyph_y < SHELL_FONT_HEIGHT; glyph_y++) {
		uint16_t *pixels;
		unsigned int glyph_x;

		if (y + (int)glyph_y < 0 || y + (int)glyph_y >= TEST_HEIGHT)
			continue;
		pixels = (uint16_t *)((uint8_t *)buffer->map +
			(size_t)(y + glyph_y) * buffer->create.pitch);
		for (glyph_x = 0; glyph_x < SHELL_FONT_WIDTH; glyph_x++) {
			int pixel_x = x + glyph_x;

			if (pixel_x >= 0 && pixel_x < TEST_WIDTH &&
			    (glyph[glyph_y] & BIT(7 - glyph_x)))
				pixels[pixel_x] = foreground;
		}
	}
}

static void draw_text(struct test_buffer *buffer, int x, int y,
		      const char *text, uint16_t color)
{
	while (*text) {
		draw_character(buffer, x, y, (unsigned char)*text++, color);
		x += SHELL_FONT_WIDTH;
	}
}

static void draw_character_scaled(struct test_buffer *buffer, int x, int y,
				  unsigned char character, int scale,
				  uint16_t color)
{
	const uint8_t *glyph = font_vga_8x16.data +
		character * SHELL_FONT_HEIGHT;
	unsigned int glyph_y;

	for (glyph_y = 0; glyph_y < SHELL_FONT_HEIGHT; glyph_y++) {
		unsigned int glyph_x;

		for (glyph_x = 0; glyph_x < SHELL_FONT_WIDTH; glyph_x++)
			if (glyph[glyph_y] & BIT(7 - glyph_x))
				fill_rect(buffer, x + glyph_x * scale,
					  y + glyph_y * scale, scale, scale, color);
	}
}

static void draw_text_scaled(struct test_buffer *buffer, int x, int y,
			     const char *text, int scale, uint16_t color)
{
	while (*text) {
		draw_character_scaled(buffer, x, y, (unsigned char)*text++, scale,
				      color);
		x += SHELL_FONT_WIDTH * scale;
	}
}

static void draw_splash(struct test_buffer *buffer)
{
	fill_rect(buffer, 0, 0, TEST_WIDTH, TEST_HEIGHT, rgb565(COLOR_SKY));
	fill_rect(buffer, 0, TEST_HEIGHT - 72, TEST_WIDTH, 72,
		  rgb565(COLOR_GREEN));
	fill_rect(buffer, 0, TEST_HEIGHT - 72, TEST_WIDTH, 4,
		  rgb565(COLOR_TERMINAL));
	draw_text_scaled(buffer, 239, 176, "WiiDesk", 3,
			 rgb565(COLOR_TERMINAL));
	draw_text_scaled(buffer, 235, 172, "WiiDesk", 3,
			 rgb565(COLOR_TEXT));
	draw_text(buffer, 260, 232, "Wii Linux NGX", rgb565(COLOR_TERMINAL));
	draw_text(buffer, 280, 360, "Starting...", rgb565(COLOR_TEXT));
}

static void draw_wallpaper(struct test_buffer *buffer,
			   const struct shell_state *shell)
{
	int y;

	switch (shell->settings.wallpaper % SETTINGS_WALLPAPER_COUNT) {
	case 0:
		fill_rect(buffer, 0, SHELL_WORKSPACE_TOP, TEST_WIDTH,
			  SHELL_WORKSPACE_BOTTOM - SHELL_WORKSPACE_TOP,
			  rgb565(COLOR_SKY));
		fill_rect(buffer, 0, 312, TEST_WIDTH, 144, rgb565(COLOR_TEAL));
		fill_rect(buffer, 0, 354, TEST_WIDTH, 102, rgb565(COLOR_GREEN));
		for (y = 0; y < 5; y++)
			fill_rect(buffer, 420 + y * 18, 92 + (y & 1) * 7,
				  44, 3, rgb565(COLOR_TEXT));
		break;
	case 1:
		fill_rect(buffer, 0, SHELL_WORKSPACE_TOP, TEST_WIDTH,
			  SHELL_WORKSPACE_BOTTOM - SHELL_WORKSPACE_TOP,
			  rgb565(COLOR_VIOLET));
		fill_rect(buffer, 0, 248, TEST_WIDTH, 208, rgb565(COLOR_RED));
		fill_rect(buffer, 0, 322, TEST_WIDTH, 134, rgb565(COLOR_GOLD));
		fill_rect(buffer, 388, 90, 72, 72, rgb565(COLOR_GOLD));
		fill_rect(buffer, 398, 100, 52, 52, rgb565(COLOR_TEXT));
		break;
	default:
		fill_rect(buffer, 0, SHELL_WORKSPACE_TOP, TEST_WIDTH,
			  SHELL_WORKSPACE_BOTTOM - SHELL_WORKSPACE_TOP,
			  rgb565(COLOR_DESKTOP));
		for (y = SHELL_WORKSPACE_TOP; y < SHELL_WORKSPACE_BOTTOM; y += 32)
			fill_rect(buffer, 0, y, TEST_WIDTH, 1, rgb565(COLOR_BORDER));
		for (y = 0; y < TEST_WIDTH; y += 32)
			fill_rect(buffer, y, SHELL_WORKSPACE_TOP, 1,
				  SHELL_WORKSPACE_BOTTOM - SHELL_WORKSPACE_TOP,
				  rgb565(COLOR_BORDER));
		fill_rect(buffer, 390, 96, 180, 4, rgb565(shell_accent(shell)));
		fill_rect(buffer, 470, 100, 4, 180, rgb565(shell_accent(shell)));
		break;
	}
}

static void draw_app_symbol(struct test_buffer *buffer, enum shell_app app,
			    int x, int y, enum shell_color color)
{
	fill_rect(buffer, x, y, 34, 34, rgb565(COLOR_PANEL));
	stroke_rect(buffer, x, y, 34, 34, rgb565(color));
	switch (app) {
	case SHELL_APP_TERMINAL:
		draw_text(buffer, x + 5, y + 9, ">_", rgb565(color));
		break;
	case SHELL_APP_FILES:
		fill_rect(buffer, x + 5, y + 11, 24, 17, rgb565(color));
		fill_rect(buffer, x + 7, y + 7, 10, 6, rgb565(color));
		fill_rect(buffer, x + 7, y + 15, 20, 2, rgb565(COLOR_PANEL));
		break;
	case SHELL_APP_SYSTEM:
		fill_rect(buffer, x + 6, y + 20, 4, 8, rgb565(color));
		fill_rect(buffer, x + 14, y + 14, 4, 14, rgb565(color));
		fill_rect(buffer, x + 22, y + 8, 4, 20, rgb565(color));
		break;
	case SHELL_APP_SETTINGS:
		fill_rect(buffer, x + 6, y + 8, 22, 2, rgb565(color));
		fill_rect(buffer, x + 6, y + 16, 22, 2, rgb565(color));
		fill_rect(buffer, x + 6, y + 24, 22, 2, rgb565(color));
		fill_rect(buffer, x + 11, y + 5, 4, 8, rgb565(COLOR_TEXT));
		fill_rect(buffer, x + 21, y + 13, 4, 8, rgb565(COLOR_TEXT));
		fill_rect(buffer, x + 14, y + 21, 4, 8, rgb565(COLOR_TEXT));
		break;
	default:
		break;
	}
}

static void draw_desktop_icons(struct test_buffer *buffer,
			       const struct shell_state *shell)
{
	unsigned int i;

	for (i = 0; i < SHELL_APP_COUNT; i++) {
		int y = SHELL_DESKTOP_ICON_Y + i * SHELL_DESKTOP_ICON_HEIGHT;

		if (shell->desktop_selected == (int)i) {
			fill_rect(buffer, SHELL_DESKTOP_ICON_X - 6, y - 6,
				  SHELL_DESKTOP_ICON_WIDTH, 62,
				  rgb565(COLOR_PANEL));
			stroke_rect(buffer, SHELL_DESKTOP_ICON_X - 6, y - 6,
				    SHELL_DESKTOP_ICON_WIDTH, 62,
				    rgb565(shell_accent(shell)));
		}
		draw_app_symbol(buffer, i, SHELL_DESKTOP_ICON_X + 12, y,
				app_accents[i]);
		draw_text(buffer, SHELL_DESKTOP_ICON_X, y + 40, app_titles[i],
			  rgb565(COLOR_TEXT));
	}
}

static void draw_login_field(struct test_buffer *buffer,
			     const struct shell_login *login,
			     enum login_field field, int y, const char *text)
{
	uint16_t border = login->field == field ? rgb565(COLOR_TEAL) :
		rgb565(COLOR_BORDER);

	fill_rect(buffer, LOGIN_FIELD_X, y, LOGIN_FIELD_WIDTH, 34,
		  rgb565(COLOR_TERMINAL));
	stroke_rect(buffer, LOGIN_FIELD_X, y, LOGIN_FIELD_WIDTH, 34, border);
	draw_text(buffer, LOGIN_FIELD_X + 10, y + 9, text, rgb565(COLOR_TEXT));
}

static void draw_login(struct test_buffer *buffer,
		       const struct shell_state *shell)
{
	char password[LOGIN_PASSWORD_SIZE];
	char status[35];
	const char *title = shell->view == SHELL_VIEW_LOCK ? "Locked" : "WiiDesk";
	int title_x = shell->view == SHELL_VIEW_LOCK ? 272 : 264;
	size_t i;

	fill_rect(buffer, 0, 0, TEST_WIDTH, TEST_HEIGHT, rgb565(COLOR_SKY));
	fill_rect(buffer, 0, TEST_HEIGHT - 56, TEST_WIDTH, 56,
		  rgb565(COLOR_GREEN));
	fill_rect(buffer, LOGIN_PANEL_X + 6, LOGIN_PANEL_Y + 6,
		  LOGIN_PANEL_WIDTH, LOGIN_PANEL_HEIGHT, rgb565(COLOR_TERMINAL));
	fill_rect(buffer, LOGIN_PANEL_X, LOGIN_PANEL_Y, LOGIN_PANEL_WIDTH,
		  LOGIN_PANEL_HEIGHT, rgb565(COLOR_PANEL));
	stroke_rect(buffer, LOGIN_PANEL_X, LOGIN_PANEL_Y, LOGIN_PANEL_WIDTH,
		    LOGIN_PANEL_HEIGHT, rgb565(COLOR_BORDER));
	draw_text_scaled(buffer, title_x, 100, title, 2, rgb565(COLOR_TEXT));
	draw_text(buffer, LOGIN_FIELD_X, 156, "User", rgb565(COLOR_MUTED));
	draw_login_field(buffer, &shell->login, LOGIN_FIELD_USERNAME,
			 LOGIN_USERNAME_Y, shell->login.username);
	draw_text(buffer, LOGIN_FIELD_X, 222, "Password", rgb565(COLOR_MUTED));
	for (i = 0; i < shell->login.password_length &&
	     i + 1 < sizeof(password); i++)
		password[i] = '*';
	password[i] = '\0';
	draw_login_field(buffer, &shell->login, LOGIN_FIELD_PASSWORD,
			 LOGIN_PASSWORD_Y, password);
	fill_rect(buffer, LOGIN_BUTTON_X, LOGIN_BUTTON_Y, LOGIN_BUTTON_WIDTH,
		  LOGIN_BUTTON_HEIGHT, rgb565(COLOR_TEAL));
	stroke_rect(buffer, LOGIN_BUTTON_X, LOGIN_BUTTON_Y, LOGIN_BUTTON_WIDTH,
		    LOGIN_BUTTON_HEIGHT, rgb565(COLOR_TERMINAL));
	draw_text(buffer, LOGIN_BUTTON_X + 42, LOGIN_BUTTON_Y + 10,
		  shell->view == SHELL_VIEW_LOCK ? "Unlock" : "Sign in",
		  rgb565(COLOR_TERMINAL));
	snprintf(status, sizeof(status), "%.34s", shell->login.status);
	draw_text(buffer, LOGIN_FIELD_X, 360, status,
		  rgb565(shell->login.auth_state == LOGIN_AUTH_RUNNING ?
			 COLOR_GOLD : COLOR_MUTED));
}

static int menu_height(void)
{
	return SHELL_MENU_HEADER_HEIGHT +
		SHELL_MENU_ITEM_COUNT * SHELL_MENU_ROW_HEIGHT +
		SHELL_MENU_PADDING;
}

static int menu_y(void)
{
	return SHELL_WORKSPACE_BOTTOM - menu_height();
}

static void draw_start_menu(struct test_buffer *buffer,
			    const struct shell_state *shell)
{
	unsigned int i;
	int top;

	if (!shell->menu_open)
		return;
	top = menu_y();
	fill_rect(buffer, SHELL_MENU_X + 4, top + 4, SHELL_MENU_WIDTH,
		  menu_height() - 4, rgb565(COLOR_TERMINAL));
	fill_rect(buffer, SHELL_MENU_X, top, SHELL_MENU_WIDTH, menu_height(),
		  rgb565(COLOR_PANEL));
	stroke_rect(buffer, SHELL_MENU_X, top, SHELL_MENU_WIDTH, menu_height(),
		    rgb565(COLOR_TEAL));
	fill_rect(buffer, SHELL_MENU_X, top, SHELL_MENU_WIDTH,
		  SHELL_MENU_HEADER_HEIGHT, rgb565(COLOR_BORDER));
	fill_rect(buffer, SHELL_MENU_X, top, 4, SHELL_MENU_HEADER_HEIGHT,
		  rgb565(COLOR_RED));
	draw_text(buffer, SHELL_MENU_X + 14, top + 9, "WiiDesk",
		  rgb565(COLOR_TEXT));
	for (i = 0; i < SHELL_APP_COUNT; i++) {
		int y = top + SHELL_MENU_HEADER_HEIGHT +
			i * SHELL_MENU_ROW_HEIGHT;

		if (shell->selected == i) {
			fill_rect(buffer, SHELL_MENU_X + 6, y + 3,
				  SHELL_MENU_WIDTH - 12, SHELL_MENU_ROW_HEIGHT - 4,
				  rgb565(COLOR_BORDER));
			fill_rect(buffer, SHELL_MENU_X + 6, y + 3, 3,
				  SHELL_MENU_ROW_HEIGHT - 4,
				  rgb565(app_accents[i]));
		}
		fill_rect(buffer, SHELL_MENU_X + 18, y + 12, 16, 16,
			  rgb565(app_accents[i]));
		draw_text(buffer, SHELL_MENU_X + 46, y + 12, app_titles[i],
			  rgb565(COLOR_TEXT));
		if (shell->windows[i].visible)
			fill_rect(buffer, SHELL_MENU_X + SHELL_MENU_WIDTH - 20,
				  y + 17, 6, 6,
				  rgb565(app_accents[i]));
	}
	{
		static const char *const titles[] = { "Lock", "Log out" };
		static const enum shell_color colors[] = { COLOR_GOLD, COLOR_RED };
		unsigned int action;

		for (action = 0; action < ARRAY_SIZE(titles); action++) {
			unsigned int item = SHELL_MENU_LOCK + action;
			int y = top + SHELL_MENU_HEADER_HEIGHT +
				item * SHELL_MENU_ROW_HEIGHT;

			if (shell->selected == item) {
				fill_rect(buffer, SHELL_MENU_X + 6, y + 3,
					  SHELL_MENU_WIDTH - 12,
					  SHELL_MENU_ROW_HEIGHT - 4,
					  rgb565(COLOR_BORDER));
				fill_rect(buffer, SHELL_MENU_X + 6, y + 3, 3,
					  SHELL_MENU_ROW_HEIGHT - 4,
					  rgb565(colors[action]));
			}
			fill_rect(buffer, SHELL_MENU_X + 18, y + 12, 16, 16,
				  rgb565(colors[action]));
			draw_text(buffer, SHELL_MENU_X + 46, y + 12,
				  titles[action], rgb565(COLOR_TEXT));
		}
	}
}

static void draw_terminal(struct test_buffer *buffer,
			  const struct shell_state *shell,
			  const struct shell_window *window)
{
	int x = window->x + 8;
	int y = window->y + 36;
	unsigned int row;
	unsigned int column;

	fill_rect(buffer, x, y, window->width - 16, window->height - 44,
		  rgb565(COLOR_TERMINAL));
	x += 7;
	for (row = 0; row < TERMINAL_ROWS; row++)
		for (column = 0; column < TERMINAL_COLUMNS; column++) {
			const struct terminal_cell *cell =
				&shell->terminal.cells[row][column];
			int cell_x = x + column * SHELL_FONT_WIDTH;
			int cell_y = y + row * SHELL_FONT_HEIGHT;

			if (shell->focused == SHELL_APP_TERMINAL &&
			    shell->cursor_visible &&
			    row == shell->terminal.cursor_y &&
			    column == shell->terminal.cursor_x) {
				fill_rect(buffer, cell_x, cell_y, SHELL_FONT_WIDTH,
					  SHELL_FONT_HEIGHT, rgb565(COLOR_TEXT));
				if (cell->character != ' ')
					draw_character(buffer, cell_x, cell_y,
						       cell->character,
						       rgb565(COLOR_TERMINAL));
			} else if (cell->character != ' ') {
				draw_character(buffer, cell_x, cell_y,
					       cell->character,
					       rgb565(cell->color));
			}
		}
}

static void draw_files(struct test_buffer *buffer,
		       const struct shell_state *shell,
		       const struct shell_window *window)
{
	const struct shell_files *files = &shell->files;
	char status[47];
	char path[47];
	size_t path_length = strlen(files->path);
	unsigned int i;

	if (path_length < sizeof(path))
		strcpy(path, files->path);
	else
		snprintf(path, sizeof(path), "...%s",
			 files->path + path_length - sizeof(path) + 4);
	draw_text(buffer, window->x + 16, window->y + 38, path,
		  rgb565(COLOR_TEXT));
	fill_rect(buffer, window->x + 16, window->y + 58,
		  window->width - 32, 1, rgb565(COLOR_BORDER));

	for (i = 0; i < FILES_VISIBLE_ROWS; i++) {
		unsigned int index = files->scroll + i;
		const struct shell_file_entry *entry;
		char label[39];
		int x = window->x + 16;
		int y = window->y + 64 + i * 20;

		if (index >= files->count)
			break;
		entry = &files->entries[index];
		if (index == files->selected)
			fill_rect(buffer, x, y, window->width - 32, 19,
				  rgb565(COLOR_BORDER));
		fill_rect(buffer, x + 4, y + 5, 10, 10,
			  rgb565(entry->directory ? COLOR_GOLD : COLOR_MUTED));
		snprintf(label, sizeof(label), entry->directory ? "%.35s/" :
			 "%.36s", entry->name);
		draw_text(buffer, x + 22, y + 2, label, rgb565(COLOR_TEXT));
	}
	snprintf(status, sizeof(status), "%.46s", files->status);
	draw_text(buffer, window->x + 16, window->y + window->height - 22,
		  status, rgb565(COLOR_MUTED));
}

static void draw_status_row(struct test_buffer *buffer, int x, int y,
			    const char *label, const char *value,
			    enum shell_color indicator)
{
	fill_rect(buffer, x, y + 4, 8, 8, rgb565(indicator));
	draw_text(buffer, x + 22, y, label, rgb565(COLOR_MUTED));
	draw_text(buffer, x + 166, y, value, rgb565(COLOR_TEXT));
}

static void draw_system(struct test_buffer *buffer,
			const struct shell_state *shell,
			const struct shell_window *window)
{
	const struct shell_system *system = &shell->system;
	uint64_t memory_used_kb = system->memory_total_kb >
		system->memory_available_kb ?
		system->memory_total_kb - system->memory_available_kb : 0;
	char graphics[28];
	char network[28];
	char traffic[28];
	char memory[28];
	char uptime[28];
	char cpu[28];
	unsigned int rx_mib = system->network_rx_bytes >> 20;
	unsigned int tx_mib = system->network_tx_bytes >> 20;
	int x = window->x + 26;
	int y = window->y + 44;

	snprintf(cpu, sizeof(cpu), "%u%% Broadway", system->cpu_percent);
	if (system->memory_total_kb)
		snprintf(memory, sizeof(memory), "%llu / %lu MiB",
			 (unsigned long long)(memory_used_kb / 1024),
			 system->memory_total_kb / 1024);
	else
		strcpy(memory, "Unavailable");
	snprintf(uptime, sizeof(uptime), "%lluh %02llum %02llus",
		 (unsigned long long)(system->uptime_seconds / 3600),
		 (unsigned long long)(system->uptime_seconds / 60 % 60),
		 (unsigned long long)(system->uptime_seconds % 60));
	if (system->network_name[0])
		snprintf(network, sizeof(network), "%s %s", system->network_name,
			 system->network_up ? "Up" : "Down");
	else
		strcpy(network, "No interface");
	if (system->network_rx_bytes >> 20 > 999999)
		rx_mib = 999999;
	if (system->network_tx_bytes >> 20 > 999999)
		tx_mib = 999999;
	snprintf(traffic, sizeof(traffic), "R%uM T%uM", rx_mib, tx_mib);
	snprintf(graphics, sizeof(graphics), "%s / %s",
		 system->gx_loaded ? "GX" : "CPU",
		 system->drm_present ? "DRM" : "No DRM");

	draw_status_row(buffer, x, y, "CPU", cpu, COLOR_TEAL);
	draw_status_row(buffer, x, y + 34, "Memory", memory, COLOR_GOLD);
	draw_status_row(buffer, x, y + 68, "Uptime", uptime, COLOR_VIOLET);
	draw_status_row(buffer, x, y + 102, "Network", network,
			system->network_up ? COLOR_TEAL : COLOR_RED);
	draw_status_row(buffer, x, y + 136, "Traffic", traffic, COLOR_BLUE);
	draw_status_row(buffer, x, y + 170, "Graphics", graphics,
			system->gx_loaded && system->drm_present ?
			COLOR_TEAL : COLOR_RED);
}

static void draw_settings(struct test_buffer *buffer,
			  const struct shell_state *shell,
			  const struct shell_window *window)
{
	static const char *const labels[SETTINGS_ROW_COUNT] = {
		"Wallpaper", "Accent", "Clock", "Pointer", "Auto lock",
	};
	static const char *const wallpapers[SETTINGS_WALLPAPER_COUNT] = {
		"Aqua", "Sunset", "Graphite",
	};
	static const char *const accents[SETTINGS_ACCENT_COUNT] = {
		"Teal", "Sky", "Violet",
	};
	static const char *const locks[SETTINGS_LOCK_COUNT] = {
		"Off", "5 minutes", "15 minutes", "30 minutes",
	};
	const struct shell_settings *settings = &shell->settings;
	char pointer[16];
	char account[40];
	const char *values[SETTINGS_ROW_COUNT];
	unsigned int i;
	int x = window->x + 20;
	int y = window->y + 44;

	snprintf(pointer, sizeof(pointer), "%ux", settings->pointer_scale);
	values[0] = wallpapers[settings->wallpaper % SETTINGS_WALLPAPER_COUNT];
	values[1] = accents[settings->accent % SETTINGS_ACCENT_COUNT];
	values[2] = settings->clock_24h ? "24 hour" : "12 hour";
	values[3] = pointer;
	values[4] = locks[settings->lock_option % SETTINGS_LOCK_COUNT];
	for (i = 0; i < SETTINGS_ROW_COUNT; i++) {
		int row_y = y + i * SETTINGS_ROW_HEIGHT;

		if (settings->selected == i) {
			fill_rect(buffer, x, row_y, window->width - 40,
				  SETTINGS_ROW_HEIGHT - 4, rgb565(COLOR_BORDER));
			fill_rect(buffer, x, row_y, 3, SETTINGS_ROW_HEIGHT - 4,
				  rgb565(shell_accent(shell)));
		}
		draw_text(buffer, x + 14, row_y + 9, labels[i],
			  rgb565(COLOR_MUTED));
		draw_text(buffer, x + 172, row_y + 9, "<", rgb565(COLOR_TEXT));
		draw_text(buffer, x + 194, row_y + 9, values[i],
			  rgb565(COLOR_TEXT));
		draw_text(buffer, window->x + window->width - 38, row_y + 9,
			  ">", rgb565(COLOR_TEXT));
	}
	snprintf(account, sizeof(account), "Account: %.28s", shell->login.username);
	draw_text(buffer, x, window->y + window->height - 42, account,
		  rgb565(COLOR_MUTED));
	draw_text(buffer, x, window->y + window->height - 22,
		  settings->status, rgb565(shell_accent(shell)));
}

static void draw_window(struct test_buffer *buffer,
			const struct shell_state *shell,
			const struct shell_window *window)
{
	int close_x = window->x + window->width - 24;
	int maximize_x = close_x - 20;
	int minimize_x = maximize_x - 20;
	uint16_t frame = shell->focused == (int)window->app ?
		rgb565(app_accents[window->app]) : rgb565(COLOR_BORDER);
	int i;

	fill_rect(buffer, window->x + 4, window->y + 4, window->width,
		  window->height, rgb565(COLOR_TERMINAL));
	fill_rect(buffer, window->x, window->y, window->width, window->height,
		  rgb565(COLOR_PANEL));
	stroke_rect(buffer, window->x, window->y, window->width, window->height,
		    frame);
	fill_rect(buffer, window->x, window->y, window->width,
		  SHELL_TITLE_HEIGHT,
		  rgb565(COLOR_BORDER));
	fill_rect(buffer, window->x, window->y, 3, SHELL_TITLE_HEIGHT, frame);
	draw_text(buffer, window->x + 14, window->y + 6,
		  app_titles[window->app], rgb565(COLOR_TEXT));
	if (window->app == SHELL_APP_TERMINAL)
		draw_text(buffer, window->x + 102, window->y + 6,
			  shell->terminal.child_pid > 0 ? "Running" : "Exited",
			  rgb565(shell->terminal.child_pid > 0 ?
				 COLOR_TEAL : COLOR_MUTED));
	fill_rect(buffer, minimize_x, window->y + 8, SHELL_CONTROL_SIZE,
		  SHELL_CONTROL_SIZE, rgb565(COLOR_PANEL));
	fill_rect(buffer, minimize_x + 3, window->y + 15, 6, 2,
		  rgb565(COLOR_TEXT));
	fill_rect(buffer, maximize_x, window->y + 8, SHELL_CONTROL_SIZE,
		  SHELL_CONTROL_SIZE, rgb565(COLOR_PANEL));
	if (window->maximized) {
		stroke_rect(buffer, maximize_x + 2, window->y + 12, 6, 5,
			    rgb565(COLOR_TEXT));
		stroke_rect(buffer, maximize_x + 4, window->y + 10, 6, 5,
			    rgb565(COLOR_TEXT));
	} else {
		stroke_rect(buffer, maximize_x + 2, window->y + 10, 8, 8,
			    rgb565(COLOR_TEXT));
	}
	fill_rect(buffer, close_x, window->y + 8, SHELL_CONTROL_SIZE,
		  SHELL_CONTROL_SIZE, rgb565(COLOR_RED));
	for (i = 0; i < 6; i++) {
		fill_rect(buffer, close_x + 3 + i, window->y + 11 + i, 1, 1,
			  rgb565(COLOR_TEXT));
		fill_rect(buffer, close_x + 8 - i, window->y + 11 + i, 1, 1,
			  rgb565(COLOR_TEXT));
	}

	switch (window->app) {
	case SHELL_APP_TERMINAL:
		draw_terminal(buffer, shell, window);
		break;
	case SHELL_APP_FILES:
		draw_files(buffer, shell, window);
		break;
	case SHELL_APP_SYSTEM:
		draw_system(buffer, shell, window);
		break;
	case SHELL_APP_SETTINGS:
		draw_settings(buffer, shell, window);
		break;
	default:
		break;
	}
}

static void draw_tasks(struct test_buffer *buffer,
		       const struct shell_state *shell)
{
	unsigned int i;

	for (i = 0; i < SHELL_APP_COUNT; i++) {
		const struct shell_window *window = &shell->windows[i];
		int x = SHELL_TASK_X + i * SHELL_TASK_WIDTH;
		uint16_t background;

		if (!window->visible)
			continue;
		background = shell->focused == (int)i && !window->minimized ?
			rgb565(COLOR_BORDER) : rgb565(COLOR_TERMINAL);
		fill_rect(buffer, x, TEST_HEIGHT - 22, SHELL_TASK_WIDTH - 4,
			  SHELL_TASK_HEIGHT, background);
		fill_rect(buffer, x, TEST_HEIGHT - 22, 3, SHELL_TASK_HEIGHT,
			  rgb565(window->minimized ? COLOR_MUTED : app_accents[i]));
		draw_text(buffer, x + 12, TEST_HEIGHT - 20, app_titles[i],
			  rgb565(window->minimized ? COLOR_MUTED : COLOR_TEXT));
	}
}

static void draw_pointer(struct test_buffer *buffer, int x, int y)
{
	int row;

	for (row = 0; row < 16; row++) {
		int width = row / 2 + 2;

		fill_rect(buffer, x, y + row, width, 1, rgb565(COLOR_TERMINAL));
		if (width > 2)
			fill_rect(buffer, x + 1, y + row, width - 2, 1,
				  rgb565(COLOR_TEXT));
	}
	fill_rect(buffer, x + 3, y + 12, 3, 8, rgb565(COLOR_TERMINAL));
	fill_rect(buffer, x + 4, y + 12, 1, 6, rgb565(COLOR_TEXT));
}

static void draw_shell(struct test_buffer *buffer,
		       const struct shell_state *shell)
{
	time_t now = time(NULL);
	struct tm local;
	char clock_text[16] = "--:--";
	char account_text[20];
	unsigned int i;

	if (shell->view == SHELL_VIEW_SPLASH) {
		draw_splash(buffer);
		return;
	}
	if (shell->view == SHELL_VIEW_LOGIN || shell->view == SHELL_VIEW_LOCK) {
		draw_login(buffer, shell);
		draw_pointer(buffer, shell->pointer_x, shell->pointer_y);
		return;
	}

	draw_wallpaper(buffer, shell);
	fill_rect(buffer, 0, 0, TEST_WIDTH, 32, rgb565(COLOR_PANEL));
	fill_rect(buffer, 0, 31, TEST_WIDTH, 1, rgb565(COLOR_BORDER));
	fill_rect(buffer, 14, 8, 16, 16, rgb565(shell_accent(shell)));
	draw_text(buffer, 40, 8, "WiiDesk", rgb565(COLOR_TEXT));
	if (localtime_r(&now, &local))
		strftime(clock_text, sizeof(clock_text),
			 shell->settings.clock_24h ? "%H:%M" : "%I:%M %p", &local);
	snprintf(account_text, sizeof(account_text), "%.12s", shell->login.username);
	draw_text(buffer, 460, 8,
		  account_text, rgb565(COLOR_MUTED));
	draw_text(buffer, shell->settings.clock_24h ? 580 : 568, 8,
		  clock_text, rgb565(COLOR_MUTED));
	draw_desktop_icons(buffer, shell);

	for (i = 0; i < SHELL_APP_COUNT; i++) {
		const struct shell_window *window =
			&shell->windows[shell->z_order[i]];

		if (window->visible && !window->minimized)
			draw_window(buffer, shell, window);
	}

	fill_rect(buffer, 0, TEST_HEIGHT - 24, TEST_WIDTH, 24,
		  rgb565(COLOR_PANEL));
	fill_rect(buffer, 0, TEST_HEIGHT - 24, TEST_WIDTH, 1,
		  rgb565(COLOR_BORDER));
	fill_rect(buffer, SHELL_START_X, TEST_HEIGHT - 22, SHELL_START_WIDTH,
		  SHELL_TASK_HEIGHT,
		  rgb565(shell->menu_open ? COLOR_BORDER : COLOR_TERMINAL));
	fill_rect(buffer, SHELL_START_X, TEST_HEIGHT - 22, 3,
		  SHELL_TASK_HEIGHT, rgb565(shell_accent(shell)));
	draw_text(buffer, SHELL_START_X + 10, TEST_HEIGHT - 20, "WiiDesk",
		  rgb565(COLOR_TEXT));
	draw_tasks(buffer, shell);
	fill_rect(buffer, 602, TEST_HEIGHT - 16, 8, 8,
		  rgb565(shell->cursor_visible ? shell_accent(shell) : COLOR_BORDER));
	draw_start_menu(buffer, shell);
	draw_pointer(buffer, shell->pointer_x, shell->pointer_y);
}

static int event_bit(const unsigned long *bits, unsigned int bit)
{
	return bits[bit / (sizeof(unsigned long) * 8)] &
		(1UL << (bit % (sizeof(unsigned long) * 8)));
}

static void open_inputs(struct shell_state *shell)
{
	unsigned int index;

	for (index = 0; index < 32 && shell->input_count < SHELL_INPUT_COUNT;
	     index++) {
		unsigned long event_bits[(EV_MAX + 8 * sizeof(unsigned long)) /
			(8 * sizeof(unsigned long))] = { };
		unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
			(8 * sizeof(unsigned long))] = { };
		unsigned long relative_bits[(REL_MAX + 8 * sizeof(unsigned long)) /
			(8 * sizeof(unsigned long))] = { };
		struct shell_input *input = &shell->inputs[shell->input_count];
		char name[128] = "unknown";
		int keyboard;
		int pointer;
		int fd;

		snprintf(input->path, sizeof(input->path), "/dev/input/event%u",
			 index);
		fd = open(input->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0) {
			close(fd);
			continue;
		}
		if (event_bit(event_bits, EV_KEY))
			(void)ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
		if (event_bit(event_bits, EV_REL))
			(void)ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relative_bits)),
				    relative_bits);
		keyboard = event_bit(event_bits, EV_KEY) &&
			(event_bit(key_bits, KEY_ENTER) ||
			 event_bit(key_bits, KEY_SPACE));
		pointer = event_bit(event_bits, EV_REL) &&
			event_bit(relative_bits, REL_X) &&
			event_bit(relative_bits, REL_Y) &&
			event_bit(key_bits, BTN_LEFT);
		if (!keyboard && !pointer) {
			close(fd);
			continue;
		}
		if (ioctl(fd, EVIOCGRAB, 1) < 0) {
			perror("EVIOCGRAB");
			close(fd);
			continue;
		}
		(void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		input->fd = fd;
		shell->input_count++;
		printf("wiidesk: input %s (%s, %s%s)\n", input->path,
		       name, keyboard ? "keyboard" : "",
		       pointer ? (keyboard ? "+pointer" : "pointer") : "");
	}
}

static void init_windows(struct shell_state *shell)
{
	shell->windows[SHELL_APP_TERMINAL] = (struct shell_window) {
		.app = SHELL_APP_TERMINAL,
		.x = 150,
		.y = 70,
		.width = 430,
		.height = 270,
		.visible = 1,
	};
	shell->windows[SHELL_APP_FILES] = (struct shell_window) {
		.app = SHELL_APP_FILES,
		.x = 170,
		.y = 90,
		.width = 400,
		.height = 300,
	};
	shell->windows[SHELL_APP_SYSTEM] = (struct shell_window) {
		.app = SHELL_APP_SYSTEM,
		.x = 190,
		.y = 110,
		.width = 410,
		.height = 280,
	};
	shell->windows[SHELL_APP_SETTINGS] = (struct shell_window) {
		.app = SHELL_APP_SETTINGS,
		.x = 180,
		.y = 82,
		.width = 430,
		.height = 300,
	};
	shell->z_order[0] = SHELL_APP_SYSTEM;
	shell->z_order[1] = SHELL_APP_FILES;
	shell->z_order[2] = SHELL_APP_SETTINGS;
	shell->z_order[3] = SHELL_APP_TERMINAL;
	shell->focused = SHELL_APP_TERMINAL;
	shell->dragging = -1;
}

static void lock_desktop_session(struct shell_state *shell)
{
	shell->menu_open = 0;
	shell->selected = SHELL_MENU_LOCK;
	shell->view = SHELL_VIEW_LOCK;
	shell->login.field = LOGIN_FIELD_PASSWORD;
	shell->login.auth_state = LOGIN_AUTH_IDLE;
	wipe_secret(shell->login.password, sizeof(shell->login.password));
	shell->login.password_length = 0;
	snprintf(shell->login.status, sizeof(shell->login.status), "Session locked");
}

static void end_desktop_session(struct shell_state *shell)
{
	stop_terminal(&shell->terminal);
	memset(&shell->files, 0, sizeof(shell->files));
	memset(&shell->system, 0, sizeof(shell->system));
	settings_defaults(&shell->settings);
	init_windows(shell);
	shell->menu_open = 0;
	shell->selected = 0;
	shell->desktop_selected = -1;
	shell->desktop_last_clicked = -1;
	shell->view = SHELL_VIEW_LOGIN;
	shell->login.field = shell->login.username_length ?
		LOGIN_FIELD_PASSWORD : LOGIN_FIELD_USERNAME;
	shell->login.auth_state = LOGIN_AUTH_IDLE;
	wipe_secret(shell->login.password, sizeof(shell->login.password));
	shell->login.password_length = 0;
	snprintf(shell->login.status, sizeof(shell->login.status), "Signed out");
}

static void focus_top_window(struct shell_state *shell)
{
	int i;

	shell->focused = -1;
	for (i = SHELL_APP_COUNT - 1; i >= 0; i--)
		if (shell->windows[shell->z_order[i]].visible &&
		    !shell->windows[shell->z_order[i]].minimized) {
			shell->focused = shell->z_order[i];
			break;
		}
}

static void raise_window(struct shell_state *shell, unsigned int app)
{
	unsigned int i;

	for (i = 0; i < SHELL_APP_COUNT; i++)
		if (shell->z_order[i] == app)
			break;
	if (i == SHELL_APP_COUNT)
		return;
	for (; i + 1 < SHELL_APP_COUNT; i++)
		shell->z_order[i] = shell->z_order[i + 1];
	shell->z_order[SHELL_APP_COUNT - 1] = app;
	shell->focused = app;
}

static void open_window(struct shell_state *shell, unsigned int app)
{
	if (app == SHELL_APP_TERMINAL && shell->terminal.child_pid <= 0 &&
	    shell->terminal.master_fd < 0) {
		end_desktop_session(shell);
		return;
	}
	if (app == SHELL_APP_FILES && !shell->files.loaded)
		(void)files_load(&shell->files, "/");
	if (app == SHELL_APP_SYSTEM)
		refresh_system(&shell->system);
	shell->windows[app].visible = 1;
	shell->windows[app].minimized = 0;
	shell->menu_open = 0;
	raise_window(shell, app);
}

static void restore_window_geometry(struct shell_window *window)
{
	if (!window->maximized)
		return;
	window->x = window->restore_x;
	window->y = window->restore_y;
	window->width = window->restore_width;
	window->height = window->restore_height;
	window->maximized = 0;
}

static void close_window(struct shell_state *shell, unsigned int app)
{
	restore_window_geometry(&shell->windows[app]);
	shell->windows[app].visible = 0;
	shell->windows[app].minimized = 0;
	if (shell->dragging == (int)app)
		shell->dragging = -1;
	focus_top_window(shell);
}

static void minimize_window(struct shell_state *shell, unsigned int app)
{
	shell->windows[app].minimized = 1;
	if (shell->dragging == (int)app)
		shell->dragging = -1;
	focus_top_window(shell);
}

static void toggle_maximize_window(struct shell_state *shell, unsigned int app)
{
	struct shell_window *window = &shell->windows[app];

	if (window->maximized) {
		restore_window_geometry(window);
		return;
	}
	window->restore_x = window->x;
	window->restore_y = window->y;
	window->restore_width = window->width;
	window->restore_height = window->height;
	window->x = SHELL_WORKSPACE_LEFT;
	window->y = SHELL_WORKSPACE_TOP;
	window->width = TEST_WIDTH - SHELL_WORKSPACE_LEFT;
	window->height = SHELL_WORKSPACE_BOTTOM - SHELL_WORKSPACE_TOP;
	window->maximized = 1;
	window->minimized = 0;
	shell->dragging = -1;
}

static void toggle_task_window(struct shell_state *shell, unsigned int app)
{
	struct shell_window *window = &shell->windows[app];

	if (!window->visible)
		return;
	if (!window->minimized && shell->focused == (int)app) {
		minimize_window(shell, app);
		return;
	}
	window->minimized = 0;
	raise_window(shell, app);
}

static int start_button_at(int x, int y)
{
	return x >= SHELL_START_X && x < SHELL_START_X + SHELL_START_WIDTH &&
		y >= TEST_HEIGHT - 22 && y < TEST_HEIGHT - 2;
}

static int menu_contains(int x, int y)
{
	return x >= SHELL_MENU_X && x < SHELL_MENU_X + SHELL_MENU_WIDTH &&
		y >= menu_y() && y < SHELL_WORKSPACE_BOTTOM;
}

static int launcher_at(const struct shell_state *shell, int x, int y)
{
	unsigned int i;

	if (!shell->menu_open || x < SHELL_MENU_X + 6 ||
	    x >= SHELL_MENU_X + SHELL_MENU_WIDTH - 6)
		return -1;
	for (i = 0; i < SHELL_MENU_ITEM_COUNT; i++) {
		int top = menu_y() + SHELL_MENU_HEADER_HEIGHT +
			i * SHELL_MENU_ROW_HEIGHT;

		if (y >= top && y < top + SHELL_MENU_ROW_HEIGHT)
			return i;
	}
	return -1;
}

static int desktop_icon_at(int x, int y)
{
	unsigned int i;

	if (x < SHELL_DESKTOP_ICON_X - 6 ||
	    x >= SHELL_DESKTOP_ICON_X - 6 + SHELL_DESKTOP_ICON_WIDTH)
		return -1;
	for (i = 0; i < SHELL_APP_COUNT; i++) {
		int top = SHELL_DESKTOP_ICON_Y + i * SHELL_DESKTOP_ICON_HEIGHT - 6;

		if (y >= top && y < top + 62)
			return i;
	}
	return -1;
}

static int settings_row_at(const struct shell_window *window, int x, int y)
{
	int row;

	if (x < window->x + 20 || x >= window->x + window->width - 20 ||
	    y < window->y + 44 ||
	    y >= window->y + 44 + SETTINGS_ROW_COUNT * SETTINGS_ROW_HEIGHT)
		return -1;
	row = (y - window->y - 44) / SETTINGS_ROW_HEIGHT;
	return row < SETTINGS_ROW_COUNT ? row : -1;
}

static int window_at(const struct shell_state *shell, int x, int y)
{
	int i;

	for (i = SHELL_APP_COUNT - 1; i >= 0; i--) {
		const struct shell_window *window =
			&shell->windows[shell->z_order[i]];

		if (window->visible && !window->minimized && x >= window->x &&
		    x < window->x + window->width && y >= window->y &&
		    y < window->y + window->height)
			return window->app;
	}
	return -1;
}

static int task_at(const struct shell_state *shell, int x, int y)
{
	unsigned int i;

	if (y < TEST_HEIGHT - 22 || y >= TEST_HEIGHT - 2)
		return -1;
	for (i = 0; i < SHELL_APP_COUNT; i++) {
		int left = SHELL_TASK_X + i * SHELL_TASK_WIDTH;

		if (shell->windows[i].visible && x >= left &&
		    x < left + SHELL_TASK_WIDTH - 4)
			return i;
	}
	return -1;
}

static void focus_next_window(struct shell_state *shell)
{
	int focused_index = -1;
	int step;
	int i;

	for (i = 0; i < SHELL_APP_COUNT; i++)
		if (shell->z_order[i] == (unsigned int)shell->focused)
			focused_index = i;
	if (focused_index < 0) {
		focus_top_window(shell);
		return;
	}
	for (step = 1; step <= SHELL_APP_COUNT; step++) {
		i = (focused_index - step + SHELL_APP_COUNT) % SHELL_APP_COUNT;
		if (shell->windows[shell->z_order[i]].visible &&
		    !shell->windows[shell->z_order[i]].minimized) {
			raise_window(shell, shell->z_order[i]);
			return;
		}
	}
}

static unsigned int cycle_value(unsigned int value, unsigned int count,
				int direction)
{
	if (direction < 0)
		return value ? value - 1 : count - 1;
	return (value + 1) % count;
}

static int settings_adjust(struct shell_state *shell, unsigned int row,
			   int direction)
{
	struct shell_settings *settings = &shell->settings;
	unsigned int value;

	switch (row) {
	case 0:
		value = cycle_value(settings->wallpaper, SETTINGS_WALLPAPER_COUNT,
				    direction);
		settings->wallpaper = value;
		break;
	case 1:
		value = cycle_value(settings->accent, SETTINGS_ACCENT_COUNT,
				    direction);
		settings->accent = value;
		break;
	case 2:
		settings->clock_24h = !settings->clock_24h;
		break;
	case 3:
		if (direction < 0)
			settings->pointer_scale = settings->pointer_scale > 1 ?
				settings->pointer_scale - 1 : 3;
		else
			settings->pointer_scale = settings->pointer_scale < 3 ?
				settings->pointer_scale + 1 : 1;
		break;
	case 4:
		value = cycle_value(settings->lock_option, SETTINGS_LOCK_COUNT,
				    direction);
		settings->lock_option = value;
		break;
	default:
		return 0;
	}
	(void)settings_save(settings);
	return 1;
}

static int handle_settings_key(struct shell_state *shell, unsigned int key)
{
	struct shell_settings *settings = &shell->settings;

	switch (key) {
	case KEY_UP:
		settings->selected = settings->selected ? settings->selected - 1 :
			SETTINGS_ROW_COUNT - 1;
		return 1;
	case KEY_DOWN:
	case KEY_TAB:
		settings->selected = (settings->selected + 1) % SETTINGS_ROW_COUNT;
		return 1;
	case KEY_LEFT:
		return settings_adjust(shell, settings->selected, -1);
	case KEY_RIGHT:
	case KEY_ENTER:
	case KEY_KPENTER:
	case KEY_SPACE:
		return settings_adjust(shell, settings->selected, 1);
	default:
		return 0;
	}
}

static int handle_key(struct shell_state *shell, unsigned int key)
{
	switch (key) {
	case KEY_LEFT:
	case KEY_UP:
		if (!shell->menu_open)
			return 0;
		if (shell->selected)
			shell->selected--;
		else
			shell->selected = SHELL_MENU_ITEM_COUNT - 1;
		return 1;
	case KEY_RIGHT:
	case KEY_DOWN:
	case KEY_TAB:
		if (!shell->menu_open)
			return 0;
		shell->selected = (shell->selected + 1) % SHELL_MENU_ITEM_COUNT;
		return 1;
	case KEY_ENTER:
	case KEY_SPACE:
		if (!shell->menu_open)
			return 0;
		if (shell->selected == SHELL_MENU_LOCK) {
			lock_desktop_session(shell);
			return 1;
		}
		if (shell->selected == SHELL_MENU_LOGOUT) {
			end_desktop_session(shell);
			return 1;
		}
		open_window(shell, shell->selected);
		return 1;
	case KEY_F1:
		shell->selected = SHELL_APP_TERMINAL;
		open_window(shell, SHELL_APP_TERMINAL);
		return 1;
	case KEY_F2:
		shell->selected = SHELL_APP_FILES;
		open_window(shell, SHELL_APP_FILES);
		return 1;
	case KEY_F3:
		shell->selected = SHELL_APP_SYSTEM;
		open_window(shell, SHELL_APP_SYSTEM);
		return 1;
	case KEY_F4:
		if (shell->focused >= 0)
			close_window(shell, shell->focused);
		return 1;
	case KEY_F5:
		if (shell->focused >= 0)
			minimize_window(shell, shell->focused);
		return 1;
	case KEY_F6:
		focus_next_window(shell);
		return 1;
	case KEY_F7:
		if (shell->focused >= 0)
			toggle_maximize_window(shell, shell->focused);
		return 1;
	case KEY_F8:
		shell->menu_open = !shell->menu_open;
		return 1;
	case KEY_F9:
		shell->selected = SHELL_APP_SETTINGS;
		open_window(shell, SHELL_APP_SETTINGS);
		return 1;
	case KEY_ESC:
		if (shell->menu_open) {
			shell->menu_open = 0;
			return 1;
		}
		return 0;
	default:
		return 0;
	}
}

static unsigned char key_character(unsigned int key)
{
	static const unsigned char characters[KEY_MAX + 1] = {
		[KEY_A] = 'a', [KEY_B] = 'b', [KEY_C] = 'c', [KEY_D] = 'd',
		[KEY_E] = 'e', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h',
		[KEY_I] = 'i', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',
		[KEY_M] = 'm', [KEY_N] = 'n', [KEY_O] = 'o', [KEY_P] = 'p',
		[KEY_Q] = 'q', [KEY_R] = 'r', [KEY_S] = 's', [KEY_T] = 't',
		[KEY_U] = 'u', [KEY_V] = 'v', [KEY_W] = 'w', [KEY_X] = 'x',
		[KEY_Y] = 'y', [KEY_Z] = 'z',
		[KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4',
		[KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8',
		[KEY_9] = '9', [KEY_0] = '0',
		[KEY_MINUS] = '-', [KEY_EQUAL] = '=', [KEY_LEFTBRACE] = '[',
		[KEY_RIGHTBRACE] = ']', [KEY_BACKSLASH] = '\\',
		[KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'',
		[KEY_GRAVE] = '`', [KEY_COMMA] = ',', [KEY_DOT] = '.',
		[KEY_SLASH] = '/', [KEY_SPACE] = ' ',
	};

	return key <= KEY_MAX ? characters[key] : 0;
}

static unsigned char shifted_character(unsigned char character)
{
	switch (character) {
	case '1': return '!';
	case '2': return '@';
	case '3': return '#';
	case '4': return '$';
	case '5': return '%';
	case '6': return '^';
	case '7': return '&';
	case '8': return '*';
	case '9': return '(';
	case '0': return ')';
	case '-': return '_';
	case '=': return '+';
	case '[': return '{';
	case ']': return '}';
	case '\\': return '|';
	case ';': return ':';
	case '\'': return '"';
	case '`': return '~';
	case ',': return '<';
	case '.': return '>';
	case '/': return '?';
	default: return character;
	}
}

static void queue_login(struct shell_state *shell)
{
	if (shell->login.auth_state != LOGIN_AUTH_IDLE)
		return;
	if (!shell->login.username_length || shell->login.username[0] == '-' ||
	    shell->login.username[0] == '.') {
		snprintf(shell->login.status, sizeof(shell->login.status),
			 "Enter a valid user name");
		shell->login.field = LOGIN_FIELD_USERNAME;
		return;
	}
	snprintf(shell->login.status, sizeof(shell->login.status), "Signing in...");
	shell->login.auth_state = LOGIN_AUTH_QUEUED;
}

static int login_username_character(unsigned char character)
{
	return (character >= 'a' && character <= 'z') ||
		(character >= 'A' && character <= 'Z') ||
		(character >= '0' && character <= '9') ||
		character == '_' || character == '-' || character == '.';
}

static int handle_login_key(struct shell_state *shell, unsigned int key)
{
	struct shell_login *login = &shell->login;
	unsigned char character;
	char *field;
	size_t *length;
	size_t capacity;

	if (shell->view == SHELL_VIEW_SPLASH)
		return 0;
	if (login->auth_state != LOGIN_AUTH_IDLE)
		return 0;
	if (shell->view == SHELL_VIEW_LOCK)
		login->field = LOGIN_FIELD_PASSWORD;
	if (key == KEY_TAB || key == KEY_UP || key == KEY_DOWN) {
		if (shell->view == SHELL_VIEW_LOCK)
			return 0;
		login->field = login->field == LOGIN_FIELD_USERNAME ?
			LOGIN_FIELD_PASSWORD : LOGIN_FIELD_USERNAME;
		return 1;
	}
	if (key == KEY_ENTER || key == KEY_KPENTER) {
		if (login->field == LOGIN_FIELD_USERNAME &&
		    shell->view != SHELL_VIEW_LOCK) {
			login->field = LOGIN_FIELD_PASSWORD;
			return 1;
		}
		queue_login(shell);
		return 1;
	}
	if (login->field == LOGIN_FIELD_USERNAME &&
	    shell->view != SHELL_VIEW_LOCK) {
		field = login->username;
		length = &login->username_length;
		capacity = sizeof(login->username);
	} else {
		field = login->password;
		length = &login->password_length;
		capacity = sizeof(login->password);
	}
	if (key == KEY_BACKSPACE) {
		if (*length) {
			field[--*length] = '\0';
			login->status[0] = '\0';
			return 1;
		}
		return 0;
	}
	character = key_character(key);
	if (!character)
		return 0;
	if (character >= 'a' && character <= 'z') {
		if (shell->shift_down ^ shell->caps_lock)
			character -= 'a' - 'A';
	} else if (shell->shift_down) {
		character = shifted_character(character);
	}
	if (login->field == LOGIN_FIELD_USERNAME &&
	    !login_username_character(character))
		return 0;
	if (*length + 1 >= capacity)
		return 0;
	field[(*length)++] = character;
	field[*length] = '\0';
	login->status[0] = '\0';
	return 1;
}

static int start_desktop_session(struct shell_state *shell)
{
	int authenticated = authenticate_login(shell->login.username,
					       shell->login.password,
					       &shell->terminal);

	shell->login.password_length = 0;
	shell->login.auth_state = LOGIN_AUTH_IDLE;
	if (authenticated <= 0) {
		snprintf(shell->login.status, sizeof(shell->login.status), "%s",
			 authenticated < 0 ? "Login service unavailable" :
			 "Login failed");
		shell->login.field = LOGIN_FIELD_PASSWORD;
		return 0;
	}
	(void)settings_load(&shell->settings, shell->login.username);
	init_windows(shell);
	refresh_system(&shell->system);
	shell->login.status[0] = '\0';
	shell->view = SHELL_VIEW_DESKTOP;
	shell->last_input_ms = shell_monotonic_ms();
	return 1;
}

static int unlock_desktop_session(struct shell_state *shell)
{
	int authenticated = authenticate_login(shell->login.username,
					       shell->login.password, NULL);

	shell->login.password_length = 0;
	shell->login.auth_state = LOGIN_AUTH_IDLE;
	if (authenticated <= 0) {
		snprintf(shell->login.status, sizeof(shell->login.status), "%s",
			 authenticated < 0 ? "Unlock service unavailable" :
			 "Unlock failed");
		return 0;
	}
	shell->login.status[0] = '\0';
	shell->view = SHELL_VIEW_DESKTOP;
	shell->last_input_ms = shell_monotonic_ms();
	return 1;
}

static int terminal_send_key(struct shell_state *shell, unsigned int key)
{
	static const struct {
		unsigned int key;
		const char *sequence;
	} sequences[] = {
		{ KEY_UP, "\033[A" }, { KEY_DOWN, "\033[B" },
		{ KEY_RIGHT, "\033[C" }, { KEY_LEFT, "\033[D" },
		{ KEY_HOME, "\033[H" }, { KEY_END, "\033[F" },
		{ KEY_DELETE, "\033[3~" }, { KEY_PAGEUP, "\033[5~" },
		{ KEY_PAGEDOWN, "\033[6~" },
	};
	unsigned char character = key_character(key);
	unsigned int i;

	if (shell->terminal.master_fd < 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(sequences); i++)
		if (sequences[i].key == key)
			return terminal_write(&shell->terminal, sequences[i].sequence,
					      strlen(sequences[i].sequence));
	if (key == KEY_ENTER || key == KEY_KPENTER)
		return terminal_write(&shell->terminal, "\r", 1);
	if (key == KEY_BACKSPACE)
		return terminal_write(&shell->terminal, "\177", 1);
	if (key == KEY_TAB)
		return terminal_write(&shell->terminal, "\t", 1);
	if (key == KEY_ESC)
		return terminal_write(&shell->terminal, "\033", 1);
	if (!character)
		return 0;
	if (character >= 'a' && character <= 'z') {
		if (shell->shift_down ^ shell->caps_lock)
			character -= 'a' - 'A';
		if (shell->control_down)
			character &= 0x1f;
	} else if (shell->shift_down) {
		character = shifted_character(character);
	}
	return terminal_write(&shell->terminal, &character, 1);
}

static int handle_files_key(struct shell_files *files, unsigned int key)
{
	char parent[FILES_PATH_SIZE];

	switch (key) {
	case KEY_UP:
		return files_move_selection(files, -1);
	case KEY_DOWN:
		return files_move_selection(files, 1);
	case KEY_PAGEUP:
		return files_move_selection(files, -FILES_VISIBLE_ROWS);
	case KEY_PAGEDOWN:
		return files_move_selection(files, FILES_VISIBLE_ROWS);
	case KEY_HOME:
		return files_move_selection(files, -FILES_ENTRY_COUNT);
	case KEY_END:
		return files_move_selection(files, FILES_ENTRY_COUNT);
	case KEY_ENTER:
	case KEY_KPENTER:
	case KEY_RIGHT:
		return files_open_selected(files);
	case KEY_BACKSPACE:
	case KEY_LEFT:
		files_parent_path(files->path, parent, sizeof(parent));
		if (!strcmp(parent, files->path))
			return 0;
		(void)files_load(files, parent);
		return 1;
	default:
		return 0;
	}
}

static int handle_key_event(struct shell_state *shell, unsigned int key,
			    int value)
{
	if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
		shell->shift_down = value != 0;
		return 0;
	}
	if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL) {
		shell->control_down = value != 0;
		return 0;
	}
	if (key == KEY_CAPSLOCK && value == 1) {
		shell->caps_lock = !shell->caps_lock;
		return 0;
	}
	if (value != 1 && value != 2)
		return 0;
	shell->last_input_ms = shell_monotonic_ms();
	if (shell->view != SHELL_VIEW_DESKTOP)
		return handle_login_key(shell, key);
	if (shell->menu_open && value == 1 &&
	    (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT ||
	     key == KEY_RIGHT || key == KEY_TAB || key == KEY_ENTER ||
	     key == KEY_KPENTER || key == KEY_SPACE || key == KEY_ESC))
		return handle_key(shell, key == KEY_KPENTER ? KEY_ENTER : key);
	if (key >= KEY_F1 && key <= KEY_F9)
		return handle_key(shell, key);
	if (shell->focused == SHELL_APP_SETTINGS &&
	    shell->windows[SHELL_APP_SETTINGS].visible) {
		if (value == 2 && key != KEY_UP && key != KEY_DOWN &&
		    key != KEY_LEFT && key != KEY_RIGHT)
			return 0;
		return handle_settings_key(shell, key);
	}
	if (shell->focused == SHELL_APP_TERMINAL &&
	    shell->windows[SHELL_APP_TERMINAL].visible &&
	    shell->terminal.master_fd >= 0) {
		if (terminal_send_key(shell, key) < 0)
			stop = 1;
		return 0;
	}
	if (shell->focused == SHELL_APP_FILES &&
	    shell->windows[SHELL_APP_FILES].visible) {
		if (value == 2 && key != KEY_UP && key != KEY_DOWN &&
		    key != KEY_PAGEUP && key != KEY_PAGEDOWN)
			return 0;
		return handle_files_key(&shell->files, key);
	}
	return value == 1 ? handle_key(shell, key) : 0;
}

static int update_pointer(struct shell_state *shell, int delta_x, int delta_y)
{
	int launcher;

	if (!delta_x && !delta_y)
		return 0;
	shell->last_input_ms = shell_monotonic_ms();
	shell->pointer_x += delta_x;
	shell->pointer_y += delta_y;
	if (shell->pointer_x < 0)
		shell->pointer_x = 0;
	if (shell->pointer_x >= TEST_WIDTH)
		shell->pointer_x = TEST_WIDTH - 1;
	if (shell->pointer_y < 0)
		shell->pointer_y = 0;
	if (shell->pointer_y >= TEST_HEIGHT)
		shell->pointer_y = TEST_HEIGHT - 1;
	if (shell->view != SHELL_VIEW_DESKTOP)
		return 1;

	launcher = launcher_at(shell, shell->pointer_x, shell->pointer_y);
	if (launcher >= 0)
		shell->selected = launcher;

	if (shell->dragging >= 0) {
		struct shell_window *window = &shell->windows[shell->dragging];
		int maximum_x = TEST_WIDTH - window->width;
		int maximum_y = SHELL_WORKSPACE_BOTTOM - window->height;

		window->x = shell->pointer_x - shell->drag_offset_x;
		window->y = shell->pointer_y - shell->drag_offset_y;
		if (window->x < SHELL_WORKSPACE_LEFT)
			window->x = SHELL_WORKSPACE_LEFT;
		if (window->x > maximum_x)
			window->x = maximum_x;
		if (window->y < SHELL_WORKSPACE_TOP)
			window->y = SHELL_WORKSPACE_TOP;
		if (window->y > maximum_y)
			window->y = maximum_y;
	}
	return 1;
}

static int files_entry_at(const struct shell_state *shell,
			  const struct shell_window *window)
{
	int row;
	unsigned int index;

	if (shell->pointer_x < window->x + 16 ||
	    shell->pointer_x >= window->x + window->width - 16 ||
	    shell->pointer_y < window->y + 64 ||
	    shell->pointer_y >= window->y + 64 + FILES_VISIBLE_ROWS * 20)
		return -1;
	row = (shell->pointer_y - window->y - 64) / 20;
	index = shell->files.scroll + row;
	if (index >= shell->files.count)
		return -1;
	return index;
}

static int handle_pointer_button(struct shell_state *shell, int pressed)
{
	struct shell_window *window;
	uint64_t now;
	int icon;
	int task;
	int launcher;
	int app;

	if (!pressed) {
		shell->dragging = -1;
		return 1;
	}
	shell->last_input_ms = shell_monotonic_ms();
	if (shell->view == SHELL_VIEW_SPLASH)
		return 1;
	if (shell->view == SHELL_VIEW_LOGIN || shell->view == SHELL_VIEW_LOCK) {
		if (shell->view == SHELL_VIEW_LOGIN &&
		    shell->pointer_x >= LOGIN_FIELD_X &&
		    shell->pointer_x < LOGIN_FIELD_X + LOGIN_FIELD_WIDTH &&
		    shell->pointer_y >= LOGIN_USERNAME_Y &&
		    shell->pointer_y < LOGIN_USERNAME_Y + 34)
			shell->login.field = LOGIN_FIELD_USERNAME;
		else if (shell->pointer_x >= LOGIN_FIELD_X &&
			 shell->pointer_x < LOGIN_FIELD_X + LOGIN_FIELD_WIDTH &&
			 shell->pointer_y >= LOGIN_PASSWORD_Y &&
			 shell->pointer_y < LOGIN_PASSWORD_Y + 34)
			shell->login.field = LOGIN_FIELD_PASSWORD;
		else if (shell->pointer_x >= LOGIN_BUTTON_X &&
			 shell->pointer_x < LOGIN_BUTTON_X + LOGIN_BUTTON_WIDTH &&
			 shell->pointer_y >= LOGIN_BUTTON_Y &&
			 shell->pointer_y < LOGIN_BUTTON_Y + LOGIN_BUTTON_HEIGHT)
			queue_login(shell);
		return 1;
	}
	if (start_button_at(shell->pointer_x, shell->pointer_y)) {
		shell->menu_open = !shell->menu_open;
		return 1;
	}
	launcher = launcher_at(shell, shell->pointer_x, shell->pointer_y);
	if (launcher >= 0) {
		shell->selected = launcher;
		if (launcher == SHELL_MENU_LOCK)
			lock_desktop_session(shell);
		else if (launcher == SHELL_MENU_LOGOUT)
			end_desktop_session(shell);
		else
			open_window(shell, launcher);
		return 1;
	}
	if (shell->menu_open &&
	    menu_contains(shell->pointer_x, shell->pointer_y))
		return 1;
	shell->menu_open = 0;
	task = task_at(shell, shell->pointer_x, shell->pointer_y);
	if (task >= 0) {
		toggle_task_window(shell, task);
		return 1;
	}
	app = window_at(shell, shell->pointer_x, shell->pointer_y);
	if (app < 0) {
		icon = desktop_icon_at(shell->pointer_x, shell->pointer_y);
		now = shell_monotonic_ms();
		if (icon >= 0) {
			shell->desktop_selected = icon;
			if (shell->desktop_last_clicked == icon &&
			    now - shell->desktop_last_click_ms <= 500) {
				shell->desktop_last_clicked = -1;
				open_window(shell, icon);
			} else {
				shell->desktop_last_clicked = icon;
				shell->desktop_last_click_ms = now;
			}
			return 1;
		}
		shell->desktop_selected = -1;
		shell->desktop_last_clicked = -1;
		shell->focused = -1;
		return 1;
	}
	raise_window(shell, app);
	window = &shell->windows[app];
	if (shell->pointer_y >= window->y &&
	    shell->pointer_y < window->y + SHELL_TITLE_HEIGHT) {
		int control_x = shell->pointer_x -
			(window->x + window->width - 72);

		if (control_x >= 48) {
			close_window(shell, app);
			return 1;
		}
		if (control_x >= 24) {
			toggle_maximize_window(shell, app);
			return 1;
		}
		if (control_x >= 0) {
			minimize_window(shell, app);
			return 1;
		}
	}
	if (shell->pointer_y < window->y + SHELL_TITLE_HEIGHT &&
	    !window->maximized) {
		shell->dragging = app;
		shell->drag_offset_x = shell->pointer_x - window->x;
		shell->drag_offset_y = shell->pointer_y - window->y;
		return 1;
	}
	if (app == SHELL_APP_FILES) {
		int index = files_entry_at(shell, window);
		uint64_t click_time = shell_monotonic_ms();

		if (index < 0)
			return 1;
		shell->files.selected = index;
		if (shell->files.last_clicked == index &&
		    click_time - shell->files.last_click_ms <= 500) {
			shell->files.last_clicked = -1;
			return files_open_selected(&shell->files);
		}
		shell->files.last_clicked = index;
		shell->files.last_click_ms = click_time;
	} else if (app == SHELL_APP_SETTINGS) {
		int row = settings_row_at(window, shell->pointer_x,
					  shell->pointer_y);

		if (row >= 0) {
			shell->settings.selected = row;
			return settings_adjust(shell, row, 1);
		}
	}
	return 1;
}

static void collect_input_event(struct shell_state *shell,
				const struct input_event *event, int *delta_x,
				int *delta_y, int *wheel, int *button_pressed,
				int *button_released, int *changed)
{
	if (event->type == EV_REL) {
		if (event->code == REL_X)
			*delta_x += event->value;
		else if (event->code == REL_Y)
			*delta_y += event->value;
		else if (event->code == REL_WHEEL)
			*wheel += event->value;
		return;
	}
	if (event->type != EV_KEY)
		return;
	if (event->code == BTN_LEFT) {
		if (event->value == 1)
			*button_pressed = 1;
		else if (event->value == 0)
			*button_released = 1;
		return;
	}
	*changed |= handle_key_event(shell, event->code, event->value);
}

static int poll_inputs(struct shell_state *shell)
{
	struct pollfd poll_fds[SHELL_INPUT_COUNT + 1];
	unsigned int i;
	unsigned int poll_count = shell->input_count;
	int terminal_index = -1;
	int changed = 0;
	int ret;

	for (i = 0; i < shell->input_count; i++) {
		poll_fds[i].fd = shell->inputs[i].fd;
		poll_fds[i].events = POLLIN;
		poll_fds[i].revents = 0;
	}
	if (shell->terminal.master_fd >= 0) {
		terminal_index = poll_count++;
		poll_fds[terminal_index].fd = shell->terminal.master_fd;
		poll_fds[terminal_index].events = POLLIN;
		poll_fds[terminal_index].revents = 0;
	}
	do {
		ret = poll(poll_fds, poll_count, SHELL_POLL_MS);
	} while (ret < 0 && errno == EINTR && !stop);
	if (stop)
		return 0;
	if (ret < 0)
		return -1;
	if (terminal_index >= 0 &&
	    (poll_fds[terminal_index].revents & (POLLIN | POLLHUP | POLLERR))) {
		ret = drain_terminal(&shell->terminal);
		if (ret < 0)
			return -1;
		changed |= ret;
	}

	for (i = 0; i < shell->input_count; i++) {
		struct input_event events[16];
		ssize_t bytes;
		unsigned int j;
		int delta_x = 0;
		int delta_y = 0;
		int wheel = 0;
		int button_pressed = 0;
		int button_released = 0;

		if (!(poll_fds[i].revents & POLLIN))
			continue;
		while ((bytes = read(poll_fds[i].fd, events, sizeof(events))) > 0) {
			for (j = 0; j < (unsigned int)bytes / sizeof(events[0]); j++)
				collect_input_event(shell, &events[j], &delta_x,
						    &delta_y, &wheel,
						    &button_pressed,
						    &button_released, &changed);
		}
		if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		changed |= update_pointer(shell,
					  delta_x * (int)shell->settings.pointer_scale,
					  delta_y * (int)shell->settings.pointer_scale);
		if (wheel && shell->view == SHELL_VIEW_LOGIN) {
			unsigned int key = wheel > 0 ? KEY_UP : KEY_DOWN;

			changed |= handle_login_key(shell, key);
		} else if (wheel && shell->view == SHELL_VIEW_DESKTOP) {
			unsigned int key = wheel > 0 ? KEY_UP : KEY_DOWN;

			shell->last_input_ms = shell_monotonic_ms();
			if (shell->focused == SHELL_APP_FILES)
				changed |= handle_files_key(&shell->files, key);
			else
				changed |= handle_key(shell, key);
		}
		if (button_pressed)
			changed |= handle_pointer_button(shell, 1);
		if (button_released)
			changed |= handle_pointer_button(shell, 0);
	}
	return changed;
}

static int present_shell(int drm_fd, __u32 crtc_id,
			 struct shell_state *shell)
{
	unsigned int next = (shell->visible + 1) % SHELL_BUFFER_COUNT;
	struct drm_mode_crtc_page_flip flip = {
		.crtc_id = crtc_id,
		.fb_id = shell->buffers[next].fb.fb_id,
		.flags = DRM_MODE_PAGE_FLIP_EVENT,
		.user_data = shell->serial++,
	};
	__u32 sequence;

	draw_shell(&shell->buffers[next], shell);
	if (xioctl(drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0 ||
	    wait_flip_event(drm_fd, flip.user_data, &sequence) < 0)
		return -1;
	shell->visible = next;
	update_vnc(shell, &shell->buffers[next]);
	return 0;
}

static void shell_usage(const char *program)
{
	fprintf(stderr, "Usage: %s [CARD]\n", program);
}

int main(int argc, char **argv)
{
	const char *card = "/dev/dri/card0";
	struct shell_state shell = {
		.terminal = {
			.master_fd = -1,
		},
		.serial = 1,
		.view = SHELL_VIEW_SPLASH,
		.login = {
			.field = LOGIN_FIELD_USERNAME,
			.auth_state = LOGIN_AUTH_IDLE,
		},
		.cursor_visible = 1,
		.desktop_selected = -1,
		.desktop_last_clicked = -1,
		.pointer_x = TEST_WIDTH / 2,
		.pointer_y = TEST_HEIGHT / 2,
	};
	struct drm_mode_card_res resources;
	struct drm_mode_modeinfo mode;
	struct drm_mode_crtc crtc = { };
	struct drm_mode_crtc saved_crtc = { };
	__u32 *connector_ids = NULL;
	__u32 *crtc_ids = NULL;
	__u32 connector_id;
	unsigned int created = 0;
	unsigned int i;
	uint64_t last_second = 0;
	int crtc_active = 0;
	int drm_fd = -1;
	int status = EXIT_FAILURE;

	if (argc > 2 || (argc == 2 && !strcmp(argv[1], "--help"))) {
		shell_usage(argv[0]);
		return argc == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	if (argc == 2)
		card = argv[1];

	for (i = 0; i < ARRAY_SIZE(shell.buffers); i++)
		shell.buffers[i].map = MAP_FAILED;
	for (i = 0; i < ARRAY_SIZE(shell.inputs); i++)
		shell.inputs[i].fd = -1;
	settings_defaults(&shell.settings);
	init_windows(&shell);
	shell.login.splash_until_ms = shell_monotonic_ms() + LOGIN_SPLASH_MS;

	drm_fd = open(card, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror(card);
		goto out;
	}
	if (ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0) < 0 && errno != EINVAL) {
		perror("DRM_IOCTL_SET_MASTER");
		goto out;
	}
	if (get_resources(drm_fd, &resources, &crtc_ids, &connector_ids) < 0 ||
	    !resources.count_crtcs || !resources.count_connectors) {
		perror("DRM_IOCTL_MODE_GETRESOURCES");
		goto out;
	}
	if (select_output(drm_fd, &resources, connector_ids, &connector_id,
			  &mode) < 0) {
		perror("no connected DRM output");
		goto out;
	}
	if (mode.hdisplay != TEST_WIDTH || mode.vdisplay != TEST_HEIGHT) {
		fprintf(stderr, "unsupported mode %ux%u\n", mode.hdisplay,
			mode.vdisplay);
		goto out;
	}

	saved_crtc.crtc_id = crtc_ids[0];
	if (xioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &saved_crtc) < 0) {
		perror("DRM_IOCTL_MODE_GETCRTC");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(shell.buffers); i++) {
		if (create_buffer(drm_fd, &shell.buffers[i], -1,
				  TEST_FORMAT_RGB565) < 0) {
			created = i + 1;
			perror("create RGB565 dumb framebuffer");
			goto out;
		}
		created = i + 1;
	}
	draw_shell(&shell.buffers[0], &shell);
	if (start_vnc(&shell, &shell.buffers[0]) < 0) {
		fprintf(stderr, "unable to start VNC server\n");
		goto out;
	}

	crtc.set_connectors_ptr = user_ptr(&connector_id);
	crtc.count_connectors = 1;
	crtc.crtc_id = crtc_ids[0];
	crtc.fb_id = shell.buffers[0].fb.fb_id;
	crtc.mode_valid = 1;
	crtc.mode = mode;
	if (xioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
		perror("DRM_IOCTL_MODE_SETCRTC");
		goto out;
	}
	crtc_active = 1;

	open_inputs(&shell);
	printf("wiidesk: active %ux%u rgb565 with %u input device(s)\n",
	       mode.hdisplay, mode.vdisplay, shell.input_count);
	printf("wiidesk: greeter ready; unprivileged su provides PAM authentication\n");
	fflush(stdout);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	while (!stop) {
		uint64_t now;
		uint64_t second;
		int changed = poll_inputs(&shell);

		if (changed < 0) {
			perror("poll input");
			goto out;
		}
		changed |= process_vnc(&shell);
		now = shell_monotonic_ms();
		if (shell.view == SHELL_VIEW_SPLASH &&
		    now >= shell.login.splash_until_ms) {
			shell.view = SHELL_VIEW_LOGIN;
			changed = 1;
		}
		if (shell.login.auth_state == LOGIN_AUTH_QUEUED) {
			int unlocking = shell.view == SHELL_VIEW_LOCK;

			shell.login.auth_state = LOGIN_AUTH_RUNNING;
			if (present_shell(drm_fd, crtc.crtc_id, &shell) < 0) {
				perror("page flip");
				goto out;
			}
			if (unlocking)
				(void)unlock_desktop_session(&shell);
			else
				(void)start_desktop_session(&shell);
			changed = 1;
		}
		if (shell.view == SHELL_VIEW_DESKTOP &&
		    settings_lock_minutes(&shell.settings) &&
		    now - shell.last_input_ms >=
		    (uint64_t)settings_lock_minutes(&shell.settings) * 60000) {
			lock_desktop_session(&shell);
			changed = 1;
		}
		second = now / 1000;
		if (second != last_second) {
			last_second = second;
			shell.cursor_visible = !shell.cursor_visible;
			if (shell.view == SHELL_VIEW_DESKTOP &&
			    shell.windows[SHELL_APP_SYSTEM].visible)
				refresh_system(&shell.system);
			changed = 1;
		}
		if (changed && present_shell(drm_fd, crtc.crtc_id, &shell) < 0) {
			perror("page flip");
			goto out;
		}
	}
	status = EXIT_SUCCESS;

out:
	stop_vnc(&shell);
	stop_terminal(&shell.terminal);
	for (i = 0; i < shell.input_count; i++) {
		if (shell.inputs[i].fd >= 0) {
			(void)ioctl(shell.inputs[i].fd, EVIOCGRAB, 0);
			close(shell.inputs[i].fd);
		}
	}
	if (crtc_active && saved_crtc.fb_id) {
		saved_crtc.set_connectors_ptr = user_ptr(&connector_id);
		saved_crtc.count_connectors = 1;
		if (xioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &saved_crtc) < 0)
			perror("restore CRTC");
	}
	while (created)
		destroy_buffer(drm_fd, &shell.buffers[--created]);
	free(crtc_ids);
	free(connector_ids);
	if (drm_fd >= 0)
		close(drm_fd);
	return status;
}
