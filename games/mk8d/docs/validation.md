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

## 2026-10-29: the Booster Course Pass

The same Mac, OS and tools. Inputs: the player's own base game NSP, 4.0.1 update NSP and Booster Course Pass NSP (`0100152000023001`, `v65536`, 121 KB: a ticket, a certificate, the meta NCA and a 114 KB data NCA), each with its own ticket.

| Check | Result |
|---|---|
| `make -C games/mk8d test` | 19 passed: the earlier 13 (the add-on test now takes the Booster Course Pass by its ticket), plus other add-on versions refused, links to moved originals replaced, a changed add-on image refused, the add-on pins matching the gates, and the launcher's save backups (compiled from `launcher.c`, run on APFS) |
| Build (`scripts/build_mk8d.py`, the work folder of the 2026-10-28 build) | Three NSPs linked. The DLC extracted to `aoc/1.img` (4,228 bytes, 2 files), which matched its pin; extraction and rewrite reused; split 29,651 shaders from 6,599 files (the update's 6,597 and the add-on's 2, which hold no shader); 29,651 of 29,651 Mac shaders for chain `d9fda03e8b0d` (9:23); pipeline archive for `applegpu_g13s` 504.3 MB (6:00); app 13.1 GB with `Resources/aoc/1.img`, ad-hoc signed; `codesign --verify --deep --strict` passed. The same image, extracted twice by hand with nx_extract, had the same SHA-256 |
| Boot and Mii pick, fresh private save (`f960:L+R~0.3,f1300:Right,f1450:A,f1800:A,f2100:A`, 220 s) | `aoc: reporting 1 add-on contents {1}`, then `fs mount aoc: → …/Resources/aoc/1.img (read-only)`. The title ("Ver. 4.0.1"), "Select a Mii", the class choice; `userdata.dat` written. The host exited with 0 |
| Cup screen, that save | The first page as in the 2026-10-28 runs (12 cups, L and R page markers, the next page's row above it) |
| A Booster Course Pass race, that save (`f960:L+R~0.3,f1260:A,f1500:A,f1740:A,f2000:A,f2240:A,f2400:A,f2560:A,f3000:R,f3300:A,f4500:A~90`, 170 s) | R to the next page, its first cup: **Golden Dash Cup, Tour Paris Promenade**, its intro and lap 1 of 3 (3rd place, an item held). In the race (after frame 6,300): 2,997 frames on screen, 11 dropped, p99 over 16.7 ms in 1 of 10 300-frame windows (worst 33.3 ms), footprint 2.67 GB. The host exited with 0 |
| Save backups (the app repacked with the new launcher; the stages before it reused, packing 2:13), from a copy of the save of the race above | 1st start (after macOS's first-start wait): `saves backed up to …/backups/20261029-091331`, a copy byte for byte the save before it. 2nd start: another backup, since the game had committed at boot (`save commit #1`; 5 bytes of `userdata.dat` differ, at 57–60 and 8,485: a time and a start count). 3rd and 4th, 4 s each: the game committed within those seconds, so one backup each. Then three starts whose host stops at boot (`DM_ROMFS` set to a missing image; exit 139): the first backed up the 4th start's save, the other two logged `saves unchanged since the backup …` and added none. Five backups, each with `save/` and `save_device/` |

Every run printed the one `[nvn-mtl] [RED] texture ID … not registered` line and no other. The player's own save folder was not used: every run had its own (`DM_SAVE`, `DM_TEMP`, `DM_ROOT`).

### The cup pages, a Booster Grand Prix and full screen

Later the same day, the app installed in `/Applications` (commit `62306a8`), each run from a copy of the save of the Mii pick above. The route to the cup screen is `f960:L+R~0.3,f1260:A,f1500:A,f1740:A,f2000:A,f2240:A,f2400:A,f2560:A`. Two windowed runs stalled when another app took the focus: the game's focus went out (`oe focus state → 2`) and it ignored the keys from then on, so those two were run again. The reruns were meant to be windowless (`DM_WINDOW=0`), but the front end opens a window whenever `DM_WINDOW` is set, to any value, and the launcher sets it: their logs show `frame latency 1 (window)`. They were windowed like the others and kept the focus (corrected the same day; a run without a window starts the host directly with `DM_WINDOW` unset, as in [the controller applet section](#2026-10-29-the-controller-applet-froze-the-character-screen)).

| Run | Result |
|---|---|
| The cup pages with the add-on content (windowed, 120 s; then `R`, `Down`, `Left`, `A`) | The next page holds the 12 Booster Course Pass cups, Golden Dash Cup (Tour Paris Promenade, 3DS Toad Circuit, N64 Choco Mountain, Wii Coconut Mall) to Spiny Cup (Tour Madrid Drive, 3DS Rosalina's Ice World, SNES Bowser Castle 3, Wii Rainbow Road). Spiny Cup, the last wave's, opens "Start Grand Prix?" |
| The same pages without it (`DM_AOC_OWNED=0`, the host reports no add-on content; windowed rerun, 100 s; then `R`, `A`) | The next page shows the same 12 cups, but choosing one opens the game's notice that they come with the Booster Course Pass or the Expansion Pack, and no race starts. So the add-on content is what makes them playable |
| A Booster Course Pass Grand Prix (windowed rerun; then `R`, `A`, and `A` held for 19 s of every 20 s to frame 57,000) | 50cc Golden Dash Cup: Paris Promenade, Toad Circuit and Choco Mountain each to their results (Mario 3rd after each, 35 points), then Coconut Mall's intro and lap 1, where the keys ended (the route was too short for its last race). 8 save commits, no focus change. The host exited with 0 |
| PR #1226's scripted race in **full screen** (`DM_FULLSCREEN=1`, windowed otherwise; the built-in Liquid Retina XDR display; 340 s) | `display link: frame latency 2 (fullscreen)`. 50cc Mushroom Cup, Mario Kart Stadium to the results (Mario 1st, 15 points). To frame 18,900: **18,884 frames on screen, 58 dropped**, p99 over 16.7 ms in 4 of 63 windows (worst 37.5 ms), footprint 2.14–2.82 GB. The same race in a window on 2026-10-28: 18,873 on screen, 68 dropped, 4 of 63 windows (worst 33.3 ms) |

Each of these printed the one `[nvn-mtl] [RED] texture ID … not registered` line and no other.

### By hand: a Booster Grand Prix in full screen

By the player, 11:17–11:35, on the same MacBook Pro's built-in display: the app in `/Applications` started from its settings window (docked, full screen, English, rumble on), with a DualSense Wireless Controller, from the player's own save. The player chose a Booster Course Pass cup, played its whole Grand Prix and reported no problem. From the host log:

| | |
|---|---|
| Start | `saves backed up to …/backups/20261029-111748`, the player's save of 2026-10-28 21:04; `aoc: reporting 1 add-on contents {1}`, `fs mount aoc:`; `display link: frame latency 2 (fullscreen)`; the DualSense player 1 with motion (gyro) and rumble |
| Frames | 61,200 in about 17 minutes: **61,184 on screen, 33 dropped**; p99 over 16.7 ms in 3 of 204 300-frame windows (33.3, 100 and 33.3 ms) |
| Memory | footprint 2.25–3.04 GB |
| Audio | 0 underruns in 103 reports |
| Saves | 11 commits; `userdata.dat` rewritten at 11:34, `save_device/` holds 9 files (5 before) |
| Other | no focus change, the one known `[RED] texture ID … not registered` line, the host exited with 0 |

This is the first race played by hand in this target's own app, and the first session in full screen played by hand in it.

Not checked: Coconut Mall to its end in the scripted run, the other Booster cups by hand, their courses in Time Trials, VS Race and battle.

## 2026-10-29: the controller applet froze the character screen

**Report.** In a later session by hand (17:55–18:15, full screen, DualSense), after several races the player went to the character screen, pressed Plus or Minus, and the game froze: nothing responded, and the app had to be quit with Cmd+Q.

**The log.** At about 18:14 (frame 65,764) the menu's movie preview stopped, the draws per frame fell to 150 and the GPU time per frame from 3.8 to 0.9 ms, and the game's worker threads went idle. The game kept presenting 60 frames a second (none dropped) of one still picture for the last minute, until the quit. There was no error, no focus change and no service call in the log. The last save commit came before, and the host exited with 0, so nothing was lost.

**Reproduced.** Scripted runs from a copy of the player's save: Minus on the character screen gave the same picture and the same log. Two runs identical but for that Minus, each counting every host take-over the game called (`DM_HLECOUNT`), differed in one call: `nn::hid::ShowControllerSupport`, the Switch's controller applet (the "Controllers" screen), once.

**Cause, in the game's code.**
- On Minus, `main+0x861e3c` stops the scene (`main+0x7c6218`), marks the applet busy and wakes the game's `ControllerAppletThread`, which calls the applet (`main+0x86105c`) and stores its result.
- The scene starts again (`main+0x7c62bc`, from `main+0x86b0bc`) only after the game's notification thread (`main+0x8da04c`) receives focus message 15 with the focus back in state 1. On the Switch the system sends it when the applet closes.
- The host answered the applet at once and sent no focus message, so the scene never started again.
- No other place in the game stops the scene this way.

Until the fix, losing and regaining the window's focus (Cmd+Tab away and back) would have freed the game. This was checked with a test host that switches the focus to 2 and back to 1 on a timer: the scene came back and the keys worked again. Cmd+Tab itself was not tried by hand. The timer is `DM_OE_SCRIPT`, which as copied starts only on the non-blocking notification calls and so never in this game: the test host started it on the blocking one too. That host change is not part of the fix.

**The fix.** `runtime/host/hle_hid.c`: both forms of `ShowControllerSupport` take the game out of focus and back (`dm_focus_set(2)`, then `dm_focus_set(1)`) before returning, as the system's applet does. They do this only when the game is in focus; a window already out of focus gets its message when it comes back.

**Checks.** The headless runs below start the host directly, with the launcher's settings but `DM_WINDOW` unset (the front end opens a window whenever `DM_WINDOW` is set, to any value), from a copy of the player's current save. The keys go to Single Player, Grand Prix, 50cc and the character screen, then press Minus on the character grid (frame 2,700), A on Link, Minus on his variant panel (3,300), and A three more times.

| Check | Result |
|---|---|
| `make -C games/mk8d test` | **20 passed.** New: the copied host's `t_hid.c`, built from `game.tsv` with no game files: 122 checks, among them the focus round trip of both applet forms and none when out of focus. Without the fix, two of them fail |
| Build | Only the Mac host was built again; the other stages reused. App 13.1 GB, `codesign --verify --deep --strict` passed, installed in `/Applications` (the previous app moved to the Trash) |
| The previous app, headless | After Minus on the grid, 150 draws per frame and one still picture to the end (frame 6,600); the six later key presses changed nothing. `ShowControllerSupport` called once |
| The fixed app, headless | `ShowControllerSupport` called twice, each followed by `oe focus state → 2`, `→ 1` and `oe notification popped by the game: 15`. The game went on after both, chose Link, played the kart screen's new-part animation, showed the kart parts and then the cup screen (Mushroom Cup) |

Both headless runs printed the one known `[RED] texture ID … not registered` line and no other, and exited gracefully.

Not checked: the fix by hand with a controller, and other screens that may open the controller applet (such as multiplayer).

## 2026-10-30: 2P and every library applet

**Found.** After the fix above, a headless run of the installed app on the Multiplayer path showed a new problem: L+R at the title, Down, A (2P), A (Grand Prix), A (50cc), with the one player a headless run has. The 2P character select appeared, and the game called the controller applet **642 times in about 50 s**. The game wants two players there. The host answered OK with one player, and with the focus pair the game applied that answer and opened the applet again. Before the fix it froze there instead.

**Upstream.** nxnative fixed the same game the same day, in `137c2291` ("Library applets take the application's focus", GitHub issue #550, found through 2P):
- every applet stand-in holds the game out of focus while it runs (`DM_APPLET`);
- the controller applet answers canceled (2202-3101) when fewer players are connected than the game's minimum.

It is ported here as it is, in place of this target's own fix. `third_party/COPIES.md` lists the files.

**Keyboard row.** The settings window gets a Keyboard choice for the host's `DM_KBD_PLAYER`. "Own player" makes the keyboard player 1 and the controllers players 2 on, so two can play with the keyboard and one controller.

**Checks.** Headless runs started the host directly with `DM_WINDOW` unset, from a copy of the player's save. The host was built from this commit's sources and put into an APFS copy of the installed app, with the settings list made again; the app in `/Applications` was not changed.

| Check | Result |
|---|---|
| `make -C games/mk8d test` | **21 passed.** New: the copied `t_hid` (124 checks, among them the applet's focus and the canceled answer) and `t_oe` (53 checks: applets opening and closing, nesting, the window's focus while one is open) built and run; the Keyboard row's values. The commit's other tests, `t_swkbd`, `t_pctl`, `t_shared_font` and `t_system_event`, were built and run once by hand: all passed |
| Settings list | `settings.py` writes 13 rows for this app, the Keyboard row among them; the other 12 are as before |
| 1P, Minus on the character grid and then on Link's variant panel | Each opened the controller applet once: `oe library applet: controller support`, `oe focus state → 2 (library applet open)`, `hid controller applet: 1 to 1 players wanted, 1 connected`, `oe focus state → 1 (library applet closed)`, and the game took the message. It went on to the kart parts and the cup screen |
| 2P with one controller | **One** applet call: `2 to 2 players wanted, 1 connected: too few, closed as the player would (canceled)`. The game went back to the engine class menu, and B twice reached the main menu |

Both runs printed the one known `[RED] texture ID … not registered` line and no other.

**The built app.** The app was then built from this branch (`45dcf62`; the branch was later rebased onto `main`, which changed no file in `games/mk8d`): only the shader split ran again, and all 29,651 Mac shaders came from the cache. It is 13.1 GB, `codesign --verify --deep --strict` passed, its `launcher.json` holds the Keyboard row, and it was installed in `/Applications` (the previous app moved to the Trash). The same two headless runs on the installed app gave the same results. In 1P, the applet was called twice (`1 to 1 players wanted`), and the game went on to the kart parts. In 2P, it was called once (`canceled`), and the game went back to the engine class menu and then, with B twice, to the main menu.

**By hand, then fixed.** The player tried 2P twice, with a DualSense, and both times the game went back from the character select. The logs show `DM_KBD_PLAYER=shared` and `2 to 2 players wanted, 1 connected`: the Keyboard row was among the Advanced settings, which the window keeps folded, so it was never changed. The row is now among the basic settings, under Controls (`fd84538`). The app was built again (only the packing ran) and installed, and one headless 2P run gave the same answer as before.

**By hand, after the fix.** The player then tested on that app, in full screen with a DualSense.

| Session | From the host log |
|---|---|
| 2P, 13:27 | Keyboard set to Own player in the window: `DM_KBD_PLAYER=own (changed)`, `the keyboard is player 1, each controller a player of its own from player 2 on`. The DualSense was player 2 (npad 1), with gyro. The player reached the 2P character select and reported that it works; the draws per frame rose to about 320 there (262 with the one player of the scripted runs). The game opened no controller applet in this session, since two players were connected. No 2P race was played |
| 1P, 13:28–13:41 | Keyboard back on Plays with player 1, the DualSense player 1. One more race session by hand: 47,400 frames, **47,382 on screen**, 74 dropped, p99 over 16.7 ms in 5 of 158 windows (33.3 ms each). Footprint 2.32–3.00 GB, 0 audio underruns in 79 reports, 15 save commits, no focus change, the one known red line, host exit 0 |

Not checked: a race with two players, how the single-player modes treat a controller that is player 2, and this game's keyboard, Mii and error applets.

## Known limitations

- Mii heads draw nothing: the host ships no MiiModel firmware data (nxnative's stand-in has every part empty). Bodies and favourite colours show.
- Only 4.0.1, and no add-on content but the Booster Course Pass `v65536`.
- Online play and local wireless are unavailable by design.
- One `[RED] texture ID … not registered` line per run, also upstream; its effect on the picture was not seen in the runs above.
- Fullscreen on the built-in 120 Hz display (frame latency 2): the hand race in the upstream build dropped 32–40 of every 300 frames for part of the session. In the port's app the scripted race in full screen dropped 58 of 18,900, as many as in a window (68), and a whole Grand Prix played by hand in full screen dropped 33 of 61,200.
- The app is ad-hoc signed. The first start of a newly built app waited 90–115 s before anything ran (every new build so far, presumably macOS assessing the 13 GB bundle); later starts did not. Distribution and other Macs were not tested.

Screenshots, saves, logs and translated shaders used here are not in Git: they contain or derive from game content.
