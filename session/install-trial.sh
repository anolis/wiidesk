#!/bin/sh
# Run from the extracted session-build archive as root, after package installation.
set -eu
[ "$(id -u)" -eq 0 ]
target=/usr/local/lib/wiidesk-session
install -d -m 755 "$target"
install -m 755 build/x11/wiidesk-x11 build/x11/wiidesk-x11-app "$target/"
install -m 755 session/Xsession "$target/Xsession"
install -m 644 session/xdm-config session/Xservers session/Xresources session/xorg.conf "$target/"
install -m 644 session/wiidesk-lock.pam /etc/pam.d/wiidesk-lock
printf 'Trial installed. Start: /usr/bin/xdm -config %s/xdm-config\n' "$target"
