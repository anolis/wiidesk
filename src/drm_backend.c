// SPDX-License-Identifier: GPL-2.0-only
/* Minimal raw-ioctl KMS test client for the Wii VI DRM driver. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#define TEST_WIDTH 640
#define TEST_HEIGHT 480
#define TEST_DRM_MODE_CONNECTED 1
#define TEST_FLIP_TIMEOUT_MS 2000

struct test_buffer {
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map_req;
	struct drm_mode_destroy_dumb destroy;
	struct drm_mode_fb_cmd fb;
	void *map;
};

enum test_pixel_format {
	TEST_FORMAT_XRGB8888,
	TEST_FORMAT_RGB565,
};

static volatile sig_atomic_t stop;

static void handle_signal(int signo)
{
	(void)signo;
	stop = 1;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static void *xcalloc(size_t count, size_t size)
{
	void *ptr;

	if (!count)
		return NULL;
	ptr = calloc(count, size);
	if (!ptr) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	return ptr;
}

static __u64 user_ptr(const void *ptr)
{
	return (__u64)(uintptr_t)ptr;
}

static __u64 monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (__u64)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static int get_resources(int fd, struct drm_mode_card_res *res,
			 __u32 **crtcs, __u32 **connectors)
{
	__u32 *fbs;
	__u32 *encoders;

	memset(res, 0, sizeof(*res));
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) < 0)
		return -1;

	fbs = xcalloc(res->count_fbs, sizeof(*fbs));
	*crtcs = xcalloc(res->count_crtcs, sizeof(**crtcs));
	*connectors = xcalloc(res->count_connectors, sizeof(**connectors));
	encoders = xcalloc(res->count_encoders, sizeof(*encoders));
	res->fb_id_ptr = user_ptr(fbs);
	res->crtc_id_ptr = user_ptr(*crtcs);
	res->connector_id_ptr = user_ptr(*connectors);
	res->encoder_id_ptr = user_ptr(encoders);

	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) < 0) {
		free(fbs);
		free(encoders);
		return -1;
	}

	free(fbs);
	free(encoders);
	return 0;
}

static int get_connector(int fd, __u32 connector_id,
			 struct drm_mode_get_connector *connector,
			 struct drm_mode_modeinfo **modes)
{
	__u32 *props;
	__u32 *encoders;
	__u64 *prop_values;

	memset(connector, 0, sizeof(*connector));
	connector->connector_id = connector_id;
	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, connector) < 0)
		return -1;

	*modes = xcalloc(connector->count_modes, sizeof(**modes));
	props = xcalloc(connector->count_props, sizeof(*props));
	prop_values = xcalloc(connector->count_props, sizeof(*prop_values));
	encoders = xcalloc(connector->count_encoders, sizeof(*encoders));
	connector->modes_ptr = user_ptr(*modes);
	connector->props_ptr = user_ptr(props);
	connector->prop_values_ptr = user_ptr(prop_values);
	connector->encoders_ptr = user_ptr(encoders);

	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, connector) < 0) {
		free(*modes);
		*modes = NULL;
		free(props);
		free(prop_values);
		free(encoders);
		return -1;
	}

	free(props);
	free(prop_values);
	free(encoders);
	return 0;
}

static int select_output(int fd, const struct drm_mode_card_res *res,
			 const __u32 *connector_ids, __u32 *connector_id,
			 struct drm_mode_modeinfo *mode)
{
	unsigned int i;

	for (i = 0; i < res->count_connectors; i++) {
		struct drm_mode_get_connector connector;
		struct drm_mode_modeinfo *modes;
		unsigned int j;

		if (get_connector(fd, connector_ids[i], &connector, &modes) < 0)
			continue;
		if (connector.connection != TEST_DRM_MODE_CONNECTED ||
		    !connector.count_modes) {
			free(modes);
			continue;
		}

		for (j = 0; j < connector.count_modes; j++) {
			if (modes[j].hdisplay == TEST_WIDTH &&
			    modes[j].vdisplay == TEST_HEIGHT)
				break;
		}
		if (j == connector.count_modes)
			j = 0;
		*connector_id = connector.connector_id;
		*mode = modes[j];
		free(modes);
		return 0;
	}

	errno = ENODEV;
	return -1;
}

static uint32_t pattern_color(unsigned int x, unsigned int y, int marker_x)
{
	static const uint32_t colors[4] = {
		0x00ff2020, 0x0020ff20, 0x002040ff, 0x00ffffff,
	};
	unsigned int quadrant = (y >= TEST_HEIGHT / 2) * 2 +
				  (x >= TEST_WIDTH / 2);
	uint32_t pixel = colors[quadrant];

	if (x < 8 || x >= TEST_WIDTH - 8 || y < 8 ||
	    y >= TEST_HEIGHT - 8)
		pixel = 0x00ffffff;
	else if ((x % 80) < 3 || (y % 60) < 3)
		pixel = 0x00000000;
	if (x >= 240 && x < 400 && y >= 180 && y < 300)
		pixel = ((x / 10) ^ (y / 10)) & 1 ?
			0x00ff00ff : 0x0000ffff;
	if (marker_x >= 0 && x >= (unsigned int)marker_x &&
	    x < (unsigned int)marker_x + 16 && y >= 32 &&
	    y < TEST_HEIGHT - 32)
		pixel = 0x00ffff00;
	return pixel;
}

static uint16_t xrgb8888_to_rgb565(uint32_t pixel)
{
	return ((pixel >> 19) & 0x1f) << 11 |
	       ((pixel >> 10) & 0x3f) << 5 |
	       ((pixel >> 3) & 0x1f);
}

static void draw_pattern(void *map, __u32 pitch, int marker_x,
			 enum test_pixel_format format)
{
	unsigned int x;
	unsigned int y;

	for (y = 0; y < TEST_HEIGHT; y++) {
		uint8_t *row = (uint8_t *)map + (size_t)y * pitch;

		for (x = 0; x < TEST_WIDTH; x++)
			if (format == TEST_FORMAT_RGB565)
				((uint16_t *)row)[x] = xrgb8888_to_rgb565(
					pattern_color(x, y, marker_x));
			else
				((uint32_t *)row)[x] = pattern_color(x, y,
								marker_x);
	}
}

static int create_buffer(int fd, struct test_buffer *buffer, int marker_x,
			 enum test_pixel_format format)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->map = MAP_FAILED;
	buffer->create.width = TEST_WIDTH;
	buffer->create.height = TEST_HEIGHT;
	buffer->create.bpp = format == TEST_FORMAT_RGB565 ? 16 : 32;
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer->create) < 0)
		return -1;

	buffer->destroy.handle = buffer->create.handle;
	buffer->map_req.handle = buffer->create.handle;
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &buffer->map_req) < 0)
		return -1;
	buffer->map = mmap(NULL, buffer->create.size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, buffer->map_req.offset);
	if (buffer->map == MAP_FAILED)
		return -1;
	draw_pattern(buffer->map, buffer->create.pitch, marker_x, format);

	buffer->fb.width = TEST_WIDTH;
	buffer->fb.height = TEST_HEIGHT;
	buffer->fb.pitch = buffer->create.pitch;
	buffer->fb.bpp = buffer->create.bpp;
	buffer->fb.depth = format == TEST_FORMAT_RGB565 ? 16 : 24;
	buffer->fb.handle = buffer->create.handle;
	if (xioctl(fd, DRM_IOCTL_MODE_ADDFB, &buffer->fb) < 0)
		return -1;
	return 0;
}

static void destroy_buffer(int fd, struct test_buffer *buffer)
{
	if (buffer->fb.fb_id &&
	    xioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer->fb.fb_id) < 0)
		perror("DRM_IOCTL_MODE_RMFB");
	if (buffer->map != MAP_FAILED)
		munmap(buffer->map, buffer->create.size);
	if (buffer->destroy.handle &&
	    xioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &buffer->destroy) < 0)
		perror("DRM_IOCTL_MODE_DESTROY_DUMB");
}

static unsigned int parse_unsigned(const char *value, const char *name,
				   unsigned int maximum)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || !*value || *end || parsed > maximum) {
		fprintf(stderr, "invalid %s: %s\n", name, value);
		exit(EXIT_FAILURE);
	}
	return parsed;
}

static int wait_flip_event(int fd, __u64 expected, __u32 *sequence)
{
	unsigned char data[256];
	struct pollfd poll_fd = {
		.fd = fd,
		.events = POLLIN,
	};
	ssize_t length;
	size_t offset;
	int ret;

	do {
		ret = poll(&poll_fd, 1, TEST_FLIP_TIMEOUT_MS);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0) {
		if (!ret)
			errno = ETIMEDOUT;
		return -1;
	}
	length = read(fd, data, sizeof(data));
	if (length < 0)
		return -1;

	for (offset = 0; offset + sizeof(struct drm_event) <= (size_t)length;) {
		const struct drm_event *event = (const void *)(data + offset);
		const struct drm_event_vblank *vblank;

		if (event->length < sizeof(*event) ||
		    offset + event->length > (size_t)length) {
			errno = EPROTO;
			return -1;
		}
		if (event->type == DRM_EVENT_FLIP_COMPLETE) {
			if (event->length < sizeof(*vblank)) {
				errno = EPROTO;
				return -1;
			}
			vblank = (const void *)event;
			if (vblank->user_data != expected) {
				errno = EPROTO;
				return -1;
			}
			*sequence = vblank->sequence;
			return 0;
		}
		offset += event->length;
	}

	errno = EPROTO;
	return -1;
}

static int run_flips(int fd, __u32 crtc_id, struct test_buffer *buffers,
		     unsigned int count, unsigned int delay_ms,
		     enum test_pixel_format format, int animate, int pipeline)
{
	__u64 start_ns = monotonic_ns();
	__u64 elapsed_ns;
	__u64 rate_millihz;
	unsigned int i;
	__u32 last_sequence = 0;

	if (pipeline)
		draw_pattern(buffers[1].map, buffers[1].create.pitch, 8,
			     format);

	for (i = 0; i < count && !stop; i++) {
		unsigned int next_index = pipeline ? (i + 1) % 3 : (i + 1) & 1;
		struct test_buffer *next = &buffers[next_index];
		struct drm_mode_crtc_page_flip flip = {
			.crtc_id = crtc_id,
			.fb_id = next->fb.fb_id,
			.flags = DRM_MODE_PAGE_FLIP_EVENT,
			.user_data = i + 1,
		};
		int marker_x = 8 + i * 7 % (TEST_WIDTH - 32);

		if (animate && !pipeline)
			draw_pattern(next->map, next->create.pitch, marker_x,
				     format);

		if (xioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
			return -1;
		if (pipeline && i + 1 < count) {
			unsigned int frame = i + 2;
			struct test_buffer *future = &buffers[frame % 3];

			marker_x = 8 + (frame - 1) * 7 % (TEST_WIDTH - 32);
			draw_pattern(future->map, future->create.pitch, marker_x,
				     format);
		}
		if (wait_flip_event(fd, flip.user_data, &last_sequence) < 0)
			return -1;
		if (delay_ms && poll(NULL, 0, delay_ms) < 0 && errno != EINTR)
			return -1;
	}

	elapsed_ns = monotonic_ns() - start_ns;
	rate_millihz = elapsed_ns ?
		(__u64)i * 1000000000000ULL / elapsed_ns : 0;
	printf("wii-drm-test: flips=%u last-vblank=%u elapsed-us=%llu",
	       i, last_sequence, (unsigned long long)(elapsed_ns / 1000));
	printf(" avg-us=%llu rate=%llu.%03llu-hz\n",
	       (unsigned long long)(i ? elapsed_ns / i / 1000 : 0),
	       (unsigned long long)(rate_millihz / 1000),
	       (unsigned long long)(rate_millihz % 1000));
	return i == count ? 0 : -1;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s [--format xrgb8888|rgb565] ", program);
	fprintf(stderr, "[--flips COUNT] [--delay-ms MSEC] ");
	fprintf(stderr, "[--animate] [--pipeline] [--exit-after-flips] [CARD]\n");
}

int main(int argc, char **argv)
{
	const char *card = "/dev/dri/card0";
	struct test_buffer buffers[3] = {
		{ .map = MAP_FAILED },
		{ .map = MAP_FAILED },
		{ .map = MAP_FAILED },
	};
	struct drm_mode_card_res res;
	struct drm_mode_modeinfo mode;
	struct drm_mode_crtc crtc = { };
	__u32 *connector_ids = NULL;
	__u32 *crtc_ids = NULL;
	__u32 connector_id;
	unsigned int flip_count = 0;
	unsigned int delay_ms = 250;
	int animate = 0;
	int pipeline = 0;
	int exit_after_flips = 0;
	unsigned int buffer_count;
	unsigned int created = 0;
	unsigned int i;
	enum test_pixel_format format = TEST_FORMAT_XRGB8888;
	int card_set = 0;
	int fd = -1;
	int status = EXIT_FAILURE;

	for (i = 1; i < (unsigned int)argc; i++) {
		if (!strcmp(argv[i], "--format")) {
			if (++i >= (unsigned int)argc) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			if (!strcmp(argv[i], "xrgb8888"))
				format = TEST_FORMAT_XRGB8888;
			else if (!strcmp(argv[i], "rgb565"))
				format = TEST_FORMAT_RGB565;
			else {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			continue;
		}
		if (!strcmp(argv[i], "--flips")) {
			if (++i >= (unsigned int)argc) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			flip_count = parse_unsigned(argv[i], "flip count", 10000);
			continue;
		}
		if (!strcmp(argv[i], "--delay-ms")) {
			if (++i >= (unsigned int)argc) {
				usage(argv[0]);
				return EXIT_FAILURE;
			}
			delay_ms = parse_unsigned(argv[i], "flip delay", 60000);
			continue;
		}
		if (!strcmp(argv[i], "--exit-after-flips")) {
			exit_after_flips = 1;
			continue;
		}
		if (!strcmp(argv[i], "--animate")) {
			animate = 1;
			continue;
		}
		if (!strcmp(argv[i], "--pipeline")) {
			pipeline = 1;
			continue;
		}
		if (!strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		}
		if (argv[i][0] == '-' || card_set) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		card = argv[i];
		card_set = 1;
	}
	if ((animate || exit_after_flips) && !flip_count) {
		fprintf(stderr, "--animate and --exit-after-flips require --flips\n");
		return EXIT_FAILURE;
	}
	if (pipeline && !animate) {
		fprintf(stderr, "--pipeline requires --animate\n");
		return EXIT_FAILURE;
	}
	buffer_count = flip_count ? (pipeline ? 3 : 2) : 1;

	fd = open(card, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(card);
		goto out;
	}
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0 && errno != EINVAL) {
		perror("DRM_IOCTL_SET_MASTER");
		goto out;
	}
	if (get_resources(fd, &res, &crtc_ids, &connector_ids) < 0) {
		perror("DRM_IOCTL_MODE_GETRESOURCES");
		goto out;
	}
	if (!res.count_crtcs || !res.count_connectors) {
		fprintf(stderr, "DRM device exposes no usable CRTC/connector\n");
		goto out;
	}
	if (select_output(fd, &res, connector_ids, &connector_id, &mode) < 0) {
		perror("no connected DRM output");
		goto out;
	}
	if (mode.hdisplay != TEST_WIDTH || mode.vdisplay != TEST_HEIGHT) {
		fprintf(stderr, "unsupported mode %ux%u\n", mode.hdisplay,
			mode.vdisplay);
		goto out;
	}

	for (i = 0; i < buffer_count; i++) {
		int marker_x = flip_count ? (i ? 544 : 80) : -1;

		if (create_buffer(fd, &buffers[i], marker_x, format) < 0) {
			created = i + 1;
			perror("create dumb framebuffer");
			goto out;
		}
		created = i + 1;
	}

	crtc.set_connectors_ptr = user_ptr(&connector_id);
	crtc.count_connectors = 1;
	crtc.crtc_id = crtc_ids[0];
	crtc.fb_id = buffers[0].fb.fb_id;
	crtc.mode_valid = 1;
	crtc.mode = mode;
	if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
		perror("DRM_IOCTL_MODE_SETCRTC");
		goto out;
	}

	printf("wii-drm-test: active %ux%u %s crtc=%u connector=%u\n",
	       mode.hdisplay, mode.vdisplay, mode.name, crtc.crtc_id,
	       connector_id);
	printf("wii-drm-test: format=%s\n",
	       format == TEST_FORMAT_RGB565 ? "rgb565" : "xrgb8888");
	for (i = 0; i < buffer_count; i++) {
		printf("wii-drm-test: buffer=%u fb=%u handle=%u pitch=%u",
		       i, buffers[i].fb.fb_id, buffers[i].create.handle,
		       buffers[i].create.pitch);
		printf(" size=%llu\n",
		       (unsigned long long)buffers[i].create.size);
	}
	fflush(stdout);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	if (flip_count &&
	    run_flips(fd, crtc.crtc_id, buffers, flip_count, delay_ms,
		      format, animate, pipeline) < 0) {
		perror("page-flip test");
		goto out;
	}
	fflush(stdout);
	if (exit_after_flips) {
		status = EXIT_SUCCESS;
		goto out;
	}
	while (!stop)
		pause();
	status = EXIT_SUCCESS;

out:
	while (created)
		destroy_buffer(fd, &buffers[--created]);
	free(crtc_ids);
	free(connector_ids);
	if (fd >= 0)
		close(fd);
	return status;
}
