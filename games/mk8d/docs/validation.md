# Validation record

Date: **2026-10-28**. Game: **Mario Kart 8 Deluxe 4.0.1**, `0100152000022000`, update `v1507328`, SDK 4.4.0. The 4.0.1 ExeFS sizes, SHA-256 values and build IDs are in [expected-4.0.1.json](../assets/expected-4.0.1.json).

Hardware: MacBook Pro (`MacBookPro18,2`), Apple M1 Max (GPU `applegpu_g13s`), 32 GB. OS: macOS **27.0.1 (26A434)**, Xcode 27.1 (Metal `32023.921`), .NET SDK 10.0.102, Python 3.14.7. This is the test machine, not a performance promise for other Macs. iOS, iPadOS and visionOS were neither built nor played for this target.

Inputs: the player's own base game NSP (1.0.0) and 4.0.1 update NSP, with their common tickets. A third NSP in the same folder, add-on content, was left out by the wrapper (`ignored (not the base game or the 4.0.1 update)`) and never reached the build.

## Checks without game data

| Check | Result | What it establishes |
|---|---|---|
| `make -C games/mk8d test` | 13 passed | Tool checksums and file set, staging (executable tools, local NuGet config, chain header), input selection by ticket (add-on content left out, other updates refused), the builder's title / add-on / version gates, the pinned-hash gate before the rewrite, offline LibHac with NuGet auditing off, the pins matching the profile's build row, no RomFS path in the profile's tables |
| `make -C games/mk8d test-gpu` (Metal validation on) | `t_cube_placeholder` 42 checks, `t_vbuf` 84, `t_geometry` 1,920; 0 failed | The copied NVN layer builds from this folder and passes its regressions on an M1 Max, among them the zero cube for a 2D array in a cube slot that this game's races need |

## The port against an unmodified upstream build

To tell the port's changes from the game's behaviour on this Mac, the same two NSPs were first built with an unmodified nxnative checkout at `062cd06` (`scripts/build_ipa_from_nsp.py --platform mac-app`, outside the repository), and both builds were compared stage by stage.

| Stage | Upstream | Port | Comparison |
|---|---|---|---|
| Extraction | 6,602 files, 11.3 GB, 0:19 | the same | the same NSPs |
| Pinned 4.0.1 ExeFS | — | `main`, `main.npdm`, `rtld`, `sdk`, `subsdk0` match | |
| Rewrite | `main`, `subsdk0`, `sdk` | the same | all 18 rewritten modules and tables byte-identical; only `build_src.sha256` differs (it lists upstream's other games' `patches.py`, which the port does not carry) |
| Shader split | 29,651 shaders, 18,747 pairs | the same | `shader_map.tsv`: the same stage and key in every row, packages named by the generic group (`vs_archive.bin` 12,522 + `vs_main.bin` 8, `ps_archive.bin` 17,063 + `ps_main.bin` 29, 28 compute, 1 geometry); `shader_facts.tsv`, `shader_pairs.tsv`: identical |
| Mac shaders | 29,651 of 29,651 ready, 36:37 | | chain `a78f966a7b38` (upstream) / `fd8ba7b84f3c` (port) |
| Pipeline archive | `applegpu_g13s` (metal-tt) | | |
| App | 13.1 GB | | |

## Upstream baseline on this Mac

Runs of the upstream app with `DM_LAUNCHER=0`, a private save folder, muted Core Audio with a PCM dump (`DM_AUDIO_MUTE=1`, `DM_AUDIO_DUMP`), scripted keys (`DM_KEYS`) and screenshots (`DM_NVN_SHOT`):

- **Boot to the title** (fresh save, 75 s): "Press L+R to start" and "Ver. 4.0.1" by frame 900; 75 s of stereo audio, every sampled block non-zero (RMS 990, peak 7,563).
- **Mii pick** (fresh save, `f960:L+R~0.3,f1300:Right,f1450:A,…`): "Select a Mii" with the six default Miis (heads empty: no MiiModel firmware data, a known limit), then the single-player menu and the class choice with its movie preview; `userdata.dat` written.
- **A race** (that save, upstream's route `f960:L+R~0.3,f1260:A,…,f4300:A~70`): 50cc Grand Prix, Mushroom Cup, Mario Kart Stadium's intro and the race (lap 1/3, an item held, coins counted). On screen 299–300 of every 300 frames, 1–3 dropped, p99 16.67 ms (60 fps); footprint 2.7 GB.
- One red line per run, the same in every run: `[nvn-mtl] [RED] texture ID … not registered`.

## The port's app

Built with `scripts/build_mk8d.py` (work folder outside the checkout, the LibHac package from a local file): extraction, rewrite and split reused or redone as above, 29,651 of 29,651 Mac shaders ready (13:07: the Metal compiler's own cache had most of them from the upstream build), host 0:25, pipeline archive for `applegpu_g13s` 504.4 MB (6:00), app 13.1 GB, ad-hoc signed; `codesign --verify --deep --strict` passed. The same scripted runs as for the upstream app, each from its own private save folder:

| Run | Keys | Result |
|---|---|---|
| First launch | none | The first start of the newly built app waited about 90 s before the launcher ran (its log appeared at 16:03:21 for a start at 16:01:52); the upstream app's first start waited too. Later starts begin at once. Title by frame 900 |
| Mii pick, fresh save | `f960:L+R~0.3,f1300:Right,f1450:A,f1800:A,f2100:A` | The title, "Select a Mii" with the six default Miis (empty heads), the single-player menu, the class choice with its movie preview; `userdata.dat` written. 70 s of audio, RMS 737, peak 7,856 |
| Grand Prix, that save, 320 s | `f960:L+R~0.3,f1260:A,f1500:A,f1740:A,f2000:A,f2240:A,f2400:A,f2560:A,f2800:A,f3040:A,f4300:A~280` | 50cc, Mushroom Cup, Mario Kart Stadium: the intro, laps 1–3 (accelerate held, the game's own steering assist on a new save), 1st place, and the results table (Mario 15 points). To frame 18,900: 18,873 frames on screen, 68 dropped, p99 over 16.7 ms in 4 of 63 300-frame windows (worst 33.3 ms), footprint 2.74 GB, flat |
| Online Play | `…,f1300:Down,f1360:Down,f1420:A,f2000:A` | The online intro, "Connecting…", then back to the main menu; no crash |
| Wireless Play | `…,f1300:Down,f1360:Down,f1420:Down,f1480:A,f2000:A` | Stays on the main menu, no lobby; no crash |

Every run printed the same single red line as upstream, `[nvn-mtl] [RED] texture ID … not registered`, and no other.

## Played by hand

By the player, on the same MacBook Pro (its built-in Liquid Retina XDR display), with a DualSense Wireless Controller, docked, English, from the save folder both apps share. Both apps were named Mario Kart 8 Deluxe, and the first session ran the upstream app by mistake. Everything below is from the host logs, except the race, which the player reported:

| Session | App | Result |
|---|---|---|
| 17:55–18:00, fullscreen | the **upstream** app built above for the comparison | The DualSense became player 1 with its gyro, its rumble on both handles. The player raced and saw no problems. To frame 16,800: 16,787 frames on screen, 743 dropped (32–40 of every 300 from frame 10,500 to 13,200), p99 over 16.7 ms in 25 of 56 windows (worst 29.2 ms), footprint 2.15–2.83 GB, no audio underrun; the host exited with 0 |
| 18:09–18:10, in a window | **the port's app**, moved to `/Applications` | The DualSense became player 1 with its gyro. About 30 s to frame 1,800, the title and menus, no race: 81 frames dropped in the first 600, none from frame 600 to 1,500, footprint 2.09–2.17 GB, no audio underrun. The window lost focus twice; the second time the display link stalled 8.6 s (237 frames dropped) and recovered when the window came back. The host exited with 0 |

Each printed the one `[RED] texture ID … not registered` line and no other.

So a race by hand with a controller was played in the upstream build of the same inputs, whose rewritten modules are byte-identical to the port's. The port's own app was only started by hand (the title, the menus and the controller), and its race is the scripted one above. Fullscreen was used only in the upstream session.

## Known limitations

- Mii heads draw nothing: the host ships no MiiModel firmware data (nxnative's stand-in has every part empty). Bodies and favourite colours show.
- Only 4.0.1 with no add-on content: the Booster Course Pass courses are in the update's data, but the game offers them only with the purchased add-on content, which this target does not take.
- Online play and local wireless are unavailable by design.
- One `[RED] texture ID … not registered` line per run, also upstream; its effect on the picture was not seen in the runs above.
- Fullscreen on the built-in 120 Hz display (frame latency 2): the hand race in the upstream build dropped 32–40 of every 300 frames for part of the session, against 68 in the whole scripted race in a window. Fullscreen was not measured in the port's app.
- The app is ad-hoc signed. The first start of a newly built app waited about 90 s before anything ran (both apps, presumably macOS assessing the 13 GB bundle); later starts did not. Distribution and other Macs were not tested.

Screenshots, saves, logs and translated shaders used here are not in Git: they contain or derive from game content.
