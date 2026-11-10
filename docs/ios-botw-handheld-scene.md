# BOTW handheld scene sizes on iOS

With Full HD off, the new Handheld scene setting selects an internal 3D
scene of 960×540 (unchanged default), 800×450 or 640×360. Fully relaunch
the game after changing it. The game's own upscale still targets its
1280×720 canvas, and the presentation size and UI layout remain unchanged.
Lower internal sizes soften the world image. Docked mode keeps its separate
scene choice; this setting does not apply to macOS, visionOS or other games.
Unknown or malformed environment values fall back to 960×540.

A four-GB A15 iPad mini 6 diagnostic run at the existing handheld size
logged outdoor GPU times of roughly 23–24 ms. This exceeds the 16.7 ms
budget for 60 FPS, motivating an explicit lower-cost option. Changing a
frame cap alone cannot remove that bottleneck. A lower scene size reduces
pixel work but does not reduce every draw, CPU work, texture upload or
pipeline compilation; sustained 60 FPS has not been established.

## Code and export

The original handheld instruction pair synthesizes `{1280.0, 960.0}`.
The export now installs ADRP + LDR, analogous to the existing docked scene
patch, to load a pair of floats at main's writable data page +0x28. At
launch, the runtime writes `{1280.0, selected render width}` to that pair.
Both game versions keep their original store instruction and register x8.
The data slot is separate from frame timing, pad tick, docked/output sizes,
aspect, FOV and font tables.

The iOS executable's code pages are never modified at runtime. A fresh iOS
export/build is required: an older executable with the immediate form will
fail the existing embedded-instruction check rather than run a mixed patch.

## Validation

The compiled production patch is exercised with synthetic modules for
BOTW 1.6.0 and 1.9.0, both export and on-device verification paths, under
UBSan. Tests decode the installed instructions, follow their effective
address, check all offered sizes and invalid-value fallback, check that
neighboring data is untouched, and reject wrong instructions/layouts.
Packaging checks keep this option BOTW-only, off visionOS, and at the
existing default. Localization checks cover the updated pane.

Real-device scene-size, image-quality and sustained-frame-rate validation
remain pending. There is no claim of a locked 60 FPS result.
