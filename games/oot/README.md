# The Legend of Zelda: Ocarina of Time (Switch 2)

Experimental, on the Tears of the Kingdom runtime (`games/totk`, `GAME_OOT` + `GAME_NS2`). Title `0400486024DEC000`, SDK 22.2.8, NVN2 231.40. No game content is included. There is no app packaging yet: it runs from a development build. See `docs/ns2-port.md`.

Works on a 16 GB Apple silicon Mac: title and menu text, a new game, the opening movie with subtitles, the first in-engine scene at about 30 fps, and writing a save.

Known problems:

- Memory: system wired memory reaches 12 GB and the game stops itself (exit 87, `wired_guard_check`), before the GPU driver can panic macOS. From a new game that took about 12 minutes; **loading a save takes under a minute, so a saved game cannot be played yet**. Memory pools are about 5.4 GB of what is resident, and lowering `TOTK_POOL_CAP_MB` to 6500 did not help.
- The picture has a pale haze; the cause is not confirmed.
- agl's upscaler network is bypassed (a plain copy, `TOTK_NDSR_BYPASS=0` runs it), so the picture is softer than the console's.
- Draws of shaders that read an image or buffer texture handle from memory are skipped.

Build and run, from `games/totk`, with the game imported as `extracted/oot`:

```sh
make -j6 binary-oot
TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_ASYNC_SHADERS=1 build/totk-native-oot rewritten/oot boot
```

`TOTK_ASYNC_SHADERS=1` keeps frames coming while shaders translate; without it the first scene stays black for as long as its shaders take. The runtime needs a shader service built from the current patches (`tools/totk_xlate/publish_native.sh <dotnet> <absolute path>/games/totk/build/shader-service`), as `prebuilt/` predates them.
