# Minecraft Dungeons — experimental macOS native port

Apple Silicon runtime for title `01006C100EC08000`, update `01006C100EC08800` (1.17.0.0).
**Experimental, not release-validated:** a development worktree runtime has entered
an actual level with visible scene/character/HUD rendering, controller-driven
movement and changing objectives (2026-10-27). The user confirmed entering and
operating the game with a controller. Later frame captures corroborate that
progress; an earlier all-black capture was taken during startup and is not
evidence of a persistent black-screen fault. This does not establish complete
rendering correctness, level completion or long-session stability. The installed
app bundle has not been refreshed or signed with these development changes.

Known gaps include Metal depth-bounds-test semantics and unsupported movie decoder
modes. Audio now uses the host libopus bridge through the SDK's two-argument
work-size entrypoint and named C++ virtual-method relocations; real-packet tests
pass and the game produces nonzero audio output, but full A/V validation remains
pending. Final packaging/signature/data-layout checks are still required before
release.

## Build and Package

Run the standalone build inside `games/mcd` using your own dumps, `prod.keys` and Switch firmware shared fonts (`scripts/extract_code.py`, `scripts/merge_romfs.py`, `scripts/extract_shared_fonts.py`, `scripts/prepare_modules.py` and the rewriter into `MCD_build/`), then build and package:

```sh
cd games/mcd
make prepare runtime app WORK=MCD_build \
  GAME="/path/to/update.nsp" KEYS="/path/to/prod.keys" \
  APP="Minecraft Dungeons.app"
```

All intermediates stay under `MCD_build/`. After packaging, the main RomFS exists only inside the signed app data bundle; the work directory uses a symlink. No internal-disk data staging is used.

User data: `~/Library/Application Support/MacNativeGames/MCD`.
Log: `~/Library/Logs/MacNativeGames/mcd.log`.
Networking is offline; host CSRNG uses macOS secure randomness. TLS and online
multiplayer are not implemented.
