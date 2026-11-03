# Changelog

## 3 November 2026: outbreaks and Tera raids reset every few seconds

- **Fixed: the game's clock check converted a calendar time through the SDK with the port's own time-zone rule.** The game calls `nn::time::ToPosixTime(int*, PosixTime*, int, const CalendarTime&, const TimeZoneRule&)` about five times a second, after each read of the user clock. The port's `LoadTimeZoneRule` fills a small host record (zone name), so the SDK's version read garbage. The game then took the clock as changed: mass outbreaks dispersed and appeared again every few seconds, Tera raid spots moved, and the sky ran from midday to sunset in about 80 s. `runtime/time_host.c` now answers it from the host time zone: 0 results in a DST gap, 2 in an overlap. Measured from the same save: 90 s without a single outbreak notice, the sky stays at midday. Before, a new notice came every 10 s, at 30 and 60 fps alike.
- **Local wireless no longer stops the game.** Two minutes into a session Violet opened `ldn:u` (local play) and the port stopped with "IPC service not implemented". As in the Smash port, `nn::ldn::Initialize` now answers that wireless communication is off (2203-0022), so the service is never opened.

## 3 November 2026: ride jump and animation distance

- **Ride jumps at 60 fps are higher, but still short.** A Lua script sets the ride's speed through the binding at `main+0x19776c0`: 8.51 upward at the jump, then 0.633 less per script step (gravity 19 per second times a 1/30 s step). The script steps in whole 1/30 s of the delta it reads through the binding at `main+0xb926f0`, the site of the fixed step. With the fixed step it steps every 60 fps frame, so the jump curve plays twice as fast: half the height, half the air time. As in the mod, the stored speed's upward part is now scaled by 1.95 in the 60 fps mode (`main+0xcce37c`, `PSV_JUMP_SCALE=0` turns it off). Measured on Miraidon in the plaza with the step pinned: landing after about 0.8 s, 0.7 s without the scale, 1.2 s at 30 fps. By the logged speeds the height now matches 30 fps (2.0 against 2.05 units), the air time does not.
- **The fixed step runs all scripted fixed-step logic twice as fast at 60 fps.** That is the price of smooth rooms, and the jump scale only covers the jump. The exact fix is a 1/60 s script step in the 60 fps mode. The binding passes the plain frame delta, so the 1/30 s is in the Lua scripts.
- **Animation distance limit 100000.** At `main+0x20a8d3c` a command read from game data sets a flag when its first value is above 50000. The mod raises the limit to 100000, and its notes link this to the windmills' animation distance. While riding in the plaza the check ran 4 times in 40 s, each time with 110, so no effect was visible there. `PSV_FAR_DISTANCE=0` keeps 50000.

## 2 November 2026: jerky walking in rooms and fast menus fixed

- **Rooms are smooth at 60 fps.** At `main+0xb92784` a system reads clock 2's frame delta and advances in whole 1/30 s steps. In the 60 fps mode it stepped every second frame, and the room camera follows it. It now gets 1/30 s every frame in that mode (two small stubs in the free end of the text page, `runtime/game_patches.c`). Measured in Clavell's office with the step pinned: the camera moves 15 px every frame, before 0 and 29 px alternating. `PSV_FIXED_STEP=0` turns it off. The game's own delta stays in state 2 of the manager at `main+0x29364c8` (seen 0 indoors, outdoors and in battle).
- **Interface animations play at their speed at 60 fps.** The second time-scale reader (`main+0xd96428`) sets the step of `nn::ui2d::Layout`, the 2D interface player, whose animations are authored in 30 fps frames. It got scale 1 per 60 fps frame, so menus and the Pokédex registration book ran twice as fast. It now has its own table with 1/30 s in the 60 fps mode (scale 0.5). `PSV_UI_SCALE=0` turns it off.
- **Credit:** the patch sites and values come from the community "Dynamic FPS Violet v4.0.0" mod by Fl4sh9174 (GameBanana mods/506559), which patches the same two sites for emulators. The code here is written for this port, and the mechanism was measured in the port. Its other sites were read but not taken over: present interval 1 (the runtime already sets it), a float 1.0 to 60.0 at `main+0x170c408` and a return value at `main+0x2b7c540`. Its jump scale and distance limit followed on 3 November.

## 2 November 2026: jerky walking in rooms (hunt)

- **In rooms the camera moved every second frame at 60 fps.** With the frame step pinned (`framestep 16.667`) the background in Clavell's office shifted 26 to 29 px, then 0, alternating. Pinned at 25, 20 and 12.5 ms it moved on 3 of 4, 3 of 5 and about 3 of 8 frames: an accumulator of fixed 1/30 s steps of game time. Overworld cameras move every frame. On a real Switch 2 (4.0.0) walking in the office is smooth.
- **No hardware switch to flip.** Violet 4.0.0 is built with SDK 13.3.13, older than the Switch 2, and imports nothing that tells the console apart. The frame mode byte (application settings, `refreshRate`, default block `main+0x146fde4`) is only written by the built-in 30 fps default: the settings file `config/trinity_config/data.trowc` is not in the RomFS. The runtime's 60 fps mode is the port's own construction. Tried without effect: FPS60 in the settings from the start, `enableVFR`, the Switch 2 tick rate of 31.25 MHz, the NVN device properties, the performance mode.
- The three time-scale readers: `main+0xb83d1c` divides by the object's own index (effects such as the Tera shine), `main+0xd96428` by the global index (the interface, fixed above), `main+0x27b49b8` is a frame-time statistic.
- Debugging notes: lldb must ignore EXC_BAD_ACCESS (`settings set platform.plugin.darwin.ignored-exceptions EXC_BAD_ACCESS`, the dirty tracker's faults) and the game must run with `PSV_RENDER_STREAM=0` (else the encode watchdog aborts after a 2 s stop). Killing an attached lldb kills the game: stop the game with SIGSTOP to let a waiting script detach. `PSV_STOP_AT_START=1` stops the process before the game's first instruction for lldb. `PSV_FRAME_MODE=game` logs the game's own mode.
- Diagnostics: `findq` / `poke8`, `findd` / `poke4` and `findf <x> <y> <z> [<tolerance>]` search and set memory from the live input file. `PSV_MFX_CAMLOG=1` logs the camera position every frame (frame interpolation on).

## 2 November 2026: frame interpolation

- **MetalFX frame interpolation works in Violet:** 60 game frames plus 60 interpolated frames on a 120 Hz screen. Settings panel row "Frame interpolation" (`interp` in settings.txt, `PSV_MFX` decides first). A 15-minute player session: 97% of game frames interpolated, a picture on 97% of the 120 Hz ticks, MetalFX 3.3 ms GPU per frame, about 5 ms more delay. The player found it good to play.
- Violet's hooks (`nvn_obj.h`, `psv_mfx.m`): the canvas is the 8-bit sRGB texture the game tone-maps into and draws the HUD over; the UI begins at the first canvas draw that is not an unblended full-screen triangle; frames without UI copy the canvas as the scene; the camera comes from the view-projection matrix in vertex uniform 2 of the G-buffer pass (reversed depth, 0..1); the depth has stencil; the output is plain RGBA8 because the game's last pass stores the decoded canvas (sRGB output made interpolated frames too light).
- **Fixed: the hiding vignette (press B in tall grass) flickered at 120 Hz.** It is drawn with eight targets bound and only the first used, so the UI layer (attachment 7) was not added and the vignette was missing from every interpolated frame. The layer is now added when the game's attachment 7 is empty.
- Diagnostics: `ishot <path>` in the live input file saves the next interpolated frame; key 0 saves it as `glitch-<time>-interp.png` next to the game frame.

## 1 November 2026: characters exploding for one frame

- **Fixed: people and objects that appear (pop-in) were drawn for one frame as screen-wide triangles**, most often in the crowded Mesagoza plaza. The game skins characters in two transform-feedback draws in one pass, the second reading the first's output as vertices. The layer set the vertex-to-vertex barrier only after other vertex-stage stores, not after feedback draws, so the second read the previous frame's output: for a character already on screen last frame's pose, for one that just appeared whatever the memory held. Two plaza walks with `PSV_ANOMALY`: 0 one-frame anomalies (before: 3 to 10 per walk). Frame times unchanged. `PSV_TF_BARRIER=0` = old behaviour.
- Found with the anomaly capture (`PSV_ANOMALY=<dir>`): every exploding draw was new in its frame and fine the next. Ruled out on the way: the lookup caches, the LOD mod, the game overwriting data in flight (`DM_EXP_SUBMIT_WAIT=1`), uniform snapshots, the storage-buffer guard (made it a little rarer, not gone), dropped feedback output, Pause/Resume offsets (the game only uses Begin/End). The anomaly records now also list vertex streams 0-3 and a capturing draw's feedback buffer 0, and a feedback buffer outside every memory pool is logged.
- Still open: the plaza's white and blue look comes from the lighting step (surface colours are right before it; colour grading and exposure inputs match the grassland).

## 1 November 2026: glitch key and Apple's GPU tools

- **Key 0 marks a glitch while playing:** the game frame as `logs/glitch-<time>.png`, a `[glitch]` line in the log and, when the game started with `MTL_CAPTURE_ENABLED=1`, a Metal capture of that frame. Capture mode costs nothing measurable (grassland p50 16.66 ms). A capture is about 2.7 GB and takes 2 to 6 s.
- **macOS 27's `gpucapture` and `gpudebug` work on this game, but replay does not.** `gpudebug` lists a frame's 13 command buffers, ~2,300 draws, pipelines and 15,700 resources. Apple's GPUToolsReplayService crashes on our captures (null pointer, offset 8): once loading an in-game capture (`AddResourceRestoreRequest`), once fetching a render target from a `gpucapture` capture made with `--shared-mem-opt disabled` (`debugSubCommand`). So no textures, render targets or pixel history yet. Note: `gpudebug` needs to run outside a sandbox (XPC).
- **`make boot` keeps each session's log** as `logs/play-<date>.log`. A test run had overwritten the log of an 18-minute session with texture glitches.
- Texture glitches reported in that session (when a caught Pokemon goes into the ball, entering the Academy grounds): not reproduced yet. Two scripted battles with `DM_NVN_DIRTYCHECK=1` and both cache checks on found no missed texture change and no cache mismatch (8 fps under the full check, so the catch itself was not covered). Seen once each, not reproduced: a hang at the Game Freak logo, and "The wild Naclstack used Rock Slide!" standing for 12 s with no rocks.

## 1 November 2026: the build

- **The app said macOS 26, the binaries need 27.** Both were built for the SDK's macOS (27.0) while the Info.plist said 26.0. `MACOS_MIN` (27.0) now sets the compiler, the linker and the plist. Kept at 27 on purpose: the Homebrew opus and libvpx are built for the macOS they were installed on.
- **Every step is a make target.** `make doctor` (what is missing and which target makes it), `venv`, `extract`, `rewrite`, `verify`, `test`. The rewrite runs again when a module or the rewriter changes. Run again into a scratch folder, it gave byte-identical modules. `make verify` reported 0 failures and 0 divergent sites on all three.
- The default Python is the work folder's venv, or `python3.14`. Plain `python3` was 3.15 on the test Mac, without the packages.
- `make boot` builds the shader tools first. Homebrew paths come from `brew --prefix`. structck's llvm is `llvm@23`, and its cache now goes to the work folder (it went to `~/dm-work`).
- **The runtime builds per file** (`make -j`): an edit to one header rebuilds only its users, with a dSYM as before. Full build 7 s on an M3 Pro. The new binary boots the test profile into the world (frame 3,000, no GPU error).
- `unicorn` moved to `requirements-dev.txt`: only `make verify` needs it.

## 1 November 2026: the stutter stretches

- **The slow frames wait, they do not work.** A 10-minute player session had two stretches of a minute or more with every tenth frame at about 29 ms. The `slow frames` lines show those frames spending 22 ms in AcquireTexture (11 ms in the others) with the same CPU time (5 to 6 ms): pacing, not load. Presents then come 16.7 and 29.2 ms apart (29.2 is 3.5 refreshes of the 120 Hz screen).
- The `slow frames` line now gives the display link's actual rate. When a stretch is pacing (slow frames wait 6 ms more, most frames normal) and the link runs below 97% of the screen's rate, the runtime sets the link's frame-rate range again (`[present] display link: frame-rate range set again`), at most every 30 s. Unproven: whether the link is slow in those stretches. `PSV_LINK_NUDGE=0` only logs. The live input file takes `flag link_nudge 1`.
- Not the cause: the frame-lock switch that ended a stretch earlier does not change the link's range (checked: 120 Hz before and after).

## 1 November 2026: CPU profile

- **The game thread spends 3.5 ms per frame in the NVN replay, down from 7.0 ms** (grassland, test profile, same draws). At a 120 fps lock the grassland now holds 120 fps (was about 85, CPU-bound). Found with Time Profiler (`test-tools/prof.sh`, `profsum.py`).
- **53 static textures were uploaded again on every use (30 MB a frame).** NVN pools are 4 KiB aligned, Mac pages 16 KiB, so neighbouring pools share edge pages. The dirty-page tracker saw that as an overlap and left the second pool untracked, which counts as always written. A new pool now gets the pages after (and before) its neighbours, and a texture that crosses into a neighbour's edge page is checked across both ranges (`nvn_dirty_tracked`, `nvn_dirty_since`, `nvn_dirty_protect`, the GPU write bracket). Re-uploads in a 20 s run: 195,688 before, 21 after. With `DM_NVN_DIRTYCHECK=1` (full hash of every clean texture) a 2-minute walk found no missed change.
- **The tracker's range lookup is a binary search.** A lookup that missed its recent-hit cache scanned all ~4,000 live pools, and an address no pool covers always did (8.7% of the game thread). Live ranges are kept sorted by base under the slot lock with a sequence count. A reader that sees the list changing falls back to the scan (the fault handler never spins).
- `skip_kind` (the `DM_SKIP_KIND` diagnostic) formatted a string on every draw while off: 3.9% of the game thread.
- Diagnostics: `DM_REUP_LOG=<n>` logs the first n texture re-uploads with the reason a texture is untracked. The log names each module's base (`[psv] mapped main at ...`).
- **Three lookup caches are on by default** (they were experiments, off): `PSV_POOL_QUERY_CACHE` (pool lookups by address), `PSV_USNAP_INDEX` (uniform copies by address) and `PSV_ARG_BIND_CACHE` (no repeated argument-table address within an encoder). In one session switched every 20 s: 3.96 ms of replay per frame off, 3.82 on. A 2-minute walk with `flag pool_query_check 1` and `flag usnap_index_check 1` compared 72 M and 28 M lookups with the uncached ones: no mismatch. The grassland draws the same. `=0` turns each off.
- **The uniform copy helpers arm their fault guard once per wakeup.** `sigsetjmp` saves the signal mask and the alternate stack state (two system calls). Per copied range it was 2.3% of all CPU. A fault still drops only the range being copied.
- `PSV_DIAG_ENV_ONCE` is on by default and `PSV_CHAIN_LOG` is read once: a `getenv` per BindProgram and per EndRecording.
- **All threads together: 23% less CPU** in the same 20 s of the grassland (Time Profiler samples 32,791 before the first fix, 25,150 after the last). Game-thread replay 7.0 ms to 3.5 ms a frame.
- Not changed: the encode thread sits in a low-power wait (`wfe`) between packets, which shows as 70% of its samples in `nvn_encode_fifo_peek`. It is not CPU work. The IOKit submits (10% of all CPU) run on Metal's own submission thread: about ten command buffers a frame (the first parts start the GPU early) and the GPU keep-alive.

## 1 November 2026: LOD mod

- **Trinity Mod Loader mods load.** The game finds a file by the FNV-1a hash of its path in the sorted hash list of `arc/data.trpfd` and reads the loose RomFS file when the hash is missing. `scripts/make_mod_manifest.py` now moves the hash of every added file the index lists by one (the list stays sorted and keeps its size) and puts that index in the mod's manifest. Checked with Team Seri's "Improved Dynamic LODs" for 4.0.0 (`common_resident_0/1.trscn`, the `trinity_ModelLodSetting` of terrain, Pokémon and characters): the game read the loose file, and GPU time rose from 6.4 to 6.9 ms per frame in the grassland.
- `make boot` passes the app's mods folder (`PSV_ROMFS_MODS`), so a mod set up with `make mods` loads without the app.
- Looked up for the Switch 2 update: Digital Foundry found draw distance, pop-in and shadow resolution mostly unchanged, so a "Switch 2 mode" would not bring them.

## 31 October 2026: stutter and a hang in a play session

- **A player's 12-minute session stuttered and then hung.** For minutes at a time every other frame took about 29 ms (GPU 6 ms), four times; a switch to 30 fps and back ended one. After 12 minutes the picture stopped and the log said nothing more. Two 5- and 8-minute walking runs (one from a copy of the player's save and settings) stayed at a steady 60 and did not reproduce either. Cause unknown.
- **A stalled picture now leaves a thread dump.** No new frame for 5 s while the window is visible prints every guest thread and the graphics wait points, once per stall (`PSV_STALL_DUMP`). Checked with a forced 8 s stall.
- **Slow stretches now say where the time goes.** Every 300 frames with 30 or more frames over 20 ms, the log gets a `slow frames` line: the time in AcquireTexture, the present thread's CPU, the NVN time, and the number of 2-tick clock steps. The `DM_NVN_TSV` trace has the clock step per frame (`@step_ticks`, `@step_dt_ms`).

## 31 October 2026: the grassland GPU faults

- **Found: 2D image atomics wrote past a 4-byte dummy.** Fragment shader `318346b1` does `atomic_fetch_max` on an r32ui image. SPIRV-Cross emulates 2D image atomics through a buffer indexed `aligned width * y + x`; the replay bound a 4-byte dummy buffer for it, so nearly every atomic wrote past it into whatever GPU memory followed. In the western grassland every run faulted within 20 s of loading (Address Fault or GPU Hang; once the whole Mac). Bisected with `DM_SKIP_KIND` and `DM_SKIP_FS`: skipping only this shader's draws made the runs clean.
- **The fix:** the image's own buffer when its texture is made on one, else a scratch buffer of the image's size; and the MSL rewrite (`nvn_sb_guard.h`, `img_atomic_guard_msl`) sends atomics at coordinates outside the image to a spare slot after the last row, as NVN drops them. Three grassland runs: 9,000 frames each, clean (before: every run faulted at frame 900 to 1,500).
- **Storage buffers are bounded like NVN's.** NVN checks every storage-buffer access against the size bound with it (a load past it reads 0, a store is dropped); the translation lost those checks. `nvn_sb_guard.h` rewrites each storage access of the translated MSL at load (stores guarded, loads 0 past the end, atomics to a scratch word) and adds an argument with the bytes bound per storage argument (`NVN_RB_SBSZ`, filled by `encode_args`). All 348 storage shaders in the cache rewrite and compile. `PSV_SB_GUARD=0` turns it off.
- MetalFX's Metal 4 spatial scaler reads an internal 4-byte buffer that Metal shader validation reports as not resident, 1,169 times in one run. MetalFX has no API for it; the faults stayed with MetalFX off, so it was not their cause. Open.
- **Effects ran slow and in steps at 60 fps** (the Tera shine, the grass in the air, Psystrike; normal at 30, seen by the player in a live switch). Three systems divide the clock by `table[index]` with the index an object got when it was made: effects made in the 30 fps mode keep 33.3 ms as their frame. Those three readers (`PSV_FRAME_SCALE_SITES`) now read a table that holds the current mode's frame in every entry (16.7 ms in the 60 fps mode). Checked by the player together with the tick fix below. The live input file takes `fpslock <n>` to switch modes in one session.
- **Effects at 60 fps are right now** (checked by the player in the grassland: the Tera shine, the grass in the air, Psystrike). Part of the game counts whole 60 Hz ticks of the clock step, so a measured step just under 16.67 ms gave 0 ticks: effects jerked at 60 and stopped at 120. In the 60 fps mode up to a 60 fps lock the clocks now step by whole ticks with the remainder carried, at least one per frame. **A step of 0 stops the game** (tried for 120: at its second frame, and again in the world), so above 60 fps the measured step stays and effects there still run slow. `PSV_FRAME_QUANT=0` turns the ticks off.
- **The frame lock offers 30, 60, 90 and 120.** Off, 40 and 72 are gone (a saved one becomes 60, as does no saved value). 90 and 120 carry a warning in the panel and the log: effects run slow there and the game can hang (the player saw the game hang after turning to the Tera Naclstack at 120).
- Diagnostics added for the hunt: `DM_SKIP_KIND=<dispatch|tf|vswrite|copy|draw|fswrite|fswritelog>`, `DM_SKIP_FS=<keys>`, `DM_POOL_CHECK=1`, `DM_POOL_WRITERS=<lo>:<hi>`, `PSV_build/test-tools/val.sh` runs the grassland under Metal shader validation.

## 31 October 2026: the game's own 60 fps mode, night run

- **The game's own 60 fps mode is on with a frame lock of 40 or more.** 4.0.0 is the Switch 2 update, which runs at 60 fps. Two values select it: the frame function's step byte `[[main+0x4741210]+0x30]+0x20` (1 = 30 fps, 0 = 60 fps; the game then asks for present interval 1) and the time-scale index `[main+0x47350b8]+0xd0`, by which the battle, catch and Pokédex systems divide their clock. `runtime/game_patches.c` keeps both at 0 with a lock of 40 or more and at 1 otherwise. With the measured clock step the time scales are 1 at 60 fps (as on the Switch 2) and 0.5 at 120. `PSV_FRAME_MODE=game` leaves them to the game. Measured: 1.5 s of walking ends at the same spot; Psystrike plays its whole animation, the HP bar, the faint and the experience screen in about 5 s.
- **A hang in the game's job system** (main thread and job workers all waiting in `main+0xb47d18`, nobody signalling): seen at 60 and 120 fps, in both frame modes, about once per 5 to 15 minutes of walking in the grassland. A CPU crash in the same job code (`main+0xd7218c` via `main+0xb47e24`) once. Under investigation: the lock and condition SVCs now keep a ring of their last 65,536 calls, printed per waiting key by the `threads` command.
- The zero-copy pool refusal at boot is temporary: the span is one readable, writable VM region. A refused pool now waits for the GPU and tries again (up to 20 times); the VM regions are logged if it still fails.
- `nvn_dirty_remove_range` finds the range its own pool registered. A pool that overlapped a live range was never tracked, and its finalize ended the other pool's tracking (that pool's textures were not seen as written again).
- Removed: the per-frame stack walk for the earlier port's system object (`find_game_sys`, `game_work_tick`, the `framewait` flag) and the world-survey stub. Kept on purpose: the earlier port's shader-key handlers (bloom 1/4, distortion fix, bright LUT, MetalFX dock); their keys never match here and they are woven into live paths.
- The GPU write-back of renders to guest memory never runs in Violet (0 times in 9,000 frames with battles and in the grassland): not a cause of the overwritten game objects.
- Grassland GPU faults after the type-view fix: 1 in 6 A/B runs (one with the keep-alive off).
- **The hang is a lost job, not a lost wake-up.** The ring of lock and condition SVCs (`threads` command, global order `#n`) shows the main thread waking both job threads, the job threads finding nothing and sleeping again, and nobody signalling completion. A watchdog that wakes every condition waiter after 1.5 s did not end it (10 kicks, still hung); it is opt-in (`PSV_CV_WATCHDOG=1`).
- **Game objects next to small pools get overwritten.** Two crash objects lay in the 16 KiB rounding of a pool's zero-copy buffer, past the pool end: an array entry 0x1d48 past pool 0x90457f3000+0x6000 (crash at `main+0xd7218c`) and an object 0x1f20 past pool 0x904cf70000+0x2000. 2,645 of 3,519 pools have such a tail (21 MB of game heap the GPU can reach). No write binding runs past its pool, so the suspect is a shader writing past its bound size. `DM_POOL_CHECK=1` checks every GPU write path into guest memory (storage, transform feedback, buffer copies, uniform updates, counters); `DM_POOL_WRITERS=<lo>:<hi>` logs writes to a range. Probably the image atomics past the 4-byte dummy (see the grassland entry above): they wrote into whatever GPU memory followed, which includes the pools' zero-copy buffers.
- **A test that hangs the GPU can take the Mac down:** at 08:54 a watchdog panic (AGX / IOGPU in the backtrace) followed a test session that ended with the game still hung.
- Diagnostics: the live input file takes `peek`, `poke1` and `find`; `PSV_POOL_DEL_WAIT=1` waits for the GPU before a pool goes; `PSV_build/test-tools/ab.sh` and `soak.sh` run the grassland checks with the GPU-error guard.

## 31 October 2026: own names

- Every name taken from the earlier ports now carries this game's prefix, with no change in behaviour: environment variables `PSV_*` (was `TOTK_*`), log tags `[psv…]`, identifiers and files `psv_*`, the translator `tools/psv_xlate` / `prebuilt/macos-arm64/psv_xlate` (same binary, same hash). Comments say "the earlier port"; `third_party/COPIES.md` is the one place that names the sources.
- The dead `GAME_BOTW` branches and their empty macros are removed.
- **The shader cache identity keeps its old text on purpose** (`DMSH_CHAIN` in `dmsh_chain.h`): every cached shader stores it, so a new text would retranslate all of them.
- The RomFS index magic is `PSVRFS01`. The existing index was updated in place (and its hash in `romfs_config.h`); the first boot verified the RomFS once again.

## 30 October 2026: GPU address faults

- **Type views were never resident.** A texture view made by `tex_type_view` (a 2D texture sampled as a 2D array or the reverse) is its own object for Metal 4 residency; the parent's residency does not cover it. Metal shader validation in the western grassland reported 654 "Texture is not resident", every one such a view. They now enter the residency set when made and leave it with `defer_obj_release` once the GPU is done. Validation then reported none.
- **GPU faults still happen there** (2 of 3 runs after the fix), and the game twice crashed on garbage pointers in game objects next to GPU memory pools (`main+0x7dc414`, `main+0xd7cb94`), once right after a GPU hang. Not solved.
- Transform feedback stops at the end of its buffer, as the hardware does: a capturing draw is cut to the vertices that still fit. Never needed in the runs so far, kept because the capturing shader stores without a bound.
- The zero-copy boot stop (a refused 0xb0000 pool) also came with every tracked page unprotected first: protection is not its cause.
- Diagnostics: `DM_POOL_LOG=1` logs every pool and its buffer span, `DM_TYPE_VIEW_IDLE=off` keeps unasked type views, `PSV_build/test-tools/run2.sh` boots the test profile with a guard that kills the game at the first GPU error (a fault had hung the Mac once).

## 30 October 2026: game speed above 30 fps

- Animations and walking ran twice as fast at a 60 fps lock and three times at 90. The frame function (`main+0x2b79f60`) advances the game clocks by one fixed entry of a frame-duration table each frame, 33.3 ms in its 30 fps mode, whatever the frame took. The earlier "cutscenes follow real time" check missed this.
- `PSV_FRAME_STEP=real` patches the two clock-advance functions (`PSV_FRAME_ADVANCE_SITES`, `runtime/game_version.h`) to read a table of ours in the page after main, filled with the measured present interval (average of the last 10 frames: the earlier port uses 5). 1.5 s of walking then ends at the same spot at 30, 60, 90 and 120 fps.
- The game's own table stays on purpose. Three other sites divide the clock by it to get a time scale (battle animations, catching, the Pokédex). The first version redirected the whole table, which kept that scale at 1: battles ran fast and dropped their animations.
- On by default (`PSV_FRAME_STEP=game` turns it off). Measured at 60 fps from the player's save: 2 of 3 runs clean with it (one GPU fault), 1 of 1 without.
- **GPU address faults are a separate problem:** at one spot in the western grassland (951, -3155) every run faulted within two minutes, with or without the frame step, at 60 and 90 fps, also with type-view eviction off (`DM_TYPE_VIEW_IDLE=off`). One such fault at 120 fps hung the GPU, WindowServer and macOS (watchdog panic). Not solved.
- `nvn_dirty_each_tracked` jumps to the next tracked range instead of stepping page by page (a probe with many pools took minutes).

## 30 October 2026: settings panel

- Key 1 opens a settings panel (`runtime/settings_panel.h`, after `games/wonder`): frame lock, MetalFX output upscaling, overlay. Picks are kept in `settings.txt` in the profile folder.
- Frame lock in the present pacing (`hle_nvn.c`, copied from another port in this repository, see `third_party/COPIES.md`). The game asks for present interval 2, which gave 60 fps on a 120 Hz display only by accident of the display rate. The lock holds an exact rate by display ticks, or by its own clock where the rate does not divide the display's.
- Measured before choosing the rates: a cutscene takes the same 4 s at 30 and at 60 fps, so the game does not run fast above 30.
- `PSV_IGNORE_GAMEPAD=1` for test runs: a connected controller had moved the player during a measurement.
- MetalFX upscales the finished 1920x1080 picture to the window (2954x1654 here, 0.7 ms GPU per frame). the earlier port's docked and scene stages key on the earlier port shaders and stay idle.

## 30 October 2026: stutter that grew with play time

- Every 64th frame took longer the longer the game ran: 25 ms after one minute, 137 ms after 17 minutes in the box screen. The sweep of cached texture-type views walked every entry, and the game gets a new sampling view of a re-uploaded texture about 80 times a frame. Each new view became a new entry of a texture that stays resident, so none ever left.
- Fix copied from `games/odyssey` (546123b): a type view no draw asked for in 64 submissions leaves the cache. In a 21-minute box tour the cache stayed at 353 views, the sweep at 0.25 ms, memory flat at 3.5 GB.
- Box tour with every species and form (957) in a test save: all models render, no new shaders (Pokémon share one shader set), 109 new pipelines.
- A boot stopped at 9 s: the driver refused the zero-copy buffer of a 0xb0000 memory pool that lay only partly in a dirty-tracked range. The retry after unprotecting covered pools lying wholly in tracked ranges. It now unprotects each tracked part (`nvn_dirty_each_tracked`). **Unproven in the failing case**: it happened once and has not come back since.
- Move effects are where the shaders are: battles with one wild Lechonk and level-1 Ghost-type Pokémon (Normal moves do not touch them), Heal Pulse keeping the wild Pokémon alive, 18 moves per boot. The first 46 moves gave about 1,200 new shaders and 900 pipelines. Stopped after 160 of 656 moves (9,135 shaders, 6,299 pipelines).
- The overworld gives few: 60 land points across Paldea plus Levincia and Mesagoza at street level gave about 900 shaders and 1,400 pipelines. Most came from the wild battles on the way (one point with a battle and a level-up: +279). Cities gave 13 to 27, the rest are left to runtime on purpose.
- A test save moves the player only when all three position blocks are written (`0x50540575`, `0x5C6F8291`, `0x708D1511`). A point in water, inside a cliff or on the Academy grounds sends the player home.

## 29 October 2026: first boot to the house

Copied from `games/xc3` at f1d0077 and brought up on 4.0.0. Each entry is what stopped the game, why, and the fix.

### Inputs and code
- 4.0.0 pinned from the player's own NSPs: ExeFS, RomFS (9.9 GB in 30 files, mostly `arc/data.trpfs`), control data, rewrite hashes.
- `main` holds a static 1 MiB arena whose end pointer lies 0x18ab0 past its bss. `prepare_runtime.py` and `loader.c` accept relative relocations up to 1 MiB past a module and list them.

### Services
- Wwise checks that its base path `data:/audio/wwise/bin/Switch/` is a folder. The RomFS path check rejected the trailing `/`, the sound engine stopped and a later `Create` crashed. `GetEntryType` now accepts one trailing `/` for folders.
- HID calls the runtime did not answer went to the missing `hid` service: style-set event (signalled once), supported styles, communication mode, joy assignment, player LEDs, controller colours, strap guide.
- `RequestSharedFontLoad`, then the fonts themselves. In shared memory only the 8-byte header is obfuscated. With the obfuscated payload `LoadFont` failed, the `?` glyph was missing and a glyph lookup crashed. Decoded as in `games/eow`.
- The add-on content entry calls `nn::ec::ShowShopAddOnContentList` with only the user. The override copied from XC3 covered the overload with an application id, so the SDK started the eShop applet, read a null self controller and crashed. That overload now closes at once too.
- Offline answers: BCAT (as `games/acnh`), `GenerateCryptographicallyRandomBytes` (host entropy), Bluetooth LE (no accessory).
- RomFS and save reads into game memory failed with EFAULT now and then: the GPU layer's dirty tracker can write-protect a page of the buffer again after it was prepared, and a system call does not take the fault handler. Such a read now goes through host memory and a copy. EINTR is retried.
- Sound: Wwise plays through AudioOut, which the renderer's "output level" line does not see. `PSV_AUDIOOUT_LEVEL=1` logs that path (peaks around 6000 in the house).

### NVN on Metal
- Queue priority, and builder and device getters that `nn::gfx` reads back.
- Multisample coverage state is stored for its getters.
- A shader with SPIRV-Cross's optional `spvLinearTextureAlignmentOverride` is specialised before use (Metal refused the pipeline).
- Vertex shaders that write the layer themselves get the triangle topology class, and their point size is kept in a local.
- `c0+0x640` (base vertex) stays 0 on purpose: Metal's `vertex_id` already counts it. Filling it caused GPU address faults.
- Scaled texture copies (1920x1080 to 480x270, colour and depth) are drawn as a small render pass.
- A pool made over write-protected pages of two tracked pools: the check accepts the span, and the buffer is made after unprotecting when the driver refuses it.
- **The magenta picture:** the colour-grading table is a 32x32x32 3D texture written one depth plane per instance through `gl_Layer`. A Metal pass covered plane 0 only, so the final composite read garbage. Passes into whole 3D textures now cover every depth plane.
