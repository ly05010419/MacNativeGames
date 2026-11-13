# Fire Emblem Engage (community port)

Fire Emblem Engage **2.0.0** running natively on Apple silicon Macs, built from **your own copy** of the game. Unity /
IL2CPP's ARM64 code is rewritten ahead of time and runs directly on the Mac's CPU: no interpreter, no JIT, no CPU
emulator. System services are answered by a small native host, and the Switch's NVN graphics are rebuilt on Metal, with
the game's GPU shaders translated to Metal.

**No game content is in this repository.** You need a dump of your own game and keys from your own Switch.

This folder carries its own copy of everything it builds and runs with (runtime, graphics layer, rewriter, shader
translator, scripts), forked from `games/totk` through the Ori and the Blind Forest port; nothing outside `games/engage`
is read at build or run time.

## Status (13 November 2026)

Measured on this machine (Apple M1 Pro, macOS 27.2), from logs under `dist/logs/` in the workspace:

| Works | Details |
|---|---|
| Boot | SDK init, module constructors (main 379 / subsdk0 45 / sdk 112), `nnMain`, Metal + VI; first presented frame at 0.87 s |
| Picture | **Unity renders natively at 2560x1440** and the window is 1280x720 points at 2x, so the layer and the drawable are the same size (1:1, no scaling). The game creates its own 2560x1440 swapchain (3 textures); the host only reads their size |
| Frame rate | **60.0 fps** in every 20 s window after the first 20 s (warm-up 54); 5,130 frames / 88.3 s including warm-up |
| Movies | the opening movie decodes (VP9, 2400 frames, 2.14 ms per frame) and fills the window; the source is 1080p, so that segment is scaled to the canvas |
| Log | **0 `[RED]` lines** |

| Not verified yet | Details |
|---|---|
| Played past the intro | no human has gone title → menu → save → battle yet, so the UI layer, saves and battle effects are unconfirmed |
| Whole game | not played to the end |
| Online | not attempted |
| iPhone / iPad | no build |

## Build

Needs a Mac with Apple silicon and macOS 26 or later, the Xcode command line tools (Xcode's Metal toolchain,
`xcodebuild -downloadComponent MetalToolchain`, to compile shaders ahead of time; optional), Python 3.14, and from
Homebrew `opus libvpx`.

```sh
cd games/engage
python3.14 scripts/build_engage.py --game "<base game>.nsp" -k "<your prod.keys>" \
  --update "<2.0.0 update>.nsp" --firmware "<unpacked firmware NCA folder>" --out "<where the app goes>"
```

- The base game (`0100A6301214E000`) plus the 2.0.0 update (`0100A6301214E800`, v327680) are both required: the update
  carries the code and the DLC flag files, the base carries the data. `.nsz` inputs are converted on the way in.
- The code is checked against `assets/expected-v327680.json` (hashes only: `main`, `subsdk0`, `sdk`).
- Game data stays in this folder (`extracted/`, `rewritten/`, `build/`); `profile/` holds the game profile. Never share
  or commit any of it. A second build reuses every finished step.
- The app goes where `--out` says and carries everything it runs with. Saves, the shader cache and the log live in
  `~/Library/Application Support/MacNativeGames/Engage` and `~/Library/Logs/MacNativeGames/engage.log`.

**Shaders on a new Mac.** Building the app after playing puts the shaders and pipelines your play collected into it
(`build/shader-seed`, compiled ahead of time to `build/shader-libs`). Another Mac then imports and warms them at its
first start instead of translating them while you play. Areas not played yet are still translated the first time.

**Shader extraction** (`third_party/unity_shaders/`): nxnative's UnityFS reader and engine-wide shader splitter, taken
whole (15 modules) with four path fixes, because nxnative's `dm/` layout does not exist here. It reads the RomFS image
and `main` directly and produces the same four tables byte for byte (9,335 shaders: 4,637 vertex, 4,641 pixel, 56
compute, 1 geometry). Run it with `DM_GAME=0100A6301214E000 DM_GAMES_DIR=$PWD/profile`.

## What it needed beyond the Ori runtime

- **Services**:
  - `nn::ro`: the host has no `ldr:ro`, and running the SDK's `Initialize` as shipped reads a null session in
    `InitializeWithPortName` and crashes (`sdk+0x35b5b0`). Taken over; `LookupSymbol` resolves against the loaded
    modules; module loading fails honestly because this port has no rewritten NROs.
  - `nn::fs::IsMounted`: taken over instead of faking a `fsp-srv` service object, and answered from the real mount
    table (`rom:` via `runtime_romfs_owns`). The other 39 `nn::fs` imports were already covered.
  - `nn::prepo::Initialize`: a no-op, as upstream does for this game (the port already answers `Save`/`Add*`).
  - `nn::pctl`: taken over (`runtime/pctl_host.c`). Without it the SDK connects to a `pctl` service the host has not
    got and the game exits with `FAIL: IPC service not implemented` — the exact moment it leaves the prologue and draws
    the first battle map. `IsFreeCommunicationAvailable` answers 0 (no networking, no system UI).
- **Output resolution** (`third_party/d2r/patch_nso.py`, 13 cuts keyed to build `8c08b971…`): Unity's
  `Screen::get_width/height` and `Display::GetSystemExtImpl/GetRenderingExtImpl` all forward to one platform object's
  vtable (`+0xa8` width, `+0xb0` height). Two of the cuts lift that object's 1920/1080 caps, the rest change the size
  source that the swapchain and the WindowCrop share. Lifting the caps alone is not enough: the requested size is
  1080p, so both have to change, and the host is not the source (making the host's drawable 1280x720 still left the
  game building a 1080p swapchain).
- **Window** (`runtime/frontend.m`): the window content is fitted to `drawable / backingScale` (2560x1440 → 1280x720
  points) and `contentsScale` follows the drawable, so the layer and the drawable are never different sizes.
- **Movies** (`runtime/movie_host.c`, `nvn_mtl.m`): the game sizes its movie texture and quad from the video it parsed
  (1920x1080), so those frames arrive as 1080p content in the top-left of a 2560x1440 frame. Reported format changes do
  not move the quad (measured), so the present step draws the video rectangle and the existing aspect-preserving scale
  fills the canvas — a player's normal behaviour for a 1080p source.
- **Shader reflection** (`nvn_refl.m`, `nvn_mtl.m`):
  - a constant-bank read is a 16-byte row, so overlapping any filled table makes it fillable (Unity reads the c2+0x160
    row for texture slot 0 at 0x168 — the same rule upstream `dmsh_reflect.py` records);
  - a binding word that lands in the image region (Engage declares `cp_t_tcb_48` and `cp_i_cb2_48_` with the same word
    0x48) binds to that image's texture, and the sampler half to a dummy sampler, because an image entry has no
    sampler half and that sampler is declared but never sampled.
- **Rewriter**: the d2r rewriter takes Unity's `main` as it is; no x18 or tail-call changes were needed (unlike Ori).
- **NVN `nvnCommandBufferDownsample`** (`hle_nvn.c`, `nvn_obj.h`, `nvn_mtl.m`): the first battle map calls it as a
  **multisample resolve**, not a 2x2 box filter — measured 480x480 with 2 samples into 480x480 with 1, same format
  (Metal 115 = RGBA16Float, 92 = BGRA8_sRGB). The replay layer records it as a texture copy with an internal
  `NVN_COPY_DOWNSAMPLE` flag, decides which argument is the source by size, and the Metal side runs a resolve-only pass
  (`resolveTexture` + `StoreAndMultisampleResolve`), which the hardware performs exactly. A single-sample 2:1 branch
  (full-screen triangle, bilinear == box at 2:1) is implemented for the other half of the NVN semantics but this game
  never calls it.
- **Switch 2 Pro over USB** (`runtime/pad_switch2_usb_apple.m`, `runtime/gamepad.m`, `runtime/gamepad_layout.h`):
  the same chain as `games/totk` (interface 1 wake, its own run loop, 21-bit report), merged into this port's single
  `RuntimeGamepadState`. The axis handling is **copied whole** from `games/totk/runtime/gamepad_layout.h`
  (`gamepad_hid_reach`: per-direction reach since the pad connected, full at 0.85, never below 0.65). A hand-written
  `(v - 2048) / 1200` divisor instead — 1200 of a 2048-count travel — read a 59% push as full tilt, so one small push
  arrived as a held full deflection, the game's menu repeat fired several steps and the overshoot looked like an
  inverted stick. Do not substitute a fixed divisor for that helper.
  - **One press is one press**: the game counts *any* 0 -> 1 transition of a bit in the merged state as a fresh press, so
    a single report that disagreed with its neighbours made a held button fall to 0 and rise again and the menu repeated
    ("按一下变成连按"). Two guards: only the full 64-byte `id=9` report is decoded (a shorter one is a different format and
    is logged once per length), and a button change is published only when the next report repeats it (~4 ms at 240 Hz).
    Sticks are not debounced: `stick_axis`'s deadzone already covers their rest noise.
  - **Gate drift before the reach self-calibration, not after.** The pad rests off centre (measured 0.07..0.09 on the
    right stick); `gamepad_hid_reach` divides by a fraction of the travel until it has seen a full push, so that offset
    became 0.12..0.17 -- exactly on `stick_axis`'s 0.12 deadzone, flickering in and out of it and moving menus by itself
    ("漂移"). `sw2_drift_gate` (SW2_DRIFT 0.10) zeroes it first and re-maps the remaining travel, so a full push is still
    1.0 and the feel is unchanged.

No patch to the game's code beyond the 13 output-resolution cuts: it runs as shipped.

## Native 1440p (the resolution switch)

Default: the game renders at its own 1920x1080 and the host presents it into a 2560x1440 drawable through MetalFX
(`[mfx-up] 出图放大 … 1920x1080 → 2560x1440`). That path keeps the UI correct.

Native 1440p is the same feature behind one environment variable — 15 cuts on `main` (13 for the 3D canvas, 2 for the
UI/canvas render target, see `third_party/d2r/patch_nso.py` `ENGAGE_RES_CUTS`):

```sh
ENGAGE_OUTPUT_RES_2K=1 make rewrite      # 15 cuts, not 0
ENGAGE_OUTPUT_RES_2K=1 .venv/bin/python scripts/build_engage.py …
```

**Since 2026-11-13 the render resolution is a runtime setting instead** (`runtime/render_res.c`): the image keeps the
1440p instructions and the host rewrites those 15 immediates at module load from `renderResolution` (0 = 1080p,
1 = 1440p, 2 = 2160p), chosen in the F1 panel and applied on the next launch. Measured with one binary: the game builds a
1920x1080 / 2560x1440 / 3840x2160 swap chain respectively, all at the game's own 60 Hz cadence, VRAM 1788 / 1856 / 2118 MB.
The 2160p words are computed (`0x52800000 | ((imm & 0xffff) << 5) | Rd`, Rd decoded from the 1440p word) and the table
refuses to write a site whose current word is not one of the three levels.

The old build-time switch is still there for a clean image:

**The variable only acts in the rewrite step, and `build_engage.py` skips that step when the rewritten modules are
already there** — so setting it only on `build_engage.py` silently produces the 1080p build again (measured 2026-11-13).
Run `make rewrite` with the variable first, then check `rewritten/<version>/main.cuts.tsv` has 15 `^0x` lines before
packing.

## Output resolution and upscaling (what each index means)

`runtime/display_settings.h` `displaySizes[]` — the drawable size, chosen with F1 (or `defaults write
org.macnativegames.totk.display resolution -int N`):

| index | drawable | what happens | measured |
|---|---|---|---|
| 2 | 1920×1080 | game 1080p, 1:1, no MetalFX | 59.6 fps (1440p panel) |
| 3 | 2560×1440 | game 1440p (the cuts), 1:1, MetalFX has nothing to do | 58.0 fps |
| 4 | 3840×2160 | game 1440p, MetalFX spatial upscales 2560×1440 → 3840×2160 at present | see game note |

Frame rate ceiling on this machine is the active display's refresh (the 4K panel reports 60 Hz), and the game itself is
vSync-locked through Unity's `vSyncCount`; the host does not cap it.
