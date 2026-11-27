# BotW: a sharper Edge smoothing without slow-motion trails

Date: 2026-11-25. Status: design, awaiting approval.

## Intent and the why

The player wants **MetalFX temporal upscaling in Breath of the Wild on the Mac**: rendering the scene below the canvas
and reconstructing it, for more FPS at high resolutions. It would sit beside the existing MetalFX spatial upscale, which
stays.

Today's **Edge smoothing → MetalFX** (the 1:1 temporal scaler in `totk_taa.m`) has two flaws the player sees:

1. **The image is soft.**
2. **Fast-moving things look like slow motion:** wind streaks and Link's hair lag behind.

An upscaling temporal pass relies on its history *more* than a 1:1 pass, so it would make both flaws worse. So the work
comes in two stages:

- **This design (approach A):** better inputs for the existing Edge smoothing, with no change to the game's shaders.
- **Afterwards:** measure what is left, then decide whether per-object motion vectors (approach B) are worth it. After
  that, build the temporal upscale (the FPS path) on the same base. Both get designs of their own later.

## How it works today (BotW 1.9.0, Mac)

1. The G-buffer pass (`taa_pass`: D32F depth at the colour size, 3 or more colour attachments) draws with a sub-pixel
   viewport jitter (Halton 2,3).
2. Lighting fills the linear HDR scene. Transparent and forward passes (particles, wind) follow. Exactly where they run
   is not yet known.
3. At the compose draw `f7ee6fe1` (`TAA_COMPOSE`, tone mapping and bloom), `taa_encode` runs the MetalFX temporal
   scaler 1:1 on the scene (or the Fast compute resolve). Compose samples our output.
4. The game's FXAA and upscale draw `bd7ffdd4` (`BOTW_MFXUP_KEY`, `botw_finish.h`) runs next. The AA method row
   (Off / FXAA / SMAA, default FXAA) replaces it with our passes. **Nothing turns it off while Edge smoothing is on.**
5. Sharpening (RCAS, 0–100 %, default off) runs on the canvas before the UI.

The inputs to the temporal pass (`taa_mv` kernel in `totk_taa.m`):

- **Motion vectors** come from the **camera only**: the depth is reprojected with this frame's and the previous frame's
  view-projection. A character, hair or a particle moving on its own gets the camera's motion.
- **Reactive mask:** the depth at the reprojected spot in last frame's depth, nearest of 3×3. It ramps from 4 % apart
  (on) to 10 % (current frame only). Anything that writes no depth (transparents) never triggers it. Thin geometry that
  moves about a pixel finds a near match in the 3×3 and keeps its history.
- MetalFX runs with auto exposure off and exposure 1, deliberately (see the comment in `ensure_scaler`).

## Hypotheses (each slice confirms its own before changing code)

- **H1 – softness:** the game's FXAA blurs the temporally resolved picture a second time.
- **H2 – wind and particles:** transparents draw after the G-buffer and write no depth, so the mask misses them. MetalFX
  keeps their old positions in history.
- **H3 – hair:** thin, moving geometry passes the 3×3 depth test (and disocclusion behind it does too), so its history
  is kept.

## Goals

- Edge smoothing (MetalFX and Fast) shows no visible lag on wind streaks, particles or Link's hair while running and
  while turning the camera.
- With Edge smoothing on, the image is no softer than Edge smoothing off with the game's FXAA. Judged by the player on
  A/B captures of the same scene.
- Added GPU cost stays at or under **0.5 ms per frame at 1920×1080 on the development Mac (M4 Pro, macOS 27.2.1)** for
  all new passes together (our call; measured with `TOTK_PASS_GPU`). Our new passes are plain compute and scale with the
  GPU. MetalFX's own network does not: per the notes in `totk_taa.m`, it runs partly on the Neural Engine on M3 Pro, and
  on M5 Pro / Max (macOS 27) its network runs on the Neural Engine, where the command buffer waits for it but GPU pass
  timings do not show it. So MetalFX-mode comparisons use frame time as well as pass timings.
- Each slice ships on its own, can be reverted on its own, and can be switched off by an environment variable for A/B
  (`TOTK_TAA_REACTIVE=0` is the existing precedent).

## Non-goals

- Per-object motion vectors (approach B), shader translator changes and temporal upscaling: later designs.
- Tears of the Kingdom: its behaviour stays as it is. Its `tup_*` experiment is left alone, neither removed nor reused.
- iOS, visionOS and zeldaVR (BotW 1.6.0 VR): not targeted. Shared code must not change their behaviour.
- New settings rows.

## Anti-goals (a failure even if it technically works)

- Wind or hair stop lagging but now **shimmer or crawl** worse than Edge smoothing off. Trading trails for flicker is a
  failure.
- The UI, HUD or text gets softer or ghosts.
- A change for BotW silently alters TotK's image.
- Quality is claimed from reasoning alone. Each slice is judged on captures from the game.

## Approaches considered

- **A. Better inputs, no game-shader changes:** this design. Low risk, ships in slices. It fixes trails by telling
  MetalFX to trust the current frame where motion is wrong. Those spots lose temporal smoothing and show the frame's
  own aliasing.
- **B. Per-object motion vectors:** each G-buffer draw's vertex shader also runs with the previous frame's uniforms.
  This removes trails *and* keeps smoothing on moving things, but needs draw matching across frames, kept uniforms and
  translator work (`games/lm3/tools/totk_xlate`). Weeks of work and high risk.
- **C. A now, measure, then decide on B:** **chosen by the player (2026-11-25).**

## The design (approach A), in slices

### Slice 0: capture for evidence

The hypotheses and the goals need captures from the same moment.

- Add a diagnostic dump, `TOTK_TAA_DUMP=<dir>`, modelled on `TOTK_MFX_DUMP` in `totk_mfx.m`. For a few consecutive
  frames it writes the scene input, our motion texture, the reactive mask, the MetalFX output and the final canvas
  (before UI), so the player can capture a scene and we can both look at the same frames.
- Use the existing `TOTK_PASS_CHAIN` / pass logging to find where BotW 1.9.0 draws its transparents and particles into
  the HDR scene. Slice 2 needs this.
- Output: a short note in the slice's commit that confirms or rules out H1–H3.

### Slice 1: no FXAA on top of Edge smoothing (H1)

- While Edge smoothing (either mode) has anti-aliased this frame (`taa_effective`), `botw_fin_upscale` treats the AA
  method as **off**: the picture is drawn into the canvas as with "FXAA off" today. An env switch keeps the game's FXAA
  for A/B.
- Risk (our call on how to handle it): pixels the reactive mask hands to the current frame have no temporal AA. Without
  FXAA they show the jittered frame's raw aliasing, which may crawl (anti-goal). Slice 1 ships as the plain switch. Its
  captures are re-checked after Slices 2 and 3, because those slices put more pixels in the mask. If masked pixels
  crawl, a follow-up runs the game's FXAA (or SMAA) only where the mask is set, still inside the 0.5 ms budget.
- The AA method row shows "Edge smoothing replaces it". This mirrors the existing "MetalFX replaces it"
  (`dm_gfx_botw_fxaa_replaced`). The saved AA choice is not changed and comes back when Edge smoothing is off.
- Sharpening is unchanged and stays the player's choice. Whether Edge smoothing should get a default sharpening is
  decided after this slice's captures, not before.

### Slice 2: transparents in the reactive mask (H2)

- Copy the HDR scene once, right after lighting and before the first transparent or forward pass (the point Slice 0
  locates).
- At `taa_encode`, a compute pass works out what the transparents added this frame (the compose input minus the copy,
  in a tone-mapped space).
- Only **changing** transparents go into the mask (our call). Static ones (water, glass, fog, light shafts) must keep
  temporal AA, otherwise a lake or full-screen fog would switch it off over much of the screen. So the pass compares
  this frame's added layer with last frame's at the camera-reprojected spot, kept like the depth history. Where the
  layer changed beyond a threshold, the value is max-combined into the reactive mask that `taa_mv` writes. Animated
  water may be partly masked; captures decide whether that is acceptable.
- If Slice 0 finds no clean copy point (lighting and particles in one render pass, or fog after particles), the slice
  stops and reports. Splitting a game render pass is not done inside this design.
- Both MetalFX and Fast use the same mask, so both benefit.
- Env switch to disable it for A/B.

### Slice 3: thin and moving geometry (H3)

- Tighten the depth test in `taa_mv` for thin geometry. The candidate is a test in both directions: this frame's depth
  against last frame's, and last frame's against this frame's at the reprojected spot. That catches disocclusion
  behind moving hair. Thresholds are tuned on Slice 0 captures.
- Investigate one cheaper signal: whether the BotW G-buffer pass writes a stencil value that marks characters or
  dynamic objects. If so, those pixels go into the mask too. Do this only if the captures show the depth test alone is
  not enough.
- Env switch to disable it for A/B.

### When a slice ships

Our call: a slice ships when it improves at least one Edge smoothing mode (MetalFX or Fast) and makes neither worse on
the captures. MetalFX's macOS 27 scaler is a neural network, and how closely it follows the reactive mask is not
documented. So lag that remains only in MetalFX mode after Slice 3 is recorded as input to the approach B decision. It
is not chased with MetalFX-only work inside this design.

### After Slice 3: measure and decide

The player compares A/B captures. We write down what still lags or softens and whether approach B is warranted.

## Who runs it

The builder runs the game on the development Mac (M4 Pro, decided by the player 2026-11-25): the installed
`~/Applications/Breath of the Wild.app`, built from `~/MacNativeGames-work/botw`. Captures, thresholds and timings are
done in quick loops there. The plan must first establish how to reach a repeatable scene (wind, Link running, camera
turning) without a person at the controls: a save the game loads on its own (`runtime/autoload.c`) and scripted input,
if the runtime has them. If it does not, the plan stops and asks the player for a save at such a spot. The player judges
the final A/B captures.

## Constraints

- `totk_taa.m`, `nvn_mtl.m` and `botw_finish.h` are shared by TotK and BotW builds. Game-specific behaviour goes behind
  `GAME_BOTW` / `GAME_BOTW190` (or the per-game shader keys in `nvn_obj.h`), so TotK and other builds compile and
  behave as before.
- Metal 4 / MetalFX 4 APIs only (`MTL4FXTemporalScaler`, `MTL4CommandBuffer`), as the code uses today. macOS 27 SDK
  features (`jitteredMotionVectorsEnabled`, content offsets) may be used behind `@available` if they help, but nothing
  in this design needs them.
- Every texture we create goes into the TAA residency set. Encoding happens outside open encoders
  (`rp_end(); blit_close(); rs_sync();`), as `taa_encode` does now.
- Codebase conventions: comments explain the why with dates and measurements, logging goes through `TLOG` / `MLOG`,
  and tests follow `games/totk/tests` (a standalone Metal probe like `tests/metalfx_interp` can check the mask kernels
  on synthetic textures). Run the suite with the CI test Python setup.

## Left to the builder

- Exact thresholds, the tone-mapped space for the Slice 2 difference, and the dump's file format.
- How the Slice 2 copy point is detected (by pass, by shader key, by render-target serial), once Slice 0 has found it.
- Unit-test shape for the kernels, within the existing test conventions.

## Findings (2026-11-25)

Captured on the M4 Pro (macOS 27.2.1) with BotW 1.9.0 from the worktree. The scene is the newest save (Great Plateau,
grass near the Temple of Time): Link runs front-right, then stands while the camera pans. Session averages: Edge
smoothing off 108.0 FPS, Fast 101.8, MetalFX 94.4. Dumps are frames 2950–2952 (`TOTK_TAA_DUMP`). The pass structure
is from `passdump 1` (frame 2979).

- **H1 (FXAA on top softens): confirmed.** On static areas of the still shot (camera and Link still), the mean |Laplacian|
  of luma (higher is sharper) is:

  | Area | MetalFX + FXAA | MetalFX, FXAA off | Off + FXAA | Off, raw |
  |---|---|---|---|---|
  | Mountains | 2.86 | 4.47 | 3.44 | 5.10 |
  | Temple | 5.28 | 5.60 | 5.20 | 7.63 |

  MetalFX + FXAA is softer than no anti-aliasing with FXAA. Without FXAA it is sharper than that, with smooth edges
  (visual A/B of the tower). Task 4 goes ahead.
- **H2 (particles trail): confirmed.** The floating light motes in the sky have depth 1.0 (nothing opaque) and a reactive
  mask of 0.00. In MetalFX's output each one becomes a fading trail of 4–5 copies at its earlier positions, and the
  current spot keeps only 30–50 % of its brightness (scene +22 over its surroundings, output +6). This is the
  "slow motion" of small moving things.
- **H3 (hair and thin geometry): not observed as stated.** On Link, running with the camera following, output minus
  input differs only along edges (the anti-aliasing itself). There are no delayed copies of the hair, ponytail or limbs,
  and the mask already covers most of his outline. **A related finding:** grass and bushes swaying in the wind are
  blurred well beyond static geometry. With the camera still, temporal AA keeps about 49 % of the raw grass detail
  (10.67 → 5.18) against 73 % on the temple (7.63 → 5.60). The difference is spread across the foliage, and the mask is
  mostly 0 there. The sway is sideways with nearly the same depth, so no depth test can see it, including the two-way
  test planned in Task 7. Task 7 is dropped. Foliage is recorded as the main input to the approach B decision (it needs
  real motion, or a per-draw signal).
- **Copy point for Slice 2: found.** The HDR scene (RG11B10Float, the texture `f7ee6fe1` samples) is the G-buffer pass's
  colour attachment 5. After the G-buffer passes, single-attachment passes into it come in this order:
  - two without blending on c0: lighting and sky;
  - then four with blending on c0: two single full-screen draws (fog and the half-resolution effects buffer
    composite), then 14 draws with 7 shaders, then 11 draws with 3 shaders (effects and particles);
  - then the compose `f7ee6fe1` and the FXAA / upscale `bd7ffdd4`.

  Lighting and particles never share a pass, and fog comes before particles. A pass's blending is only known at its
  draws, not when it opens. So the copy point is learned: count the scene-only passes after the G-buffer pass; the
  index of the first one with a draw blending into c0 is kept, and from the next frame on the copy is taken when that
  pass opens. One copy per frame.

## Results (2026-11-25)

Shipped slices: Slice 1 (no FXAA on top of edge smoothing) and Slice 2 (changing transparents in the reactive mask).
Slice 3 was dropped by the findings. All measured on the M4 Pro, same scene.

- **Particles: fixed.** The floating light motes now get a mask of 1.00 (it was 0.00) and keep their brightness in the
  output (+22 over the sky against +6 before), in MetalFX and fast modes. Three consecutive dumped frames show each mote
  once, at its current position, instead of a trail of 4–5 fading copies.
- **Softness: improved on static detail.** Mean |Laplacian| of the mountains in the still shot is 2.86 → 4.94 with
  MetalFX (4.62 in a second run). On the temple, visual A/B shows crisper edges, but the number moves with run-to-run
  variance (5.28 → 5.20 in one run, 6.16 in another). Single-run sharpness and FPS numbers vary between runs of the
  same build: edge smoothing off measured 108.0 and 100.7 FPS in two runs without a code change on that path.
- **Cost:** +0.05 ms GPU a frame for the scene copy (0.172 → 0.220 ms of copies). Compute time and frame span are
  unchanged within noise. Mask coverage is 1.0 % → 1.7 % of the frame.
- **The Slice 1 risk (pixels taken from the current frame crawling without FXAA): not observed.** Still-frame flicker
  (temporal std of luma over 8 frames) stays well below edge smoothing off with FXAA, and the layer on/off made no
  difference beyond run-to-run noise (which reaches about 0.1).
- **What is left:**
  - Foliage swaying in the wind is still blurred: temporal AA keeps about half the grass detail, against about
    three-quarters on static geometry. The sway is sideways at nearly the same depth, so neither the depth test nor the
    transparent layer sees it.
  - Hair lag was not reproduced in this scene.
- **Recommendations:**
  - Sharpening stays the player's choice. Without FXAA on top, the image no longer needs a default.
  - Approach B: foliage is the one remaining case, and it matters more for the FPS path (upscaling leans harder on
    history). Before committing to per-object motion vectors, spike how much of the foliage blur is wrong history
    rather than the anti-aliasing itself: for example, with wind frozen if the game allows it, or by marking the foliage
    shaders' draws in the mask as an experiment.

### Slice 1 taken back (2026-11-25, the player's decision)

After testing, the player asked for the anti-aliasing choice and edge smoothing to stay unlinked. Forcing the AA
method off took the choice away from whoever wants FXAA or SMAA with edge smoothing. Slice 1 is reverted: the AA row
always acts on its own, and the game's FXAA stays the default. The finding still holds: FXAA on top of edge smoothing
softens the picture. The sharpest combination is edge smoothing with the AA method set to Off, chosen by the player.
`tests/test_botw_finish.py` (AAIndependentOfEdgeSmoothing) keeps the two unlinked.
