# Optional 60+60 frame interpolation

Press **2** to request or disable MetalFX frame interpolation. It starts disabled.
`TOTK_MFX=interp` requests it at launch. The request takes effect at a game-frame
boundary, only on a supported Metal 4 GPU and a window screen reporting 120 Hz.
On 60, 100, 144 Hz or an unknown screen rate, generation stays off. Moving back
to 120 Hz restores the request with fresh history; pressing 2 again cancels it.
The shortcut help reports the effective mode.

Odyssey advances its logic once per frame. The host keeps the game at 60 real
frames per second with interpolation both off and on. Extra pictures come from
the host; no guest code, frame-step constant or game version table is patched.
Legacy nonzero interpolation requests (`interp30`, `interpmax`, numeric flags)
map to the same 60+60 request rather than slowing or accelerating game logic.
This does not promise 120 displayed pictures when the game or GPU misses its
budget, or when macOS stops presenting an occluded window.

## Inputs and limitations

- The G-buffer camera is decoded from Odyssey's vertex uniform layout: view at
  +0, inverse view at +0x30, view-projection at +0x60 and near/far at +0x160.
  Uniform slots vary with the material, so the host checks the bound blocks.
  It reads pending updates and derives FOV/aspect from the projection instead
  of assuming Zelda's 2368-byte camera layout or a placeholder FOV.
- The final FXAA draw (`5345f3231650f0b7...`) identifies the tone-mapped canvas.
  The canvas is copied before subsequent HUD/text/tutorial-video draws, which
  are also written to a separate UI attachment. A frame without HUD is copied
  at the final canvas-to-swapchain draw and uses a transparent UI layer.
- Depth keeps the existing D32F, matching-size, three-colour-target check.
  Missing inputs, loading screens, first frames and cuts reset history and
  present the real picture. The translation cut threshold is 800 centimetres;
  normal camera follow motion must not trigger Zelda's 8-unit cut threshold.
- Motion vectors use camera reprojection. Mario, Cappy, skeletal motion,
  particles and disocclusions can still leave trails or incorrect edges.
  This is an optional feature, not a claim of native 120 Hz gameplay or a fix
  for all interpolation artifacts. TAA and spatial upscaling are separate.
- While 60+60 is on, the display link runs at 120 Hz (`DM_PRESENT_LINK` mode 2) so the real and the generated
  picture each get a refresh; at the default 60 Hz link every tick took the generated picture and the real frames
  never reached the screen. The game itself keeps its own 60 Hz clock.
- The actual window's screen is sampled on the AppKit main thread; the render
  threads read an atomic rate. Screen, mode and backing changes refresh it,
  even if the drawable dimensions do not change. Presentation intervals and
  the existing presented-time statistics use the updated rate.

## Core checks

With Xcode selected:

```sh
python -B -m unittest discover -s games/odyssey/tests -v
```

The added probes exercise the production camera callback with a rotated
projection, changing uniform bytes, a different slot and invalid input; cuts
at Odyssey's scale; fixed 60 Hz periods; and the actual interpolator request/
latch lifecycle across unsupported screens, 120 Hz, suspension and recovery.
They do not execute guest code or encode GPU work.

For a short game check, use a separate test profile and cache, the pinned 1.3.0
inputs and this directory's own runtime. Resume into a 3D scene, enable key 2,
move the camera, open and close the menu, disable it and exit. Inspect both
`[mfx]` generation counts and `上屏节奏` (drawable `presentedTime`) lines: generated
counts alone do not establish that the pictures reached the screen. Capture a
real and generated picture to check the HUD, not just an FPS label. Repeat on
the other screen if checking automatic suspension/recovery.

Keep generated apps, game data, saves, shaders, caches and logs outside the
source checkout. Never upload the game-derived seed or libraries with the PR.
