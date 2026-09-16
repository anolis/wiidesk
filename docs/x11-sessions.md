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

## Isolated hardware trial

The trial uses display **:1 on VT9**. The earlier desktop on :0/VT8 and native
DRM greeter on VT7 stay running. The native boot service is not replaced.
Do not stop that service while testing X11: it also controls DRM ownership.

Resolve dependencies against the Wii's dpkg status and download packages on the
host. Transfer the complete `.deb` directory to the Wii. From the transferred
archive, run `sh session/install-packages.sh /path/to/debs` as root. This uses
only `dpkg`, temporarily inhibits service starts, disables XDM boot startup, and
restores the prior default-display-manager and policy files. Its backup
directory is printed. Inspect installation errors before continuing.

Build the PowerPC WM and companion app as described in [x11.md](x11.md). Transfer
`session/`, `build/x11/wiidesk-x11`, and `build/x11/wiidesk-x11-app` together, then
run `sh session/install-trial.sh` from that archive directory as root. It installs
root-owned binaries/configuration under `/usr/local/lib/wiidesk-session` and the
`wiidesk-lock` PAM policy. Start explicitly:

```sh
/usr/bin/xdm -config /usr/local/lib/wiidesk-session/xdm-config
```

Enter an ordinary local account name, press Enter (or Tab), then enter its
password and press Enter. The password prompt appears after submitting the
username. XDM's PAM stack applies
the system authentication/account/session policy. There is no autologin or
password storage in WiiDesk. The trial does not change existing passwords.

To stop this trial, first save all work in its session, then as root:

```sh
kill -TERM "$(cat /run/wiidesk-xdm.pid)"
busybox chvt 8
```

This ends **only the trial session** and returns to the earlier X11 desktop.
`busybox chvt 7` returns to the native greeter. No boot enablement is performed.

## Boundaries and validation

Xorg disables keyboard VT switching and server-zap shortcuts for the managed
display; root can still switch VTs remotely for recovery. Xorg does not listen
on TCP, XDMCP is disabled, and X authentication is required. A lock protects this
X11 session; it does not revoke existing SSH access, protect other logged-in
consoles, or prevent a privileged administrator from terminating it. Programs
already trusted with this X server share X11's security boundary.

There is no idle autolock, fast user switching, keyring integration, or sleep
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

## Hardware results, 2026-09-16

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

The final installed PowerPC binaries were verified against the host hashes:

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
