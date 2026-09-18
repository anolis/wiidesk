# Lightweight utility apps

The shared companion executable now accepts `logs [FILE]`, `calculator`, and
`network`, also available through the launcher. Apps run as the session user.
They use the existing small Xlib controls rather than a new GUI toolkit.

## Log Viewer

Opens regular files, displays the last 64 KiB with at most 512 lines, and
supports substring filtering, scrolling, pause/follow and rotation/truncation.
The default is the Xorg log. Nonprintable bytes are displayed as spaces; long
lines are truncated. Tab switches path/filter fields, Enter opens the path,
F5 refreshes, F6 pauses/resumes, and Page Up/Down pauses following and scrolls.
Following checks the file once per second while visible; hidden windows do
not poll. Errors such as missing permissions remain visible in the status line.

## Calculator

Supports `+ - * / % ^`, parentheses, `pi`, `e`, and `sin`, `cos`, `tan`, `sqrt`,
`ln`, `log`, `exp`, and `abs`. Angles use radians. Enter evaluates; Up/Down
recalls successful expressions. History holds at most 64 entries in memory.
Input is limited to 256 characters, parsing depth is bounded, and invalid or
nonfinite results are rejected. No expression is passed to a shell or script
interpreter.

## Network Settings

Shows interface IPv4 addresses/netmasks, default gateway, DNS servers and
available Wi-Fi status. A connectivity check resolves a host and attempts a
TCP connection to the specified port without sending application data. It is
not a full Internet, TLS or captive-portal validation. Checks run in a child
process and are time-limited, leaving the UI responsive. Status refreshes every
five seconds while visible; periodic work stops while hidden or editing Wi-Fi.

F2 or Wi-Fi setup opens the SSID/password form. Initial configuration supports
WPA/WPA2 personal networks with ASCII passphrases and DHCP, not enterprise,
static-IP or WPA3-only setup. The password is masked and is sent only over the
local supplicant control socket; it is not put in command arguments or a
user-readable temporary profile. The form's password/undo buffers are cleared
after starting a connection. Failed or cancelled attempts try to select the
previous active network and remove the new profile; rollback failure is
reported explicitly. The app saves a profile only after supplicant reports a
completed connection; DHCP may still be pending.

An administrator configures access with
`sh session/configure-network.sh wii [--live]`. This backs up the root-owned
WPA configuration, enables its control socket for the `netdev` group, enables
profile saving, and adds the selected user to that group. Existing sessions
need logout/login for the new membership. `--live` reloads the Wi-Fi daemon's
configuration; omitting it takes effect on its next start. This permission
grants Wi-Fi management through the daemon, not general root command execution.
Never run the installer against a private host's real network as part of tests.

## Verification

Host tests exercise calculator input/history/errors, log open/filter/follow,
rotation/truncation/pause, status loading, a TCP check against a private local
listener and rejection of an invalid Wi-Fi password. The Wi-Fi backend uses a
separate fake Unix socket to verify command encoding, cancellation, oversized
responses and rollback after save failure; it never changes the host network.
Host and PowerPC builds use warnings as errors. Physical network changes and
audio/image/archive/package apps are not covered by these results.

Run backend checks with `make CROSS_COMPILE= BUILD_DIR=build/host utility-unit-tests`.
The GUI suite also checks a 60,000-short-line log: only the newest 512 lines
are retained, without shifting the entire line table for every discarded line.

The full core/session suite also includes the new launcher entries. A busy
host exposed the bounded Processes list's inability to select a PID outside
its first 512 entries. Ctrl+F / PID lookup now finds one explicit PID without
expanding the list allocation; Ctrl+L / All clears the filter. Termination still
uses a pidfd and requires confirmation. Tests select only their own fixture.

### September 17 verification checkpoint

The full host X11 suite passed after the short-line tail change, including
window management, pointer controls, core apps, utilities, locking and logout.
Evidence: `/tmp/wiidesk-desktop-test.T89KYy` and
`/media/anolis/dev/wiidesk-utilities-host-tests.log`. The calculator and fake
Wi-Fi transaction tests passed through the Makefile target; the PowerPC build
also passed with warnings treated as errors.

The initial utility binaries are installed on the Wii and its supervisor
reports `X11 ready`. The calculator backend test passed on hardware. Desktop
interaction checks remain pending because the current session is locked;
the attempted smoke test produced no passing UI results. New launcher entries
and netdev membership become available to the existing session after normal
logout/login. No real Wi-Fi profile switch has been tested.
