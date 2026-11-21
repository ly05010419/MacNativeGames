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
| `runtime/users.h`, `runtime/account.c` | Local users, one save area each (the User page); Odyssey's own account entries behind `GAME_ODYSSEY`; Breath of the Wild's and Mario Kart 8 Deluxe's (SDK 7 / 4.4) behind `GAME_BOTW` / `GAME_MK8DX`. Animal Crossing keeps its own single-user account.c: one island per console, see its settings_game.h; Echoes of Wisdom keeps its own too: its saves stay under their one account until moving them has been tried on a real profile | totk, oot, odyssey |
| `runtime/dsu_client.h`, `runtime/dsu_protocol.h`, `runtime/dsu_settings_rows.h` | DSU (cemuhook) motion and input over UDP, and its input-page rows. Ocarina of Time links the client and the protocol for the shared `gamepad_core.h` but has no rows, so DSU stays off there | totk, odyssey, acnh, eow (oot: no rows) |
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
| `runtime/offline_host.c` | The offline console's answers (nifm, ldn, friends, sockets, SSL, play reports, the profanity filter, `oe` exit sections, display version and pseudo device id); moved here from games/totk on 2026-11-21 | totk (its Switch 2 games, `GAME_NS2`), mk8dx |
| `runtime/time_host.c` | `nn::time` from the Mac's clock and time zone. Ocarina of Time keeps its own (its POSIX clock providers for the SDK's `clock_gettime`), and Animal Crossing keeps its own too (its extra `nn::time` answers). The other games' modules reference these names too, so sharing those answers would replace code those games run | totk, odyssey, eow |
| `runtime/bindings_diagram.h` | The key bindings drawings of the panel; what each action is called on a key, its kind and colour, the legend's words and the mouse's and right stick's captions are in each game's `runtime/bindings_game.h` | totk, oot, odyssey, acnh, eow |
| `runtime/shared_font.c` | The system fonts from `shared-fonts.txt` in the game data, decoded as the pl service gives them | totk, oot |
| `runtime/game_slot.c`, `runtime/agent_slot_limit.h` | One of these games at a time on the Mac (`MacNativeGames/TOTK/game-slot`), and agents' test games closing themselves | totk, oot |
| `runtime/gamepad_core.h`, `gamepad_settings.h`, `gamepad_hid_pad.h`, `gamepad_mouse.h` | The controller plumbing under every game's `gamepad.m`: the one-word settings files (`runtime_setting_word` / `_save` / `_remove`), the layout file (a bad line leaves the default instead of stopping the game), the controller type, the settings panel's and the screenshot's buttons, background input, GameController's controllers and their physical buttons, the Options button without the system's delay, the generic HID pads, a Switch controller's HID sticks and capture button, mouse mode, the display kept awake while a controller is used, the battery (a Joy-Con's from its HID reports), iPad keyboards, and the start with GameController's handshake off the main thread. What a game adds is in its `runtime/gamepad_game.h`: whether it has mouse mode, and the hook groups it uses (Tears of the Kingdom's live button prompts, ability wheel pointing and zeldaVR's chord tap and Quest pads; Odyssey's worn-stick reach; Ocarina of Time's input trace); a group a game leaves out does nothing | totk, oot, odyssey, acnh, eow |
| `runtime/gamepad.m` | One player's controller input on top of `gamepad_core.h`: every controller, HID pad, DSU pad, the mouse's buttons and the touch controls together through the layout (hold, tap, toggle, aim), the settings chord and the screenshot hold. Odyssey keeps its own `gamepad.m` on the same core, with two seats for Mario and Cappy | totk, oot, acnh, eow |
| `runtime/gamepad_feedback.m` | The controller's rumble (Core Haptics, a Switch Pro Controller's or a Joy-Con pair's HD rumble, a profile pad's HID motors) and motion (GCMotion, a Switch controller's or a generic pad's HID sensor). A game's `runtime/feedback_game.h` says whether rumble and motion stay with the first controller that can take them (Odyssey: its gamepad.m offers Mario's controller) or go to the one attached last, and adds its own: Tears of the Kingdom's and Breath of the Wild's DualSense bow triggers | totk, oot, odyssey, acnh, eow |
| `runtime/hid_imu.h`, `runtime/switch_left_fallback.h` | The six-axis profiles of pads whose motion only their HID reports carry (8BitDo Ultimate 2, DualShock 4); a Switch controller's sticks from its HID reports when GameController's stays at zero | totk, oot, odyssey, acnh, eow |
| `runtime/mouse_look.h` | Mouse movement as the right stick and the gyro (mouse mode), and pointing | totk, oot, odyssey |
| `runtime/controller_map.h` | The controller layout file (`controls.txt`): the physical buttons with the mouse's, modes, the screenshot hold. Odyssey's keeps its `reach` lines, and Animal Crossing's and Echoes of Wisdom's have no mouse lines (their mouse belongs to the keyboard layout), so those stay their own | totk, oot |
| `runtime/input_file.h` | Scripted input: `TOTK_INPUT_SCRIPT`, and the `TOTK_INPUT_FILE` command stream with the commands every game has (keys, screenshots, capture, text entry, flags, traces, close), read on when the file is replaced. Each game's `runtime/input.c` keeps its key table and HID answers and gives its own commands and flags (`input_game_command`, `input_game_flag`) | totk, oot, odyssey, acnh, eow |
| `runtime/gate.S`, `runtime/thread_enter.S`, `runtime/thread_compat.h`, `runtime/hle_stack.c` | The host-call gate the rewritten code's `svc` and timer reads enter (every register kept), a guest thread's first entry, the thread code's log and clock names, and host functions wrapped onto the guest thread's host stack (with `hletrace`). Each game's `hle_stack.S` stays its own: TotK's and OoT's pass stack arguments beyond the eighth (48ea32ba, for DKB), which would add instructions to every host call of the other games | totk, oot, odyssey, acnh, eow |
| `runtime/audio_effects.{c,h}`, `runtime/audio_reverb.{c,h}` | The audio renderer's effects (delay, I3DL2 reverb, biquad, limiter and the rest) and the reverb, as Ryujinx.Audio computes them | totk, oot, odyssey, acnh, eow |
| `runtime/audio_ramp.h` | The volume ramp and depop arithmetic (`tests/test_audio_ramp.c`). Whether a game ramps is its own `audio_mixer.c`'s: Breath of the Wild under `GAME_BOTW`, Odyssey always | totk, oot, odyssey |
| `runtime/memory_probe.c` | The `probe-memory` mode: stack moves and memory accounting checked through the production SVC path | totk, oot, odyssey, acnh, eow |
| `runtime/romfs_path.h` | The RomFS path checks of `romfs.c` (`tests/test_romfs_paths.c`). Ocarina of Time keeps its own, with `//` inside a path collapsed | totk, odyssey, acnh, eow |
| `runtime/text_keyboard.h` | The Mac's text entry keyboard that a controller can operate (`frontend.m` includes it) | totk, oot, odyssey |
| `runtime/freeze_watchdog.c` | `ODYSSEY_FREEZE_WATCHDOG`: what the game's threads do when frames stop; it only logs | odyssey, acnh |
| `third_party/d2r_gfx/gfx/tex/*` but `nvn_deswizzle_gpu.{m,h}`, `third_party/d2r_gfx/third_party/bcdec/*` | Texture decoding: the block-linear deswizzle (the Switch 2's GOB order under `GAME_NS2`), the BCn decoder and the BCn to ASTC/EAC transcoder for GPUs without BC, the NVN to Metal formats (Ocarina of Time's depth reads under `GAME_OOT`) | totk, oot, odyssey, acnh, eow |
| `third_party/d2r_gfx/host/{dm,front,hle}.h`, `gfx/nvn/include/{nvn.h,mach_vm_compat.h}`, `gfx/nvn/nvn_impl.h`, `nvn_entry.S` | The graphics layer's host interfaces, NVN's types, and the entry thunk every NVN call goes through | totk, oot, odyssey, acnh, eow |
| `third_party/d2r_gfx/gfx/nvn/` `nvn_arg_address_cache.h`, `nvn_copy_alias_probe.h`, `nvn_cover.h`, `nvn_cube_group_probe.h`, `nvn_depth_clear.h`, `nvn_diag_range.h`, `nvn_encode_fifo.h`, `nvn_hmap.h`, `nvn_pace.h`, `nvn_pool_index.h`, `nvn_pool_retire_probe.h`, `nvn_tex.{c,h}`, `nvn_tex_slots_probe.h`, `nvn_texture_plan.h`, `rt_range_check.inc`, `totk_serial_set.h`, `totk_vpool.{c,h}` | Pieces of the graphics layer that are the same for every game: the hash map (8-byte keys compared as one word), pool and range indexes, the encoder FIFO, frame pacing, the texture descriptor, the virtual pool, and their probes | totk, oot, odyssey, acnh, eow |

The platform layer that runs the game (2026-11-18, step 4) is shared only where the copies were already the same file
(above); each game's copy of the rest stays its own, because the copies carry that game's tuning or answers:
`threads.c` (TotK's asynchronous preemption, core migration and spin gates, OoT's core-wait diagnostics, Odyssey's and
Animal Crossing's starvation rule and core watch; Echoes of Wisdom's is an older TotK copy, and taking TotK's would turn
on TotK's scheduling there); `fs_host.c` (each game's save layout and answers, OoT's drive waits off the core, b1b65908)
and `fs_host.h`; `romfs.c` (each game reads its data its own way); `audio_mixer.{c,h}`, `audio_renderer.c` and
`audio_out.c` (end-of-stream behaviour, ramps and depop, and the warm-up hold differ per game); `opus_codec.c`
(Animal Crossing's extra overload); `loader.c`, `services.c`, `system_services.c`, `applet_host.c`, `movie_host.{c,h}`
(per-game SDK starts, heaps, service answers and movie codecs); `autoload.{c,h}` and `game_patches.h` (headers of
per-game code); `world_survey.{c,h}`, `gfx_bridge.c`, `gfx_probe.c`, `vpool_probe.m`, `gpu_path_probe.m` (each follows
its game's own graphics layer or diagnostics); `audio_renderer.h` (with its renderer). Where Odyssey, Animal Crossing and
Echoes of Wisdom have the same older copy, sharing it would only freeze it.

Left in each game on purpose (2026-11-18): `runtime/keyboard_map.h` (each game's actions); Odyssey's, Animal Crossing's
and Echoes of Wisdom's `runtime/controller_map.h` (above); Odyssey's `runtime/gamepad.m` (its two seats, layout per seat
and stick reach dominate the file; it uses the shared core); each game's `runtime/input.c` but its scripted input (the key
table, the HID answers and Odyssey's two players are the game's); `third_party/d2r` (its `patch_nso.py` holds each
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

## Why the shader translator is not shared

Each game pins its own build of `prebuilt/macos-arm64/totk_xlate`. Every one of these is a copy of `games/totk`'s at an
earlier commit (EoW at d58cde0b, Odyssey at 92957c94, ACNH at f3e34052), and none carries a fix of its own. They were
compared on 2026-11-18 by translating every seed variant of Odyssey (31,228), ACNH (7,937) and EoW (10,091) with both
the game's translator and TotK's. All of TotK's outputs compiled. Still, 91%, 86% and 97% of them differ in meaning:
`fast::` min/max/clamp become `precise::` (NaN behaviour), some control flow is restructured, and for EoW there are
also vertex-output initialisation, attribute types and the depth-sampling rewrite. A game therefore keeps its own
translator until someone has checked its picture with TotK's. A switch needs:
- the game's shader libraries rebuilt;
- its seed translated again (Odyssey's and EoW's express updates do not do this by themselves);
- a runtime cache identity bump, because their `rt-totk-native-v4-unverified` cache has no translator in its name.

## The graphics layer (step 6)

On 2026-11-18 the graphics layer (`third_party/d2r_gfx`) was shared where it changes nothing a game runs: the files
that were already the same in TotK, Odyssey, Animal Crossing and Echoes of Wisdom, and seven where TotK's copy only
adds code under `GAME_NS2` / `GAME_OOT`, a query nobody else calls, or a faster equal lookup (`nvn_deswizzle.c` and
`nvn_fmt_mtl.c` compile to the same instructions for the three). The rest stays each game's own:
- `hle_nvn.c`, `nvn_mtl.m`, `nvn_refl.m`, `nvn_obj.h`, `nvn_dirty.c`, `totk_shader_rt.m`, `totk_mfx.m`, `totk_mfx_mode.h`,
  `totk_nvn_ext.c`, `totk_timeline.{c,h}`, `totk_shader_seed.h`, `totk_contrib_upload.h` and the rest that differ in
  every copy: each game's rendering work, its present and MetalFX choices, its caches and its probes.
- `nvn_protos.h`, `names.txt`, `nvn_count.h`, `nvn_thunks.S`, `nvn_mtl.h`, `nvn_pso_skey.h`: made for the NVN functions a
  game's layer implements (TotK 739, the three 523). Sharing them means taking TotK's whole layer.
- `dmsh_chain.h`: the game's shader cache identity (TotK `totk-native-v9-unverified`, the three `v4`).
- `nvn_rtex.inc`, `nvn_cube_group.h`: TotK's count their Metal compiles through `nvn_counted.h`, which the others lack.
- `nvn_deswizzle_gpu.{m,h}`, `totk_lifelog.{c,h}`: TotK's add the Switch 2's GOB order, a region kernel and OoT's
  texture lifetime trace.

`nvn_tex.c` (the texture sizes) is shared too, since 2026-11-18: TotK's 3c482a7d (cube array sizes, a packaged 3D
texture's block depth, linear BCn sizes in pixel rows, all measured against the original driver) now applies to the three,
with its two lines of `nvn_mtl.m` in each game's own copy (Metal's cube array length is depth / 6 cubes; the upload does
not multiply the layers by six). Up to the title screen, only Animal Crossing has such a texture: two cube arrays of one
cube each (4x4 and 256x256), whose storage was six times too large; its title screen looks the same.
