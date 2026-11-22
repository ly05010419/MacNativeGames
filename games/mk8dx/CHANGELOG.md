# Changelog: Mario Kart 8 Deluxe on the Zelda-line runtime

Each entry says what was found, what was done and how it was checked. Addresses are of 4.0.1 (main build ID
158c0c73…, sdk ba520906… = SDK 4.4.0, subsdk0 d9de423e… = the SDK's movie library). nxnative's notes on this game
(`games/mk8d/runtime/games/0100152000022000/README.md`) were read for findings; no nxnative code is used.

## 2026-11-21 — from nothing to the title screen, the menus and a race

### Base and scaffold
- **Base: Super Mario Odyssey's port** (sead / agl engine family, `main` / `subsdk0` / `sdk`, NVN 53). Its `runtime/`
  and `third_party/d2r_gfx` were copied with their links into `shared/` kept as links; Odyssey-only parts left out or
  replaced (`game_patches.c` written anew with no patches, `odyssey_aspect.c` dropped). `GAME_MK8DX` takes Odyssey's
  NVN 53 branches in `hle_nvn.c` (device values 53 / 311, the default depth mode, the queue-side acquire) and the shared
  files' generic branches (account, hid feedback, scheduler). Provenance: `third_party/COPIES.md`.
- Inputs: the player's base + 4.0.1 update, already extracted on this Mac (`ExeFS/` and a merged 12 GB RomFS image in
  another build's flat DMROMFS1 layout). `scripts/index_romfs_image.py` turns either layout into the runtime's index
  without copying the 12 GB (the runtime verifies the image's SHA-256 once and keeps a proof). Checked: 6,597 files,
  `AI/AIRivalTable.byaml` present; ExeFS SHA-256s equal `assets/expected-4.0.1.json`.

### Rewriter
- `patch_nso.py` stopped on `subsdk0`: `jump table 1ae1f0`. A byte-class dispatch whose table base (`adrp/add x10`) is
  set before the loop, out of `ldrsw_table`'s window, with two moves between `add x18` and `br x18`. Found with the new
  `scripts/find_signed_tables.py` (two copies, +0x1ae1f0 and +0x1b18ec, 27 entries each, every target inside its
  function) and registered as `SIGNED_TABLES` rows for this build (bytes and entries re-checked by the rewriter). `main`
  (0 gates) and `sdk` (91 gates) rewrote unchanged. Checked: `mk8dx-native rewritten/local probe` passes (4000
  cross-module calls through the timer gate on 4 threads; 207,367 relocations).

### Host answers this game needed (in order of the crashes)
1. **`nn::vi::SetLayerScalingMode`** (sdk+0x1d4818 dereferenced the host's layer): answered as games/totk does.
2. **`nn::account::OpenPreselectedUser` (SDK 4.4's aborting form), `ListAllUsers`, `OpenUser`**: the shared
   `account.c`'s Breath of the Wild branch now covers `GAME_MK8DX` too, plus `OpenUser` of the user in force.
3. **Storage** (`fs_host.c`): `MountDeviceSaveData` gets a save of its own (`save/device`): the game mounts CommonSave
   and UserSave and deletes `CommonSave:/userdata.dat` at boot, which in a shared folder was the user's save (nxnative's
   finding). `IsSaveDataExisting(uid)`, `MountAddOnContent` / `QueryMountAddOnContentCacheSize` (the RomFS working
   memory of the folder: 0x88 for the Booster Course Pass's two files, `romfs_work_size.h`, which the game's 0x3130-byte
   decryption heap needs), `MountSystemData` / `QueryMountSystemDataCacheSize`. Read-only mounts refuse writes.
4. **Add-on content list** (`services.c`): `ListAddOnContent` / `CountAddOnContent` from `extracted/aoc/<index>`,
   `GetAddOnContentListChangedEvent` bound and never signalled. Checked: the title shows the Booster Course Pass star.
5. **The offline console's answers** (nifm, ldn, friends, sockets, ssl, prepo, `oe::EnterExitRequestHandlingSection`,
   `GetDisplayVersion`, `GetPseudoDeviceId` …): games/totk's `offline_host.c` moved to `shared/runtime/offline_host.c`
   (totk links it) and built for `GAME_MK8DX` too, instead of writing them again.
6. **The Mii database** (`mii_host.c`, the game's `mii:e` connection aborted): an empty console database with the six
   default Miis (`Get(…, SourceFlag 2)`), `BuildRandom` for the title's demo race, `UpdateLatest` / `GetIndex` not
   found, `AppendMii` cancelled. Every CharInfo within nn::mii's ranges, version 4 create ids, fixed for the defaults.
   Checked: the game's own `CharInfoAccessor::IsValid` accepts them (the Mii pick shows six Miis);
   `tests/mii_probe.c` checks ranges, ids and Get.
7. **The Mii parts** (system data MiiModel 0100000000000802): mounted from the **player's own firmware**, extracted by
   `scripts/extract_mii_model.py` (games/totk's NCA reader imported, FS header and six IVFC levels verified; the archive
   is public-data content type 5). The game reads one texture and one shape file whole and sizes its pool from their
   headers (layout traced at main+0x8ea5f0..0x8eacd8: shape header +0xc / +0x3c, 0x58-byte entries from +0x6c; texture
   +0xc / +0x38, 0x1c-byte entries from +0x64), so a missing archive cannot be skipped. Unlike nxnative's empty stand-in,
   **the Mii heads draw**. Checked: screenshot of "Select a Mii".
8. **Audio renderer REV4**: the output has no render-info block before REV5 (`audio_renderer.c`); it asked for 16 bytes
   more than the game gave. Checked: sound plays (rms ≈ 0.05 on the title).
9. **System fonts** (`pl::GetSharedFont`, `RequestSharedFontLoad`): Animal Crossing's `shared_font.c` (Odyssey's plus
   these), the player's own fonts.
10. **hid**: `GetTouchScreenStates<16>` (no touches), `GetNpadJoyAssignment`, `SetNpadJoyAssignmentModeSingle`,
    `SwapNpadAssignment` (as games/acnh answers).
11. **Graphics**: the game renders into a BC1 cube map (format 0x44 through view 0x2f), into all 8 levels of 128x128,
    whose last three are one block each. `blk_aux` (`nvn_mtl.m`) took 2D / 2D array / 3D only and gave the block
    texture the full level count of a 32x32 chain (6). Now a cube is addressed by slice like an array, and a block
    texture is made large enough for every level the texture has. `TOTK_GPU_MIDCLEAR=1` is needed as for TotK's launcher.
12. **Menu previews** (H.264 in `subsdk0`'s movie library): Odyssey's VideoToolbox decoder, with SDK 4.4's
    three-argument `movie::Decoder` constructor; and a deadlock fixed in `movie_host.c`: with every held-picture slot
    full (20) and inputs waiting (4), a released output slot emitted nothing because inputs were waiting, so the preview
    stood still. Checked: `TOTK_MOVIE_LOG=1` shows frames flowing at 60 fps (1,050 frames in 17.4 s, pts in step with
    the clock); the mode menu's preview moves in screenshots. Odyssey's copy has the same latent bug.

### Where it stands (checked with screenshots through `TOTK_INPUT_FILE`; copies and logs in `<work>/evidence`)
- Title screen, Select a Mii (heads drawn), main menu, mode menu, VS Race → character select → cups / courses →
  "Start Race?" → loading → **a VS race at Mario Kart Stadium**, driven with A held: the kart drives through the
  anti-gravity section, HUD, coins, lap 1/3, position and map draw. In the race: frame time p50 16.67 ms (60 fps), p95
  34–58 ms while shaders and pipelines are still built for the first time (the first race on an empty cache translated
  ~380 shaders and built ~430 pipelines: one 29 s frame; the next start reuses the translation cache), GPU p50 ~4 ms on
  an M4; sound rms ~0.1 with no underruns. The add-on content cache answered 0x88 at runtime, as computed.
- Also added: `GAME_INBOX_ID "mk8dx"` (the copied graphics layer reported as "odyssey" to the shader inbox / board) and
  `mng_active_start(@"mk8dx")` (tests/test_active_players.py lists the game).
- **Open**: A on Grand Prix is seen by the game (`TOTK_INPUT_TRACE`: buttons 0x1 for 300 ms) and a play report is saved,
  but the engine-class menu does not open; VS Race, B and the d-pad work. Not traced yet (found and fixed on
  22 November: the preview's pacing, below).
- Next: a race through to the results and Grand Prix; the app and MNG Launcher card; extraction straight from the NSPs;
  a shader seed; the iPhone / iPad app on the Zelda shell (`games/totk/ios/tools/build_port_ipa.py` with a `GAME_MK8DX`
  profile, as Odyssey's `ios/tools/build_ipa.py` + `ios/Sources/ios_front.m`).

## 2026-11-21 (evening) — the iPhone / iPad app on the Zelda shell, and a whole-game shader seed

### The app (as Super Mario Odyssey's and Pokémon Legends: Arceus's; no nxnative code, `games/mk8d` not used)
- `ios/tools/build_ipa.py`: this game's profile for the shared `games/totk/ios/tools/build_port_ipa.py` (layout of the
  work folder, `GAME_MK8DX`, bundle `org.macnativegames.mk8dx`, the icon of its control data `extracted/local/icon.jpg`,
  the Settings pane without the sync folder and the shader inbox). GameData carries the player's firmware fonts (as the
  Mac reads them), the Mii parts (`extracted/firmware/mii-model`, `TOTK_MII_MODEL`) and the Booster Course Pass's two
  files (`extracted/aoc`, `TOTK_AOC_DIR`); the 12 GB image stays outside (`--image-outside`, copied once by
  `install_device.py --game-data`, read through `TOTK_ROMFS_DATA` as Odyssey's).
- `ios/Sources/ios_front.m`: Odyssey's answers (display rate, no panel screenshot, its own 60 fps).
- The shell (`games/totk/ios/Sources`): `GAME_MK8DX` in `port_features.h` (a port, no frame-rate choice),
  `picture_settings.h` / `picture_profile.h` (`PP_GAME_MK8DX` = Odyssey's picture: the mode and MetalFX to the screen),
  `main.m` (no shader inbox as Animal Crossing / Arceus; its own block: no RomFS overlay or mods, the Mii parts and the
  add-on content in GameData). Its active players' id is `GAME_INBOX_ID` "mk8dx". `games/totk/ios/tools/build_ipa.py`
  `pane_for` gives it the picture rows.
- Runtime: nothing of the game's text changes for the phone (`export-ios`: `patched_words=0` in `main`, `sdk`,
  `subsdk0`; this game has no code patch, so no Mac-only switch and no host address). The shared iOS front end
  (`shared/runtime/frontend_ios.m`) gained `runtime_ios_device_uuid`, which the shared `offline_host.c`
  (`GetPseudoDeviceId`) needs on iOS (the first link failed on it); Tears of the Kingdom's front end has its own. The
  shared scheduler keeps the asynchronous preemption on for this game on the phone, as on the Mac where its races were
  checked (Odyssey's runtime and audio renderer: Odyssey needed it on the phone).
- Checked: `tests/test_ios_ports_unified.py` (`mk8dx` in `PORTS`, `Mk8dxPort`) and the other packaging tests; the Mac
  host rebuilt and driven again from a fresh profile to a VS race at Mario Kart Stadium (lap 1/3, HUD, map;
  `<work>/evidence/7-race-after-ios-changes.png`).

### Shader seed (`scripts/prepare_mk8dx_shader_seed.py`)
- Odyssey's seed tool run as it is (this runtime is Odyssey's: chain v4, the same keys and requests), over every RomFS
  folder: the courses' and karts' models carry their own BNSH programs. Two of this game's things: some UI layout
  archives name no SARC members (SFAT attributes 0, e.g. `UI/cmn/boot/bt_TitleLoading_00.szs`), which the tool refused;
  they are read by name hash. And the rule variants come from the Mac cache instead of a hand-written table: a vertex
  input read as uint (attribute 6; 154 of the 562 vertex variants of the first races) is translated for every shader
  that has that input.
- Result: 29,717 shaders discovered (2,559 containers), 29,690 with an index, 31,707 variants (1,054 of them the uint
  rule), 980 MB; 1,129 s on an M4. 27 skipped: vertex programs in the agl / gsys packages
  (`Common/Gfx/*_resource.Nin_NX_NVN.release.sarc`) the translator refuses (IndexOutOfRange); none was met while playing.
  Every one of the 1,101 shaders the Mac translated while playing (title, menus, two races) has its key in the seed.
- The Mac host imports it at its next start (93,105 files, 23 s once; then 0 and the title as before, warm-up 0.6 s).

### The build (`ios/tools/build_ipa.py --image-outside --device-gpu applegpu_g14p --device-gpu applegpu_g15p`)
- IPA 73 MB (unsigned; no image), the shaders beside it (`<name>-shader-seed`, 1.4 GB, 128,142 files): the seed plus
  the Mac's 1,101 translations, 32,808 device libraries (`compile_seed.py`, about 16 minutes on an M4), and the
  pipelines the Mac collected (1,200 rows after the seed check run) compiled for the iPhone 14 (applegpu_g14p) and the
  iPad A16 (applegpu_g15p): 1,221 each in 10 files, none failed; 7 still use a shader the device compiles itself. `seed_check`
  is ok for both GPUs.
- Not yet run on a device (first install 22 November).

### The shell without the discarded attempt
- The branches of the earlier nxnative-based iPhone app (`GAME_MK8D`, `PP_GAME_MK8D`, `'mk8d'` in `pane_for`; its
  `games/mk8d/ios` is gone) are removed from `games/totk/ios`; `GAME_MK8DX` is this game's. `tests/test_ios_ports_unified.py`
  keeps them out. The IPA was built again with it (the pipelines then: 1,221 per GPU).

## 2026-11-22 — Grand Prix: the one-minute wait after A

### What waited on what
- On the iPhone 14, A on Grand Prix (Single Player) left the mode menu standing for about a minute at 60 fps, then the
  engine-class menu opened; on the Mac it was "A on Grand Prix does nothing" (still there after 124 s). VS Race went on
  at once.
- Traced on the Mac (`TOTK_INPUT_FILE` `threads`, `hletrace`, `TOTK_MOVIE_LOG=1`): no guest thread was stuck in a host
  call; the preview's movie calls stopped 1.5 s after A (103 `AcquireInputBufferIndex` in 85 s) with the decoder full
  (inputs free 0 / pending 4, held 20, decoded-and-paced 8, ready 0), the game's movie thread (main+0x37baa4) waiting on
  its events and its feeder (main+0x37b920) on a condition variable. The paced queue's next frame had pts **144.017 s**,
  10.47 s after the preview's first frame: the mode-menu preview is one H.264 file of clips, and A on Grand Prix moves
  its reader to the engine-class clip at 144 s **without a Flush** (none of Flush / Stop / Reset is called). The game
  opens the engine-class menu only when that clip's first frame comes out.
- The host decoder (`runtime/movie_host.c`, Odyssey's copy) paces frames from one anchor, the first frame handed over
  (TotK's 30 fps cutscenes at 60+ fps); only Flush / Stop dropped it. So the 144 s frame was due 144 s after the preview
  began: 134 s after A on the Mac (confirmed: the engine-class menu opened at 144.0 s since the first frame,
  `<work>/evidence` run of 22 November), about a minute on the phone, where the player had sat longer in the menus.
  A backward jump (a loop) went wrong the other way: every frame after it was due at once, and the frames queued behind
  one another were dropped as "late" until the pts caught up with the old clock.

### Fix
- `shared/runtime/movie_pace.h` (new, linked into `runtime/`): `movie_pace_jump(prev, pts)`: a pts that goes backwards
  or more than 1 s ahead of the previous frame starts a new run. `movie_host.c` keeps the last frame handed over (or
  dropped) and starts the pacing again at such a frame (`[movie] pts … a seek or loop without Flush` in the log), and
  never drops a frame as late across a jump. Shared, because every port's `movie_host.c` paces the same way
  (Odyssey, TotK, ACNH, PLA … have the same latent rule; not changed here).
- Test: `tests/movie_pace_probe.c` (run by `tests/test_mk8dx.py` `MoviePacing`) drives the real `movie_host.c` with
  stand-ins, libvpx-encoded VP9 frames and a fake 60 Hz clock: a forward seek (50 ms → 144.017 s) and a loop
  (5.05 s → 16.7 ms) with no Flush must give every frame, one per game frame. Against the old pacing (Odyssey's file)
  the forward case gives 3 of 6 frames within 600 game frames and the loop 4 of 6.

### Checked
- Mac (M4, fresh `TOTK_PROFILE_DIR`): A on Grand Prix → the engine-class menu in under a second (screenshots every
  0.5 s: leaving at 0.7 s, 50cc–200cc shown at 2.2 s), then character, kart, Mushroom Cup "Start Grand Prix?" → **a
  Grand Prix race at Mario Kart Stadium** (lap 1/3, 12th, map). Before: still on the mode menu 124 s after A, menu at
  134 s. `<work>/evidence/8-grand-prix-engine-class.png`, `9-grand-prix-start.png`, `10-grand-prix-race.png`,
  `grand-prix-run.log`.
- `python3 -m unittest tests.test_mk8dx`; the repository's `tests.test_ios_ports_unified tests.test_hud_panel_shared
  tests.test_active_players`.

### Then: every port
- The other Zelda-line ports' `movie_host.c` had the same one-anchor pacing (each probe run: 3 of 6 frames after a forward
  seek without Flush). All of them take `movie_pace.h` now (a `runtime/movie_pace.h` link each). The ten copies that were
  the same file (TotK's, byte for byte in mkw, oot and wonder; alc, by3, ot0, pla, tomodachi and unicorn had it without
  its `GAME_BOTW` default, a branch only Breath of the Wild's build takes) are one file, `shared/runtime/movie_host.c`, linked as `runtime/movie_host.c`; acnh, dread, eow, kirby, lm3,
  mvdk, odyssey, ori, pokesv, ssbu, tmnt, xc3 and xcx keep their own copies with the same change as this port's.
- The probe moved to the repository's `tests/movie_pace_probe.c` (designated `MovieGuest`, weak stand-ins for the
  graphics calls some copies make); `tests/test_movie_pacing.py` builds and runs it against every port's `movie_host.c`
  with pacing on (`TOTK_MOVIE_PACE=1` / `PSV_MOVIE_PACE=1`: Animal Crossing and Mario vs. Donkey Kong have it off by
  default) and keeps the include and the link in each; `tests/test_mk8dx.py` `MoviePacing` builds the same probe.
