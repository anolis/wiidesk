# Image Viewer

Open Image Viewer from the launcher, run `wiidesk-x11-app images [FILE]`, or
run the separate `wiidesk-x11-image [FILE]` executable. Enter a PNG/JPEG path
and press Enter (or Open). Previous/Next and Page Up/Down browse pictures in
filename order within the current image's directory. Browsing stops at either
end and reports an error for directories beyond its bounded scan.

Fit / F5 fits the image inside the window, up to 400%. The 1:1 button displays
actual pixels. The +/- buttons or mouse wheel select 25–400% zoom; Alt+arrow
keys pan a zoomed image. Escape / Cancel stops loading. Closing the window
also terminates and reaps any decoder worker. Path entry supports the shared
selection, undo, and clipboard controls.

## Wii resource limits

- Regular files only, at most 16 MiB encoded.
- At most 1,048,576 decoded pixels, with each side at most 4096 pixels.
- A shared RGB buffer of at most 3 MiB. Display pixels are generated only for
  the visible viewport, capped at 1024x768 and 4 MiB of client image storage.
- Decoding and directory scans run in a separate process with a 32 MiB virtual
  memory limit, five CPU seconds, and a 15-second wall-clock deadline. No core
  dumps; no required decoder daemon. The previous image is discarded before
  loading another, keeping simultaneous image storage bounded.
- Browse scans at most 4096 directory entries and 256 image filenames.

PNG transparency is composited over the viewer background. The initial
version uses nearest-neighbor scaling and does not apply JPEG EXIF rotation,
provide full ICC color management, animate images, edit/save pictures, or
decode CMYK JPEGs. Unsupported, truncated, oversized and resource-limited
inputs produce a visible error; no shell command is built from a filename.

The decoder libraries are linked only into the image executable. Install it
beside the existing desktop companions. See [X11 build instructions](x11.md)
for host-resolved development/runtime packages; never use APT on the Wii.

## Verification

`make CROSS_COMPILE= BUILD_DIR=build/host image-tests` runs real PNG/JPEG
fixtures through the decoder. Tests cover RGB/grayscale/alpha pixels,
progressive JPEG, directory browsing, oversized dimensions/files, malformed
data, missing files and FIFO rejection. These host tests require Pillow.

`tests/x11_image_smoke.py` checks the launcher, actual XImage pixel values,
fit/1:1, previous/next, file errors, and cancellation/close cleanup of a
deliberately stopped worker. It is included in the full X11 suite. Set
`WIIDESK_TEST_XVFB=/path/to/Xvfb` to run that suite without physical host
mouse/focus interference. Xephyr remains the default for visible testing.

The September 18 host decoder tests passed. The 16-bit Xephyr run passed with
a reviewed screenshot at
`/media/anolis/dev/wiidesk-image-20260918/viewer-16bit.png`. The full headless
desktop/core/session suite passed (`/tmp/wiidesk-desktop-test.VaQZhe`). Earlier
visible full-suite runs failed at varying input-driven steps; they are not
counted as passing runs.

The PowerPC build passed. The Wii decoder probe confirmed PNG RGB 255/0/0,
JPEG RGB 0/255/1, and rejection of an oversized PNG and malformed input.
The maximum allowed 1024x1024 PNG also decoded correctly on both host and Wii.
Target staging and fixtures: `/var/tmp/wiidesk-image-20260918`.

Hardware GUI checks passed for PNG/JPEG load, fit/actual size, previous/next,
oversized-image error display and normal close. Resident memory for the small
image fixture was 3756 KiB (one sample, not peak memory). The new viewer, app
dispatcher and WM are installed with previous binaries saved under the staging
directory's `previous/`. The supervisor still reported `X11 ready`; the running
desktop was not restarted. The new launcher entry appears after normal
logout/login. The existing OS image remains the earlier baseline and has not
been rebuilt to include this app.
