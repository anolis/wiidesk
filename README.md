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

## Source Layout

- `src/wiidesk.c`: desktop, windows, input, greeter, terminal, files, and VNC
- `src/drm_backend.c`: raw standard DRM/KMS dumb-buffer and page-flip backend
- `src/font_8x16.c`: GPL Linux VGA 8x16 font data
- `scripts/build-libvncserver.sh`: pinned minimal PowerPC VNC dependency build

The pre-split development history is preserved on the historical
`feature/wii-kolibri-shell` branch of Wii Linux NXT until this repository and
its release process are fully established.

## License

WiiDesk is licensed under GPL-2.0-only. See `LICENSE` and
`licenses/GPL-2.0`.
