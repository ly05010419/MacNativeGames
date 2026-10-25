# A merged (mod manager) mod pack

A mod manager (TKMM, or Ryujinx's `mods/contents/<title id>/<name>`) hands you one folder holding `exefs/` and
`romfs/` together. This page is the practical companion to [Player mods](mods.md): what of such a pack can load in the
Mac app, the arithmetic between an IPS file and a pchtxt, and how to tell whether the pack is actually live. It contains
no game content, keys or mod code.

## What can load

| Content of the pack | Where it goes | What reads it |
| --- | --- | --- |
| an extra code module (`subsdk1`, a hooking framework's `subsdk9`) | - | not loaded by this app; the build prints which module it left out |
| the `exefs/` patch for `main` | merged into `main` by `--mod` at build time, or applied at start with `TOTK_PCHTXT` | build / app |
| ordinary RomFS files | `~/Library/Application Support/MacNativeGames/TOTK/mods/<version>/` | `TOTK_ROMFS_MODS`, set by the launcher |
| RomFS files whose path the app replaces itself | merged by `make_mod_manifest.py`, or folded in by `--mod` (the heap table) | app |

A pack that also ships a code module keeps its `main` patches and its whole RomFS, but not the code that lives in that
module. Installing its data correctly and still seeing none of its features usually means the feature was in the module.

## IPS to pchtxt

The `exefs/` folder often carries a small IPS file whose *name is the build id it targets* (`<build id>.ips`, the
manager's convention). It patches `main`; a RomFS overlay cannot do that, so it must go through the code-patch route.

```
IPS32 record = 4-byte big-endian offset, 2-byte big-endian length, then the bytes
pchtxt line  = (offset - 0x100) in hex, a space, the bytes in file order as hex
```

and the file itself:

```
@nsobid-<main's build id, 40 hex digits>
@enabled
@flag offset_shift 0x100
<main address in hex> <bytes in hex>
@stop
```

`@flag offset_shift 0x100` is not optional: the addresses in a pchtxt are `main` addresses, while an IPS offset counts
the 0x100-byte NSO header. Both routes below stop the start when the flag is missing or names another shift, instead of
letting every patch land 0x100 bytes off. The build id is read from `extracted/<version>/exefs/main` at offset 0x40 and
differs per game version.

Two routes, pick one:

| Route | When | How |
| --- | --- | --- |
| `--mod` at build time | packaging | `python3 build_app.py <game file> -k <keys> --mod <the manager's folder>` - `scripts/mod_exefs.py` does the 0x100 arithmetic and refuses a patch that leaves `main`'s segments or overlaps the app's own patches (60 FPS, resolution, ultrawide, cloth) |
| `TOTK_PCHTXT=<file>` at start | while trying a community patch | Mac only; no rebuild per edit |

Never edit `rewritten/<version>/main.patched.bin` by hand: the loader compares the image's SHA-256 with the digest the
build recorded and stops (`image SHA256 differs from verified rewrite`). Use one of the two routes.

## The overlay folder

```bash
D="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/<version>"
rsync -a --delete "<the manager's folder>/romfs/" "$D/"   # contents go directly here, no extra romfs/ level
python3 games/totk/scripts/make_mod_manifest.py "$D" \
  --romfs extracted/<version>/romfs --version <version>   # or --app "<path>/Tears of the Kingdom.app"
```

- Run `--check` first: it writes nothing and says what the manifest would hold, including which paths the app replaces
  itself (the UI archive is merged, the heap table is folded in with `--mod`, anything else is left out).
- The manifest lives at the overlay root, one line per file: `path<TAB>size<TAB>sha256<TAB>original sha256` with `-` as
  the original for a file the RomFS does not have.
- Do not re-run the `rsync --delete` above over an overlay that has been in use: a long-lived one holds files the mod
  folder does not (a ResourceSizeTable that was repaired, the `<name>.mod-original` copies the merge tool keeps). A full
  copy deletes them.
- To switch the mod off, rename `manifest.tsv`; do not delete the mod's files.

A pack can also carry its own repaired `ResourceSizeTable` or a recompressed `.zs`; the traps that live in those files
are not about mods at all - see [the packaging traps](mods.md#three-packaging-traps-that-look-like-a-crash).

## Is it live?

The start logs one line per stage (with `defaults write org.macnativegames.totk TOTK_DEBUG_LOG -bool YES`):

```
[totk] mapped main size=... symbols=...
[totk] TOTK_PCHTXT <file>: N patches, M bytes in main (build id ...)     # only with TOTK_PCHTXT
[totk] output resolution patch: ...                                     # 2K builds only
[totk] RomFS overlay: extracted/<version>/romfs-patches checked, ...
[totk] RomFS overlay: .../mods/<version> checked, N files, ... MB hashed in ...
[totk] RomFS overlay: K files added to the RomFS
```

`N files` with no `FAIL:` means the folder matches its manifest; `K files added` is the number of `-` originals. A
feature that is neither a picture nor an added file (a code patch) shows only in the gameplay: the title screen taking
the pack's theme is the usual first proof that both halves loaded.

To drive the game without mouse and keyboard, append to the launcher's command file (`<data dir>/input.txt`, or the path
in `TOTK_INPUT_FILE`):

```bash
D="$HOME/Library/Application Support/MacNativeGames/TOTK"
printf 'key 38 120\n' >> "$D/input.txt"    # A: confirm (continue, save, load)
echo "shot $D/shot.png" >> "$D/input.txt"  # the game frame
echo "close" >> "$D/input.txt"             # a clean exit
```

`key <Mac keycode> <milliseconds>` needs 1..5000 ms (`key 0 0` stops the game with a diagnostic-input error). Note that
`runtime/input.c` maps **WASD to the left stick (movement) and the arrow keys to the right stick (camera)**: a run that
only presses arrows turns the camera and walks nowhere.

Two lines that are not the mod's fault: `reserve guest stack, heap and alias regions below 2^39` can fail once when macOS
places a large reserved range on top of the low gap (start again), and a `shot` of a 2K build has a magenta border
outside the 1920x1080 game area, which the window does not show.
