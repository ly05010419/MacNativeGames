# BotW Edge Smoothing: Sharper, No Slow-Motion Trails — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make BotW 1.9.0's Edge smoothing (MetalFX and Fast) less soft and free of lag on wind, particles and Link's hair, by giving the temporal pass better inputs, with no changes to the game's shaders.

**Architecture:** Three slices on the existing TAA in `totk_taa.m`:
1. The game's FXAA stops running on top of the temporal result.
2. Changing transparents are marked in the reactive mask, by comparing the HDR scene before and after the transparent passes.
3. The depth test that builds the mask is tightened for thin, moving geometry.

The mask kernels move into one MSL header, so a GPU probe can test them on synthetic textures. Each slice has an env switch for A/B and is judged on in-game captures.

**Tech Stack:** Objective-C / Metal 4 (`MTL4CommandBuffer`, `MTL4FXTemporalScaler`), MSL compute kernels compiled at run time, Python `unittest` source tests and GPU probes (`tests/*_probe.m` + `tests/test_*.py`).

**Spec:** `docs/superpowers/specs/2026-11-25-botw-edge-smoothing-design.md`

## Global Constraints

- Target: BotW 1.9.0 on the Mac (`GAME_BOTW190`, `!TARGET_OS_IPHONE`). TotK, iOS, visionOS and zeldaVR (1.6.0) behave as before.
- Added GPU cost ≤ **0.5 ms per frame at 1920×1080 on the M4 Pro (macOS 27.2.1)** for all new passes together, measured with `TOTK_PASS_GPU`. MetalFX-mode comparisons use frame time too.
- Every slice has an env switch that restores the old behaviour for A/B.
- No new settings rows. The AA method row's text may change (Task 4).
- Metal 4 APIs only. Our textures go into the TAA residency set (`g.rs`, `mk()`). We encode only outside open encoders (`rp_end(); blit_close(); rs_sync();`).
- `botw_finish.h` stays within 300 lines (`tests/test_botw_finish.py::test_within_300_lines`).
- A slice ships when it improves at least one mode (MetalFX or Fast) on the captures and makes neither worse. Trading trails for shimmer is a failure.
- Comments follow the codebase: the why, with dates and measurements. Logging goes through `TLOG` (totk_taa.m) / `MLOG`.
- zsh: no reserved names (`status`, `path`), quote globs, `sed -i ''`.

## Shared commands

```zsh
REPO=/Users/luisz/orca/workspaces/MacNativeGames/botw-temporal
PY=~/Library/Caches/MacNativeGames/deps/python-ui-pack-74b86bee2823f5ec/bin/python
export BOTW190_SHADER_SEED=~/MacNativeGames-work/botw/totk/build/shader-seed/botw190
export TOTK143_SHADER_SEED=~/MacNativeGames-work/totk/totk/build/shader-seed/v143
# one test file
cd $REPO && $PY -m unittest discover -s tests -p 'test_botw_finish.py' -v
# full suite, as CI
cd $REPO && $PY .github/run_source_tests.py core && $PY .github/run_source_tests.py settings
# BotW 1.9.0 binary (PY=python3 if runtime/botw_pchtxt_sites.h regenerates in a fresh worktree)
make -C $REPO/games/totk PY=python3 binary-botw190
# TotK must still build (shared files)
make -C $REPO/games/totk PY=python3 binary-v143
```

The game run command is established in Task 1 and written into `$SCRATCH/run_botw.sh`, where `$SCRATCH` is the session scratchpad. Every later "capture" step uses it.

## Review Focus

1. **Edge smoothing switched off or on mid-session.** The game's FXAA returns on the next frame it is off, and the AA row goes back to its normal choices within 120 frames. → Task 4, test `test_fxaa_back_when_taa_not_this_frame`.
2. **Camera cut, reset or first frame.** No mask is built from stale history: transparent-layer history and depth history are treated as invalid on `reset`. → Task 6 `reset_gives_no_layer_mask` and Task 7 `reset_gives_zero_mask`.
3. **Resolution change in play (1080p ↔ 2K).** A lit snapshot whose size or format no longer matches the scene is skipped for that frame, with no crash and no mask. → Task 6, test `test_mismatched_lit_snapshot_is_skipped`.
4. **Frames without anti-aliasing (menus, loading, no G-buffer).** No lit copy, no FXAA override: the game's FXAA runs. → Task 4 test 1, Task 6 `test_lit_copy_only_when_taa_will_run`.
5. **Fast mode.** It gets the FXAA override and the new mask terms exactly as MetalFX mode does, because both read `g.react`. → Task 4 `test_override_does_not_depend_on_mode` and Task 6 `test_layer_mask_before_both_resolves`.

---

### Task 1: Run harness and baseline captures (no product code)

**Files:**
- Create: `$SCRATCH/run_botw.sh`, `$SCRATCH/scene.txt` (input command stream), `$SCRATCH/taa_png.py` (raw dump → PNG; used from Task 2 on). Scratch only, not committed.

**Interfaces:**
- Produces: `run_botw.sh <out-dir> [ENV=VAL ...]` launches the worktree's `games/totk/build/totk-native-botw190` against the installed game data, with `TOTK_AUTOLOAD=1`, `TOTK_TAA=1` and `TOTK_INPUT_FILE` fed from `scene.txt`. It saves screenshots and the log into `<out-dir>` and exits on `close`.

- [ ] **Step 1:** Build `binary-botw190` (shared commands). Expected: `games/totk/build/totk-native-botw190` exists.
- [ ] **Step 2:** Find the launch recipe. Start with `cd ~/MacNativeGames-work/botw/totk && $REPO/games/totk/build/totk-native-botw190 rewritten/botw190`; the installed app's `Contents/Resources/totk` has the same layout. Expected: the game reaches the title, and with `TOTK_AUTOLOAD=1` it loads the newest save. If it cannot run from the worktree build, stop and report to the player.
- [ ] **Step 3:** Write `scene.txt` from the commands in `shared/runtime/input_file.h`: wait for load, `flag taa 1`, then `down`/`up` with the forward key (run) for 4 s and a camera-turn key for 2 s, `shotn 8 <out>/shot-%d.png` while running, `passdump 1`, `close`. Script key codes are in `input_file.h` (J=38 is A, Return=36 is Plus). Find the run and camera keys in the script layout there.
- [ ] **Step 4:** Run it and check that the shots show Link running with wind streaks, or grass and hair moving. If the newest save is not at such a spot, stop and ask the player for a save in a windy field (the plan cannot go on without a repeatable scene).
- [ ] **Step 5:** Keep the baseline: one run with Edge smoothing MetalFX, one with `flag taa_fast 1`, one with `flag taa 0`. Note each run's FPS and `TOTK_PASS_GPU` TAA timing.

### Task 2: `TOTK_TAA_DUMP` diagnostic (Slice 0)

**Files:**
- Modify: `games/totk/third_party/d2r_gfx/gfx/nvn/totk_taa.m` (dump, next to the header comment's diagnostics), `totk_taa.h` (two declarations; the visionOS stub block in `totk_taa.m` gets no-op versions)
- Modify: `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m` (call site next to `mfx_dump_due()` at ~23511; flag in the flag table next to `"taa_fast"` at ~4527)
- Test: `tests/test_taa_dump.py` (create)

**Interfaces:**
- Produces: `int taa_dump_due(void);` and `void taa_dump(long frame);`. Same contract as `mfx_dump_due` / `mfx_dump`: the caller has waited for the GPU.
- Produces: `TOTK_TAA_DUMP=<dir>` and flag `taa_dump 1`, which arms three consecutive anti-aliased frames, then disarms itself.
- Raw files, as `mfx_dump`: `<dir>/taa-<frame>-<name>-<w>x<h>.bin`, with names `scene` (the compose input as-is; log its pixel format), `depth` (R32F, `g.dh[current]`), `motion` (RG16F), `mask` (R16F) and `out` (RGBA16F).

- [ ] **Step 1: Write the failing source test** `tests/test_taa_dump.py`, class `TaaDump`:
  - `test_off_without_env`: `taa_dump_due` returns 0 unless `getenv("TOTK_TAA_DUMP")` is set (the regex finds the getenv inside `taa_dump_due`).
  - `test_called_where_mfx_dump_is`: in `nvn_mtl.m`, `taa_dump_due()` appears within 15 lines after `mfx_dump_due()`.
  - `test_flag`: `!strcmp(name, "taa_dump")` is in `nvn_mtl.m`.
  - `test_visionos_stub`: the `TARGET_OS_VISION` block defines both functions.
- [ ] **Step 2:** Run it. Expected: 4 failures.
- [ ] **Step 3:** Implement, reusing `dump_tex`'s approach from `totk_mfx.m:388` (blit to a shared buffer, wait, write).
- [ ] **Step 4:** Run the test (PASS) and build `binary-botw190` and `binary-v143` (both succeed).
- [ ] **Step 5:** Write `$SCRATCH/taa_png.py` (numpy; float16 / float32 / RG11B10 → PNG, tone-mapped `c/(1+c)`; motion as false colour; mask as grey). Run `run_botw.sh` with `TOTK_TAA_DUMP` and `flag taa_dump 1` mid-run. Expected: 15 `.bin` files that convert to sensible pictures.
- [ ] **Step 6: Commit** `feat: TOTK_TAA_DUMP, the edge smoothing's inputs and output of three frames (diagnostic)`.

### Task 3: Findings: H1–H3 and the transparent pass

**Files:**
- Modify: `docs/superpowers/specs/2026-11-25-botw-edge-smoothing-design.md` (append `## Findings (2026-11-25)`)

- [ ] **Step 1:** From the Task 2 dumps and Task 1 shots, confirm or rule out:
  - **H1:** compare the `out` dump with the final shot. Does the softness grow after FXAA?
  - **H2:** are wind and particles absent from `depth` and zero in `mask` where they trail?
  - **H3:** is the mask zero on hair and behind it?
  Record the pixel evidence and file names for each.
- [ ] **Step 2:** From the `passdump 1` output (Task 1), find the first render pass after the G-buffer pass that draws into the HDR scene texture that the compose draw `f7ee6fe1` later samples. Record how to recognise it: its colour attachment is that texture, its blending is on, it loads rather than clears. Also record whether lighting and particles share one pass, and whether fog comes after particles.
- [ ] **Step 3:** Decide and record:
  - If H1 is ruled out, Task 4 is dropped.
  - If Step 2 found no clean copy point, Task 6 stops here (spec: no splitting of game passes), and the player is told.
  - If H3 is ruled out, Task 7 is dropped.
- [ ] **Step 4: Commit** `docs: edge smoothing findings: <one line per hypothesis>`.

### Task 4: No FXAA on top of Edge smoothing (Slice 1, H1)

**Files:**
- Modify: `games/totk/third_party/d2r_gfx/gfx/nvn/totk_taa.m` + `totk_taa.h` (new query; no-op on visionOS)
- Modify: `games/totk/third_party/d2r_gfx/gfx/nvn/botw_finish.h` (`botw_fin_upscale`; new `dm_gfx_botw_taa_replaces_aa`)
- Modify: `games/totk/runtime/botw_effects.h` (AA rows)
- Modify: `tests/settings_panel_probe.m` (fake for the new function, next to the `dm_gfx_botw_fxaa_replaced` fake at line 84, or the probe fails to link)
- Test: `tests/test_botw_finish.py`

**Interfaces:**
- Produces: `int taa_this_frame(void);` returns 1 when `taa_encode` produced an output in the current game frame (`g.encoded_frame == g.frame + 1`). The compose draw runs before `bd7ffdd4` in the same frame.
- Produces: `int dm_gfx_botw_taa_replaces_aa(void);` returns 1 while the override acted within the last 120 frames, as `dm_gfx_botw_fxaa_replaced` does.
- Env: `TOTK_TAA_KEEP_FXAA=1` keeps the player's AA method under Edge smoothing (A/B).

- [ ] **Step 1: Write the failing source tests** in `tests/test_botw_finish.py`:
  - `test_fxaa_back_when_taa_not_this_frame`: in `botw_fin_upscale`, the override condition calls `taa_this_frame()`, not `taa_enabled()`. Otherwise a menu frame with Edge smoothing "on" but not run would lose FXAA.
  - `test_override_does_not_depend_on_mode`: the override branch mentions neither `taa_fast_enabled` nor `g.fast`.
  - `test_override_forces_off`: inside the override, `off` becomes 1 and `smaa` and `quality` become 0, before `ours` is computed.
  - `test_keep_fxaa_env`: `getenv("TOTK_TAA_KEEP_FXAA")` guards the override.
  - `test_saved_choice_untouched`: the override branch calls neither `dm_gfx_aa_method_set` nor `settings_aa_save`.
  - In `Page`, `test_rows_say_edge_smoothing_replaces_it`: `rows` has a branch `if(dm_gfx_botw_taa_replaces_aa()){` with `settings_text("边缘平滑取代了它","Edge smoothing replaces it")` in fixed rows, and the MetalFX branch stays first (MetalFX replacing the draw wins).
- [ ] **Step 2:** Run `test_botw_finish.py`. Expected: the new tests FAIL, the old ones PASS.
- [ ] **Step 3:** Implement `taa_this_frame`, the override in `botw_fin_upscale` and the row branch. Add the fake to `settings_panel_probe.m`.
- [ ] **Step 4:** Run `test_botw_finish.py`, `test_settings_panel.py` and the full suite. Expected: PASS, with no skips beyond those the suite already had (report any skip).
- [ ] **Step 5:** Build both binaries. Capture A/B with `run_botw.sh`: MetalFX and Fast, each with and without `TOTK_TAA_KEEP_FXAA=1`, at the same frames. Convert, compare sharpness on the same crops, and check for crawl on edges. Record the result in the commit body.
- [ ] **Step 6: Commit** `feat: BotW: the game's FXAA no longer blurs edge smoothing's picture (TOTK_TAA_KEEP_FXAA=1 keeps it)`.

### Task 5: Mask kernels in one MSL header, with a GPU probe (refactor, behaviour pinned)

**Files:**
- Create: `games/totk/third_party/d2r_gfx/gfx/nvn/taa_kernels.h`. It holds `static const char taa_kernels_msl[]`: today's `taa_mv` kernel moved verbatim from `setup()` in `totk_taa.m`, with its `struct P`.
- Modify: `totk_taa.m` (`setup()` compiles `taa_kernels_msl`)
- Create: `tests/taa_mask_probe.m` (compiles `taa_kernels_msl` with `MTLMathModeSafe`, dispatches on synthetic textures, prints `name ok|FAIL value`, exits 77 without a Metal device)
- Create: `tests/test_taa_mask.py` (builds the probe with `clang -fobjc-arc -Wall -Wextra -Werror -I<GFX> -framework Foundation -framework Metal`, runs it, and maps exit 77 to a skip, as `tests/test_mfx_up_cache.py` does)

**Interfaces:**
- Produces: `taa_kernels_msl` (MSL source) and kernel `taa_mv` with its binding layout unchanged: textures 0 depth, 1 mv, 2 previous depth, 3 current-depth copy, 4 mask; buffer 0 `P`. Tasks 6 and 7 add kernels and fields to this header.
- Probe helper: `static float mask_at(id<MTLTexture> depthNow, id<MTLTexture> depthPrev, simd_float4x4 reproj, int prevValid, int x, int y)` runs `taa_mv` and returns the mask at a pixel.

- [ ] **Step 1: Write the probe cases**, pinning today's behaviour with an identity reprojection on a 64×64 depth:
  - `static_scene_zero`: equal depths → mask 0 everywhere.
  - `jump_20pct_full`: one pixel 20 % nearer than last frame and its 3×3 → 1.
  - `jump_7pct_partial`: 7 % → (0.07−0.04)/0.06 = 0.5 ± 0.02.
  - `prev_invalid_zero`: `prev_valid` 0 → 0.
  - `thin_strand_1px_shift_missed`: a 1-px-wide near strand moved 1 px → 0. This documents H3. Task 7 flips it.
- [ ] **Step 2:** Run `test_taa_mask.py` before the move. Expected: FAIL (header missing).
- [ ] **Step 3:** Move the kernel source into `taa_kernels.h` and compile it from `setup()`.
- [ ] **Step 4:** Run `test_taa_mask.py` (PASS) and build both binaries. One `run_botw.sh` capture must match the Task 1 MetalFX baseline (`taa_png.py` diff < 1/255 mean).
- [ ] **Step 5: Commit** `refactor: the edge smoothing's mask kernel in taa_kernels.h, pinned by a GPU probe`.

### Task 6: Changing transparents in the reactive mask (Slice 2, H2)

Only if Task 3 found a clean copy point.

**Files:**
- Modify: `taa_kernels.h` (kernel `taa_layer`), `totk_taa.m` (lit copy, layer history, dispatch), `totk_taa.h` (`taa_lit_pass`, `taa_note_lit`; no-ops on visionOS)
- Modify: `nvn_mtl.m` (at the pass start beside `taa_pass(pd)` at ~7647: when `taa_lit_pass(pd)` is 1, end encoders and call `taa_note_lit`)
- Test: `tests/taa_mask_probe.m`, `tests/test_taa_mask.py`, `tests/test_taa_dump.py` (source checks)

**Interfaces:**
- Produces: `int taa_lit_pass(MTL4RenderPassDescriptor *pd);` returns 1 for the pass Task 3 recorded (recognised as recorded there), at most once per frame, and only while `taa_ready()`.
- Produces: `void taa_note_lit(id<MTL4CommandBuffer> cb, id<MTLTexture> scene);` blits the scene into `g.lit` (same size and format, made with `mk()`).
- Kernel `taa_layer`:
  - Inputs: texture 0 compose input, 1 `g.lit`, 2 `g.mv`, 3 last frame's layer, 4 mask (read-write). Output: texture 5, this frame's layer (RG16F ping-pong `g.layer[2]`, holding luma and the layer's mask contribution). Buffer 0: `struct L { float2 size; float2 inv_size; float prev_valid; float threshold; }`.
  - Algorithm: `tm(c) = c / (1 + max3(c))` as in the Fast resolve. The layer is `luma(tm(in)) - luma(tm(lit))`. Its change is `abs(layer - layer_prev(at uv + mv/size, bilinear))`. The mask is `max(mask, saturate((change - threshold) / threshold))`. With `prev_valid` 0, it writes the layer and leaves the mask alone.
  - It runs in `taa_encode` after `taa_mv`, in the same encoder, after a dispatch barrier, so both the MetalFX and the Fast resolve read it.
- Env: `TOTK_TAA_LAYER=0` disables it. `TOTK_TAA_LAYER_THRESHOLD` (default 0.02, range 0.005–0.2) tunes it.

- [ ] **Step 1: Write the probe cases:**
  - `static_layer_pan_zero`: a constant transparent layer (water) under a 3-px camera pan with matching motion → mask 0.
  - `moving_particle_masks_both_spots`: a 4×4 bright particle moved 6 px with zero motion vectors → mask ≥ 0.9 at its old and new spots, 0 elsewhere.
  - `no_transparents_zero`: lit equals in → 0.
  - `reset_gives_no_layer_mask`: `prev_valid` 0 with a moved particle → 0.
- [ ] **Step 2: Write the source tests** (`tests/test_taa_mask.py`, class `Hooks`):
  - `test_lit_copy_only_when_taa_will_run`: `taa_lit_pass` checks `taa_ready()`.
  - `test_mismatched_lit_snapshot_is_skipped`: `taa_encode` dispatches `taa_layer` only when `g.lit` has the source's width, height and pixel format, and the lit copy was taken this frame.
  - `test_layer_mask_before_both_resolves`: the `taa_layer` dispatch comes before both `if (fast)` and `encodeToCommandBuffer`.
  - `test_layer_env_switch`.
- [ ] **Step 3:** Run both. Expected: the new cases FAIL.
- [ ] **Step 4:** Implement.
- [ ] **Step 5:** Run the tests (PASS) and the full suite. Build both binaries.
- [ ] **Step 6:** Capture A/B (`TOTK_TAA_LAYER=0` vs on; MetalFX and Fast). Check:
  - the mask dump covers the wind streaks;
  - large static transparents (water, fog, if in view) stay mostly unmasked;
  - the `TOTK_PASS_GPU` time of the copy plus the kernel is within the remaining budget.
  Tune `TOTK_TAA_LAYER_THRESHOLD` if needed, and set the default to the tuned value.
- [ ] **Step 7: Commit** `feat: BotW edge smoothing: transparents that change (wind, particles) take the current frame (TOTK_TAA_LAYER=0 turns it off)`.

### Task 7: Thin and moving geometry (Slice 3, H3)

Only if Task 3 did not rule out H3.

**Files:**
- Modify: `taa_kernels.h` (`taa_mv` gains a second test), `totk_taa.m` (a parameter)
- Test: `tests/taa_mask_probe.m`

**Interfaces:**
- `struct P` gains `float bidir;` at the end (1 on; `TOTK_TAA_MASK_BIDIR=0` sets 0). The 256-byte parameter buffer and the Fast resolve's offset (+128) still fit; assert `sizeof` stays ≤ 128 in the probe.
- Second test: last frame's depth *at this pixel* against this frame's depth at the same spot, both reprojected. When the camera-reprojected surface this frame is not where last frame's surface lands, the pixel is a disocclusion. Use the exact-tap difference, not the 3×3 nearest, and the same 4 %→10 % ramp. Max-combine it with today's term.

- [ ] **Step 1:** Change `thin_strand_1px_shift_missed` into `thin_strand_1px_shift_masked`: mask ≥ 0.9 at the strand's new and old columns. Add:
  - `flat_wall_pan_zero`: a slanted plane under a 2-px camera pan with exact reprojection → mask ≤ 0.05 everywhere (the anti-goal: no new masking on static surfaces, which would bring back aliasing).
  - `bidir_off_restores_old`: `bidir` 0 → identical to Task 5's values.
  - `reset_gives_zero_mask`.
- [ ] **Step 2:** Run. Expected: the new cases FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run (PASS), run the full suite and build both binaries.
- [ ] **Step 5:** Capture A/B (`TOTK_TAA_MASK_BIDIR=0` vs on; MetalFX and Fast). Check:
  - hair and character edges no longer trail;
  - static distant geometry (grass fields, fences) does not shimmer more than with it off (compare 8 consecutive shots);
  - timing within budget.
- [ ] **Step 6:** If hair or characters still trail, check the Task 1 `passdump` for a stencil attachment on the G-buffer pass and whether character draws write a distinct stencil value. Record the finding in the commit body. Using it is a follow-up decided in Task 8, not done here.
- [ ] **Step 7: Commit** `feat: BotW edge smoothing: a two-way depth test catches thin moving geometry (TOTK_TAA_MASK_BIDIR=0 turns it off)`.

### Task 8: Results and the next decision

**Files:**
- Modify: `docs/superpowers/specs/2026-11-25-botw-edge-smoothing-design.md` (append `## Results`)

- [ ] **Step 1:** Final capture set (Edge smoothing MetalFX, Fast and off, all slices on) at the Task 1 frames. Re-check the Slice 1 risk: do pixels in the mask crawl without FXAA? If yes, record that the follow-up "FXAA only where masked" is needed.
- [ ] **Step 2:** Record the total added GPU time and frame time per mode on the M4 Pro.
- [ ] **Step 3:** Record what still lags or softens, per mode. Recommend whether to keep sharpening as is or give Edge smoothing a default, and whether approach B is warranted.
- [ ] **Step 4:** Send the player the A/B PNG pairs (paths in full) for their judgement.
- [ ] **Step 5: Commit** `docs: edge smoothing results and the case for approach B`.
