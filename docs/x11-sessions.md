# X11 user sessions

WiiDesk manages windows and session actions. Debian XDM performs password login
through PAM and owns the X server lifecycle. Debian XSecureLock performs screen
locking and password unlock through PAM. WiiDesk never handles passwords.

Open **Session** from the launcher, or press **Ctrl+Alt+L** to lock. On the lock
screen, press a key to show the password prompt, then enter your password.
Only one locker is launched at a time. If it exits unsuccessfully in a managed
session, WiiDesk exits so XDM resets that session. This can discard unsaved work;
it prevents a locker failure from silently returning to an unlocked desktop.
The UI reports a lock request, not proof that the lock has taken effect.

Managed sessions automatically lock after **five minutes without input** by
default. Settings offers Off, 1, 5, 10, 15, and 30 minutes. Save applies the
change immediately. Keyboard and pointer activity anywhere on this X server
count, including inside ordinary apps. A once-per-second XScreenSaver query
reads the server's idle timer; no extra idle daemon is needed. Manual sessions
do not autolock. The underlying `idle_lock_seconds` preference also accepts
0–86400 seconds, with zero disabling the timer.

**Log out** asks applications to close. Applications with unsaved work can keep
the session open while you save. **Cancel logout** stops waiting, but cannot
reopen applications that already closed. **Force logout** requires a second
confirmation and discards unsaved work. XDM terminates the session's X server
and starts a fresh login display. Logout is unavailable in manually launched
WiiDesk sessions because those have no display manager to perform cleanup.
Root graphical sessions are refused by `Xsession`.

Suspend and hibernate are unavailable on the current Wii kernel:
`CONFIG_PM` and `CONFIG_HIBERNATION` are unset, and `/sys/power/state` is absent.
The architecture's `CONFIG_ARCH_HIBERNATION_POSSIBLE=y` is not working suspend
or hibernation support. Enabling those features would require separate kernel,
driver resume, storage, and hardware validation. No sleep command is issued.
See the [Wii power-management assessment](wii-power-management.md) for the
specific source/configuration blockers and a staged investigation plan.

## Installation and boot

Managed X11 uses display **:1 on VT9**. The native service prepares DRM and
keeps its greeter available on VT7. The separate `wiidesk-session` service starts
after it and stops before it during shutdown. The earlier manual desktop on
:0/VT8 is left running during installation. Do not stop the native service to
restart X11: it also controls DRM ownership.

Resolve dependencies against the Wii's dpkg status and download packages on the
host. Transfer the complete `.deb` directory to the Wii. From the transferred
archive, run `sh session/install-packages.sh /path/to/debs` as root. This uses
only `dpkg`, temporarily inhibits service starts, disables XDM boot startup, and
restores the prior default-display-manager and policy files. Its backup
directory is printed. Inspect installation errors before continuing.

Build as described in [x11.md](x11.md). Transfer `session/` and the three binaries
`build/x11/wiidesk-x11`, `build/x11/wiidesk-x11-app`, and
`build/x11/wiidesk-session-health` together. Run `sh session/install-boot.sh`
from that archive directory as root. It installs
root-owned binaries/configuration under `/usr/local/lib/wiidesk-session` and the
`wiidesk-lock` PAM policy, then enables the separate SysV service at boot. The
stock `xdm` service remains disabled to avoid competing display managers.

Stop an older manual XDM trial before starting the service; the service refuses
to take over an already-running manual trial. Normal service commands are:

```sh
/etc/init.d/wiidesk-session start
/etc/init.d/wiidesk-session status
# Save work and log out before stopping/restarting this service.
/etc/init.d/wiidesk-session stop
```

Enter an ordinary local account name, press Enter (or Tab), then enter its
password and press Enter. The password prompt appears after submitting the
username. XDM's PAM stack applies
the system authentication/account/session policy. There is no autologin or
password storage in WiiDesk. Installation does not change existing passwords.
The supplied Xorg configuration uses a US keyboard layout, indicated on the
greeter; the locker also displays the layout. If an administrator changes the
Xorg keyboard layout, update the greeter label to match.

The supervisor checks for a responsive X server and either the XDM greeter or
a managed WiiDesk session every 20 seconds. Each probe has a five-second limit.
If XDM exits, or six consecutive probes fail (roughly two minutes), it stops the
test display and switches back to VT7. It does not repeatedly restart a broken
display configuration. Inspect `/run/wiidesk-session.status`,
`/var/log/wiidesk-session.log`, and the XDM/Xorg logs before restarting it.
This detects startup/display-manager failures, not every possible application
or window-manager hang.

To persistently bypass X11 at boot and return to the native desktop:

```sh
touch /etc/wiidesk-native-only
/etc/init.d/wiidesk-session stop
```

Remove that marker and start the service to re-enable X11. A kernel command-line
argument `wiidesk.native=1` also bypasses X11. `busybox chvt 8` returns to the
earlier manual desktop while it exists; `busybox chvt 7` selects the native one.
Stopping the X11 service ends that session's apps, so save work first.

For a manual-only trial, `sh session/install-trial.sh` installs the payload
without enabling boot startup. With the service stopped, run
`/usr/bin/xdm -config /usr/local/lib/wiidesk-session/xdm-config` explicitly.

## Boundaries and validation

Xorg disables keyboard VT switching and server-zap shortcuts for the managed
display; root can still switch VTs remotely for recovery. Xorg does not listen
on TCP, XDMCP is disabled, and X authentication is required. A lock protects this
X11 session; it does not revoke existing SSH access, protect other logged-in
consoles, or prevent a privileged administrator from terminating it. Programs
already trusted with this X server share X11's security boundary.

There is no fast user switching, keyring integration, or sleep
resume path yet. XDM owns the login screen in this first integration, rather
than the native WiiDesk greeter. The account name and password are entered with
the configured X keyboard layout.

The host regression suite tests session UI, unmanaged logout refusal, successful
locker completion, dirty-editor logout waiting/cancellation, force confirmation,
and managed-session termination after locker failure. Fake lockers in that test
exercise supervision only; they do not validate authentication or screen grabs.
`tests/wii-session-trial.sh` is an explicit hardware helper for a disposable
account. It keeps its generated password in a root-only file, never in argv or
printed output, and removes it when cleaning up the account.

The locker is Debian's `xsecurelock` 1.9.0-3. Its
[upstream documentation](https://github.com/google/xsecurelock) describes the
authentication helpers and failure behavior; upstream was archived in April
2026, so future maintenance should track the Debian package and alternatives.
XDM behavior follows the [Debian manual](https://manpages.debian.org/testing/xdm/xdm.1.en.html).

## Initial login/lock integration results, 2026-09-16

The full host suite passed, including locking while the launcher owns the
keyboard: WiiDesk releases its own grabs before starting the locker. Host and
PowerPC builds pass `-Wall -Wextra -Werror`; session shell scripts pass ShellCheck.

On the Wii, a disposable account successfully signed in through PAM. Incorrect
login left the greeter active. Incorrect unlock left XSecureLock running;
correct unlock returned to the same WM. A System app kept PID 9771 and window
0x800001 across lock/unlock. Logout closed the app and WM and started a new Xorg
server with a fresh greeter. An authorization file saved before logout was
rejected by the responsive new server. The greeter access-control shortcut was
also checked with `xhost` before and after the key sequence.

The initial integration binaries were verified against the host hashes (the
later boot/idle-lock build is listed below):

```text
51b119066f39d9a11bb71f9d46550791860ba1f5fab836f0d03c8df214e51a6d  wiidesk-x11
4b4aeecdea72627cc6a8d21cf14da979b36f3fff246810f2d76660fec53b6494  wiidesk-x11-app
```

Twenty new packages were downloaded on the host and installed with `dpkg` on the
Wii; none were upgraded or removed. XDM boot startup remains disabled. One
logged-in snapshot showed 1,840 KiB WM RSS and 3,728 KiB Xorg RSS; these are
individual observations, not peak or unique memory measurements. Keeping two
X servers and the native desktop running increases memory/swap pressure, and
authentication/server startup can take several seconds on this trial setup.

Host artifacts and VLC captures: `/media/anolis/dev/wiidesk-x11-session-20260916`.
Wii staging: `/var/tmp/wiidesk-x11-session-20260916`; runtime logs:
`/var/log/wiidesk-xdm.log` and `/var/log/wiidesk-Xorg.1.log`. The greeter was
visually checked through the capture feed. Authentication input was automated
through XTEST; physical keyboard interaction is not independently verified.
This is functional integration testing, not a security audit of Xorg, XDM, PAM,
or the packaged locker. Native boot and the earlier desktop are retained.

## Boot and automatic-lock follow-up

The supervised service is installed and enabled in runlevels 2–5. On the Wii,
startup order is `S01wiidesk` then `S02wiidesk-session`; shutdown order is
`K01wiidesk-session` then `K02wiidesk`. Stock XDM remains disabled. Debian's
helper printed a systemd-unit warning on this SysV machine; actual SysV links
and service operation were verified. Repeating `start` is harmless, and the
PID check matches the supervisor name before stopping a process.

Hardware checks passed for display-manager termination, failed X server
startup, six consecutive health-probe failures, and the native-only marker.
Each failure returned to the running native desktop on VT7. The recovery test
restored the working server/probe configuration and restarted the greeter.
`sh tests/wii-session-recovery.sh --run` refuses an active managed WiiDesk session
and restores its temporary configuration changes on exit.

A disposable account with a one-minute timeout locked without any lock
shortcut. Password unlock and subsequent logout passed. XTEST attempts at
Ctrl+Alt+F7 and Ctrl+Alt+Backspace left the same server and VT active while
locked. `tests/wii-idle-lock.sh` covers this sequence and removes its account
and password afterward. The native desktop and earlier manual X11 apps were
preserved.
The final greeter's background, text contrast, and US-keyboard label were
visually checked through the live VLC capture.

Host tests cover client activity preventing idle lock, timeout changes saved
and reloaded in Settings, disabled automatic locking, and enabling it in an
already-running session. Host and PowerPC builds pass with warnings treated as
errors; the shell scripts pass ShellCheck. The new dependency is libXss (already
installed on the Wii); development files were extracted on the host only.

Current PowerPC hashes:

```text
3182c97111c711189d3e0a064aad222de5cd002c641b96c423dbccbcfdb769cf  wiidesk-x11
a212116b955a5314233f7b6f76b961cc5df5ddde601c12231679ba89a80dc031  wiidesk-x11-app
b57ae42e1a5c0b05856cf481e25a6de60c3b0fb9451d712ccbc5cfd054def5cf  wiidesk-session-health
```

Artifacts: `/media/anolis/dev/wiidesk-x11-boot-20260916` on the host and
`/var/tmp/wiidesk-x11-boot-20260916` on the Wii. The previous session payload is
saved as `previous-session` in the Wii artifact directory.

### Cold boot, 2026-09-16 (local time)

The editor closed cleanly before an orderly `shutdown -h now`. SSH became
unreachable and the capture feed went black. The operator powered the Wii back
on after the requested ten-second wait. The boot ID changed from
`444193a6-aee4-4ae3-a619-4f6dd90fccf1` to
`a6fda003-5e2f-4127-b122-4cabf880676e`, running `6.18.40-wii+`.

Without a manual service start, native WiiDesk started as PID 858, followed by
the supervisor (870), XDM (873), and Xorg (879) on display :1 / VT9. The old
manual :0 session did not return. The service reached `X11 ready`, the display
probe returned zero, and the login greeter was visually checked in
`cold-boot.png` in the host artifact directory. Xorg detected the Dell USB
keyboard. This verifies automatic startup to the greeter after power-off.

The operator completed physical keyboard login and confirmed the desktop
taskbar. A subsequent remote check confirmed WiiDesk X11 running as user `wii`
(PID 1231), `X11 ready`, and a successful display probe. XDM recorded session
startup at 00:07:13 UTC on September 17. The attempted desktop capture
(`cold-boot-desktop.png`) is black, so desktop appearance is confirmed by the
operator rather than that image. An earlier capture (`cold-boot-login.png`)
preceded completed login and still showed the greeter. The cold-boot-to-desktop
test passed. Caps Lock,
physical lock/unlock, and physical VT/zap shortcut checks remain pending;
automated input does not substitute for them.

Boot diagnostics reported an unclean FAT volume on `/dev/mmcblk0p1`, mounted at
`/boot/firmware`; the unmounted check and repair are recorded below.
The missing `regulatory.db`
warning also remains, although Wi-Fi obtained its expected address. Early XDM
timestamps precede network clock synchronization, so use kernel uptime and the
boot ID when interpreting this boot's logs.

Suspend/hibernate remain unavailable for the reasons in
[the power-management assessment](wii-power-management.md).

### Boot FAT check and repair

The target was missing `fsck.fat`, despite `/etc/fstab` requesting a pass-2
check of the FAT boot partition. The SysV checkfs and shutdown unmount services
were present. Downloaded `dosfstools` 4.2-1.2 for PowerPC on the host, verified
SHA-256 against the cached Debian Ports package index, transferred it, and
installed with `dpkg`. No apt command ran on the Wii; its existing libc6
satisfied the only dependency.

With `/boot/firmware` unmounted, `fsck.fat -n -v` found the dirty bit and a
one-byte primary/backup boot-sector difference at offset 65 (`01/00`), without
allocation or directory errors. Before repair, the complete 128 MiB partition
was copied to `/var/tmp/wiidesk-boot-fsck-20260916/boot-before.img`. Both device
and backup hashed to:

```text
0cca964fa3a2306c98f8e038bcce658cabaf18aedc61257fc6d50998cf998686
```

`fsck.fat -a -v` cleared the dirty flag. The subsequent read-only check returned
zero with no sector mismatch; 27 files and 232037 allocated clusters remained.
A further mount/unmount/read-only-check cycle also passed. The partition was
remounted with its fstab options, and X11 remained ready with a successful
display probe. The SD-to-SD backup took 196 seconds and temporarily slowed SSH.
This does not establish when the dirty flag was originally set, or verify a
second full shutdown/power-on cycle.

Host evidence is in `/media/anolis/dev/wiidesk-boot-fsck-20260916/results.txt`;
the Wii directory of the same name under `/var/tmp` retains the backup and
individual logs. WiiDesk OS commit `77471c8` adds `dosfstools` to future images
and requires the target checker in image verification. Shell syntax,
ShellCheck, and diff checks passed; a complete OS image was not rebuilt.
