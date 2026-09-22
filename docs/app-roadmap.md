# WiiDesk application roadmap

This records the core suite agreed in the development conversation. Keep it
updated as features are implemented and verified. An app appearing in the
launcher does not by itself mean its planned feature set is complete.

## Current priority

Restore the **Wii kernel audio driver** so the new Audio Player can produce
physical sound. Refresh the OS image with the app suite and dependencies, then
return to pointer/window performance work.

| App | Planned initial scope | Status |
| --- | --- | --- |
| Text Editor | Open/save dialogs, plain-text editing, search, undo, selection, clipboard, and configuration editing | Implemented and host-tested for bounded ASCII text; hardware file/control tests and startup/display verified |
| File Manager | Extend Files with copy, move, rename, new folders, and recoverable trash | Implemented and host-tested for regular-file copy, same-filesystem moves, and last-trash restore; hardware file/control tests and startup verified |
| Process Manager | Per-process CPU/memory, refresh, selection, and deliberate termination of stuck applications | Implemented and host-tested, including confirmed termination of a disposable process; hardware startup/display verified |
| Network Settings | Connection status, IP/DNS details, connectivity checks, and connection configuration | Host-tested including profile rollback/cancellation; hardware status and local TCP check passed; real Wi-Fi profile switching remains unverified |
| Image Viewer | Browse pictures, fit-to-screen, zoom, and bounded image decoding | Implemented for PNG/JPEG with fit, zoom/pan, bounded folder navigation and cancellable decoding; host/16-bit and Wii GUI checks passed; installed, launcher updates on next login; see [limits and evidence](image-viewer.md) |
| Log Viewer | Follow logs, filter messages, pause, and search | Host-tested for bounded regular ASCII logs, pause, rotation/truncation and filtering; hardware open/filter/append/rotation passed |
| Archive Manager | List and extract archives, with progress and cancellation | Implemented for ZIP/tar with bounded streaming, private output folders and incomplete markers; host/16-bit and Wii extraction/cancellation checks passed; installed, launcher updates on next login; see [rules and evidence](archive-manager.md) |
| Calculator | Basic arithmetic, scientific functions, and history | Host and hardware tests passed for evaluation, errors and history; radians, bounded parser and 64-entry in-memory history |
| Package Installer | Inspect/install transferred `.deb` bundles using `dpkg`; resolve/download dependencies on the host | Implemented with bounded snapshots, metadata review, sudo terminal and verified dpkg results; host/16-bit checks and Wii installation passed; see [limits and verification](package-installer.md) |
| Audio Player | Local playback, playlists, and simple controls | Implemented for bounded WAV/MP3 and local M3U playlists; host and Wii controls/playback passed with silent ALSA output; installed with launcher available; physical output blocked by the missing Wii kernel audio driver; see [controls and hardware gap](audio-player.md) |

## Existing desktop components

- WiiDesk X11 window manager and application launcher: implemented; continue
  compatibility testing as the suite grows.
- Terminal: uses xterm in the X11 session.
- System: live CPU, memory/swap, uptime, network counters, and graphics presence.
- Settings: persistent desktop background/accent colors.
- Session: XDM login, XSecureLock lock/unlock, polite/cancellable logout and
  separately confirmed force logout, configurable idle locking, and supervised
  X11 boot with native fallback; see [X11 sessions](x11-sessions.md).
- Native DRM desktop and greeter: retained as the boot fallback.
- Suspend/hibernate: unavailable in the current kernel; separate resume work.

## Constraints and completion criteria

- Keep the Wii's memory and CPU limits central: bounded data, no required large
  GUI toolkit, and suspend periodic refresh when apps are hidden.
- Run apps as the session user. Privileged operations need a narrow, explicit
  mechanism when those features are designed.
- Never run APT on the Wii, including simulations. Resolve/download packages on
  the host, transfer `.deb` files, and use `dpkg` on the Wii.
- File editing and operations must report errors, avoid silent data loss, and
  preserve a recovery path where practical.
- Test new app behavior directly in an isolated host X session, then verify
  PowerPC builds and the relevant behavior on hardware.
- Mark features complete only after implementation and appropriate verification;
  record remaining limits in [the X11 documentation](x11.md).

The ordering after the first three apps is intentionally open. The table is the
planned suite, not a promise to implement every app in one development pass.
