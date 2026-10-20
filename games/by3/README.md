# Bayonetta 3 (work in progress)

An experiment: run Bayonetta 3 natively the way `games/totk` runs Tears of the Kingdom. It started from that code
(rewriter, loader, Horizon host, NVN on Metal) and keeps its own copies of all of it in this folder (`runtime/`,
`third_party/d2r`, `third_party/d2r_gfx`, `prebuilt/`, `scripts/`), so changes here never affect another game and
nothing here reads `games/totk`. **It is playable**: title, menus and the story run with picture,
sound, keyboard input and saves, from a small app with a settings window. Known gaps are listed in CHANGELOG.md.

How the work was done, what was found and what went wrong on the way: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own NSPs and the `prod.keys` dumped from your own Switch. Nothing from the game is in
this repository; `BY3_build/` (the work folder) and `mods/` are ignored by git.

## Inputs

- Base `01004A4010FEA000` (v65536) and update 1.2.0 `01004A4010FEA800` (v131072), both NSP.
- The base Program NCA is **sparse**: its ExeFS is empty and the RomFS parts the update replaces are not stored.
  The code always comes from the update, and the pinned hactool cannot read the base RomFS.

## Build and play

From `games/by3`, with a Python environment made from `requirements.txt` in this folder (`PY=<that python>`):

```sh
python scripts/extract_code.py "<update>.nsp" --keys prod.keys --output BY3_build/extracted/v120/exefs
python scripts/merge_romfs.py "<base>.nsp" "<update>.nsp" --keys prod.keys --output BY3_build/extracted/v120/romfs
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 $PY third_party/d2r/patch_nso.py BY3_build/extracted/v120/exefs/$m BY3_build/rewritten/v120 $m
done
$PY scripts/prepare_modules.py BY3_build/extracted/v120/exefs --output BY3_build/extracted/v120/elf
make prepare patches mods app PY=$PY GAME="<update>.nsp" KEYS=prod.keys
open "Bayonetta 3.app"
```

Optional mods go in `mods/`: exefs patches as `mods/1.2.0/<name>/exefs/*.pchtxt`, RomFS mods as `mods/<name>/romfs/`.
The app keeps running the game from `BY3_build`; do not move or delete it.

## Stage 1 (done)

From `games/by3`, with the Python environment of `requirements.txt`:

```sh
python scripts/extract_code.py "<update>.nsp" --keys prod.keys --output BY3_build/extracted/v120/exefs
python scripts/merge_romfs.py "<base>.nsp" "<update>.nsp" --keys prod.keys --output BY3_build/extracted/v120/romfs
for m in sdk subsdk0 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 python third_party/d2r/patch_nso.py BY3_build/extracted/v120/exefs/$m BY3_build/rewritten/v120 $m
done
```

`assets/expected-v1.2.0.json` pins every resulting hash.

| Check | Result |
|---|---|
| ExeFS (main, sdk, subsdk0, rtld, main.npdm) | FS header, hash table and every block verified |
| Merged RomFS (sparse base + BKTR update) | all six IVFC levels: 918,430 / 918,430 data blocks; 552 files, 15.0 GB; one flipped byte is caught |
| Rewrite structure (`structck.py`) | OK for all three modules, 0 violations |
| Rewrite semantics (`equiv.py`, emulator, random states) | 36,627 sites, 0 divergent; all 600 mutation controls caught |

The rewrite needed one new block kind: `main` sets `sp` from x18 for a variable-length stack array
(`mov sp, x18` at `0x12874f4`). `site_spset` in `patch_nso.py` keeps the old sp in a scratch register and restores the
scratch pair from it; `structck.py` accepts that one restore form only when the register is an untouched copy of the
entry sp. The TotK rewrites never contain such a site, so their output bytes do not change.

## What the game uses

- NVN (SDK 13.3.2). Of the 533 NVN names it loads, 419 are implemented here and 5 are intentional no-ops; most of the
  rest are getters. Unimplemented ones abort with their name when called.
- Wwise audio (through `audren:u` / `audout:u`), Nintendo's online SDK (NPLN: needs offline answers for `nifm`,
  sockets, `ssl`, `friends`, `bcat`), movies as WebM (478 files), data in Platinum `.pkz` archives.

## Next

Stage 2: a runtime target with every TotK-specific patch, address and overlay compiled out, booting to `nnMain`.
Stage 3: follow each unknown service, command or NVN call it aborts on until the first `nvnQueuePresentTexture`.
