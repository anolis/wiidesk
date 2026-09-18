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

## Follow-up source audit

Audited kernel commit `bdad35dada0c81d8c7c6f458db62d0c4662e388e`:

| Component | Existing support | Work still required |
| --- | --- | --- |
| PowerPC CPU | `arch/powerpc/kernel/swsusp_32.S` saves/restores registers, BATs and timebase; common `swsusp.c` restores the MMU context | Validate Broadway-specific state and restore ordering on a recovery kernel |
| SD host | `drivers/mmc/host/sdhci-of-hlwd.c` wires `sdhci_pltfm_pmops`; DMA and ADMA are disabled by quirks | Exercise the SD controller and card reinitialization before any image restore |
| USB | OF EHCI/OHCI have hub suspend callbacks and shutdown handlers | Their platform drivers have no system `.pm` operations; hub callbacks alone do not prove controller restoration after power loss |
| Interrupts | Hollywood PIC has mask, unmask and acknowledge operations | Audit PI cascade and Hollywood register restoration; there is no explicit suspend/resume implementation in `hlwd-pic.c` |
| VI/GX | DRM shutdown exists and GX probe initializes hardware | Add quiescing, fence cancellation/draining, register restoration, workspace reinitialization and display recovery; neither driver has system PM callbacks |
| Reserved memory | Wii DTS reserves GX texture, render, FIFO and XFB regions plus a `no-map` OHCI DMA pool | Establish which contents are saved by the snapshot and which must be recreated; do not assume restoring ordinary RAM restores these allocations |

The reserved regions include texture memory at `0x01300000`, render workspace
at `0x01600000`, FIFO at `0x01684000`, and XFB at `0x01698000`. GX directly
addresses these MEM1 allocations. The OHCI pool is at `0x01500000`. Resume must
not restart DMA or GX command processing against stale contents or pointers.

The initial separate test configuration should enable `HIBERNATION`,
`PM_DEBUG`, and `PM_SLEEP_DEBUG`, retaining the existing boot image as the
default and using `noresume`. A compile test only establishes that these
options build. The first hardware stage is `pm_test=freezer`; device, platform,
processor, core, and real image restore stages are separate gates. No real
hibernate operation should be enabled in the desktop while the driver gaps
above remain. A failed freezer test must return to the ordinary kernel before
continuing desktop testing.

For a future real restore, use a dedicated test swap area and explicitly
configured resume location. The running root filesystem must not be mounted
and modified between image creation and restoration. The current desktop SD
card and its swapfile are not a validated hibernation target.

## Separate compile test

A clean out-of-tree build at `/media/anolis/dev/wiidesk-pm-test-build` passed
`zImage modules` with `CONFIG_HIBERNATION=y`, `CONFIG_PM=y`, `CONFIG_PM_DEBUG=y`
and `CONFIG_PM_SLEEP_DEBUG=y`, starting from the audited commit's
`wii_defconfig`. Suspend-to-RAM remains unavailable. This kernel has not been
installed or booted; no freezer/device/restore hardware test has run.

```text
2ce4f7337bcfce7435032b5c14476fd808b713e000c0d0406b5db041fe661fc3  zImage
1fdfac7616b565be0fec38039c0f92534dd7a8699644d5563e6bb1f2a616c063  .config
```

Build log: `/media/anolis/dev/wiidesk-pm-build.log`. Keep this artifact separate
from the normal X11 image and default boot entry until the recovery procedure
and driver audit gates are satisfied.
