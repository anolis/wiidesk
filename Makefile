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
