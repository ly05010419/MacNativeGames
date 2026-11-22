# Changelog: Hyrule Warriors: Age of Calamity on the Zelda-line runtime

Each entry says what was found, what was done and how it was checked. Addresses are of 1.4.1 (update v458752: main
build ID e5f275b5…, subsdk0 3ab92c34… = the SDK's movie library, sdk 95832210… = SDK 9.6.0). `games/aoc` (this game on
nxnative's runtime) and its notes were read for findings; no nxnative code is used (third_party/COPIES.md).

## 2026-11-22 (evening) — the first battle's GPU hang and the texture re-upload storm, root causes

### The GPU hang (the maintainer's MacBook Air panicked five times)
- **Found**: unfixed runs into the first battle hung the GPU every time (4 of 4). Skipping compute kernels one by one
  (`TOTK_SKIP_CS=<key prefix>,...`, kept as a diagnostic in nvn_mtl.m `dispatch_bound`) narrowed it to the particle
  kernel `dc81d690` and its producer `7463cde4`. `dc81d690` keeps 68 bytes per thread from byte 304 for 64 threads:
  4656 bytes of shared memory, exactly what its control section records at +0x754; the translator was never told and
  declared its 4 KiB default, so threads past it wrote outside the threadgroup array and the counts its loops run to
  became garbage. The earlier entries' combined-handle and light-count explanations were not the cause (the light counts
  read 4..56, logged per dispatch).
- **Done**: shared `compute_shared.h` (every port): the size goes into the translation request, the variant name and the
  cache check; the chain is `aocx-native-v6-shared-memory`; the seed tool passes it too.
- **Checked**: 3 of 3 runs into the battle with `dc81d690` running, no hang; its translation declares 1164 words.

### The texture re-upload storm (the game stopped at the 4.5 GB GPU budget after the first battle's ending movie)
- **Found**: the game's 1.5 GB material pool starts on the last page of its first pool; the write tracker left an
  overlapping pool untracked, so every texture in it counted as written and was uploaded again at every submission
  (about 24 a frame in the first battle, 440 / 220 MB a frame in the next scene; the old copies piled up).
- **Done**: this port links Tears of the Kingdom's `nvn_dirty.c` / `.h`, whose split ranges (made for Breath of the Wild's
  same problem) are now the `NVN_DIRTY_SPLIT` switch, on for BotW and this game.
- **Checked**: the same run re-uploaded 1,768 times instead of 54,231, no missed change, the picture unchanged.

### Open
- Process memory grows in battle (MALLOC_SMALL, about 10 MB a second in headless runs; before these changes too):
  not traced yet. Play in sessions and restart the game until it is.

## 2026-11-22 — from nothing to the opening movies and the first battle's loading (Mac), and the iPhone / iPad app

### Inputs and extraction
- The player's base NSP (1.0.0, 11.4 GB) and 1.4.1 update NSP. The ExeFS came out with games/pla's
  `extract_exefs.py --update` from the update: main `126a76b9…`, subsdk0 `b515e170…`, sdk `71a49827…`, main.npdm
  `f7a6c8f4…`, the same hashes games/aoc pinned for 1.4.1 (`assets/expected-1.4.1.json`).
- The merged RomFS: `scripts/merge_nsp_romfs.py` (new) is Red Dead Redemption's card merge with the base Program NCA
  read from the base NSP and the patch NCA from the update NSP (games/pla's BKTR / AES-CTR-Ex readers and rdr's
  `CardBase` as they are). Checked: all six IVFC levels of the merged image (715,286 level-5 blocks), 1,102 files (as
  games/aoc counted), 11,719,235,972 bytes, SHA-256 `42192ad8…` (pinned); 11.31 GB from the base, 428 MB from the update.
- The add-on content: `scripts/extract_dlc.py` (new, as games/xc3's) reads the four add-on NSPs (01002B00111A3001-3004;
  the two files of 3002 are the same NCA and are read once), verifies each RomFS's IVFC levels and writes the files to
  `extracted/aoc/<index>` (pinned in `assets/expected-dlc.json`: 1 file, 4, 15, 186; 1.5 GB in all).

### Base and scaffold
- **Base: Red Dead Redemption's port** (`games/rdr`, Mario Kart 8 Deluxe's runtime with the load-order symbol binding,
  the offline console and SDK-15 answers), with Mario Kart's movie path for this game's three modules (main, subsdk0 =
  the SDK's movie library, sdk): the shared VP9 decoder `shared/runtime/movie_host.c` (Tears of the Kingdom's; this
  game's movies are 1280x720 VP9 .webm) and libvpx. `GAME_AOCX`; the shared files that name the ports
  (`offline_host.c`, `threads.c`) take it. The shared FPS panel comes with the graphics layer
  (`tests/test_hud_panel_shared.py` UNIFIED), `tests/test_active_players.py` its `mng_active_start(@"aocx")`.

### Rewriter
- `patch_nso.py` stopped on main: `jump table 13847dc`. A byte table whose entry is loaded into w30
  (`ldrb w30, [x14, x12]; add x18, x18, x30, lsl #2; mov w30, w13; br x18`): capstone names the register `lr`, which the
  index lookup did not read as x30. It does now (`fp` as x29 too). main (37,251 slot references), subsdk0, sdk (93
  gates) rewrote; `aocx-native rewritten/local probe` passes (4000 cross-module calls on 4 threads; 192,788 relocations).

### Host answers this game needed (in order of the stops)
1. **SDK 9.6.0's `nn::init::Start`** is the four-argument form (rdr's loader called SDK 15's five-argument one).
2. **`nn::oe::InitializeCopyrightFrameBuffer`** crashed in the SDK (sdk+0x299970): the screenshot copyright overlay. The
   host takes no system screenshots: `InitializeCopyrightFrameBuffer`, `SetCopyrightImage` (904x48 at 328,648),
   `SetCopyrightVisibility` accepted and never drawn. With them `nn::err::ShowError(ErrorCode)` (by value in x0).
3. **`nn::hid::GetNpadJoyHoldType`** reached the hid service (`sm: service "hid" not implemented`): it returns the hold
   type the game set.
4. **The movie decoder**: the shared host VP9 decoder (`[movie] configured 1280x720 VP9`), as Tears of the Kingdom's.

### Graphics
5. **`nvnCommandBufferSetCopyImageStride`** (the first unimplemented NVN call, before each movie frame's copy): games/totk's
   image stride (`p_copy_stride.image`, both copy directions).
6. **Draws without a program**: KTGL binds its vertex and fragment programs and then unbinds its compute program
   (`BindProgram(NULL, COMPUTE)`) before drawing; the graphics program was taken away and two full-screen passes a
   frame, and the movie's draw, were dropped (`no-program`). Now games/totk's rule: the graphics stages and the compute
   stage are bound apart (`S.prog` / `S.cprog`); a dispatch runs the compute program with the graphics one left bound.
7. **The movie was green**: the game copies each decoded frame (NV12) from its buffer into an R8 1280x720 and an RG8
   640x360 texture of a CPU pool with a GPU copy, and the pool bytes (never written) were uploaded over the copy at every
   use. Now games/totk's rule: a buffer-to-texture copy makes the texture the GPU's (`gpu_owned_mark`).
8. **c1 constant segments beyond 4 KB**: the first battle's shaders carry 4,352-byte c1 segments; the reflection's fixed
   4 KB array refused them and the host stopped. The segment is allocated now (games/totk's form, freed with the shader)
   and may be up to the 64 KiB the runtime declares (`tests/test_aocx.py` compiles `nvn_refl.m` and checks a 4,352-byte one).

### Test runs on the Mac (safeguards)
- The Mac (a MacBook Air) panicked three times while game hosts ran (WindowServer hung in the GPU driver). Every run
  since is headless (`TOTK_HEADLESS=1`: the picture goes to a 1920x1080 layer in no window, `runtime/frontend.m`; the
  screenshots and timing work as in a window), one game process at a time, `TOTK_GPU_KEEPALIVE=0` (no spin kernels),
  `TOTK_GPU_BUDGET_MB=3000`, a hard time limit, and a memory check before each start.

- **A headless run ends after the GPU**: runs that ended with the watchdog's SIGKILL in the middle of GPU work left the GPU
  at 100 % and about 6 GB of its memory held after the process was gone (ioreg IOAccelerator "In use system memory",
  "Device Utilization %"), which matched the hangs in the GPU driver. Now the input file's `close`, SIGTERM and SIGINT
  end a headless run after every submitted command buffer has completed (`mtl_quiesce`, at most 10 s;
  `runtime/frontend.m`); the test driver sends SIGTERM first and logs the GPU's memory before and after each run.
- **A GPU hang guard** (`nvn_mtl.m` `mtl_wait`): a wait for the GPU that sees no fence complete for `TOTK_GPU_HANG_S`
  seconds (default 10, 0 off) stops the game and lists the last 48 draws and dispatches encoded (their shader keys and
  sizes), instead of waiting forever.

### The first battle's GPU hang: found offline and fixed the Zelda way
- What hung: the hang guard's report (run h12; the last command buffer, fence 4356, never completed) lists the first
  battle's compute dispatches (dc81d690…, 7463cde4…, 72f8d532…, a8adc849…, 6356ec9c…, grid 1x1x1 and 11x1x1) and its
  first draws. dc81d690 counts its light lists in loops whose end is a value read from texture slot 0:
  `_2678 = uint(_2786) >= as_type<uint>(_2562)` with `_2562 = cp_t_tcb_8.read(uint2(int2(x, 0)), 0).x` (line 1990).
  The game binds a **buffer texture** there, through a combined handle (c[2] + 0x20). Odyssey's translator (the v4 chain
  that games/mk8dx and games/rdr use, built 23 October) had no slot for a combined handle ("贴图句柄 8 … 无 PC 类型，按 2D"):
  it read the buffer as a 2D texture and recorded the slot as any type ("*"), so that translation was used with the buffer
  bound. On Metal that read is undefined; the counts were garbage (as uint, about a billion iterations) and the GPU never
  finished. Tears of the Kingdom's translator fixed exactly this on 3 November (a73d65e2: Pokopia's histogram, a combined
  handle bound to a buffer texture).
- Fix: this port translates with Tears of the Kingdom's current translator (`prebuilt/macos-arm64/totk_xlate` links
  `games/totk/prebuilt/macos-arm64/totk_xlate`; same SHA-256), and its chain is new (`aocx-native-v5-combined-handles`), so
  no translation of the old one is reused. Checked offline with the translator alone: dc81d690 with the bound type
  "buffer" now declares `texture_buffer<float> cp_t_tcb_8` and reads `.read(uint(x))`; without an answer the slot is
  recorded as "2d", not "*", so a 2D translation is never used for a bound buffer.
- Host guard (`nvn_mtl.m`): a buffer texture bound to a slot the shader declares as another type gets the dummy (zeros)
  instead (reads 0: the loops end), with a red line.
- Safety net for every port (shared): `shared/third_party/d2r_gfx/gfx/nvn/msl_loop_cap.h` caps every loop of a translated
  shader at 65,536 iterations (a counter before the loop, `if (++c > 65536u) break;` first in its body); this host applies
  it to every MSL it compiles (`totk_msl_library`). `games/totk/ios/tools/msl_loop_cap.py`, the same rewrite byte for byte,
  caps every port's device seed in `build_port_ipa.py` before `compile_seed.py`; this port's seed tool caps its seed too.
  `tests/test_msl_loop_cap.py` checks the rewrite, that both give the same text, the shared build and which hosts call it
  (aocx; acnh, kirby, mk8dx, odyssey, p5r and rdr listed as pending, each needing its own build check).
- Checked on the Mac (run h13, headless, 120 s, after the fix): the battle's load went past the point where h12 hung,
  every command buffer completed, and the GPU stayed at 3 % / ~0.4 GB before and after the run. The run stopped at the
  host's GPU memory budget (`TOTK_GPU_BUDGET_MB=3000`: 3,006 MB allocated during the load), not at a hang.

### The shader seed (`scripts/prepare_aocx_shader_seed.py`, `scripts/ktgl_shaders.py`) — first made with the old translator
- `ktgl_shaders.py` (new) reads KTGL's GLSLC outputs: the IDRK entries of the RDB data files (zlib in chunks) whose data
  starts with `_S2G` or `_DOK`, each output by its section headers, keyed as the runtime keys it. 40,727 shaders
  (3,441 vertex, 37,045 fragment, 241 compute): 40,612 in 28,170 containers of `asset/system.rdb.bin`, `.bin1` and
  `.bin2`, 115 more in main; 17 s on the M4 (games/aoc's build had 40,729).
- `prepare_aocx_shader_seed.py` runs Odyssey's copy of the Zelda line's seed tool as Red Dead Redemption's does: the
  default bind state of every shader and the 25 vertex layouts the Mac met for every vertex shader, NVN's 0..1 depth (the
  host logs depth mode near = 0): 82,675 variants (30,837 vertex), 2 skipped, 1,346 s, 10 GB of MSL (KTGL's fragment
  shaders average 120 KB). The Mac host imported it (206,076 files) at its next start.
- On the Mac the start then warms every seeded shader (82,915 rows of the seed's list): `DM_PSO_MANIFEST=off` for test
  runs; shaders load at first use.

### Where it stands (Mac, screenshots through `TOTK_INPUT_FILE` `shot`)
- The first-start settings (difficulty, voice and text language), "Begin with these settings?", the autosave notice,
  the opening movie with its sound, the following movies (+ skips them), 30 fps (frame time p50 33.3 ms), GPU ~2.5 ms.
- **The first battle** (run h14, headless, `TOTK_GPU_BUDGET_MB=4500`, 120 s): Link before Hyrule Castle with the
  soldiers, the enemy army ahead, the HUD (hearts, Link's portrait) and the minimap
  (`<work>/evidence/5-first-battle-link-hud-minimap.png`); frame time p50 33.4 ms (the game's 30 fps), GPU p50
  1.7–2.4 ms; the run ended at SIGTERM with the GPU idle (`headless close`) and the GPU at 0–4 % / 0.2–0.35 GB for the
  60 s after it. The settings are asked again at every start (the game saves them later, after the prologue).
