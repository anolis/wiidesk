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
IMAGE_LIBS ?= -lpng -ljpeg
ARCHIVE_CPPFLAGS ?=
ARCHIVE_LIBS ?= -larchive
AUDIO_CPPFLAGS ?=
AUDIO_LDFLAGS ?=
AUDIO_LIBS ?= -lasound -lmpg123
.PHONY: audio-tests audio-probe
audio-probe: $(BUILD_DIR)/audio-probe
$(BUILD_DIR)/audio-probe: tests/audio_probe.c src/audio_decode.c src/audio_decode.h src/audio_playlist.c src/audio_playlist.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Isrc $(AUDIO_CPPFLAGS) tests/audio_probe.c src/audio_decode.c src/audio_playlist.c -o $@ $(AUDIO_LDFLAGS) $(AUDIO_LIBS)
audio-tests: $(BUILD_DIR)/audio-probe $(BUILD_DIR)/wiidesk-audio-worker
	python3 tests/audio_test.py $(BUILD_DIR)
.PHONY: package-tests package-probe
package-probe: $(BUILD_DIR)/package-probe
$(BUILD_DIR)/package-probe: tests/package_probe.c src/package_io.c src/package_io.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Isrc tests/package_probe.c src/package_io.c -o $@
package-tests: $(BUILD_DIR)/package-probe
	python3 tests/package_io_test.py $(BUILD_DIR)/package-probe
.PHONY: archive-tests archive-probe
archive-probe: $(BUILD_DIR)/archive-probe
$(BUILD_DIR)/archive-probe: tests/archive_probe.c src/archive_io.c src/archive_io.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Isrc $(X11_CPPFLAGS) $(ARCHIVE_CPPFLAGS) tests/archive_probe.c src/archive_io.c -o $@ $(X11_LDFLAGS) $(ARCHIVE_LIBS)
archive-tests: $(BUILD_DIR)/archive-probe
	python3 tests/archive_io_test.py $(BUILD_DIR)/archive-probe
.PHONY: image-tests image-probe
image-probe: $(BUILD_DIR)/image-decode-probe
$(BUILD_DIR)/image-decode-probe: tests/image_decode_probe.c src/image_decode.c src/image_decode.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Isrc $(X11_CPPFLAGS) tests/image_decode_probe.c src/image_decode.c -o $@ $(X11_LDFLAGS) $(IMAGE_LIBS)
image-tests: $(BUILD_DIR)/image-decode-test.so
	python3 tests/image_decode_test.py $(BUILD_DIR)/image-decode-test.so
$(BUILD_DIR)/image-decode-test.so: src/image_decode.c src/image_decode.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -fPIC -shared $(X11_CPPFLAGS) src/image_decode.c -o $@ $(X11_LDFLAGS) $(IMAGE_LIBS)
.PHONY: wiidesk-x11 x11-test-client
wiidesk-x11: $(BUILD_DIR)/wiidesk-x11 $(BUILD_DIR)/wiidesk-x11-app $(BUILD_DIR)/wiidesk-session-health $(BUILD_DIR)/wiidesk-x11-image $(BUILD_DIR)/wiidesk-x11-archive $(BUILD_DIR)/wiidesk-x11-packages $(BUILD_DIR)/wiidesk-package-install $(BUILD_DIR)/wiidesk-package-terminal $(BUILD_DIR)/wiidesk-x11-audio $(BUILD_DIR)/wiidesk-audio-worker
x11-test-client: $(BUILD_DIR)/x11-wm-smoke

.PHONY: x11-controls-test calculator-test utility-unit-tests
# Run on the host with CROSS_COMPILE=; the fake Wi-Fi socket never uses wlan0.
utility-unit-tests: $(BUILD_DIR)/calculator-test $(BUILD_DIR)/wifi-control-test.so
	$(BUILD_DIR)/calculator-test
	python3 tests/wifi_control_test.py $(BUILD_DIR)/wifi-control-test.so

$(BUILD_DIR)/wifi-control-test.so: src/wifi_control.c src/wifi_control.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -fPIC -shared src/wifi_control.c -o $@

calculator-test: $(BUILD_DIR)/calculator-test
$(BUILD_DIR)/calculator-test: tests/calculator_test.c src/calculator.c src/calculator.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Isrc tests/calculator_test.c src/calculator.c -lm -o $@

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

$(BUILD_DIR)/wiidesk-x11-image: src/x11_image.c src/image_decode.c src/image_decode.h src/x11_controls.c src/x11_controls.h src/x11_preferences.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) src/x11_image.c src/image_decode.c src/x11_controls.c -o $@ $(X11_LDFLAGS) $(X11_LIBS) $(IMAGE_LIBS) -lm

$(BUILD_DIR)/wiidesk-x11-archive: src/x11_archive.c src/archive_io.c src/archive_io.h src/x11_controls.c src/x11_controls.h src/x11_preferences.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) $(ARCHIVE_CPPFLAGS) src/x11_archive.c src/archive_io.c src/x11_controls.c -o $@ $(X11_LDFLAGS) $(X11_LIBS) $(ARCHIVE_LIBS)

$(BUILD_DIR)/wiidesk-x11-packages: src/x11_packages.c src/package_io.c src/package_io.h src/x11_controls.c src/x11_controls.h src/x11_preferences.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) src/x11_packages.c src/package_io.c src/x11_controls.c -o $@ $(X11_LDFLAGS) $(X11_LIBS)

$(BUILD_DIR)/wiidesk-package-install: src/package_install.c src/package_io.c src/package_io.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror src/package_install.c src/package_io.c -o $@

$(BUILD_DIR)/wiidesk-package-terminal: src/package_terminal.c
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@

$(BUILD_DIR)/wiidesk-x11-audio: src/x11_audio.c src/audio_playlist.c src/audio_playlist.h src/x11_controls.c src/x11_controls.h src/x11_preferences.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) src/x11_audio.c src/audio_playlist.c src/x11_controls.c -o $@ $(X11_LDFLAGS) $(X11_LIBS)

$(BUILD_DIR)/wiidesk-audio-worker: src/audio_worker.c src/audio_decode.c src/audio_decode.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(AUDIO_CPPFLAGS) src/audio_worker.c src/audio_decode.c -o $@ $(AUDIO_LDFLAGS) $(AUDIO_LIBS)

$(BUILD_DIR)/x11-wm-smoke: tests/x11_wm_smoke.c
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) $< -o $@ $(X11_LDFLAGS) $(X11_LIBS)

$(BUILD_DIR)/wiidesk-x11-app: src/wiidesk_x11_app.c src/x11_preferences.h src/x11_controls.c src/x11_controls.h src/x11_app_io.c src/x11_app_io.h src/x11_session.h src/x11_utilities.c src/x11_utilities.h src/calculator.c src/calculator.h src/x11_network.c src/x11_network.h src/wifi_control.c src/wifi_control.h
	mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror $(X11_CPPFLAGS) src/wiidesk_x11_app.c src/x11_controls.c src/x11_app_io.c src/x11_utilities.c src/calculator.c src/x11_network.c src/wifi_control.c -o $@ $(X11_LDFLAGS) $(X11_LIBS) -lm
