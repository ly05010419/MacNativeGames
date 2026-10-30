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
  of assuming Zelda's 2368-byte camera layout or a placeholder FOV. The affine
  view times inverse-view must be approximately identity, including translation.
  Rotation tolerance is 0.001; translation allows 0.05 cm plus 0.00001 times the
  sum of the absolute product terms to accommodate float cancellation. If two
  passing blocks decode to different cameras (including projection), no camera
  is published and the frame keeps its real picture. Identical copies are fine.
  This checks consistency, not semantic identity: a single unrelated block with
  a consistent inverse and plausible projection can still pass.
- The final FXAA draw (`5345f3231650f0b7...`) identifies the tone-mapped canvas
  once it is encoded: a draw skipped while its pipeline is missing or still
  compiling leaves the frame without a scene, and the real picture is shown.
  The canvas is copied before subsequent HUD/text/tutorial-video draws, which
  are also written to a separate UI attachment. A frame without HUD is copied
  at the final canvas-to-swapchain draw and uses a transparent UI layer.
- Depth keeps the existing D32F, matching-size, three-colour-target check.
  Missing inputs, loading screens, first frames and cuts reset history and
  present the real picture. The translation cut threshold is 800 centimetres;
  normal camera follow motion must not trigger Zelda's 8-unit cut threshold.
  800 is a guard, not a tuned value: in one 8-minute session normal steps
  stayed under 110 cm per game frame (p99 about 37 cm), and the one cut seen
  was 3,868 cm with a 29 degree turn, caught by both checks.
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
projection, changing uniform bytes, a different slot, a lower-slot false match
with an inconsistent inverse, large world positions, identical camera copies,
conflicting valid cameras/projections, and invalid input; cuts at Odyssey's
scale; the display link mode while 60+60 is on and after it; the scene canvas when the FXAA draw is skipped or
encoded; and the actual interpolator request/latch lifecycle across unsupported
screens, 120 Hz, suspension and recovery. They do not execute guest code or
encode GPU work.

The 60+60 pacing itself (a 1/60 s game period on the host clock, and no change
on a 60 Hz screen) is checked by the present-interval probe in `probe-gfx`,
which forces the screen rate because it has no window
(`TOTK_PROBE_NEGATIVE=41` is its negative control). On a GPU without Metal 4 FX
those cases are skipped and the probe says so.

With `TOTK_MFX` on, the log reports which vertex uniform slots the camera came
from, how many ambiguous frames were kept real (`[mfx] camera ...`), and where the
final copy found the canvas (`[mfx] final copy ...`).

For a short game check, use a separate test profile and cache, the pinned 1.3.0
inputs and this directory's own runtime. Resume into a 3D scene, enable key 2,
move the camera, open and close the menu, disable it and exit. Inspect both
`[mfx]` generation counts and `上屏节奏` (drawable `presentedTime`) lines: generated
counts alone do not establish that the pictures reached the screen. Capture a
real and generated picture to check the HUD, not just an FPS label. Repeat on
the other screen if checking automatic suspension/recovery.

Keep generated apps, game data, saves, shaders, caches and logs outside the
source checkout. Never upload the game-derived seed or libraries with the PR.

## Camera follow-up (2026-10-25)

On the same M3 Mac and Odyssey 1.3.0 Cap Kingdom save, a short diagnostic run
reproduced the old slot-3 false match: the maximum rotation identity error was
1.28–1.29 and translation error 18,957–19,298 cm. It is rejected by the inverse
check above. Diagnostic code used for this measurement is not shipped. This
is evidence from one kingdom, not validation of every camera in the game.

The final runtime was rebuilt and booted with a separate test save. In a
117-second smoke check (Cap Kingdom, camera turn and walking), 2,400 captures
used slot 1 and no conflicting valid camera was seen; 2,711 generated pictures
were encoded by game frame 4,200. The game exited normally and the HUD/tutorial
capture remained legible. This was a functional smoke check, not a performance
comparison: the GPU was saturated and the presentation window included dropped
pictures (p50 16.67–25 ms). It does not establish sustained 120 Hz output.
