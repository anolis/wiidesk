# Archive Manager

Open Archive Manager from the launcher, run `wiidesk-x11-app archives [FILE]`,
or run `wiidesk-x11-archive [FILE]` directly. Listing reads and validates the
contents, rather than trusting only the archive index. The initial version
supports ZIP and tar, including gzip, bzip2 and xz compression through linked-in
libarchive codecs. It does not create archives or handle encrypted archives.

Enter the archive path, then press Enter / List. Tab switches to the destination
parent field (initially the user's home directory). Ctrl+Enter / Extract new
folder creates a private `wiidesk-extract-XXXXXX` subdirectory there. Extraction
validates the archive again; it does not trust a previous listing if the file
has changed. Page Up/Down and the wheel scroll the entries. Escape / Cancel
stops a running operation; closing the window also reaps its worker.

The progress bar measures compressed input read, and the adjacent text shows
entry and decoded-byte counts. Reading 100% is not itself success: decoding
and final validation may still be underway. The status explicitly changes to
Checked or Extracted after successful completion. The output folder name stays
visible; Copy path / Ctrl+Shift+C copies its full path to the clipboard.

## Extraction rules and limits

- Regular input files only, at most 32 MiB encoded.
- Up to 512 entries, 512 newly created directories, 16 path components, and
  511 bytes per normalized path (255 per component).
- At most 32 MiB per file and 64 MiB decoded contents in total. Actual streamed
  bytes are checked as well as declared sizes; extraction uses a 16 KiB buffer.
- Absolute, parent-traversal, drive-prefixed and control-character paths are
  rejected, along with links, devices, FIFOs and encrypted entries. Backslashes
  are rejected if presented by the decoder; libarchive may first normalize ZIP
  separators. Normalized paths are always checked again.
- Output components are opened relative to the private directory descriptor,
  with `O_NOFOLLOW`. Files use exclusive creation; existing files are never
  overwritten. Directory/file permissions are 0700/0600, ignoring archive owner,
  executable/setuid bits, timestamps, ACLs and extended attributes.
- A worker has a 48 MiB virtual address-space limit, 15 CPU seconds and a
  120-second wall-clock deadline. These are ceilings, not expected resident
  memory. Large xz dictionaries or otherwise costly archives can hit a limit.
- Progress is refreshed four times per second during a job; idle viewers do
  not poll. Other apps do not load libarchive because this is a separate binary.

A new output folder starts with `.wiidesk-incomplete`. The worker removes that
marker only after successful extraction. Failure, cancellation, resource limits
or process termination leave the marker and any partial files in place, and
the UI identifies the output as incomplete. This preserves a visible recovery
path without deleting user-accessible files. There is no power-loss durability
guarantee and no resume/merge operation. Delete an unwanted partial folder with
Files after inspecting it. Archives cannot overwrite the reserved marker path.

## Builds and installation

Build with libarchive development headers/libraries in addition to the existing
X11 dependencies. `ARCHIVE_CPPFLAGS` and `ARCHIVE_LIBS` can locate host-only
extracted development packages. Cross-build against the matching PowerPC
sysroot and include its runtime dependencies in the linker's `-rpath-link`
search path. Install `wiidesk-x11-archive` beside the other desktop companions.

For the current Wii, host-side dependency resolution against a copied dpkg
inventory required only `libarchive13t64` 3.8.9-1. Its existing dependencies
were already installed. The downloaded package was checksum-verified and
installed with `DPKG_DEB_THREADS_MAX=1 dpkg -i`; APT was never run on the Wii.

## Verification

`make CROSS_COMPILE= BUILD_DIR=build/host archive-tests` exercises real ZIP/tar
archives: contents and restrictive modes, traversal and link rejection,
existing symlink pivots, no-overwrite, duplicate names, nesting/count/file/total
limits, ZIP data descriptors and CRC corruption, malformed input and FIFOs.
The exact 32 MiB file boundary is accepted; larger declared files and decoded
totals are rejected. Fixtures are private and disposable.

`tests/x11_archive_smoke.py` checks launcher/list/extract behavior, distinct
output directories, incomplete markers, unsafe paths, cancellation of a
deliberately stopped decoder, and close cleanup. It is part of the full X11
suite. September 20 checks passed on the host, including the full headless
desktop/core/session suite (`/tmp/wiidesk-desktop-test.ibVAOE`) and a focused
16-bit run with a reviewed screenshot. Evidence is under
`/media/anolis/dev/wiidesk-archive-20260920` and
`/media/anolis/dev/wiidesk-archive-16bit-tests.log`.

The PowerPC build passed. Wii backend checks confirmed ZIP/gzip-tar contents
and rejected traversal and symlink fixtures. The first hardware UI run passed
listing, extraction, content/mode checks and unsafe-path handling (4336 KiB
resident memory for the small fixture; not a peak-memory measurement). Its
cancellation test was inconclusive because the Wii kernel lacks the host's
`/proc/PID/task/PID/children` interface; the hardware test now uses `pgrep -P`.
Target staging: `/var/tmp/wiidesk-archive-20260920`.

The corrected hardware UI test passed on the final binary: listing, extraction,
exact contents and mode 0600, unsafe-path rejection, cancellation of a stopped
decoder, incomplete-marker retention, and normal close. Resident memory was
4392 KiB for the small fixture. Evidence folder:
`/tmp/wiidesk-archive-target.KPPhap` on the Wii. The supervisor reported
`X11 ready`. The app, dispatcher and launcher binaries are installed; previous
binaries are retained under the staging directory's `previous/`. The current
desktop was not restarted, so the new launcher entry appears after normal
logout/login.

The prebuilt OS image remains the older baseline; it has not been rebuilt
with the new utility apps.
