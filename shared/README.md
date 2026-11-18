# shared/

This folder holds code that every game uses, so it exists only once. Before it existed, each port copied it from
`games/totk`, and the copies drifted apart. Animal Crossing, Echoes of Wisdom and Super Mario Odyssey ended up with
older FPS boxes and settings panels than the three Zelda games (Echoes of Wisdom had none), and the interface language
followed the app bundle instead of the Mac. Super Mario Odyssey, Animal Crossing and Echoes of Wisdom link the shared
files now.

This folder mirrors the layout of a game folder. A game uses a file through a relative symlink placed at the path where
its own copy used to be, for example:

    games/odyssey/third_party/d2r_gfx/gfx/nvn/fps_overlay.c -> ../../../../../../shared/third_party/d2r_gfx/gfx/nvn/fps_overlay.c

Because of this, the game's `#include` lines, Makefile rules and object paths stay unchanged. A relative include inside
a shared file is resolved from the link's folder, so it finds that game's own headers.

| File | What it is | Linked by |
|---|---|---|
| `third_party/d2r_gfx/gfx/nvn/fps_overlay.{c,h}` | The overlay texts and modes, including "FPS full information" and the hitch lines | totk (also BotW, DKB, PKP), oot, odyssey, acnh, eow |
| `third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h` | Chinese or English interface, from the Mac's first preferred language | totk, oot, odyssey, acnh, eow |
| `runtime/hud_box.h` | The corner box and FPS graph that frontend.m shows | totk, oot, odyssey, acnh, eow |
| `runtime/settings_panel.h` | The settings panel (key 1); each game's rows and pages are in its `runtime/settings_game.h`, which also says which optional pages it has (`SETTINGS_HAS_*`, the 着色器贡献 page included) and the panel's opacity | totk, oot, odyssey, acnh, eow |
| `runtime/screenshot.h` | The capture button, P and a held controller button: the game's next frame saved as a PNG; the folder's checks | totk, oot, odyssey, acnh, eow |
| `runtime/board_upload.h`, `runtime/board_tab.h` | The frame-rate board: its rounds and upload (我要上榜), and the panel's 性能排行榜 page | totk, oot, odyssey, acnh, eow |
| `runtime/shot_upload.h`, `runtime/shot_tab.h` | Screenshot sharing (分享我的截图) and the panel's 截图分享 page | totk, oot, odyssey, acnh, eow |
| `runtime/nick_row.h` | The player's nickname row on those two pages | totk, oot, odyssey, acnh, eow |
| `runtime/users.h`, `runtime/account.c` | Local users, one save area each (the User page); Odyssey's own account entries behind `GAME_ODYSSEY`. Animal Crossing keeps its own single-user account.c: one island per console, see its settings_game.h; Echoes of Wisdom keeps its own too: its saves stay under their one account until moving them has been tried on a real profile | totk, oot, odyssey |
| `runtime/dsu_client.h`, `runtime/dsu_protocol.h`, `runtime/dsu_settings_rows.h` | DSU (cemuhook) motion and input over UDP, and its input-page rows | totk, odyssey, acnh, eow |
| `third_party/d2r_gfx/gfx/nvn/fps_board.{c,h}` | The board's frame record: frame times after the world started | totk, oot, odyssey, acnh, eow |
| `third_party/d2r_gfx/gfx/nvn/nvn_capture_region.h` | The present's crop as a capture region (the screenshot's game frame) | totk, oot, odyssey, acnh, eow |
| `app/app_launcher.h` | The app launcher's plumbing: the bundle by the real executable path, the data folder in `~/Library/Application Support/MacNativeGames/<folder>` and its one-time move from next to the app, save backups (the newest five), the Mac's first preferred language, the common environment (profile, shader cache, RomFS, fonts, slot, input file, board folder), log rotation, and the run loop with the start again that `restart-requested.txt` asks for. Each game's launcher keeps its own `main()`: its tuning (GPU and presentation variables, Animal Crossing's frame rates and add-ons, Odyssey's session limit, display, face buttons and MetalFX, Echoes of Wisdom's start by the runtime's real path), its checks and its `TOTK_CONTROLS` line. Linked as `scripts/app_launcher.h` (odyssey, acnh) and `app/app_launcher.h` (eow) | odyssey, acnh, eow |

| `third_party/d2r_gfx/gfx/nvn/totk_mfx_up.{m,h}` | The MetalFX spatial upscaler: its scene, docked, output and VR contexts, background builds and kept scalers | totk, oot, odyssey, acnh, eow |
| `third_party/d2r_gfx/gfx/nvn/totk_sysload.{c,h}` | The load line of FPS full information: the game's own GPU share, then the CPU cores and the busiest thread | totk, oot, odyssey, acnh, eow |
| `runtime/playtime.h` | The corner box's playtime: this session and the total (`profile/playtime.json`) | totk, oot, odyssey, acnh, eow |
| `runtime/gamepad_layout.h` | Where the face buttons sit for each controller family; a HID pad's button order and stick reach | totk, oot, odyssey, acnh, eow |
| `runtime/hid_feedback.c`, `runtime/feedback_settings.h` | Rumble and motion on the game's side, their switches (`rumble.txt`, `gyro.txt`, the Input page) and the hid answers; Odyssey's Cappy (its second player) gets no host device, behind `GAME_ODYSSEY` | totk, oot, odyssey, acnh, eow |
| `runtime/motion_fusion.c` | Controller, phone and mouse motion as the console's 5 ms sensor states, with the gyro's bias learned at rest | totk, oot, odyssey, acnh, eow |
| `runtime/swkbd_host.c` | Text entry (`nn::swkbd::ShowKeyboard`) in a bar in the game window; its probe's addresses come from `GAME_ADDR` in a game with TotK's version table and are unmapped elsewhere | totk, oot, odyssey, acnh, eow |
| `runtime/ipc.{c,h}` | The in-process HIPC / CMIF / TIPC server, with CMIF domains (from Odyssey); each game's `threads.c` has `runtime_handle_take_session` | totk, oot, odyssey, acnh, eow |
| `runtime/time_host.c` | `nn::time` from the Mac's clock and time zone. Ocarina of Time keeps its own (its POSIX clock providers for the SDK's `clock_gettime`), and Animal Crossing keeps its own too (its extra `nn::time` answers). The other games' modules reference these names too, so sharing those answers would replace code those games run | totk, odyssey, eow |
| `runtime/bindings_diagram.h` | The key bindings drawings of the panel; what each action is called on a key, its kind and colour, the legend's words and the mouse's and right stick's captions are in each game's `runtime/bindings_game.h` | totk, oot, odyssey, acnh, eow |
| `runtime/shared_font.c` | The system fonts from `shared-fonts.txt` in the game data, decoded as the pl service gives them | totk, oot |
| `runtime/game_slot.c`, `runtime/agent_slot_limit.h` | One of these games at a time on the Mac (`MacNativeGames/TOTK/game-slot`), and agents' test games closing themselves | totk, oot |

Left in each game on purpose (2026-11-18): `runtime/keyboard_map.h` (each game's actions); `runtime/controller_map.h`
(Animal Crossing's and Echoes of Wisdom's mouse buttons belong to the keyboard layout, and Odyssey's worn-stick `reach`
lines are applied by its `gamepad.m`: both go with the input stack); `third_party/d2r` (its `patch_nso.py` holds each
build's tables, and TotK and OoT record its SHA-256 with their rewritten modules, so any change to it means rewriting
every game); and Odyssey's, Animal Crossing's and Echoes of Wisdom's `shared_font.c` (font rows from their generated
`shared_fonts_config.h`) and `game_slot.c` (their own slot folder: joining the TotK one would keep them from running
beside a Zelda game).

The Python those three ports' `build.py` and packagers share is not linked but imported from the repository, as
`build_app.py` is: `tools/port_app/` (`adapter.py`, MNG Launcher's side of `build.py`; `bundle.py`, the packaging steps).
`tests/test_app_launcher_shared.py` runs each game's launcher in a fake bundle; `tests/test_port_app.py` covers the Python.

When you change a file here, build and test every game that links it: `git grep` for the file name under `games/`, or
run `find games -lname '*shared/<path>'`. Game-specific behaviour belongs in the game's own files, or behind its
`GAME_*` switch when it cannot be separated.

Note: `git mv` keeps a file's old modification time. After moving a file here, `touch` it, or make will keep
the game's old object file.
