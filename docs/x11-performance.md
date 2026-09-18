# X11 frame-rate baseline

The operator reported visibly poor frame rate during physical desktop testing.
Measured the existing managed session on the Wii, before deploying the new
utility apps. Kernel: `6.18.40-wii+`; Xorg uses software rendering, ShadowFB,
RGB565 and the advertised 640x480 / 60 Hz DRM mode. GX scanout acceleration is
active. No HDMI-adapter latency measurement was made.

`tests/x11_frame_probe.c` creates a regular 560x340 window, targets 60 updates
per second for ten seconds, and measures request completion using `XSync`.
One workload fills rectangles and draws a moving bar; the other uploads an
XImage before drawing the bar. XSync measures server processing, not vblank
or delivery of a distinct complete image to the monitor.

An isolated tracefs instance recorded `gcn_vi:gcn_vi_write` for offset `0x1c`
(top-field scanout address), using the monotonic clock. Samples were restricted
to each client workload's start/end times. All 595 trace events were retained;
overrun and dropped-event counters were zero. The instance was removed after
the test. The session was unlocked and no locker remained running afterward.

| Workload | Client redraws/s | Scanout address updates/s | Median XSync work | 95th percentile | Maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| Rectangles/text | 37.52 | 28.34 | 19.56 ms | 36.03 ms | 451.76 ms |
| Full-window XImage upload | 13.77 | 29.94 | 66.81 ms | 100.23 ms | 104.05 ms |

Median scanout-update spacing was 33.37 ms for both workloads. Scanout address
changes are not proof of distinct new client frames; in particular, the upload
workload produced fewer images than scanout updates. The rectangle run had one
293.4 ms scanout gap. This is a short baseline, not a sustained performance
or latency distribution benchmark.

The low update rate exists in the Wii software/display path before HDMI
conversion. An adapter could add latency, but cannot account for the measured
client throughput limits. The approximately 30 Hz scanout cadence still needs
investigation; do not infer its cause from the advertised 60 Hz mode alone.

The operator subsequently localized the poor responsiveness to mouse movement
and moving windows, rather than reporting slow typing or app launches. Xorg's
configuration enables `SWcursor` and `ShadowFB`; `gcn_drm_convert()` performs a
full 640x480 conversion for each update. This is a concrete source of work even
for cursor-only damage and a priority for the later performance investigation.
It does not by itself prove the precise cause of the 30 Hz cadence. Continue
the application suite first, as requested, and preserve the keyboard-driven
launcher, app switching and maximize shortcuts as usable navigation paths.

Host evidence: `/media/anolis/dev/wiidesk-x11-boot-20260916/frame-rate-trace.txt`.
The probe and executable are retained in that directory and the corresponding
Wii `/var/tmp/wiidesk-x11-boot-20260916` directory. The initial attempt used an
inactive authorization path and exited before measuring anything; the recorded
run used the actual XDM session authority.
