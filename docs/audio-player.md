# Audio Player

`wiidesk-x11-app audio [FILE_OR_FOLDER_OR_PLAYLIST]` opens the player. It supports
local WAV/MP3 tracks, alphabetically ordered folders, and M3U/M3U8 playlists.
Enter a path and choose **Load**. Choose a track, then **Play**. Successful
completion advances to the next track; errors stop playback. **Repeat** loops
the playlist. **Save M3U** writes the loaded list to a new path in the path box;
existing files are never replaced.

Controls: Tab switches path/list focus; Enter loads a path or plays a selected
track; Space in the list or Ctrl+Enter toggles play/pause. Left/Right seek ten
seconds, Ctrl+Left/Right select previous/next tracks, and +/- adjust software
volume. F2 saves a new playlist, F5 reloads the path, and Escape stops playback
or cancels loading. Clicking the progress bar seeks. Volume starts at 50%; it
does not change a system mixer. MP3 duration estimates carry a `~` marker.

## Bounds and playback design

- WAV: RIFF PCM, 8/16-bit, mono/stereo, 8–48 kHz; header/chunk lengths and frame
  alignment are checked. At most 1024 RIFF chunks. RF64, float and compressed
  WAV are unsupported. Samples are converted to native-endian signed 16-bit.
- MP3: libmpg123 decodes incrementally with fixed output format, skipped ID3v2
  metadata and a fixed 1000-entry seek index. No initial whole-file scan is
  required. Format changes mid-track are rejected. Seeking very large files
  can exceed the responsiveness deadline and stop playback.
- Regular local files only, at most 1 GiB each. The worker rejects final-path
  symlinks, devices and FIFOs; playlist loading resolves selected paths first.
  There is no URL/network playback. Playlists contain at most 128 tracks and
  64 KiB of text; folder scans stop after 4096 entries. UTF-8 BOM/CRLF are
  supported; embedded NUL and control characters in track paths are rejected.
- A separate, cancellable process loads playlists. A separate unprivileged
  playback worker owns the decoder and ALSA device. It has a 48 MiB virtual
  address-space cap, bounded buffers, and no core dumps. The GUI stops workers
  after 15 seconds without progress. Closing, stopping, or replacing a track
  terminates and reaps its worker; closing the GUI also kills it if the GUI dies.
- PCM writes use ALSA nonblocking mode and about 250 ms of buffering. Pause
  drops queued audio and seeks back to the estimated audible position, so it
  does not require hardware pause support. Seeking is capped at 24 hours.
  Underruns are recovered when possible and reported, not silently counted as
  clean playback. Device/decoder errors do not advance the playlist.
- Position updates are limited to four per second. Fully obscured windows
  suppress drawing. The worker keeps playing while the window is covered.
  Decoder input and PCM data are streamed rather than loaded as a whole track.

`WIIDESK_AUDIO_DEVICE` selects an ALSA PCM device; the default is `default`.
`WIIDESK_AUDIO_DEVICE=null` is a **silent test sink**, not evidence of working
physical audio. The worker refuses root execution. Install it beside the GUI.

Build with ALSA and libmpg123 development headers/libraries. `AUDIO_CPPFLAGS`,
`AUDIO_LDFLAGS`, and `AUDIO_LIBS` can point to extracted host/cross dependencies.
On the pinned PowerPC snapshot, the runtime packages are `libasound2-data`
and `libasound2t64` 1.2.16.1-1, plus `libmpg123-0t64` 1.33.7-1. The latter was
already installed on the Wii. The two ALSA packages were downloaded on the
host and installed with dpkg; no APT was run on the Wii.

API references: [ALSA PCM](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html)
and [libmpg123 decoding](https://www.mpg123.de/api/group__mpg123__input.shtml).

## Verification and remaining hardware work

`make CROSS_COMPILE= BUILD_DIR=build/host audio-tests` exercises WAV/MP3
decoding/seeking, signed sample conversion, malformed/truncated input, FIFO/
symlink/size rejection, bounded playlists, pause/resume and volume, real ALSA
null/file plugin output, and device-open failures. It needs ffmpeg on the host
only to generate a small MP3 fixture. It never uses the host's speakers.

`tests/x11_audio_smoke.py` tests load/play/pause/seek/volume, automatic next,
previous, stop, M3U save/reload/no-overwrite and closing a deliberately stopped
worker. The full desktop suite exports `WIIDESK_AUDIO_DEVICE=null`.
September 21 host checks passed: full suite
`/tmp/wiidesk-desktop-test.AgmcFy`, then focused 640×480×16 checks on the final
GUI in `/tmp/wiidesk-audio-visual.olctI3`. The screenshot was reviewed.
Cross builds passed; Wii decode probes produced the expected sample rate,
channel count and frame count for both test formats. Artifacts are in
`/media/anolis/dev/wiidesk-audio-20260921` (host) and
`/var/tmp/wiidesk-audio-20260921` (Wii).

The Wii has no Python installation. `tests/audio_target_smoke.sh STAGE` runs
the corresponding hardware checks as the session user using shell/xdotool
and pre-generated files in `STAGE/fixtures/`. Missing-device checks use an
explicitly nonexistent device, so they work with or without a soundcard.
`WIIDESK_TEST_AUDIO_DEVICE=default` runs the backend controls against hardware
with volume zero from the first sample; the default test device is `null`.
`WIIDESK_AUDIO_BACKEND_ONLY=1` skips GUI checks. On the Wii, the final null-output
and GUI suite passed in `/var/tmp/wiidesk-audio-test.YavZDl`: WAV/MP3 playback,
the missing-device error, pause/resume/seek/volume, next/previous, playlist
save/reload/no-overwrite, and close/reap of a stopped worker. GUI RSS was
3656 KiB and worker RSS 2772 KiB for the small fixture; these are not peak
whole-system measurements. No physical sound was produced by these checks.

The new app, worker, dispatcher and WM are installed with backups under the
staging directory's `final/previous/`. A normal XDM login completed during this
test, so the running desktop includes the new launcher entry. No permanent
silent-output configuration was installed. Final deployed hashes:

- GUI: `580d0aa9ee6fc6248bf6edd910e5676c28d18ae314d0bb6c7d43859715bad02c`
- Worker: `0d7b046e98bfaed992cca34bca768db07d0c01c121c8a73f5b568cb466efecb3`

At the original app milestone, physical audio was blocked by the kernel. The
6.18.40-wii+ kernel reports `No soundcards found`; `/dev/snd` contains only
sequencer/timer nodes. `CONFIG_SND`/`CONFIG_SND_PPC` are enabled, but the current
kernel source at `bdad35dada0c81d8c7c6f458db62d0c4662e388e` has no Wii/GameCube
ALSA driver in `sound/ppc`. USB audio is also disabled. This is not a missing
userspace player package or a module that merely needs loading.

Next: port/restore a Wii ALSA playback driver, including DMA/cache maintenance,
interrupts, stop/restart, and the AV encoder path; then test actual 32/48 kHz
stereo output, underruns under UI load, and audible pause/seek. The older
`wii-linux-ngx/sound/ppc/gcn-ai.c` is a reference, not a drop-in module for this
kernel. Its binding names also differ from the current `hollywood-ai` device
tree node. No kernel or device-tree change was made in this app milestone.

### Driver bring-up follow-up, September 21

An experimental `snd-gcn-ai` module now registers `WiiAI` on the running
6.18.40-wii+ kernel. Direct silent playback has passed at 32/48 kHz. With
that module loaded, the corrected worker passed WAV/MP3 playback, pause,
seek, resume and completion through ALSA `default`, without XRUN/ERROR
messages, in `/var/tmp/wiidesk-audio-test.HxNOHX`. These tests used volume
zero: physical sound quality and channel routing remain unverified.

The hardware run exposed a nonblocking drain behavior hidden by the null
plugin: the kernel can return `EAGAIN` even after reaching `SETUP`. The
worker now recognizes that completed state instead of eventually reporting
a stalled device. Host decode/control/null/file-output tests also pass.
The corrected worker is installed (SHA-256
`82d9c76b65670afc6cca7ee33eab93c2f57253b0df6be8f3c4fb65d1c1b6d496`),
with its predecessor backed up in `/var/tmp/wii-audio-driver-20260921`.
The installed worker repeated the muted hardware suite successfully in
`/var/tmp/wiidesk-audio-test.FOv0kS`.
The boot kernel and persistent module configuration remain unchanged while
the driver is being validated. Driver details and remaining checks are in
the kernel repository's `docs/wii-audio-bringup-2026-09-21.md`.

The OS image remains the earlier baseline and still needs rebuilding with
the new apps and their runtime dependencies.

## Load-test follow-up, September 22–23

The installed worker now fills available PCM space before sleeping, while
checking commands between chunks. Previously it waited 10 ms after every
successful 1024-frame write. Its requested buffer is 250 ms, with a bounded
230 ms producer lead for clockless test sinks. The driver has a fixed 64 KiB
DMA allocation (32 KiB more than the initial driver); 48 kHz stereo uses
48,000 bytes for a 250 ms ring. Pause/seek still drop queued samples, so
these controls do not wait for the whole buffer to play.

The Wii-specific SD-card request cap is also necessary to address observed
hard-IRQ stalls: large SD writes blocked audio interrupts for tens of
milliseconds. The deployed udev rule limits Hollywood MMC disks to 4 KiB
requests. It reduces sequential throughput; the OS repository documents
the measured tradeoff in `docs/sd-audio-latency.md`.

Five eight-second, muted 48 kHz WAV runs passed with simultaneous CPU load
and repeated direct SD writes/reads, without XRUN/ERROR messages. Evidence:
`/var/tmp/wii-audio-player-load.JD1eV7`. The WAV/MP3 pause/seek/resume/end and
missing-device checks then passed under the same loads in
`/var/tmp/wii-audio-player-load.3d5nEq` (control details in
`/dev/shm/wiidesk-audio-test.NtnNSZ`). Control fixtures are copied to RAM;
the sustained WAV source remains on the SD card. These finite tests do not
establish a zero failure rate under arbitrary load.

Earlier load comparisons wrote each POS line synchronously to the saturated
SD card; those results were confounded by test logging and cannot establish
the benefit of a particular player buffer size. Live logs now use tmpfs and
are persisted after load stops. The target smoke test also waits for worker
readiness before sending controls, keeps stdin open through completion, and
rechecks final output if the worker exits between checks.

`WIIDESK_AUDIO_TEST_TMPDIR=/dev/shm` keeps target smoke fixtures/logs in RAM.
`WIIDESK_AUDIO_DIAGNOSTICS=1` optionally reports write-gap/decode timings to
stderr. Metrics include startup, which can be slow without causing an
underrun before playback starts; they are not CPU-time measurements.
Diagnostics are off in normal desktop use.

The deployed worker SHA-256 is
`a2ccda9a1120fca35e3727f136e94359db053857d3d4a08342e951b58debd0e6`;
the prior worker is backed up as
`/var/tmp/wii-audio-driver-20260921/refill250/worker.previous`.
The host decode/control/null/file-output suite passed. The boot kernel was
not replaced; cold-boot autoload and a refreshed OS image remain to verify.
