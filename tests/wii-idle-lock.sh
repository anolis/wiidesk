#!/bin/sh
# Run after the recovery test restores the supervised greeter.
set -eu
helper=/var/tmp/wiidesk-x11-boot-20260916/tests/wii-session-trial.sh
account=wiidesk-session-test
test "$(id -u)" -eq 0
if id "$account" >/dev/null 2>&1; then exit 1; fi
if pgrep -f '^/usr/local/lib/wiidesk-session/wiidesk-x11' >/dev/null; then exit 1; fi
sh "$helper" prepare-account
cleanup() {
    trap - EXIT INT TERM HUP
    if pgrep -u "$account" >/dev/null; then /etc/init.d/wiidesk-session stop || true; fi
    sh "$helper" cleanup
    /etc/init.d/wiidesk-session start
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
install -d -m 700 -o "$account" -g "$account" "/home/$account/.config" "/home/$account/.config/wiidesk"
printf 'background=0\naccent=0\nidle_lock_seconds=60\n' > "/home/$account/.config/wiidesk/x11.conf"
chown "$account:$account" "/home/$account/.config/wiidesk/x11.conf"
chmod 600 "/home/$account/.config/wiidesk/x11.conf"
sh "$helper" login
count=0
until sh "$helper" is-unlocked; do
    count=$((count + 1)); [ "$count" -lt 30 ]; sleep 2
done
echo 'PASS: disposable account logged in; waiting for the one-minute idle lock'
sleep 30
sleep 35
sh "$helper" is-locked
echo 'PASS: session locked automatically without a lock shortcut'
sh "$helper" vt-shortcuts
sh "$helper" unlock
count=0
until sh "$helper" is-unlocked; do
    count=$((count + 1)); [ "$count" -lt 15 ]; sleep 1
done
echo 'PASS: password unlock after automatic locking'
sh "$helper" logout
count=0
while pgrep -u "$account" >/dev/null; do
    count=$((count + 1)); [ "$count" -lt 30 ]; sleep 2
done
echo 'PASS: logout ended the disposable account session'
