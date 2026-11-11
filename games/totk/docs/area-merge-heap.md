# TOTK 1.0.0 area-merge renderer heap (Mac)

Approaching Zora's Domain can fail a small allocation from the private
`AreaMergeTarRenderer` heap. This patch increases that heap at creation from
32 KiB to 64 KiB on Mac, costing an additional 32 KiB from its parent heap.

The investigation and size recommendation are by
[lili2029 in issue #1742](https://github.com/ly05010419/MacNativeGames/issues/1742#issuecomment-6730940890).
The same crash is also reported in
[#1792](https://github.com/ly05010419/MacNativeGames/issues/1792).

## Evidence and scope

An independent Mac reproduction captured `SIGSEGV`, `fault=0x537`,
`main+0x855560`, with the same stack as #1742. The allocator call at
`main+0x855550` returned null for a `0x8d8`-byte request aligned to 8 bytes.
The following initialization writes through null plus an object offset.

Static inspection of that exact 1.0.0 image confirmed the creation sequence,
its `AreaMergeTarRenderer` name reference, and the hard-coded 32 KiB size at
`main+0xec1f78`. Main build ID:
`082CE09B06E33A123CB1E2770F5F9147709033DB`.

The detailed live heap walk is **the upstream reporter's evidence**, not a new
heap walk from the independent reproduction. That report found insufficient
contiguous space in the dedicated heap while its parent pool still had room.
It also reported a 64-minute successful session after the size increase.

The independent local test reproduced a tower-to-tower gliding crash, rebuilt
the Mac app with the guarded size change, confirmed its startup diagnostic,
and then completed the previously failing flight without a crash. Infinite
Stamina and other local cheats remained enabled in that retest; these custom
features are not part of this change. This is one successful reported route,
not a long-duration test or complete coverage of the Zora region. The exact
tower names were not recorded for this independent run.

This is a targeted capacity adjustment. It does not establish why this port's
allocation pattern exceeds the original heap or prove that 64 KiB is sufficient
for every situation. An underlying platform allocator issue, if identified,
should be fixed in the platform rather than hidden by this patch.

## Guards and platform behavior

- Only the verified 1.0.0 Mac path applies the change. The loader still checks
  module identity before applying runtime patches.
- All twelve creation instructions and the terminated renderer name must match
  before any write. Text and file bounds are checked first. A mismatch refuses
  startup instead of applying the patch to another image.
- Only the heap-size instruction changes. The allocation and initialization
  code are retained; no null check skips the renderer object.
- The source-derived mod reservation list covers all twelve checked
  instructions for v0, preventing build-time mods from changing them.
- 1.2.1 is a no-op, and 1.4.3 uses its separate patch source.
- The native iOS path and the Mac host's `TOTK_EXPORT_IOS` path both skip this
  change; no new patch is embedded into an iOS game image.

`TOTK_AREA_MERGE_HEAP=0` retains the original 32 KiB size for a controlled
comparison. Leave it unset for the fix. With debug logging enabled, startup
reports `AreaMergeTarRenderer heap: 32 -> 64 KiB` when applied.

## Regression tests

```sh
python -m unittest discover -s tests -p test_totk_area_merge_heap.py -v
```

The tests compile the actual source helper and writer into a CPU-only probe.
Its sparse data fixture is authored from the patch fingerprint; it does not
contain a game image, execute game instructions, use saves, or run GPU work.

Normal and UBSan runs verify the exact single-word change, rejection of every
altered creation/name byte with zero writes, truncated bounds (including an
inaccessible-memory check), wrong module names, exact accepted boundaries,
and no mutation on excluded version/platform paths. They also check the
disable switch, the iOS export skip, and source-derived version reservations.

These synthetic tests validate patch behavior and bounds. The separate local
inspection of the player's verified image established the fingerprint's
identity; no game files or private logs are included in the repository.
