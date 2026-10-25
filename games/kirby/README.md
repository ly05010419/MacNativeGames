# Kirby and the Forgotten Land native Mac port

Kirby and the Forgotten Land (**1.0.0**) running natively on Apple silicon Macs, built from **your own copy** of the game. The game's ARM64 code runs directly on Apple Silicon CPU via ahead-of-time binary rewriting; graphics run via NVN rebuilt on Metal, with game GPU shaders translated and precompiled.

**No game content is in this repository, and none will ever be.** You need a legal dump of your own cartridge or download, and keys from your own Switch.

## Status

| Component | Status | Details |
|---|---|---|
| Boot & Title | Verified | Smooth 60 FPS presentation, controller & keyboard input |
| Graphics & Shaders | Verified | 609 variants over 604 shaders prepared; precompiled into `.metallib` |
| Audio & Video | Verified | Opus audio decoding and video playback supported |
| Saves | Verified | Auto-save backups retained across sessions |

## Shaders & Precompilation

Kirby shaders are catalogued in `games/kirby/assets/shader_list.tsv` (shader keys and bind state dependencies only).

To compile translated shaders into Metal libraries ahead of time:
```bash
python3 games/kirby/scripts/precompile_cached_shaders.py
```
This generates `.metallib` archives into `Kirby_build/build/shader-libs`, matching what the runtime loads directly on startup to eliminate shader compilation stutter.
