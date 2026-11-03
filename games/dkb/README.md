# Donkey Kong Bananza (Switch 2) — native macOS target

This directory builds **Donkey Kong Bananza** into a native Apple silicon Mac app from your own dump of the game. All of its engine work is nxnative's: the Switch 2 boot, NVN2 graphics, the Ampere shader translator, the game's own upscaler and its guide movies. This target carries a copy of nxnative and builds with nxnative's own NSP script, as [`games/mk8d`](../mk8d/README.md) does; the Tears of the Kingdom runtime's Switch 2 path ([docs/ns2-port.md](../../docs/ns2-port.md)) is separate and keeps its own shader list in `assets/`.

## Build

```sh
python3 games/dkb/scripts/build_dkb.py ~/Games/Switch/DonkeyKong \
  --work-dir ~/Games/Switch/dkb-work --output ~/Games/Switch/DonkeyKong/"Donkey Kong Bananza.app"
```

- The dump folder holds the game's files as nxnative's `scripts/pack_dump.py` lists them: `base/`, `updates/<version>/` and `dlc/<id>/`. No `prod.keys` are needed.
- The Python running the script needs `lz4 capstone fonttools nsz zbic` (`pip install …`).
- The SPIR-V to Metal converter is carried as nxnative ships it, and the build stops when its fingerprint differs from the one `dmsh_chain.lock` records, because another converter would be another shader chain. Building it from nxnative's pinned source does not give the same bytes with Xcode 27.2 (`098403a9…` against `e37945d8…`), which is why the binary is carried and not built here.
- The first build translates and compiles every shader, about an hour on an M1 Max. nxnative's work folder is `<dump folder>/<title ID>_build`, so a build from an nxnative checkout of the same version, or an earlier build of this target, has its shaders reused (here: 23,638 of 23,638 from the cache, 21 minutes in all).
- Saves are where nxnative keeps them (`~/Library/Application Support/nxnative/<title ID>/`), whichever build made the app.

## Known

- With the display link on (the default), one session on an M1 Max showed the picture frozen 10 times for 1 to 107 s while the window was active (`display link recovered: stalled … ms` in the log; the host's fallback did not take over). With `DM_PRESENT_LINK=0` in the app's `env.txt` (beside the save), the sessions measured had none.

## The nxnative copy

| Where | What |
|---|---|
| `runtime/` | nxnative's `dm/` at the commit in [`third_party/nxnative/import-manifest.json`](third_party/nxnative/import-manifest.json), without the other games' profiles |
| `third_party/nxnative/scripts/` | the NSP script and the two scripts it loads (`pack_dump.py`, `pack_nup.py`) |
| `third_party/nxnative/local-changes.patch` | the changes over that commit: a HID gamepad that macOS marks as not for GameController is read as raw HID (a PowerA pad for Switch 2) |
| `third_party/nxnative/LICENSE` | nxnative's MIT licence |

Update the copy with `games/dkb/scripts/import_nxnative.py <nxnative checkout> --base <upstream commit>`: it replaces `runtime/` and `third_party/nxnative/` completely and writes the manifest and the patch again. `tests/test_dkb_build.py` checks the copy against the manifest, that no other game's profile came along, that the local changes are exactly the patch, and the build script's input checks.
