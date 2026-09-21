# WiiDesk

WiiDesk is a lightweight native desktop shell for Linux on Nintendo Wii. It
renders directly into standard DRM/KMS dumb buffers and is designed around the
Wii's constrained memory and fixed 640x480 display environment.

The kernel and hardware drivers live separately in
[Wii Linux NXT](https://github.com/anolis/wii-linux-nxt). Distribution image
assembly, boot files, firmware, and rootfs policy will live in a separate
`wiidesk-os` integration repository.

## Current Features

- Triple-buffered RGB565 DRM/KMS presentation
- Keyboard, mouse, and VNC input through one event path
- Movable, minimizable, and maximizable windows
- WiiDesk start menu and task panel
- PTY-backed terminal with restartable login sessions
- Filesystem browser and live system telemetry
- Splash, authenticated greeter, logout, and session recreation
- Optional loopback-only VNC export for use through an SSH tunnel

WiiDesk is experimental. It currently runs as a privileged shell because the
development rootfs restricts DRM and input devices. Authentication is delegated
to `/bin/su`; WiiDesk does not validate or store system password hashes.

## Build

Install a 32-bit big-endian PowerPC cross compiler and provide a Wii Linux NXT
kernel tree. The default build has no external userspace library dependency:

```sh
make -j16 KERNEL_DIR=/path/to/wii-linux-nxt wiidesk
```

The binary is written to `build/wiidesk`.

For VNC support, build the pinned minimal LibVNCServer configuration and the
VNC-enabled shell:

```sh
make -j16 KERNEL_DIR=/path/to/wii-linux-nxt libvncserver wiidesk-vnc
```

This produces `build/wiidesk-vnc`. The helper pins LibVNCServer 0.9.15 commit
`9b54b1ec32731bd23158ca014dc18014db4194c3` and disables TLS, authentication,
threads, image codecs, and file transfer. Do not expose its VNC listener to a
network. Bind it to loopback and connect through authenticated SSH forwarding.

## Run

On a Wii Linux NXT system with DRM/KMS active:

```sh
./wiidesk /dev/dri/card0
```

The DRM device argument is optional and defaults to `/dev/dri/card0`.

## X11 window manager

The planned core application suite and its implementation status are tracked in
[the app roadmap](docs/app-roadmap.md).

The optional `wiidesk-x11` executable manages ordinary X11 applications itself.
It provides window frames, click-to-focus, outline move/resize, minimize,
maximize, close, a task panel, and a launcher for Terminal, Files, System,
Settings, Editor, Processes, and Session. The built-in apps run as ordinary X11 clients. It runs as the session
user under Xorg and refuses to start if another window manager owns the display.

```sh
make CROSS_COMPILE= BUILD_DIR=build/host wiidesk-x11 x11-test-client
```

This build requires Xlib, XScreenSaver, libpng, libjpeg and libarchive development headers and libraries. See
[X11 build, session, and tests](docs/x11.md) for PowerPC cross builds and isolated
testing. The native DRM build remains the fallback. The X11
implementation remains experimental; [managed X11 sessions](docs/x11-sessions.md)
add XDM login at boot, password lock/unlock, configurable idle locking, and
logout, with supervised recovery to the native desktop. Suspend and
hibernate are unavailable in the current kernel. Install `wiidesk-x11`,
`wiidesk-x11-app`, `wiidesk-x11-image`, `wiidesk-x11-archive`,
`wiidesk-x11-packages`, `wiidesk-package-install`, `wiidesk-package-terminal` and
`wiidesk-session-health` in the same directory.
The [Package Installer](docs/package-installer.md) uses existing `sudo`, `xterm`,
`dpkg`, `dpkg-deb`, `dpkg-query` and `sha256sum` tools; it never invokes APT.

## Source Layout

- `src/wiidesk.c`: desktop, windows, input, greeter, terminal, files, and VNC
- `src/drm_backend.c`: raw standard DRM/KMS dumb-buffer and page-flip backend
- `src/font_8x16.c`: GPL Linux VGA 8x16 font data
- `src/wiidesk_x11.c`: optional non-compositing X11 window manager
- `tests/`: X11 client lifecycle and pointer/keyboard integration checks
- `scripts/build-libvncserver.sh`: pinned minimal PowerPC VNC dependency build

The pre-split development history is preserved on the historical
`archive/pre-split-wiidesk-history` branch of Wii Linux NXT. New WiiDesk
development occurs only in this repository.

## License

WiiDesk is licensed under GPL-2.0-only. See `LICENSE` and
`licenses/GPL-2.0`.
