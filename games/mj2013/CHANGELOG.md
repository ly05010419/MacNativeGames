# 明星三缺一 2013 — changelog

## 2026-11-22 (later) — one core no longer busy; the Mac app

**CPU.** The game's port runs its x86 emulator in slices and asks audout:u for released buffers after every slice
(main+0x560 → main+0xc3c0, libnx `audoutGetReleasedAudioOutBuffer`), never waiting for the buffer event: ~50 000 asks a
second (`HB_IPC_STATS=1`), one host core at 94 %. Profiled (`sample`): most of the thread is real work (the native sprite
blitter), but between pieces of it the asks come in runs of 16-256 within 10 us of each other. The shared
`audio_out.c` now holds such a run (16 empty answers in a row, each within 10 us) until a buffer is played or 1 ms
(`TOTK_AUDOUT_POLL_WAIT_US`, 0 = the old answer-at-once), giving the virtual core up meanwhile; a client that asks once
per frame or works between asks is answered as before. A wait after *every* empty answer closer than 250 us was tried
first: the core fell to 8 % but the game never reached the title in 45 s (it starved the emulator) — so the run rule.

| Measured (headless, Core Audio, temp profile; CPU = cputime delta / wall time) | before | after |
|---|---|---|
| title (14-24 s) | 93.6 % | 36.0 % |
| character select | 93.3 % | 31.0 % |
| the table, a hand being played (20 s) | 94.0 % | 22.9 % |
| frames at the end, display rate | 3085, 43.7 fps | 2967, 42.8 fps |
| underruns logged (new: `[totk-audio] AudioOut underrun`) | 0 | 0 |

The same screens at the same times (title at ~20 s, the table with the same deal). `tests.test_mj2013` drives audout
end to end: two 10 ms buffers come back in order after their play time with 274 asks instead of 133 939, and a single ask
after a pause is answered at once.

**The app.** `make dist` → `明星三缺一.app` (`scripts/package_app.py` on `tools/port_app/bundle.py`, `app/launcher.c` on
`shared/app/app_launcher.h`, as Echoes of Wisdom): runtime, rewritten main and RomFS image inside, checked against
`assets/expected.json`, signed ad hoc as `org.macnativegames.mj2013`, `--check` asked. Data in
`~/Library/Application Support/MacNativeGames/mj2013` (the runtime's own default); the SD card is backed up before each
start (`launcher_play`'s new `saves` field). Started from the bundle with `MJ2013_DATA_DIR` in a temp folder: the title
at ~20 s, 开新游戏 → the mode menu, `close` → exit 0, the signature still valid. `tests.test_mj2013.App` runs the
launcher in a fake bundle. No icon yet (`ICON=` takes one).

## 2026-11-22 — first boot on the Mac: title, menus, a game of mahjong

**What the game is.** Title 01004D4A53510000 ("StarMahjong" in its NPDM) is a libnx homebrew, not a Nintendo SDK game:
ExeFS holds one 1.3 MB `main` with MOD0 and no imports. Inside is a Unicorn 2 (QEMU 5 TCG, aarch64 backend) x86 emulator
that runs the original Windows game (`game/MJ2013.exe`, its DLLs and data in the RomFS) with Win32 / DirectDraw / GDI /
MCI answered natively; it draws in software (an 800x600 DirectDraw screen scaled into a 1280x720 libnx framebuffer).
Checked: strings (`uc_open`, `tcg/aarch64/tcg-target.inc.c`, `Install native sprite blitter`), the RomFS listing
(`scripts/prepare_mj2013.py` → `extracted/v0/romfs/index.json`), the SVC list of the text (31 wrappers, among them
CreateCodeMemory / ControlCodeMemory).

**The Zelda line runs it.** The rewriter, gate, threads and IPC server of games/totk / shared are used as they are
(third_party/COPIES.md). What a homebrew needs that an SDK game does not, and how each was checked:

| Found | Done | Checked by |
|---|---|---|
| libnx links without unwind tables: MOD0's `eh_frame_hdr` is empty, the rewriter stopped (KeyError) | the rewriter treats an empty table as "no function boundaries": every site a window of its own | `tests.test_mj2013.Rewriter` (a synthetic libnx module), the game's rewrite (117 x18, 6 cond, 195 tpidrro, 32 cntpct, 31 svc sites) |
| libnx's crt0 keeps data in the text: `LNY1` (MOD0's extension) decodes as `adds w12, w18, ...` and was rewritten into a branch | words 4..15 after `HOMEBREW` and MOD0 with its `LNY` records are skipped | the same test: the records come out unchanged |
| libnx relocates itself (RELR: `*p += base`) | the loader applies no relocation | the game boots; a second application would double every pointer |
| libnx's virtual memory picks random 4 KiB pages; on Apple silicon an alias needs 16 KiB pages at equal offsets (the JIT's writable and executable views) | game patch: the random page index scaled to 16 KiB (`lsr #14`, `lsl #14` at main+0x95e5c / 0x95ea0) | `tests.test_mj2013.GamePatches` (encodings; the words in the player's main); ControlCodeMemory log shows 16 KiB aligned owner/slave |
| the JIT: CreateCodeMemory + MapOwner (RW) + MapSlave (RX) of one memory | one host backing remapped at both addresses (`mach_vm_remap`, copy = FALSE); a word written through one view is read back through the other at map time | the game's JIT runs; `tests/jit_cache_probe.c` |
| stale instructions (SIGILL on a valid `mov w20, #0x180` in the code cache): the guest's emulated `CTR_EL0` reported 128-byte lines, so libnx's `dc civac` / `ic ivau` loops skipped lines on an M4 | `CTR_EL0` reports 32-byte lines | `tests/jit_cache_probe.c` (4000 rewrites, none stale; 128-byte steps fail on the M4) |
| services reached at start: sm, set:sys, apm, appletOE (domain), hid, time:u, fsp-srv (domain), vi:m, nvdrv, audout:u | answered over IPC (`runtime/hb_*.c`; audout:u is the shared `audio_out.c`); firmware 5.1.0 is reported so libnx takes the CMIF sm and the time services without shared memory | `tests.test_mj2013.Probes.test_ipc_answers` (end to end through ipc.c), the boot log |
| the screen: libnx's framebuffer (two 1280x720 RGBA8 slots, block-linear, 16-GOB blocks) through IGraphicBufferProducer on the binder relay; the graphic buffer's plane sits at ints[13] (no padding) | QueueBuffer converts the slot (shared `nvn_bl_offset`) and shows it; 60 Hz pacing | `tests/fb_probe.c` (against libnx's own swizzler), the IPC probe (a picture drawn into nvmap memory arrives at the window), screenshots |
| the SD card: the game keeps `sdmc:/switch/mj2013/save` as an overlay of its RomFS files (`update.rom`, `update.tmp`, `*.bak`) | the card is `<profile>/sdmc`; `..`, `.`, empty components and backslashes refused | `tests/sdmc_probe.c`, the IPC probe (`update.rom` written there) |
| input: libnx reads hid's shared memory LIFOs | touch, mouse, keyboard and the controller (player 1 and handheld) published every 5 ms (`hid_publish.c`) | `tests/hid_probe.c` (libnx's `_hidGetStates` reads them back) |
| every action on the title (a button, Return) seemed to hang: frames stopped while the game ran on | the port's present decision (main+0xa6e0) shows a new picture only when its DirectDraw frame counter moves; the menus after the title draw through GDI (`SetDIBitsToDevice` in ten 800x60 strips, MJ2013.exe 0x41a073) and never move it. Game patch: the `b.eq` at main+0xa7d0 removed, so a picture is shown whenever 16.6 ms passed | screenshots of the mode menu, the class menu, character select and the table (below); found with `hbregs` (guest registers), the IPC statistics and a forced present |
| the game prints nothing visible (libnx sends stdout to `stdnull`) | before start, `stdnull`'s write_r points at a host function: guest output reaches the log as `[guest]` | — (the game prints nothing in normal play) |

**Evidence (screenshots through `TOTK_INPUT_FILE` `shot`, a temp profile, headless and windowed):** the title with its
menu at ~20 s after start (30 fps, the game's own rate); "开新游戏" → the mode menu (明星对战 / 故事模式 / 冠军争霸 /
明星教学); 明星对战 → 初级班 … 专家班; 初级班 → 选择角色 with the game settings; 确定 → the table, the deal, the AI
players' turns with their speech bubbles; a tap on a tile in the player's hand discards it (中 in the discards, the hand
sorted again). Windowed with Core Audio sound; `close` ends the run with status 0.

**Diagnostics kept** (environment / `TOTK_INPUT_FILE`): `HB_TRACE_IPC=1` every request's handle and command, binder
transactions, file opens and reads; `HB_IPC_STATS=1` requests per (handle, command) every 2 s; `hbregs` the guest main
thread's registers and call chain; `hbfind <text>` / `hbdword <hex>` text or a word in the game's heap; `TOTK_TRACE_SVC`.

**Known.** One core stays busy: between emulation slices the port polls audout's released buffers (about 40 000
requests a second, `HB_IPC_STATS=1`); a short host wait there would save power but belongs to the shared `audio_out.c`.
`close` in a headless run (`TOTK_HEADLESS=1`) now ends the process (there is no run loop to end).

**Not yet.** The iPhone / iPad: the game generates ARM64 code at run time (Unicorn's JIT), which iOS does not allow
an app to execute; see README. An app bundle and a Launcher card. Long sessions and saves across restarts are untested.
