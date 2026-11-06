# The Legend of Zelda: Ocarina of Time (Switch 2)

Experimental, on the Tears of the Kingdom runtime (`games/totk`, `GAME_OOT` + `GAME_NS2`). Title `0400486024DEC000`, SDK 22.2.8, NVN2 231.40. No game content is included. There is no app packaging yet: it runs from a development build. See `docs/ns2-port.md`.

Works on a 16 GB Apple silicon Mac: title and menu text, a new game, the opening movie with subtitles, the first in-engine scene, writing a save, and loading a save into Kokiri Forest (Link's house and the forest view).

Known problems:

- Memory: a loaded save now plays, but this is not settled. Before the fix below it stopped itself (exit 87, `wired_guard_check`) about a minute after loading. Runs of 90 to 150 s after it peaked at 10.5 to 12.1 GB of system wired memory, against the 12.3 GB limit, and no longer session has been tried. Textures are now the growing part (about 3.2 GB in one run), and footprint reaches about 13.7 GB. The raw pool table also overflows by a few entries (4,095 of them: a red line in the log, addresses in those pools are not reached).
- Frame rate: about 25 fps (40 ms a frame) in Link's house and in the forest view, up to about 50 fps at the best moments, with the GPU now the limit (about 32 ms). One earlier run in the same room, facing another way with the old pool handling, had 24 ms a frame and 19 ms of GPU. That is not a controlled comparison, and the old build cannot stay up long enough for one, so a GPU cost from these changes is not ruled out.
- The picture has a pale haze; the cause is not confirmed.
- agl's upscaler network is bypassed (a plain copy, `TOTK_NDSR_BYPASS=0` runs it), so the picture is softer than the console's.
- Draws of shaders that read an image or buffer texture handle from memory are skipped.

Memory and frame rate, what changed (`nvn_mtl.m`):

- Only pools the driver gave an address in go into the raw pool table (`mtl_pool_addressed`, after seedds/nxnative, MIT). A texture upload from any other pool borrows a temporary zero-copy buffer (`up_temp_src`); a pool under 64 MB borrowed more than 8 times stays resident instead. `TOTK_POOL_ADDR_ONLY=0` restores the old behaviour.
- The texture table walk asked `mtl_addr` whether a texture was in a pool, which also wired the pool: 3.1 GB, the 2,432 MB graphics pool among it, for a check. `pool_holds` answers without wiring. Resident pools: 5.1 GB to 2.0 GB.
- `DispatchComputeIndirect` no longer stops the CPU to read its counts: Metal reads them when the dispatch runs, and a GPU copy puts them in the driver segment for the shader. The wait was 74% of the replay thread's samples. Forest view: 148 ms to 40-46 ms a frame. `TOTK_DISPATCH_READBACK=1` restores it.

Build and run, from `games/totk`, with the game imported as `extracted/oot`:

```sh
make -j6 binary-oot
TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_ASYNC_SHADERS=1 build/totk-native-oot rewritten/oot boot
```

`TOTK_ASYNC_SHADERS=1` keeps frames coming while shaders translate; without it the first scene stays black for as long as its shaders take. The runtime needs a shader service built from the current patches (`tools/totk_xlate/publish_native.sh <dotnet> <absolute path>/games/totk/build/shader-service`), as `prebuilt/` predates them.
