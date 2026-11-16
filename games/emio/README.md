# Emio – The Smiling Man: Famicom Detective Club — experimental native Mac port

**Status: experimental / first boot.** Isolated Apple silicon port of **Emio** (`0100A9501759E000`, base v0 / 1.0.0). The game's ARM64 code is rewritten ahead of time and runs through a native NVN-on-Metal host. **Mac only.** Not a claim of complete gameplay, every ending, or save compatibility.

**No game content is in this repository.** You need a legal dump of your own base-v0 NSP or XCI and keys from your own Switch.

## Identity

- Title: Emio – The Smiling Man: Famicom Detective Club
- Title ID: `0100A9501759E000`
- Version: 1.0.0 (base v0 only)
- Publisher: Nintendo
- SDK: 16.2.6.0
- Engine: MAGES custom (`magesequence`); not Unity
- Audio: Nintendo Opus (`hwopus` / `ninopus`)
- Video: WebM / VP9 (`rom:/movie/webm/…`)
- RomFS: `.mpk` packs, `.cls` / `.lkm` / `.lka` scripts, fonts

## What was vendored

Copied from `games/tomodachi` at MacNativeGames `3edfb0b6` (2024 SDK host, opus, libvpx). Provenance: [third_party/COPIES.md](third_party/COPIES.md). The donor's Mii/cloth/pool cuts were not kept. WebM playback uses the inherited `movie_host` + libvpx path.

## Prepare and build

Run from the repository root. Put game data and the host **outside the checkout**.

```sh
python3.14 -m venv /path/to/Emio_build/venv
/path/to/Emio_build/venv/bin/python -m pip install -r games/emio/requirements.txt
/path/to/Emio_build/venv/bin/python games/emio/prepare.py \
  /path/to/your-game.nsp --keys /path/to/prod.keys \
  --work /path/to/Emio_build/prepared --romfs
```

Shared fonts come from your own firmware (or a game card's system update):

```sh
/path/to/Emio_build/venv/bin/python games/emio/tools/extract_shared_fonts.py \
  --xci /path/to/a-game-card.xci --keys /path/to/prod.keys \
  --output /path/to/your-shared-fonts
```

```sh
/path/to/Emio_build/venv/bin/python games/emio/build_host.py \
  --prepared /path/to/Emio_build/prepared \
  --work /path/to/Emio_build/host \
  --shared-fonts /path/to/your-shared-fonts \
  --spirv-cross /path/to/spirv-cross --backend metal4
```

Apple silicon, macOS 26+, Metal 4, and an externally built SPIRV-Cross (`aa217ae`) are required. opus and libvpx come from `tools/deps`.

`make -C games/emio test` is not wired; use `games/emio/tests/run_tests.py`.

The profile is `~/Library/Application Support/MacNativeGames/Emio/profile`.
