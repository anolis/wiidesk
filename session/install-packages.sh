#!/bin/sh
# Run as root on the Wii after transferring the pre-resolved debs directory.
# Never runs apt. Keep XDM disabled at boot during the trial.
set -eu
[ "$(id -u)" -eq 0 ]
debs=${1:?Usage: install-packages.sh /path/to/debs}
[ -f "$debs/xdm_1%3a1.1.17-2_powerpc.deb" ] || test -n "$(find "$debs" -maxdepth 1 -name 'xdm_*.deb' -print)"
backup=$(mktemp -d /var/tmp/wiidesk-package-backup.XXXXXX)
policy=/usr/sbin/policy-rc.d
dm=/etc/X11/default-display-manager
service=/etc/systemd/system/display-manager.service
for name in policy dm service; do
    case "$name" in policy) path=$policy;; dm) path=$dm;; service) path=$service;; esac
    if [ -e "$path" ] || [ -L "$path" ]; then cp -a "$path" "$backup/$name"; fi
done
cleanup() {
    update-rc.d xdm disable || true
    for name in policy dm service; do
        case "$name" in policy) path=$policy;; dm) path=$dm;; service) path=$service;; esac
        rm -f "$path"
        if [ -e "$backup/$name" ] || [ -L "$backup/$name" ]; then cp -a "$backup/$name" "$path"; fi
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
rm -f "$policy"
printf '#!/bin/sh\nexit 101\n' > "$policy"
chmod 755 "$policy"
DEBIAN_FRONTEND=noninteractive dpkg -i "$debs"/*.deb
printf 'Package installation finished; boot settings restored from %s\n' "$backup"
