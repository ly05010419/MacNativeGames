# Ocarina of Time native port

The Legend of Zelda: Ocarina of Time for Nintendo Switch 2 (title `0400486024DEC000`, update `v131072`) as a native Mac app on Apple silicon and macOS 26. The game's ARM64 code runs natively after the repository's offline rewrite; the runtime in this folder supplies the Horizon / NVN services and renders through Metal 4. No CPU emulator is used, and nothing from the game is in this repository: you build the app from your own dump.

**Install:** [docs/install.md](docs/install.md) ([中文](docs/install.zh-CN.md)).

## Status (Mac, 2026-11-10)

- Plays on an M4 Mac from the title into Kokiri Forest and the village, with real saves read back on the next start. The whole game has not been played through.
- Frame rates, Kokiri village, standing still, M4, 100 Hz display, no frame lock:

  | Resolution (settings) | 3D picture | Upscaler: the game's | Upscaler: MetalFX |
  |---|---|---|---|
  | **720p (native), the default** | 1280×720 | 81.4 fps | 81.5 fps |
  | 1080p (from 720p) | 1280×720 | 80.8 fps | 83.4 fps |
  | 1080p (native) | 1920×1080 | 65.4 fps | 65.2 fps |
  | 2K (from 1080p) | 1920×1080 | 61.0 fps | 69.8 fps |
  | 2K (2560×1440 native) | 2560×1440 | 44.8 fps | 45.9 fps |

  The table was measured with the docked quality profile and the end-of-frame network running. In a busier place (about 2,200 draws a frame) at 1080p (native): 44 fps so, 52 fps with the handheld quality profile, 48 fps with the network skipped, 58 fps with both, which are the defaults now.
  Busier places and new areas are slower; the first visit to a place can stutter briefly.
- MetalFX frame interpolation works: at 50+50 on a 100 Hz display 595 of 600 game frames got an interpolated frame, about 3.7 ms of GPU each.
- iPhone and iPad: a build path exists (`--platform ios`) and is being tested; the install page does not cover it yet.

## Mods and save editing

See [mods and save editing](docs/mods.md) for Switch 2 RomFS compatibility, controller prompt packs, and offline save editing on the Mac.

## Settings

Key **1** opens the settings panel. The first page:

- **Resolution**, changed while the game runs: 720p (from 480p), **720p (the default)**, 1080p (from 720p), 1080p, 2K (from 1080p), 2K. The canvas is the picture and the window; the 3D is drawn inside it and upscaled once.
- **Upscaler**: MetalFX (the default) or the game's own network, for that one upscale.
- **End-of-frame network at native resolution**: skipped (the default) or running. At a native resolution the game still runs its upscaling network 1:1, about 2 ms a frame; skipped, the picture is copied to the canvas instead and has no anti-aliasing from the network.
- **Quality profile**, changed while the game runs: handheld (the default) or docked. The game's own handheld profile draws less vegetation; the resolution stays the one chosen above.
- **Frame lock**: 30 (the default), off, 40, 60, 72, 90, 120.
- **Frame interpolation (MetalFX)**: 30+30, 40+40, 50+50, 60+60 or uncapped. On a 100 Hz display, 50+50 adds one frame between every two game frames. Choosing it turns the frame lock off.

`defaults write org.macnativegames.oot TOTK_OPERATION_MODE handheld` starts the game in handheld mode with the handheld controller, whatever the quality profile row saved; `defaults delete org.macnativegames.oot TOTK_OPERATION_MODE` removes it. The environment variable of the same name wins.

## Shaders

A full build translates every shader of the game, compiles all of them into Metal libraries and builds a pipeline archive from the [shader list](assets/shader_list.tsv) (bind states and pipelines players recorded), so the game starts warm. A pipeline no one has recorded yet is built the first time it is drawn, a short stutter; the game writes it to `shader-list-contribution.txt` and reminds you to send that file in a GitHub issue. Those issues are merged into the list.

## Known issues

- A 4K canvas is not offered: at 4K with the 3D at full size the game crashes when it rebuilds its canvas.
- The 90 fps frame lock once stopped showing new pictures for 11.6 s while the game kept running; under investigation.
- A bright ring around the planting spots.

## Development

- Code: the runtime (`runtime/`), the GPU backend (`third_party/d2r_gfx/gfx/nvn/`), the rewriter and the build scripts (`scripts/`, `build.py`) live in this folder. Source provenance and licenses: [third_party/COPIES.md](third_party/COPIES.md). The shader service can be rebuilt from pinned sources with `tools/build_shader_service.py` in an external workspace.
- Test builds: `OOT_SHADER_SUBSET=listed` compiles only the shaders the shader list uses, much faster than all of them. Every test run started through `scripts/dev/game_slot.py` skips the title movie and shortens the Kokiri opening (`OOT_AUTO_SKIP_OPENING=1`); a player's start does not.
- Notes: [frame rate work](docs/fps-60-90.md), [shader preparation](docs/shader-preparation.md), [float atomics](docs/float-atomics.md), [time step](docs/time-step.md), [GPU profiling](docs/gpu-profiling.md), [validation history](docs/validation-history.md).
