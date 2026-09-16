#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Host integration suite; uses Xephyr and a fresh HOME, never the user's settings.
set -eu
build=${1:-build/host}
WIIDESK_TEST_BUILD=$(realpath "$build")
export WIIDESK_TEST_BUILD
test_display=${WIIDESK_TEST_DISPLAY:-:99}
test_dir=$(mktemp -d /tmp/wiidesk-desktop-test.XXXXXX)
server_pid=
wm_pid=
cleanup() {
    if [ -n "$wm_pid" ]; then kill -TERM "$wm_pid" 2>/dev/null || true; wait "$wm_pid" 2>/dev/null || true; fi
    if [ -n "$server_pid" ]; then kill -TERM "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; fi
    printf 'Test logs: %s\n' "$test_dir"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
mkdir "$test_dir/home"
touch "$test_dir/Xauthority"
xauth -f "$test_dir/Xauthority" add "$test_display" . "$(mcookie)"
Xephyr "$test_display" -screen 640x480 -nolisten tcp -auth "$test_dir/Xauthority" >"$test_dir/server.log" 2>&1 &
server_pid=$!
export DISPLAY="$test_display" XAUTHORITY="$test_dir/Xauthority"
export HOME="$test_dir/home" WIIDESK_TEST_HOME="$test_dir/home"
unset XDG_CONFIG_HOME
unset XDG_DATA_HOME
i=0
until xdpyinfo >/dev/null 2>&1; do
    kill -0 "$server_pid"
    i=$((i + 1)); [ "$i" -lt 100 ]; sleep .1
done
# Check our server is alive even if a different server already used this display.
kill -0 "$server_pid"
"$build/wiidesk-x11" >"$test_dir/wm.log" 2>&1 &
wm_pid=$!
i=0
until grep -q 'wiidesk-x11: ready' "$test_dir/wm.log"; do
    kill -0 "$wm_pid"
    i=$((i + 1)); [ "$i" -lt 100 ]; sleep .1
done
"$build/x11-wm-smoke"
python3 tests/x11_pointer_smoke.py
python3 tests/x11_desktop_smoke.py
"$build/x11-controls-test"
python3 tests/x11_core_apps_smoke.py
