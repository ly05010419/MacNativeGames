# The Legend of Zelda: Echoes of Wisdom (community port, early)

Echoes of Wisdom 1.1.0 running natively on Apple silicon the way `games/totk` runs Tears of the Kingdom: the game's own
ARM64 code, rewritten ahead of time, a small Horizon host and NVN rebuilt on Metal. It started as a copy of `games/by3`
and keeps its own copies of everything it builds and runs with ([third_party/COPIES.md](third_party/COPIES.md)).

**Playable from the title into the game on the Mac**: the title screen with its movie, the prologue (Link in the dungeon,
HUD, Chinese text), sound and music, keyboard and game controllers, saves. 60 fps as on the Switch. How it was done,
what was found and what is still open: [CHANGELOG.md](CHANGELOG.md).

You must own the game: use your own NSPs and the `prod.keys` dumped from your own Switch. Nothing from the game is in
this repository; the work folder (`EoW_build`) and `mods/` are ignored by git.

## Inputs

- Base `01008CF01BAAC000` (v0) and update 1.1.0 `01008CF01BAAC800` (v196608), both NSP.
- The RomFS sections use NCA **compression** (LZ4 ranges in a bucket tree), which the pinned hactool cannot read;
  `scripts/merge_romfs.py` reads it. All code comes from the update.
- The six system fonts from your own Switch firmware in `$WORK/extracted/firmware/shared-fonts` (as the TotK build
  extracts them; the game draws its text with them).

## Build

From `games/eow`, with a Python environment made from `requirements.txt` plus `unicorn` (for `equiv.py`); `PY=` that
python, `WORK=` the work folder (an APFS volume; about 25 GB):

```sh
python scripts/extract_code.py "<update>.nsp" --keys prod.keys --output $WORK/extracted/v110/exefs
python scripts/merge_romfs.py "<base>.nsp" "<update>.nsp" --keys prod.keys --output $WORK/extracted/v110/romfs
for m in sdk subsdk0 subsdk1 main; do
  DM_RULESET=native DM_PRIVATE_GATES=0 $PY third_party/d2r/patch_nso.py $WORK/extracted/v110/exefs/$m $WORK/rewritten/v110 $m
done
make prepare runtime shader-tools PY=$PY WORK=$WORK
```

`assets/expected-v1.1.0.json` pins every resulting hash; `make prepare` checks them.

## Play

From the work folder:

```sh
$PY <repo>/games/eow/scripts/build_shader_libs.py --work .     # shaders met in earlier sessions, compiled ahead (seconds)
TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BUDGET_MB=5000 \
  DM_PRESENT_LINK_FALLBACK=1 ./build/eow-native rewritten/v110 boot 2> logs/play.err
```

- Click the window once: macOS gives controller input only to the active app, and a window started from a terminal is
  not always activated.
- Keyboard: J = A, WASD = left stick; game controllers work through GameController.
- Saves: `user-data/` in the work folder. Close the window to quit; the game finishes its save first.
- Stutter: shaders are translated once and cached; `build_shader_libs.py` compiles them ahead of time into
  `build/shader-libs`, so only shaders never seen before are compiled during play (0.3-0.6 s each, once).

## Checks

| Check | Result |
|---|---|
| ExeFS (main, sdk, subsdk0, subsdk1, rtld, main.npdm) | FS header, hash table and every block verified |
| Merged RomFS | all six IVFC levels (350,852 data blocks), then 98,996 compressed ranges; 10,965 files, 8.0 GB; a flipped byte from either NCA is caught |
| Rewrite structure (`structck.py`) | OK for all four modules, 0 violations |
| Rewrite semantics (`equiv.py`) | 36,452 sites, 0 divergent; 1,200 of 1,200 mutation controls caught |
| `probe`, `probe-audio` | pass |
| Played (Mac, M4 Pro, macOS 27.0.1) | title, prologue dungeon; about 1.5 hours over several sessions (longest about 40 minutes). Two early sessions ended in an unanswered `nn::fs::CleanDirectoryRecursively`, answered since (not yet seen in play) |

Not done yet: a launcher app, iPhone / iPad, more than 60 fps (the game's logic follows its frame count), the stencil
plane in half-size depth copies, play past the prologue (untested).

Tools for continuing: `scripts/disasm.py`, `scripts/xref.py`, `scripts/callers.py`, `scripts/symbolize.py`,
`scripts/lldb/` (see the work log).
