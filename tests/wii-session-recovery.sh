#!/bin/sh
# Explicit destructive-to-test-display exercise. Refuses an active WiiDesk login.
set -eu
[ "${1:-}" = --run ] && [ "$(id -u)" -eq 0 ]
if pgrep -f '^/usr/local/lib/wiidesk-session/wiidesk-x11' >/dev/null; then
    echo 'Log out of the managed display before running recovery tests.' >&2
    exit 1
fi
test ! -e /etc/wiidesk-native-only
base=/usr/local/lib/wiidesk-session
backup=$(mktemp /var/tmp/wiidesk-Xservers.XXXXXX)
probe_backup=$(mktemp /var/tmp/wiidesk-probe.XXXXXX)
cp "$base/Xservers" "$backup"
cp "$base/check-display" "$probe_backup"
cleanup() {
    trap - EXIT INT TERM HUP
    /etc/init.d/wiidesk-session stop || true
    cp "$backup" "$base/Xservers"
    cp "$probe_backup" "$base/check-display"
    chmod 755 "$base/check-display"
    rm -f "$backup" "$probe_backup" /etc/wiidesk-native-only
    /etc/init.d/wiidesk-session start
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
/etc/init.d/wiidesk-session stop
test "$(cat /sys/class/tty/tty0/active)" = tty7
touch /etc/wiidesk-native-only
/etc/init.d/wiidesk-session start
if /etc/init.d/wiidesk-session status >/dev/null; then exit 1; fi
echo 'PASS: service stop returns to VT7; native-only bypass prevents X11 startup'
rm -f /etc/wiidesk-native-only
printf ':1 local /bin/false\n' > "$base/Xservers"
/etc/init.d/wiidesk-session start
test_pid=$(cat /run/wiidesk-session.pid)
echo 'Testing failed X server startup and bounded native fallback...'
count=0
while kill -0 "$test_pid" 2>/dev/null; do
    count=$((count + 1))
    [ "$count" -lt 90 ]
    sleep 2
done
grep '^native fallback:' /run/wiidesk-session.status
test "$(cat /sys/class/tty/tty0/active)" = tty7
kill -0 "$(cat /run/wiidesk.pid)"
echo 'PASS: failed X server startup returns to the running native desktop'
cp "$backup" "$base/Xservers"
printf '#!/bin/sh\nexit 1\n' > "$base/check-display"
/etc/init.d/wiidesk-session start
test_pid=$(cat /run/wiidesk-session.pid)
echo 'Testing consecutive health failures with the display manager still running...'
count=0
while kill -0 "$test_pid" 2>/dev/null; do
    count=$((count + 1))
    [ "$count" -lt 90 ]
    sleep 2
done
grep '^native fallback: display failed health checks' /run/wiidesk-session.status
test "$(cat /sys/class/tty/tty0/active)" = tty7
echo 'PASS: persistent probe failure ends X11 and returns to VT7'
