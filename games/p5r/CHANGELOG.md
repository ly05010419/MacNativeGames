# Changelog: Persona 5 Royal on the Zelda-line runtime

Each entry says what was found, what was done and how it was checked. Version 1.0.2 (update v65536 of the Asian release
01004B10157F2000: main build ID d4b150b2…, subsdk0 a8cb9780… = the SDK's movie library, sdk 357efc19… = SDK 13.3.2;
`assets/expected-1.0.2.json`).

## 2026-11-23 — the Mac app; the safeguards and fixes of the other Zelda-line ports

- **The Mac app**: `scripts/package_app.py` and `app/launcher.c` on the shared packaging (`tools/port_app/bundle.py`) and
  launcher (`shared/app/app_launcher.h`, as Animal Crossing's and Mahjong 2013's): the runtime, translator, rewritten
  modules and RomFS image as APFS clones; the icon from the game's control data (`games/pla/scripts/extract_app_icon.py`).
  Checked: packaged, `--check` passes, the app opens in a window at the game's 30 fps.
- **Its own name**: the window title, the Hide / Quit menu items, the starting label and the display-sleep assertion said
  "Super Mario Odyssey" (Odyssey's front end, through Red Dead Redemption's copy); they use `GAME_NAME` now, here and in
  Red Dead Redemption, Age of Calamity and Mario Kart 8 Deluxe.
- **Write tracking**: a 32 MB pool starts on the last page of the first one, and the tracker left such a pool untracked
  (every texture in it uploaded again at every submission). It links Tears of the Kingdom's `nvn_dirty.c` with the
  `NVN_DIRTY_SPLIT` switch on, as Age of Calamity. Checked: "only its own pages tracked" in the log, 9 re-uploads in a
  7-minute run.
- **Age of Calamity's safeguards and fixes** (RDR's graphics layer plus AoC's changes): the GPU hang guard
  (`TOTK_GPU_HANG_S`), the graceful headless close that waits for the GPU, the loop cap on every translated shader
  (`msl_loop_cap.h`), compute kernels' shared memory (`compute_shared.h`), `SetCopyImageStride`, c1 segments beyond 4 KB.
- **Checked on the Mac (MacBook Air M4)**: a new game from the title through the opening movie to the casino with the
  player in control; 30 fps (frame p50 33.33 ms, p95 33.4 ms), GPU p50 15–17 ms; resident memory steady at about 1.72 GB
  and GPU memory at 1.77 GB over 7 minutes; no GPU hang.

## 2026-11-22 — bring-up to the first playable scene

- The update's ExeFS, the merged RomFS (13.7 GB) and the rewrite of main, subsdk0 and sdk (`scripts/build_p5r.py`), on Red
  Dead Redemption's runtime with this game's identity (`runtime/game_version.h`, `GAME_P5R`).
- Reached system data, the network prompt, the Atlus logo, the opening movie, the title, the main menu and the casino with
  the player in control (the screenshots in the work folder's `evidence/`).
