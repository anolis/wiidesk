#!/bin/sh
# Install/enable the separate X11 service; do not stop native DRM ownership.
set -eu
[ "$(id -u)" -eq 0 ]
sh session/install-trial.sh
base=/usr/local/lib/wiidesk-session
install -m 755 session/supervise session/check-display "$base/"
install -m 755 session/wiidesk-session.init /etc/init.d/wiidesk-session
update-rc.d xdm disable
update-rc.d wiidesk-session defaults
update-rc.d wiidesk-session enable
for level in 2 3 4 5; do
    enabled=0
    for link in /etc/rc"$level".d/S*wiidesk-session; do
        if [ -L "$link" ]; then enabled=1; fi
    done
    [ "$enabled" -eq 1 ] || { echo "No X11 startup link in runlevel $level" >&2; exit 1; }
done
echo 'Verified SysV startup links: X11 enabled for next boot. /etc/wiidesk-native-only bypasses it.'
