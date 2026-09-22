#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run as the session user, with fixtures/ under the transferred stage directory.
set -eu
stage=${1:?Usage: audio_target_smoke.sh STAGE_WITH_FIXTURES}
build=${WIIDESK_TEST_BUILD:-/usr/local/lib/wiidesk-session}
app=$build/wiidesk-x11-audio
worker=$build/wiidesk-audio-worker
job=$(mktemp -d /var/tmp/wiidesk-audio-test.XXXXXX)
cp "$stage"/fixtures/* "$job/"
for name in tone.wav tone.mp3; do
    { sleep .4; echo 'P 1'; sleep .6; echo 'S 1500'; echo 'V 0'; echo 'P 0'; sleep 2; } | "$worker" "$job/$name" null > "$job/$name.log" 2>&1
    grep -q 'STATE playing' "$job/$name.log"
    grep -q 'STATE paused' "$job/$name.log"
    grep -q 'POS 1500' "$job/$name.log"
    grep -q 'STATE ended' "$job/$name.log"
done
if "$worker" "$job/tone.wav" default < /dev/null > "$job/missing.log" 2>&1; then exit 1; fi
grep -q 'ERROR Audio device:' "$job/missing.log"
if [ "${WIIDESK_AUDIO_BACKEND_ONLY:-0}" = 1 ]; then
    printf 'PASS: Wii WAV/MP3 null playback controls and missing-device error. Evidence: %s\n' "$job"
    exit 0
fi
export DISPLAY=${DISPLAY:-:1} XAUTHORITY=${XAUTHORITY:-/home/wii/.Xauthority}
gui=
cleanup() { [ -z "$gui" ] || kill -TERM "$gui" 2>/dev/null || true; }
trap cleanup EXIT
status() { xprop -id "$window" _WIIDESK_AUDIO_STATUS; }
state() { xprop -id "$window" _WIIDESK_AUDIO_STATE; }
wait_status() {
    i=0
    until status | grep -q "$1"; do i=$((i+1)); test "$i" -lt 100; sleep .1; done
}
wait_state() {
    i=0
    until state | grep -q "$1"; do i=$((i+1)); test "$i" -lt 100; sleep .1; done
}
key() { xdotool key --clearmodifiers "$@"; }
open_app() {
    "$app" "$job/playlist.m3u" > "$job/gui.log" 2>&1 & gui=$!
    i=0
    until window=$(xdotool search --onlyvisible --name '^WiiDesk Audio$'); do i=$((i+1)); test "$i" -lt 100; sleep .1; done
    xdotool windowactivate --sync "$window"
    wait_status 'Loaded 2 track'
}
open_app
key space
wait_status 'Audio device:'
key alt+F4
wait "$gui"; gui=
export WIIDESK_AUDIO_DEVICE=null
open_app
key space; wait_status Playing
sleep .3
player=$(pgrep -P "$gui")
grep -E 'VmRSS|VmHWM' "/proc/$gui/status" > "$job/memory-gui.txt"
grep -E 'VmRSS|VmHWM' "/proc/$player/status" > "$job/memory-worker.txt"
key space; wait_status Paused
before=$(state | sed -n 's/.*position=\([0-9]*\).*/\1/p')
sleep .5
after=$(state | sed -n 's/.*position=\([0-9]*\).*/\1/p')
test "$before" = "$after"
key minus; wait_state volume=40
key Right; wait_state position=3000
key space; wait_state 'track=1 '
wait_status Playing
key space; wait_status Paused
key ctrl+Left; wait_state 'track=0 '
key Escape; wait_status Stopped
key Tab ctrl+a
xdotool type --clearmodifiers -- "$job/saved.m3u"
key F2; wait_status 'Playlist saved'
grep -q "$job/tone.wav" "$job/saved.m3u"
key F2; wait_status 'existing files are not replaced'
key Return; wait_status 'Loaded 2 track'
key space; wait_status Playing
player=$(pgrep -P "$gui")
kill -STOP "$player"
key alt+F4
wait "$gui"; gui=
if kill -0 "$player" 2>/dev/null; then exit 1; fi
cat "$job/memory-gui.txt" "$job/memory-worker.txt"
printf 'PASS: Wii WAV/MP3 null playback, missing-device GUI error, pause/resume/seek/volume, auto-next/previous, save/reload/no-overwrite and stopped-worker close. Evidence: %s\n' "$job"
