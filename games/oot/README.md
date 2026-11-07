# The Legend of Zelda: Ocarina of Time (Switch 2)

Experimental, on the Tears of the Kingdom runtime (`games/totk`, `GAME_OOT` + `GAME_NS2`). Title `0400486024DEC000`, SDK 22.2.8, NVN2 231.40. No game content is included. `build_app.py` makes a Mac app from your own dump. See `docs/ns2-port.md`.

Works on a 16 GB Apple silicon Mac: title and menu text, a new game, the opening movie with subtitles, the first in-engine scene, writing a save, and loading a save into Kokiri Forest (Link's house and the forest view).

Known problems:

- Memory: a loaded save plays, but this is not settled. A 3 minute session peaked at 11.7 GB of system wired memory, against the 12.3 GB limit (`wired_guard_check`, exit 87), and no longer session has been tried. The raw pool table also overflows by a few entries (a red line in the log; addresses in those pools are not reached).
- Frame rate, on one 16 GB Mac, single runs: 60 fps held at the spawn point in Link's house and in the forest view after leaving it, about 53 fps facing the house's door, about 55 fps on the deck outside (frame p50 17.4 to 17.8 ms, p95 about 21.7 ms). The rest of the world has not been measured. GPU 15 to 18 ms in the heavier views. Spikes of 40 to 50 ms with low GPU time remain in some views (CPU side, not explained). Pool tables are still walked whole 3 times a frame (5 ms or so).
- The picture has a pale haze; the cause is not confirmed.
- agl's upscaler network is bypassed (a plain copy, `TOTK_NDSR_BYPASS=0` runs it), so the picture is softer than the console's.
- Draws of shaders that read an image or buffer texture handle from memory are skipped.

Memory and frame rate, what changed (`nvn_mtl.m`):

- Only pools the driver gave an address in go into the raw pool table (`mtl_pool_addressed`, after seedds/nxnative, MIT). A texture upload from any other pool borrows a temporary zero-copy buffer (`up_temp_src`); a pool under 64 MB borrowed more than 8 times stays resident instead. `TOTK_POOL_ADDR_ONLY=0` restores the old behaviour.
- The texture table walk asked `mtl_addr` whether a texture was in a pool, which also wired the pool: 3.1 GB, the 2,432 MB graphics pool among it, for a check. `pool_holds` answers without wiring. Resident pools: 5.1 GB to 2.0 GB.
- `DispatchComputeIndirect` no longer stops the CPU to read its counts: Metal reads them when the dispatch runs, and a GPU copy puts them in the driver segment for the shader. The wait was 74% of the replay thread's samples. Forest view: 148 ms to 40-46 ms a frame. `TOTK_DISPATCH_READBACK=1` restores it.
- The check that a texture's pool object still exists read its magic through the kernel, per ID and per table walk: 27% of the replay thread. `pool_obj_live` remembers the answer per pool until the frame changes or a pool starts or ends (`TOTK_POOL_LIVE_CACHE=0` asks every time). Forest view: 43.5 to 34.8 ms a frame, one run each.
- Draw passes are ordered by the game's own barriers (`barrier_game`, after seedds/nxnative, MIT), on for this game only: a pass's vertex work may overlap the previous pass's fragments. `TOTK_BARRIER_GAME=0` restores a full barrier for every pass. Forest view: 34.8 to about 30.8 ms (two runs). The picture was spot-checked only.
- Compute dispatches with no game barrier between them run side by side (after cheemu21/kid-with-sword-remake, MIT; `TOTK_DISPATCH_OVERLAP=0` orders them). About 6.5 to 3.9 ms at the start of a frame.
- Task and mesh draws stay in the open pass; each used to end it before and after itself. Passes a frame 147 to 98, and the replay thread encodes the passes (`TOTK_MESH_INPASS=0` restores it).
- A raw memory access remembers the pool table entry it chose last in its invocation (`patches/0011`, after seedds/nxnative 0f510fd5, MIT) instead of searching the table for each word. Mesh draws were about two thirds of the GPU time facing the door: 27.7 to 18.1 ms, with the changes above. A new shader cache name: the first run retranslates, for minutes.
- The cached views of a texture as another type: swept in slices rather than whole every 64 submissions (frame p95 54-62 to about 21 ms at the spawn point), and dropped with their texture ID's view (they leaked 88 a frame). `TOTK_TYPE_VIEW_SWEEP=all` and `TOTK_TYPE_VIEW_DROP=0` restore the old behaviour.
- Shaders report which textures they really read (`dm_pool_mark`, a `seen` bitmap per table, in `Pools.cs` and `pool_seen` in `nvn_mtl.m`, after seedds/nxnative, MIT), so the table walk makes views only for those; the budget is now 32 MB a frame and 128 MB for what was read, up to a twentieth of the working set in all (`TOTK_POOL_CAP_MB`). Fragment shaders that do not discard or write depth run `[[early_fragment_tests]]`. Same 170 s forest run: Metal peak 12.5 to 10.7 GB, wired memory no longer over the limit, deck 19.6 to 18.4 ms.
- A direct-mapped cache in front of the map of texture type views (`g_tv_fast`, cleared at every removal; `TOTK_TYPE_VIEW_CACHE=0` turns it off). Deck, single runs: frame p50 18.5 to 17.4-17.8 ms.
- The application pool is 9 GB, as nxnative's profile, instead of the 10 GB other Switch 2 ports here take.

Game speed:

- The game stepped 1/60 s a frame whatever the frame took: slow below 60 fps, fast above it (125 fps at the title). Its step is worked out in one place, `main+0x6e6cc4`: 60 / the frame-rate controller's fps, added to a running count and handed to the game's time object (`main+0x3b4e300`), which derives seconds and its other values from it. That step is now the measured frame time, averaged over 5 frames as on Tears of the Kingdom, so game time adds up to real time at any frame rate, in even steps (`TOTK_REALTIME=0` restores it). The controller stays as shipped. The host holds 60 fps by its own clock (`TOTK_UNCAPPED_HZ`), ignoring the game's present interval.
- A `game speed` line every 30 s gives the ratio to real time: 0.97 to 1.00 in a 3 minute session at 45 to 60 fps. That is the host's count, not a comparison with the console: Link's speed has not been checked against one. It felt right to the player. `TOTK_GAME_SPEED` (0.25 to 2, default 1) scales game time without touching the frame cap.
- The same 3 s walk ends in about the same place at 51 fps and at a 30 fps cap (compared by eye from screenshots). Fractional steps have only been played for those minutes around the start of the game: cutscenes and combat are not checked.

Build the app, from the repository root, with your dump as a folder holding `base/exefs` and `base/romfs` (no update, no keys):

```sh
python3 build_app.py <dump folder> --work <work folder> -o "Ocarina of Time.app"
```

Use a work folder on an APFS disk with about 60 GB free. The first build takes about an hour on a 16 GB Mac, as it prepares the game's 42,302 shaders; the app is 29 GB. The first start compiles what Metal did not keep, about 30 s at the warm-up screen. System fonts are optional (`--shared-fonts`); no run of this game has asked for them. It is Mac only. The app uses this game's own shader translator (`games/oot/prebuilt/macos-arm64`, made with `tools/totk_xlate/publish_native.sh`): `games/totk/prebuilt` predates the patches this game needs, and changing it would change every other game.

For development, from `games/totk`, with the game imported as `extracted/oot`:

```sh
make -j6 binary-oot
TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 build/totk-native-oot rewritten/oot boot
```

Shaders translate in the background by default for this game (`TOTK_ASYNC_SHADERS=0` waits for each one, and the first scene stays black for as long as they take). A development run needs a shader service built from the current patches: `tools/totk_xlate/publish_native.sh <dotnet> <absolute path>/games/totk/build/shader-service`.
