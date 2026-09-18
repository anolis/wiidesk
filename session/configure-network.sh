#!/bin/sh
# Grant one local session user access to wpa_supplicant's Wi-Fi-only API.
# Default is next-start configuration; --live reloads the unchanged connection.
set -eu
[ "$(id -u)" -eq 0 ]
account=${1:?Usage: configure-network.sh USER [--live]}
case "$account" in ''|*[!a-zA-Z0-9_-]*|-*) exit 2;; esac
case ${2:-} in ''|--live) ;; *) exit 2;; esac
[ "$#" -le 2 ]
uid=$(id -u "$account")
[ "$uid" -ge 1000 ] || { echo 'Choose a non-root desktop user' >&2; exit 1; }
config=/etc/wpa_supplicant/wpa_supplicant.conf
[ -f "$config" ] && [ ! -L "$config" ]
[ "$(stat -c %u "$config")" -eq 0 ]
groupadd -f netdev
usermod -a -G netdev "$account"
temp=$(mktemp "$config.XXXXXX")
trap 'rm -f "$temp"' EXIT
awk '!/^[[:space:]]*(ctrl_interface|update_config)[[:space:]]*=/' "$config" > "$temp"
printf 'ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev\nupdate_config=1\n' >> "$temp"
chmod 600 "$temp"
chown root:root "$temp"
if ! cmp -s "$temp" "$config"; then
    backup=$(mktemp /var/backups/wiidesk-wpa.XXXXXX)
    cp "$config" "$backup"
    chmod 600 "$backup"
    mv "$temp" "$config"
    printf 'Wi-Fi configuration backed up to %s\n' "$backup"
fi
if [ "${2:-}" = --live ]; then
    wpa_cli -i wlan0 reconfigure | grep -qx OK
    if [ -d /run/wpa_supplicant ] && [ ! -L /run/wpa_supplicant ]; then
        chgrp netdev /run/wpa_supplicant
        chmod 755 /run/wpa_supplicant
    fi
    if [ -S /run/wpa_supplicant/wlan0 ] && [ ! -L /run/wpa_supplicant/wlan0 ]; then
        chgrp netdev /run/wpa_supplicant/wlan0
        chmod 770 /run/wpa_supplicant/wlan0
    fi
fi
echo 'Configured netdev access. Existing desktop sessions must log out/in to gain the group.'
