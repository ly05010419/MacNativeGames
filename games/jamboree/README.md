# Super Mario Party Jamboree — experimental native macOS target

[中文说明](README.zh-CN.md) · [Rendering fix](docs/rendering-fix.md) · [Validation](docs/validation.md) · [Source copies](third_party/COPIES.md) · [Licenses](THIRD_PARTY_NOTICES.md)

This directory contains an independent Apple silicon port of **Super Mario Party Jamboree 2.3.0**, title ID `0100965017338000`, update `v458752`. The opening, character selection and plaza have been exercised locally. This is an **experimental bring-up**, not a claim that every board, minigame, controller or multiplayer mode works.

The game's ARM64 instructions execute natively after an ahead-of-time rewrite. The host supplies the required services and maps NVN to Metal. There is no CPU interpreter, CPU JIT or dynamic CPU translation. Metal compiles shader source through `MTLDevice`; that GPU compilation is separate from native CPU execution. The build-time Ryujinx dependency contains the shader translator only, not its CPU or operating-system emulation.

The character geometry fix executes vertex storage writes when rasterizer discard is enabled and transform feedback is disabled. It does not hide triangles, skip character meshes or change character state. See the [root cause and regression test](docs/rendering-fix.md).

## Build

Use an Apple silicon Mac with Metal 4 support, macOS 26 or later, and Xcode with the macOS 26 SDK or later. This target is macOS only; it does not expose an iOS build. The deployment target is macOS 26; validation was on macOS 27.0.1 (26A434). macOS 26 itself still needs validation.

Build dependencies:

- Python 3.11 or later with `lz4`, `capstone`, `fonttools`, and `backports.zstd` when Python is older than 3.14.
- .NET SDK **10**, `make`, `clang`, `zstd` and `spirv-cross` on `PATH`.
- A **local LibHac 0.19.0 NuGet package** supplied with `--libhac-package`. SHA-256: `1431267861db5ff68216315b164f10c0edab4df162cedf5429bf8a143b76de19`. This is an explicit build prerequisite, like .NET and Python packages; it is not redistributed here. An existing local package/cache can supply it. The builder never downloads it and rejects a missing or mismatched package before extraction.
- The shader translator is included as **binaries only** under `prebuilt/`, with SHA-256 checksums and upstream source provenance. Duplicate translator sources, debug symbols and Humanizer satellite resources are omitted. libvpx remains source-only because it is the host's movie decoder, not a translator dependency.
- Your own unmodified base NSP or XCI, the **2.3.0 update NSP**, and your own `prod.keys`. These are private build inputs; this repository supplies none of them. Compressed inputs, other updates, DLC and iOS are outside this target's supported scope.

Put the base and update in one directory. The inventory identifies the title through the container ticket or a `[title ID]` tag in the filename; the update also needs `[v458752]`. The executable build IDs and full NSO SHA-256 values are checked after extraction, before rewriting. A filename alone cannot pass those checks. See [expected-2.3.0.json](assets/expected-2.3.0.json).

From the repository root:

```sh
python3 games/jamboree/scripts/build_jamboree.py \
  /path/to/your/Jamboree-inputs \
  --keys /path/to/your/prod.keys \
  --libhac-package /path/to/local/libhac.0.19.0.nupkg \
  --work-dir /path/to/private/Jamboree-build \
  --output '/path/to/private/Super Mario Party Jamboree.app' \
  --language en-US
```

Use `--language zh-Hans` for Simplified Chinese. The builder checks the game's supported languages and records the selection in the app. Give a new output path for each build; an existing app is never replaced. Both the work directory and app output must be outside the Git checkout. Allow at least **25 GB free**, more when keeping several app builds. The first shader build takes several minutes and the packaging step copies the private game data into the app.

Alternatively:

```sh
make -C games/jamboree build \
  INPUTS=/path/to/your/Jamboree-inputs KEYS=/path/to/your/prod.keys \
  LIBHAC=/path/to/local/libhac.0.19.0.nupkg WORK=/path/to/private/Jamboree-build \
  OUTPUT='/path/to/private/Super Mario Party Jamboree.app' LANGUAGE=en-US
```

The build is offline once the listed prerequisites are installed. The wrapper hash-checks the local LibHac package, copies it into `<work>/dependencies/nuget`, and restores `nx_extract` using only that local feed and an explicit NuGet configuration that clears inherited feeds/fallbacks. NuGet auditing is disabled, and the following compile uses `--no-restore`. There is no download fallback. An empty NuGet cache works; an absent local package fails with a clear error. The generated app does not contain LibHac or any .NET tools.

The builder stages every runtime source, script and compiler inside `<work>/source`. It never reads another game directory. Original containers are linked into `<work>/input`; extracted files, rewritten modules, logs, shader packages and caches stay under `<work>/input/0100965017338000_build`. .NET and task caches stay under `<work>/cache` and `<work>/tmp`. A repeat build uses the existing stages' fingerprints. Do not edit generated binaries; change the declared source rules and rebuild from the original inputs.

No offline Metal compiler is needed in the selected runtime shader mode. The translator DLLs must match their lock; a different installed SPIRV-Cross is reported by the existing compiler adapter. The built app needs neither Python nor .NET. It is locally ad-hoc signed and verified; it is not notarized or distributed here.

## Launch, controls and saves

Open the generated `.app`. Keyboard defaults:

| Keyboard | Game input |
|---|---|
| WASD | Left stick |
| Arrow keys | D-pad |
| J / K / U / I | A / B / X / Y |
| Q / E / Z / C | L / R / ZL / ZR |
| Return / Tab | Plus / Minus |
| F / G | Left / right stick click |

The macOS menu exposes face-button layout (`auto`, Nintendo labels, Xbox/PlayStation positions), A/B swap and full screen. `auto` recognizes Nintendo-style controller names; a manual choice is useful when a controller reports a generic name. These settings persist in `controller-layout.txt`. Keyboard inputs are unaffected by controller face-button remapping. Physical button presses, motion input and multi-controller play have **not** been verified for this contribution.

Press **F1** or **⌘,** to open the dark in-game settings panel (also available in the app menu). A compact shortcut HUD appears in the upper-right corner:

| Shortcut | Action |
|---|---|
| 1 | Show / hide shortcut hints |
| 3 | Cycle the output cap: automatic, 720p, 1080p, 1440p, 2160p |
| 6 | Cycle automatic / position / Nintendo-label controller mapping |
| ⌃⌘F | Enter / leave full screen |
| Escape (settings panel) | Close settings and return to the game |

The panel applies output, fullscreen and button changes immediately and saves output/hint/controller preferences beside the save directory. The startup-fullscreen checkbox takes effect at the next launch. Existing three-key `controller-layout.txt` files remain compatible. Opening settings clears held keyboard input so it cannot stick after the window changes focus.

**Output caps affect presentation only.** The drawable keeps the window's aspect ratio and never exceeds the window's native pixel dimensions. A 1080p cap on a 4K full-screen view yields 1920×1080 presentation; it does not change the game's internal render targets or provide true higher-resolution rendering. Original picture quality is the only implemented quality mode. MetalFX temporal anti-aliasing is visibly disabled and labeled as unsupported. Controller layout choices change face-button mapping, not the HID device into a single/dual Joy-Con.

Private user state lives in:

```text
~/Library/Application Support/Super Mario Party Jamboree/
  save/
  temp/
  controller-layout.txt
  last-launch.log
```

Rebuilding the app preserves that directory. Back up `save/` before testing a new runtime or changing game versions. The app writes its runtime caches and log outside the signed bundle. `Contents/Resources/game/build-info.json` records the input hashes, rewrite rule hashes, shader chain, shader-input stamp, language and host hash for the build.

## Tests and status

```sh
python3 -m unittest discover -s tests
make -C games/jamboree test
make -C games/jamboree test-gpu WORK=/path/to/private/Jamboree-gpu-tests
make -C games/jamboree test-ui WORK=/path/to/private/Jamboree-ui-tests
```

The first command is the repository's existing test suite. The game tests use synthetic inputs; the GPU tests need a Metal-capable Mac, but no game data or keys. The UI target requires a macOS GUI session and exercises the actual AppKit controls and output sizing in Chinese and English without game data. Tests do not replace playing the game. See the [validation record](docs/validation.md) for the precise source build, observed scenes, shader failures and untested modes.

## Layout

| Path | Purpose |
|---|---|
| `runtime/host` | Native loader, service layer and Cocoa/GameController front end |
| `runtime/gfx` | NVN/Metal layer, shader tools and native GPU regressions |
| `runtime/tools` | Original NSO reader, AOT rewriter, extraction tool and font builder |
| `runtime/games/0100965017338000` | Version-gated profile, asserted patch, shader keys and bind metadata |
| `runtime/third_party` | Source-only runtime dependencies (including the required VP9 movie decoder) |
| `prebuilt/macos-arm64` | Locked shader translator tooling and checksums; no game shaders |
| `scripts`, `app`, `assets`, `tests` | Portable build, bundle launcher, input pins and synthetic tests |
| `third_party/nxnative` | Pinned upstream provenance, local patch record and build orchestrator |

The source snapshot is based on nxnative commit `7861e7c1a9913e020600b1cc7f65132427e9cc50` (MIT), with the changes documented in [COPIES.md](third_party/COPIES.md). Do not update these copies implicitly when another game's runtime changes.
