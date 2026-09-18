# Follow-through after the first X11 cold boot

This tracks the four areas requested after the initial desktop integration.
Keep hardware observations distinct from automated host checks.

## Hardware completion

- [x] Physical lock/unlock and blocked VT/server-zap shortcuts (operator confirmed).
- [x] Caps Lock and mouse interaction with the core apps (operator confirmed).
- [x] Editor save/reopen, Files copy/trash, and Processes memory column on hardware
  (operator confirmed).
- [x] Second cold boot after the FAT repair; no FAT warning, X11 ready and probe
  passed. Boot ID `4a7f7d66-c289-4628-b69f-f0eef4f405df`.
- [x] Measure X11 workload timing and independent kernel scanout updates:
  Wii-side limits confirmed before HDMI; see [performance baseline](x11-performance.md).
- [ ] Improve mouse/window motion after the app suite, as requested. The precise
  cause of the approximately 30 Hz scanout cadence remains unconfirmed.

## Install image

- [x] Pin the current desktop and working kernel/configuration (baseline image).
- [x] Build/install X11 clients, runtime dependencies, PAM and supervised boot.
- [x] Build a separate image and verify filesystems, payloads and startup links.
- [x] Record image hashes and distinguish image checks from hardware boot
  (`wiidesk-os/docs/x11-image.md`; image has not been flashed).

## Applications

Implement and test the seven planned apps in [the roadmap](app-roadmap.md):
Network Settings, Log Viewer, Image Viewer, Calculator, Archive Manager,
Package Installer, and Audio Player. Use bounded data and shared controls.
Privileged changes require narrowly scoped authentication; never run APT on
the Wii. Keep existing app/session tests passing as the launcher grows.

## Power management

- [x] Initial source audit of Wii-specific storage, IRQ, DMA and graphics
  resume requirements; missing driver hooks documented.
- [ ] Prepare a recovery-capable PM boot entry; separate test kernel compiled
  and staged test plan documented, but no boot image has been installed.
- [ ] Run only supported stages with recovery available; document blockers.
- [ ] Enable desktop actions only after a real suspend/resume or hibernate/
  restore implementation has passed repeated hardware tests.

Unsupported hardware/kernel capabilities are research blockers, not completed
features. See [the current assessment](wii-power-management.md).
