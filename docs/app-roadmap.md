# WiiDesk application roadmap

This records the core suite agreed in the development conversation. Keep it
updated as features are implemented and verified. An app appearing in the
launcher does not by itself mean its planned feature set is complete.

## Current priority

Build reusable X11 controls, then finish **Text Editor → File Manager → Process
Manager**. Use shared controls for text entry, selection, scrolling lists,
buttons, dialogs, clipboard, and consistent keyboard navigation.

| App | Planned initial scope | Status |
| --- | --- | --- |
| Text Editor | Open/save dialogs, plain-text editing, search, undo, selection, clipboard, and configuration editing | Implemented and host-tested for bounded ASCII text; hardware file/control tests and startup/display verified |
| File Manager | Extend Files with copy, move, rename, new folders, and recoverable trash | Implemented and host-tested for regular-file copy, same-filesystem moves, and last-trash restore; hardware file/control tests and startup verified |
| Process Manager | Per-process CPU/memory, refresh, selection, and deliberate termination of stuck applications | Implemented and host-tested, including confirmed termination of a disposable process; hardware startup/display verified |
| Network Settings | Connection status, IP/DNS details, connectivity checks, and connection configuration | Implemented and host-tested for status, bounded DNS/TCP checks, and WPA/WPA2 personal profiles with DHCP; control-socket transaction tests cover save-failure rollback and cancellation; hardware verification pending |
| Image Viewer | Browse pictures, fit-to-screen, zoom, and bounded image decoding | Planned |
| Log Viewer | Follow logs, filter messages, pause, and search | Implemented and host-tested for regular ASCII logs; bounded tail, rotation/truncation handling and substring filtering; hardware verification pending |
| Archive Manager | List and extract archives, with progress and cancellation | Planned |
| Calculator | Basic arithmetic, scientific functions, and history | Implemented and host-tested; radians, bounded expression parser and 64-entry in-memory history; hardware verification pending |
| Package Installer | Inspect/install transferred `.deb` bundles using `dpkg`; resolve/download dependencies on the host | Planned |
| Audio Player | Local playback, playlists, and simple controls | Planned |

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
