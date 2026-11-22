# Changelog: Red Dead Redemption on the Zelda-line runtime

Each entry says what was found, what was done and how it was checked. Addresses are of 1.0.4 (update v262144: main
build ID 37531419…, sdk 86188952… = SDK 15.3.2; modules main and sdk only). nxnative's notes on this game
(`dm/games/01007820196A6000/README.md`, `game.tsv`, read from a clone outside the repository) were read for findings; no
nxnative code is used.

## 2026-11-22 — from nothing to the title, the agreements, the difficulty menu and the opening

### Inputs and extraction
- The player's card image holds the base game (12.1 GB Program NCA) and the 1.0.4 update (a 334 MB patch NCA with its
  ExeFS and a BKTR RomFS section). The ExeFS came out with games/pla's `extract_exefs.py --update` as it is: main
  `eb9d7829…`, sdk `50f4d98a…`, both build IDs equal nxnative's 1.0.4 row (`assets/expected-1.0.4.json`).
- The merged RomFS: games/pla's `merge_romfs.py` reads the base from an already extracted base image, which would have
  been another 11 GB on a disk with 27 GB free. `scripts/merge_card_romfs.py` (new) reuses that merge as it is and only
  reads the base section straight from the card (`CardBase`). Checked: all six IVFC levels of the merged image verified
  (736,802 level-5 blocks), 138 files, 12,071,763,888 bytes, SHA-256 `eff2df6c…`; 11.78 GB from the base, 312 MB from
  the update; under a minute and a half on the M4. 134 `.rpf` archives, two `.bk2` movies, Double Eleven's
  `d11extra/langselect.pkdswitch`, as nxnative lists.
- 1.0.6: the card image named in the task is not on this Mac any more (only the 1.0.4 folder and its cheats, which
  name the same main build ID 37531419DA7654EC). Not checked.

### Base and scaffold
- **Base: Mario Kart 8 Deluxe's port** (`games/mk8dx`, Odyssey's line, the newest copy with the offline console's
  answers). Its `runtime/` and `third_party/d2r_gfx` were taken with their links into `shared/` kept as links; the Mii
  files, the movie decoder (main decodes its Bink movies itself and imports no `nn::movie`) and libvpx were left out.
  `GAME_RDR` takes the graphics layer's default (Tears of the Kingdom's) NVN branches. Provenance:
  `third_party/COPIES.md`. The shared FPS panel (`hud_panel.inc`) comes with the graphics layer:
  `tests/test_hud_panel_shared.py` lists `rdr` in `UNIFIED`; `tests/test_active_players.py` lists its
  `mng_active_start(@"rdr")`.

### Rewriter: main's 1.2 GiB .bss
- `patch_nso.py` stopped on main: a B from the text to the patch area out of range. main's .bss is 0x4b71b91c bytes
  (1.2 GiB: RAGE keeps its heaps and thread stacks there), so a patch area after the image is 1.3 GB from the text,
  beyond B's ±128 MiB. Metroid Dread (`games/dread`, 1.8 GiB .bss) has the answer on the Zelda line: the **leading
  patch area** (the patch area before the module, the key page still after .bss within ADRP's ±4 GiB, the file holding
  the patch area and the image up to the end of data only). Taken into this folder's `patch_nso.py`, `prepare_runtime.py`,
  `runtime.h` and `loader.c` (Mac and iPhone mapping, permissions, crash names), with a 16 MiB lead for main's 20 MB of
  text (Dread's 4 MiB). sdk keeps the old layout. Checked: main rewrote (1.7 MB patch, 26,301 slot references, no
  gates: main makes no SVC), sdk (100 gates); `rdr-native rewritten/local probe` passes (4000 cross-module calls through
  the timer gate on 4 threads; 168,526 relocations); `tests/test_rdr.py` `LeadingPatchArea` rewrites a synthetic NSO
  with a 192 MiB .bss and one with 1 MiB and checks both layouts and the branch into the patch area.

### Host answers this game needed (in order of the stops)
1. **SDK 15.3.2's `nn::init::Start`** has five arguments (the module finalizers last): the loader called the SDK 4.4 form
   and stopped on the missing symbol. Now as games/dread and games/eow.
2. **`nn::fs::SetAllocator`** reached `fsp-srv` through IPC (IsEnabledAccessLog). The host's file system allocates
   nothing in the guest: answered as games/dread / games/totk do. With it, the other fs entries main imports that the
   copied host did not answer: `EnableDefaultGlobalFileDataCache`, `DeleteDirectory`, `GetDirectoryEntryCount` (RomFS
   listings too), `GetSaveDataSize` (games/totk's answers). Found by listing main's `nn::` imports against the host's
   answers (139 not answered by name, most of them SDK code that runs as it is).
3. **`nn::vi`**: the sized `CreateLayer(layer, display, 1920, 1080)`, `SetLayerCrop`, `DestroyLayer`, `CloseDisplay`,
   `vi::Finalize` (as games/eow's front end). **`nn::oe`**: `GetDefaultDisplayResolution` (1920x1080) and its change
   event (bound, never signalled), `RestartProgram` (stops with a message).
4. **NVN `nvnSamplerPoolRegisterSamplerBuilder`** (NVN 55: a sampler registered from its builder): the parameters
   `nvnSamplerInitialize` would copy. It was the first unimplemented NVN call.
5. **The trap in `rage::sysMemMultiAllocator` (main+0x16b698)**: RAGE freed a pointer that none of its four allocators
   owned. Traced with `TOTK_CRASH_MEM` dumps: the allocators are `rage::sysMemD11Allocator` → `d11::LockingAllocator_c`
   over 400 MB and 800 MB heaps in main's .bss; the pointer was a malloc'd `"dummyTechnique_fadeless"` in the OS heap,
   built by libc++ code in **sdk** with **sdk's own `operator new`** and freed by main's inline code with **main's
   `operator delete`** (RAGE's). Horizon's rtld binds a global symbol to its first definition in load order (main first),
   so main's `operator new` / `delete` and `malloc` / `free` interpose on the SDK's; the loader bound each module's
   references to its own definitions first. Now a global or weak defined symbol resolves through the load order
   (`loader.c`), a local one stays its module's. Checked: past the trap to the first frame; the effects load.
6. **No system resource**: the NPDM declares 0 MB, so on Horizon `GetInfo(16)` is 0 (`IsVirtualAddressMemoryEnabled`
   false); the copied host answered 32 MB (nxnative's notes: `sys_resource 0x0`). Answered 0 (`services.c`).
7. **Thread stacks in main's .bss**: the second audio renderer's thread (the Bink movies' sound) aborted in
   `nn::os::CreateThread`: `svcMapMemory` refused a stack source in main's .bss (0xdc01) and the SDK gave up after its
   address retries. Metroid Dread keeps its stacks there too: games/dread's rule taken (`runtime_module_data_rw` in
   `loader.c`, a CodeData source in `services.c` `stack_move` and `QueryMemory`). Checked: the Rockstar logo movie plays
   and the second renderer opens (`[totk-audio] renderer open … voices 32`); the shared audio host renders both (no
   change needed, unlike nxnative's host).
8. **Triangle fans** (NVN primitive 6; Metal has none) were skipped with a red line on the title. Now drawn as lists:
   unindexed fans through a shared index buffer (`0, k+1, k+2`, the first vertex as base vertex, as quads), indexed fans
   expanded into the submission's arena (`nvn_mtl.m`).

### Where it stands (Mac, Apple M4, screenshots through `TOTK_INPUT_FILE` `shot`; copies in `<work>/evidence`)
- From a fresh profile: language selection (the eleven languages), the three documents of this build (the license
  agreement, the Terms of Service, the Privacy Policy: each accepted with A only after scrolling to its end with the
  right stick, arrow down on the keyboard), the subtitles prompt, the health warning, the Rockstar Games logo movie
  (Bink), the Double Eleven logo, the title ("Press +"), the difficulty menu (Normal / Hardcore / Undead Nightmare), the
  "no save found" alert (Create a new save game: `RDR2SAVE0.SAV`, `RDR2OPTIONS.SAV` and `PREBOOT.SAV` written in the
  profile), the autosave warning, and **the opening: Blackwater's quay, John Marston and the agents, the train to
  Armadillo with the credits**. The agreements are kept in `PREBOOT.SAV`: the next start goes health warning → logos →
  title in about 45 s.
- **Control of John Marston at Armadillo Station** (the autosave of the opening, loaded after + → Normal): "Go to the
  saloon", the HUD's hints and the minimap; W walks him to the station (left stick), the arrows turn the camera (right
  stick). `9-control-armadillo-station.png`, `11-walk-to-the-station.png`, `12-camera-turned.png`.
- Frame time p50 33.33 ms (the game's 30 fps: it asks for present interval 2), p95 33.5 ms; GPU p50 5.7–6.3 ms on an M4.
- **Open**: glass (windows, the car's windscreen, the train's windows) is drawn bright green; some foliage at a
  distance has black blocks (nxnative saw "small dark blocks in distant foliage" in 1.0.4's title scenes too);
  `SetDepthBounds` is not available on Metal (one red line); one texture's storage outside a memory pool (CPU fallback).
  The glass is green with red and blue exactly 0 (pixel values (0, 24..56, 0) in `10-armadillo-marston.png`): one
  channel survives, which points at a texture's format or swizzle in the glass effect, not at lighting; not traced yet.
