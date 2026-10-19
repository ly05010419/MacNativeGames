# Cheats

The Mac app can apply Atmosphère-format cheat codes (the `[Name]` + hex-line files that cheat databases publish) to the game's main module when it starts. Implementation: [runtime/cheats.c](../games/totk/runtime/cheats.c).

## Where the file goes

```
~/Library/Application Support/MacNativeGames/TOTK/cheats/<build id>.txt
```

`<build id>` is the first 16 hex digits of the main module's build ID. Cheat databases use the same name:

| Game version | Title ID | File name |
|---|---|---|
| 1.4.3 | `0100F2C0115B6000` | `277178B7DBA1B6D4.txt` |
| 1.0.0 | `0100F2C0115B6000` | `082CE09B06E33A12.txt` |

The runtime reads the build ID from the game image it loads, and only opens the file with that name. Codes filed under another build ID are never loaded. Codes for another version pasted into the right file can't be detected, and will usually crash the game.

Edit the file, then start the game again. No rebuild is needed. Set `TOTK_CHEATS=0` to start without cheats, or `TOTK_CHEATS_DIR=<dir>` to use another folder.

## File format

```
[Inf. Stamina]
040A0000 009F1DC4 1E204041

#[Quickfire Bow]
040A0000 01D2FE5C 291FA268

// a comment
```

- **Sections:** `[Name]` or `{Name}` starts a section. Every section is applied, except one whose header starts with `#`, as in `#[Name]`. A `#` header switches off its whole section until the next header.
- **Comments:** any other line starting with `#` or `//` is a comment.
- **Codes:** lines of 8-digit hex words.

## What works

- **Static writes (opcode `0`) to the main module (memory type `0`):** the `04…` and `08…` lines that make up most cheats. They are written once at startup, at the same point as the built-in 60 FPS patch, before the code is locked read-only, and they stay on for the whole session.
- **Writes into the data segment** are also made once, so the game may overwrite them later.
- **Hold-a-button cheats** (`8kkkkkkk` … `21000000` else … `20000000` end). The loader runs them every time the game reads the controller, as Atmosphère's cheat VM does. Their writes may target:
  - data or read-only data, such as the glider-speed constant. These are written directly; a read-only page is made writable after the code is locked.
  - code, but only when every value written is a `MOVZ`/`MOVN` of one register. "Movement Speed (Hold B)" is like this: it writes `mov w8, #1.0f` / `#2.0f`. At startup that instruction is replaced by a branch to a 3-instruction stub, which loads the register from a slot on the main module's private data page. Each controller read updates the slot.
- **Key masks** use the console's button bits: A `1`, B `2`, X `4`, Y `8`, L3 `10`, R3 `20`, L `40`, R `80`, ZL `100`, ZR `200`, + `400`, − `800`, and the D-pad `1000`–`8000`. Stick directions (left stick `10000`–`80000`, right stick `100000`–`800000`) count when the stick is more than half way. Add masks together for combinations.
- **Folder markers** are ignored: these are sections holding only `20000000` / `20000001`, which cheat managers use for groups.

## What doesn't

- **Register, pointer and loop opcodes** (`3`, `4`–`7`, `9`, `A`, `C`…). These include "Moon Jump" and the resolution cheats. A section that uses any of them is skipped as a whole.
- **Button cheats that change other code**, for example an `fmul`, as in "Time RW/FF" or the mission-timer cheat. Code can't change while it's locked, so these are skipped.
- **Turning a static cheat on or off while playing.** Edit the file and restart the game.

The file in your cheats folder, `277178B7DBA1B6D4.txt`, was generated on 2026-10-19 from the 1.4.3 codes in Arch9SK7/MyNXCheats, which sthetix/nx-cheats-db and TKMM share. Every patched instruction was checked against this build: each original matches the source's Restore Code, and the code caves are empty and don't overlap. The file notes which cheats this loader skips.

## What the loader checks

A section is skipped, and nothing from it is written, when:
- a write falls outside the game's own image;
- a write would overwrite an instruction this port rewrote (a branch into the port's patch area);
- a line isn't 8-digit hex words.

The game still starts.

The format carries no "original instruction", so a code for a different game version can't be caught beyond the file name. Check the log after the first start with a new code. `~/Library/Logs/MacNativeGames/totk.log` lists each write as `[cheats] [Name] main+0x… (text): 4 bytes 0x<old> -> 0x<new>`, then a summary line with the number of sections applied, skipped and disabled.
