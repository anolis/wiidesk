# SPDX-License-Identifier: GPL-2.0-only

CROSS_COMPILE ?= powerpc-linux-gnu-
KERNEL_DIR ?= ../wii-linux-nxt
BUILD_DIR ?= build
HEADERS_DIR := $(BUILD_DIR)/headers
LIBVNCSERVER_DIR := $(BUILD_DIR)/libvncserver
LIBVNCSERVER_SOURCE := $(BUILD_DIR)/libvncserver-src
CC := $(CROSS_COMPILE)gcc

CFLAGS := -static -O2 -Wall -Wextra -Werror
CPPFLAGS := -I$(HEADERS_DIR)/include

.PHONY: all clean headers libvncserver wiidesk wiidesk-vnc

all: wiidesk

wiidesk: $(BUILD_DIR)/wiidesk

wiidesk-vnc: $(BUILD_DIR)/wiidesk-vnc

headers: $(HEADERS_DIR)/include/drm/drm.h

$(HEADERS_DIR)/include/drm/drm.h:
	mkdir -p $(HEADERS_DIR)
	$(MAKE) -C $(KERNEL_DIR) -j16 ARCH=powerpc \
		INSTALL_HDR_PATH=$(abspath $(HEADERS_DIR)) headers_install

$(BUILD_DIR)/wiidesk: src/wiidesk.c src/drm_backend.c src/font_8x16.c \
		$(HEADERS_DIR)/include/drm/drm.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) src/wiidesk.c -o $@

libvncserver: $(LIBVNCSERVER_DIR)/lib/libvncserver.a

$(LIBVNCSERVER_DIR)/lib/libvncserver.a:
	scripts/build-libvncserver.sh \
		$(abspath $(LIBVNCSERVER_SOURCE)) $(abspath $(LIBVNCSERVER_DIR))

$(BUILD_DIR)/wiidesk-vnc: src/wiidesk.c src/drm_backend.c src/font_8x16.c \
		$(HEADERS_DIR)/include/drm/drm.h \
		$(LIBVNCSERVER_DIR)/lib/libvncserver.a
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DWII_HAVE_VNC \
		-I$(LIBVNCSERVER_DIR)/include src/wiidesk.c \
		$(LIBVNCSERVER_DIR)/lib/libvncserver.a -o $@

clean:
	rm -rf $(BUILD_DIR)

# Optional X11 window manager; native DRM targets remain dependency-free.
# X11_CPPFLAGS/X11_LDFLAGS can point to an extracted PowerPC development sysroot.
X11_CPPFLAGS ?=
X11_LDFLAGS ?=
X11_LIBS ?= -lX11
X11_SESSION_LIBS ?= -lXss
.PHONY: wiidesk-x11 x11-test-client
wiidesk-x11: $(BUILD_DIR)/wiidesk-x11 $(BUILD_DIR)/wiidesk-x11-app $(BUILD_DIR)/wiidesk-session-health
x11-test-client: $(BUILD_DIR)/x11-wm-smoke

.PHONY: x11-controls-test
x11-controls-test: $(BUILD_DIR)/x11-controls-test
$(BUILD_DIR)/x11-controls-test: tests/x11_controls_io_test.c src/x11_controls.c src/x11_app_io.c src/x11_controls.h src/x11_app_io.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) -Isrc tests/x11_controls_io_test.c src/x11_controls.c src/x11_app_io.c -o $@ $(X11_LDFLAGS) $(X11_LIBS)
$(BUILD_DIR)/wiidesk-x11: src/wiidesk_x11.c src/x11_preferences.h src/x11_session.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) $< -o $@ $(X11_LDFLAGS) $(X11_LIBS) $(X11_SESSION_LIBS)

$(BUILD_DIR)/wiidesk-session-health: src/x11_session_health.c
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) $< -o $@ $(X11_LDFLAGS) $(X11_LIBS)

$(BUILD_DIR)/x11-wm-smoke: tests/x11_wm_smoke.c
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) $< -o $@ $(X11_LDFLAGS) $(X11_LIBS)

$(BUILD_DIR)/wiidesk-x11-app: src/wiidesk_x11_app.c src/x11_preferences.h src/x11_controls.c src/x11_controls.h src/x11_app_io.c src/x11_app_io.h src/x11_session.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) src/wiidesk_x11_app.c src/x11_controls.c src/x11_app_io.c -o $@ $(X11_LDFLAGS) $(X11_LIBS)
