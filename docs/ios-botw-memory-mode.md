# Experimental BOTW memory compatibility mode

On iOS, Settings → Breath of the Wild → Memory now exposes the existing
`TOTK_FILE_HEAP` backend. It is off by default on every device. Enable it and
fully relaunch the app to use a temporary shared file for the guest heap;
disable it and relaunch to return to anonymous backing. Other games and
visionOS keep their existing policy.

The guest addresses, heap capacity and saved game data do not change. The
runtime unlinks the temporary file immediately after creating it and retains
it through the mapping; terminating the process releases that backing. Dirty
pages can still consume storage while the process is alive. Storage failures
can prevent startup, and page-in/writeback can add loading pauses.

This is an option for memory-limit exits, not a frame-rate guarantee. Lower
`phys_footprint` for a file-backed mapping does not mean the same reduction in
physical RAM use: accounting and reclaimability differ.

## Device evidence and limits

A local BOTW 1.9.0 diagnostic build on an iPad mini 6 (A15, 4 GB RAM,
iPadOS 27.2.1) previously exited with an iOS `per-process-limit` jetsam at
about 2.8 GiB process footprint. With this backend enabled, a subsequent run
continued for over ten minutes, including user-reported outdoor movement.
Logged footprint was around 0.3–1.2 GiB; many 30-second windows averaged
29.8–30.0 FPS. New texture uploads and first-use pipeline compilation still
caused isolated long frames. Longer sessions and other devices remain unverified.

That build also carried a constrained-address-map fix and an upstream graphics
reference-lifetime fix. It tests this backend, not a complete rebuild of this
branch or the Settings toggle itself. This PR only exposes the backend through
Settings; it does not alter its implementation or make it a device default.
