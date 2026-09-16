# Wii suspend and hibernation assessment

Status: neither operation is available in the deployed kernel. The Session app
keeps both controls unavailable and never writes to a power-management node.
Screen blanking and locking are not system suspend.

The checked kernel is `6.18.40-wii+`, with sources in
`/media/anolis/dev/wii-linux-gcn-productization` and the deployed build config at
`/media/anolis/dev/wii-gcn-linear-v12-build/.config`:

- `CONFIG_PM` and `CONFIG_HIBERNATION` are unset; `/sys/power/state` is absent.
- `arch/powerpc/Kconfig` exposes `ARCH_SUSPEND_POSSIBLE` only for a list of other
  platforms; Wii is not on that list.
- `arch/powerpc/platforms/embedded6xx/wii.c` implements restart, poweroff, and
  shutdown quiescing, but no platform suspend entry/wakeup path.
- The current `drivers/gpu/drm/gcn` implementation and `gcn-gx.c` have no device
  PM suspend/resume callbacks. Their VI/GX state and in-flight work need explicit
  recovery handling; a poweroff callback is not a resume implementation.
- `ARCH_HIBERNATION_POSSIBLE=y` is a generic PowerPC capability, not evidence that
  this Wii's device/boot path can restore an image.

Hibernation is worth investigating separately because restoring a disk image
does not require the same retained-RAM platform sleep state. That is a research
direction, not a claim it works. First audit SD storage, USB, interrupt
controllers, DMA, and graphics state, including allocations outside ordinary
Linux-managed memory. The resume image location and early boot restore path
must also be defined. Kernel documentation requires resume before filesystems
are used; swap availability alone is insufficient.
See [kernel software-suspend documentation](https://docs.kernel.org/power/swsusp.html).

A separate recovery-capable test kernel should enable the relevant PM debugging
facilities. Start with process-freezing and device suspend/resume tests, then
advance only when those pass. Later use image restore testing and repeated
real resume cycles. This follows the kernel's staged
[PM debugging procedure](https://docs.kernel.org/power/basic-pm-debugging.html).
Run this work with local recovery available and disposable storage/workloads,
rather than changing the running desktop kernel merely to expose a button.

No kernel config, boot image, swap layout, or filesystem-resume configuration
was changed during the X11 session integration.
