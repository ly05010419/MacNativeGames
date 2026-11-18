# Validation record

Date: **2026-11-14**. Game: **WORLDEND SYNDROME** 1.0.0, title `010048900CF64000`, package `v0`. Input NSO sizes, SHA-256 values and 16-byte build IDs are in [expected-1.0.0.json](../assets/expected-1.0.0.json).

Hardware: Apple silicon Mac (local bring-up machine). OS: macOS 26 family. This is not a performance promise for other Macs. iOS / iPadOS / visionOS were neither built nor played.

## Packaging checks

| Check | Result |
|---|---|
| `make -C games/worldend test` | 13 passed (input rejection, title gate, staging, shader-stamp, offline tooling) |
| Isolation | `games/worldend` reads no other game folder; copied from `games/jamboree` at `92ec79b` ([COPIES.md](../third_party/COPIES.md)) |

A private full build from an unmodified base NSP rewrote `main` / `sdk` / `subsdk0` with **0 CUTS**. Shader split: 543 Unity 2017.4 keys (identifiers only). Runtime MSL compile (`DM_METAL_RUNTIME=1`). Screenshots, saves, RomFS and translated libraries stay out of Git.

## Play (Mac)

On the packaged host, with a disposable save folder:

- Title (docked 1920×1080), Continue / Load, ADV, Name Entry, in-game checkpoint save (new slot; inn slot kept).
- Story choices (D-pad then A; A alone does not arm the boxes).
- First loop ran for several hours of ADV with Auto Play. It faded to the opening card and restarted the arrival scene. That is the first-playthrough outcome this port claims; later loops and the true end were not played.

Audio (opening movie / PCM) and Plus-to-Save during ADV were exercised. Plus does not open Save on a choice prompt.

## Known limits

- Mac only. No iOS build.
- No game-code patches. Heap `mem_used=0x4A00000` and NVN `device_api=53.399` are profile facts, not instruction patches.
- First-visit shader compile is visible. No smoothness claim.
- Later routes, the town map as free roam, album/tips 100% and a second loop were not checked.

## Reproduce without game content

```sh
python3 -m unittest discover -s tests
make -C games/worldend test
```

Private build: see [README.md](../README.md). Share a redacted log and `build-info.json` identifiers; do not upload programs, shader source, keys or saves.
