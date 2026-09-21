# Package Installer

Run `wiidesk-x11-app packages [DEB_OR_FOLDER]` or choose **Package Installer**
from the launcher. Enter a local `.deb` path or a folder containing a prepared
bundle, then press Enter / **Inspect**. Review name, version, architecture,
installed status and declared installed size. **Details** / F2 opens dependency
and hash information in Log Viewer. Changing the path requires another review.

**Install** / Ctrl+Enter opens a terminal for normal `sudo` authentication and
`dpkg` prompts. Existing passwordless sudo policy is honored. Password input stays in the terminal; WiiDesk does not collect
or save it. **Log** / F3 opens the installation output. Successful completion
requires both a successful `dpkg --install` and matching installed versions in
`dpkg-query`. There is no APT invocation, dependency resolution or download on
the Wii. Resolve dependencies on the host and transfer their `.deb` files.

Only install trusted packages: maintainer scripts run as root. Inspection reads
metadata and does not run those scripts. This is a local installer, not a
repository signature verifier. The recorded SHA-256 binds installation to the
reviewed snapshot; it does not establish the publisher's identity.

## Resource bounds and privilege boundary

- At most 16 packages, 32 MiB per encoded package and 64 MiB per bundle.
  Folder scans stop at 4096 entries. Duplicate package names are rejected.
- Snapshots use private mode-0700 directories under disk-backed `/var/tmp` and
  mode-0600 files. Copying uses a 16 KiB buffer. Metadata fields and subprocess
  output are bounded. `dpkg-deb` inspection gets a 48 MiB address-space limit,
  15 CPU seconds and one decompression thread; individual captured commands
  have a 30-second deadline and the GUI inspector has a 120-second deadline.
- Escape / **Cancel** cancels inspection. Closing during inspection reaps the
  worker and removes its snapshots. Inspecting again also discards the previous
  snapshots. Completed installation logs and metadata remain in the job folder;
  the **Log** window shows its path. Output logging is capped at 1 MiB.
- The GUI and terminal runner refuse root execution. The ordinary session user
  must already have administrative access through sudo; no new sudoers rule or
  setuid executable is installed. Keep the three package executables together
  in the root-owned session directory, mode 0755.
- The root helper copies regular, singly linked, invoking-user-owned snapshots
  into a second private root staging folder. It verifies hashes, reparses
  metadata, and accepts only the native architecture or `all` before invoking
  `dpkg`. It uses absolute tool paths and a fixed environment, without a shell
  command assembled from package names or paths.

Installation itself has no cancellation button or timeout. The inspection
memory limits do not constrain `dpkg` or trusted maintainer scripts. Keep the
terminal open until it finishes. Packages can change services or require more
memory/storage than their archive size suggests.

If installation fails or is interrupted, packages can remain unpacked or
unconfigured; there is no rollback guarantee. Read the retained log, inspect
`dpkg --audit`, transfer any missing dependencies from the host, and repair
with the appropriate `dpkg --install` / `dpkg --configure` command. Root-staged
files are retained after an installation failure; their path appears in the
log. Validation failures and successful runs remove root staging automatically.
Interrupted processes may leave staging folders for manual cleanup once no
installation is running. An absent result is reported as unknown, not success.

## Verification

`make CROSS_COMPILE= BUILD_DIR=build/host package-tests` covers inspection,
metadata/hash output, script non-execution, malformed files, FIFO/symlink
rejection, duplicate names and file/bundle/count limits.
`tests/x11_package_smoke.py` is included in the desktop suite and covers the
launcher, details, review invalidation, cancellation of a stopped inspector,
and close cleanup. It never installs packages on the host.

September 20, 2026: host full-suite checks passed
(`/tmp/wiidesk-desktop-test.lARKhh`), as did the focused 640×480×16 UI run
(`/tmp/wiidesk-package-visual.7H1rdT`). The reviewed screenshot and PowerPC build
artifacts are under `/media/anolis/dev/wiidesk-package-20260920`.
Wii inspection and root-helper `--check-only` checks passed for matching hashes,
wrong hashes, wrong architecture, symlinks, hard links, wrong ownership and
pre-install staging cleanup (`/var/tmp/wiidesk-package-check.luer2y`).

The Wii GUI-to-terminal-to-sudo-to-dpkg success path installed the harmless
`wiidesk-package-smoke` 0.0.1 fixture and verified both its version and payload.
Success output is in `/var/tmp/wiidesk-packages-kVXcDd`; user snapshots and root
staging were removed afterward. GUI resident memory after completion was
1944 KiB (reported high-water mark 3116 KiB); this is not a whole-installation
peak measurement. The existing `/etc/sudoers.d/wiidesk` policy grants the Wii
user passwordless sudo, so a password-required sudo interaction remains
unverified. No authentication policy was changed.

The final Wii GUI failure-path check changed a snapshot after review. The helper
rejected its hash before invoking dpkg; the GUI reported failure and opened the
retained log (`/var/tmp/wiidesk-packages-oa04ep`). The earlier installation and
payload were unchanged. The test fixture was then removed, `dpkg --audit` was
empty, and the session supervisor still reported `X11 ready`.
The final deployed GUI SHA-256 is
`25a26c634a883df44adb3c7ae3a2b23edddc88b54857d5cfca33046b0f41da36`;
the installer helper is
`f071e037ead89f4373b2318cc9974a2aec814fa4a6e3755e1ee9338fbdca0f39`.

The prebuilt OS image remains the older baseline. Installing these executables
does not restart the current WM; its new launcher entry appears on next login.
