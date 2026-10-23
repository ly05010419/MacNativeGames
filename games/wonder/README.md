# Super Mario Bros. Wonder (early port)

Super Mario Bros. Wonder 1.0.0 (`010015100B514000`) on the Tears of the Kingdom host: the same loader, services and
NVN-on-Metal layer (`games/totk`), with one game-code patch: the real-time frame step, so the game keeps its speed at
any frame rate (`TOTK_REALTIME=0`: as shipped, held at 60 fps). `build_app.py` recognises the game by the title ID of its
Program NCA and builds it the shared way.

```
python3.14 build_app.py ~/Games/wonder.nsp -k ~/keys/prod.keys --firmware ~/Games/firmware
```

- Work folder `Wonder_build` next to the game file; app `Super Mario Bros. Wonder.app` (bundle `org.macnativegames.wonder`);
  data folder `~/Library/Application Support/MacNativeGames/Wonder`, log `~/Library/Logs/MacNativeGames/wonder.log`.
- A fresh build takes about 2 minutes on an M-series Mac (3.7 GB of game data).
- Base game 1.0.0 only (NSP or XCI); its update is refused for now. Mac only: no iPhone / iPad build yet.

## What is here

| File | What it is |
| --- | --- |
| `assets/expected-v1.0.0.json` | the hashes every build step is checked against (ExeFS, the two rewritten modules, the RomFS) |
| `runtime/game_patches_wonder.c` | the game's patch file (`games/totk/Makefile`, `runtime-wonder`): the real-time frame step; exclusive loads take acquire (see the file) |
| `runtime/offline_host.c` | the services an offline console answers for: network (nifm, sockets, ssl, nsd), friends, local wireless, play reports, the console identity |

The rest lives with the shared host in `games/totk/runtime`: the game's block in `game_version.h` (`GAME_WONDER`), user
selection and the profile image (`account.c`), the FS context and the game's own save-data calls (`fs_host.c`), a 60 Hz
VI vsync on any display (`frontend.m`), and bindless texture handles read from uniform blocks (`third_party/d2r_gfx`).

## Status

Tested on a Mac with a 120 Hz display (October 2026): title screen, user selection, a new game and the first level
with the HUD, with sound. Not yet: a startup shader set (each shader is translated the first time it is drawn, then
cached), the update, iPhone / iPad.
