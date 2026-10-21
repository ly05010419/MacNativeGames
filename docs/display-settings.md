# macOS control menu

Open **Game Controls…** from the application's menu, click **Game Controls** in
the game window, or press **F1** / **Command+,**. This replaces the passive
shortcut reminder. The menu has Graphics, Frame Rate, Shortcuts, Keyboard & Mouse and Controller tabs.
See [game input bindings](input-bindings.md) for the two input pages.
Apply saves changes; Cancel discards the draft. Full screen uses a separate
macOS Space; borderless fills the current desktop; windowed restores the prior
window frame. No option changes the physical display mode.

## Actual render resolution and MetalFX

Target presets are 1280×720, 1600×900, 1920×1080 and 2560×1440. Both the guest
canvas and Metal drawable use the selected target. Window resizing only scales
the displayed image with its aspect ratio preserved. The upstream ultrawide
camera and HUD path remains available: its fixed 16:9 canvas is expanded to the
selected wide camera ratio when presented. `TOTK_STRETCH=1` still fills the view.
The compact FPS/shader/load panel remains available without the old shortcut
reminder; `TOTK_HUD=corners` keeps the readings in the picture. Unlike the previous
output-only setting, the scene render dimensions now also follow this choice:

| MetalFX preset | Scene size relative to target | At 1920×1080 |
| --- | --- | --- |
| Off | 100% | 1920×1080 |
| Quality | about 2/3 per dimension | 1280×720 |
| Balanced | about 1/1.7 per dimension | 1120×630 |
| Performance | 1/2 per dimension | 960×540 |

Sizes are rounded to an exact 16:9 integer grid with a minimum of 640×360.
These are application-defined render-scale presets, not Apple quality enums.
1080p Quality shades about 44% as many scene pixels as native 1080p, before
upscaling cost. This is a pixel count, not an FPS improvement promise. Shadows,
compute, CPU work and fixed-size passes do not necessarily scale with this ratio.

MetalFX spatial scaling replaces the game's docked upscale before UI composition.
It works with a fixed drawable and does not depend on a larger window. A real
size change is latched at the guest present boundary into the verified render-size
and canvas data pages, then requests the existing game display-layout refresh.
No guest instruction is rewritten during play. The texture/Graphics/application
heap reserve and generated RomFS overlay use the same maximum-scene formula;
the builder regenerates old overlays that lack `scene-render-reserve=1`.

The status line reads scene and canvas dimensions from the actual game upscale
constants, plus the number of successfully encoded MetalFX frames. A requested
checkbox alone is not treated as execution evidence. New scaler configurations
build in the background; the game's existing upscale is retained until ready.

Antialiasing follows the current upstream FXAA/TAA path. The menu exposes the
existing TAA switch and retains plain key 5 for compatibility.

## Frame controls

- Vsync controls the display link and `CAMetalLayer.displaySyncEnabled`.
- Real-frame limit: 0 disables the application limit; 15–240 is an explicit cap.
  Vsync and cap can be used together; their lower rate governs. Non-divisor caps
  use their own clock rather than rounding to a divisor of refresh rate.
- MetalFX frame generation is an independent switch. It does not select 30 FPS
  or half-refresh pacing. Generated frames can increase displayed FPS without
  increasing the rate at which the game responds to input.
- With Vsync off and cap 0, no application pacing period is imposed. Presentation
  backpressure, GPU completion waits and resource lifetime protections remain.

Real-time frame stepping follows elapsed time, including above 120 real FPS
instead of advancing every frame by at least 1/120 second. Probes, iOS and
headless/environment-controlled runs retain their legacy policy unless the
macOS control configuration is activated.

## Shortcuts and preferences

Defaults: F1 opens the menu, 1 toggles overlays, 2 toggles frame generation, 3 cycles
Off/Quality/Balanced/Performance, and 4 toggles the upstream ultrawide camera
on supported versions (1.2.1/1.4.3). Every action can be rebound to a key or chord
in Shortcuts; Delete unassigns it and Escape cancels recording. Duplicate bindings
and reserved Command+Q/Command+, are rejected. Command+, remains a menu fallback.
Reassigned plain keys no longer also trigger the old hardcoded actions.

Preferences use `org.macnativegames.totk.display`; non-user game-slot owners use
the test suite. New configurations default to 1080p Quality, FXAA, Vsync on,
no explicit cap, and frame generation off. Prior window mode is retained.

## Focused verification

- `test_control_policy.py`: render dimensions and independent Vsync/limiter periods.
- `test_control_input.py`: production input dispatch and guest controller samples.
- `frontend_display_probe.m`: production AppKit display controls with guest stubs;
  Apply, mode transitions, aspect ratio, validation, Cancel and preferences.
- [Input binding probes](input-bindings.md#core-verification) cover the added pages.

This branch integrates the unmerged #42 control menu onto current main and adds
game input bindings. It retains the compact performance panel, `TOTK_HUD=corners`,
`TOTK_STRETCH`, ultrawide camera/HUD path, and the iOS embedded-code guards.
Headless probes and iOS exports do not load macOS menu preferences.

Per-platform build and play evidence is reported in the pull request; probes
are not a substitute for running each supported game version.
