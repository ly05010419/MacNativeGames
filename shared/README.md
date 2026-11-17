# shared/

This folder holds code that every game uses, so it exists only once. Before it existed, each port copied it from
`games/totk`, and the copies drifted apart. Animal Crossing, Echoes of Wisdom and Super Mario Odyssey ended up with
older FPS boxes and settings panels than the three Zelda games, and the interface language followed the app bundle
instead of the Mac.

This folder mirrors the layout of a game folder. A game uses a file through a relative symlink placed at the path where
its own copy used to be, for example:

    games/odyssey/third_party/d2r_gfx/gfx/nvn/fps_overlay.c -> ../../../../../../shared/third_party/d2r_gfx/gfx/nvn/fps_overlay.c

Because of this, the game's `#include` lines, Makefile rules and object paths stay unchanged. A relative include inside
a shared file is resolved from the link's folder, so it finds that game's own headers.

| File | What it is | Linked by |
|---|---|---|
| `third_party/d2r_gfx/gfx/nvn/fps_overlay.{c,h}` | The overlay texts and modes, including "FPS full information" and the hitch lines | totk (also BotW, DKB, PKP), oot, odyssey |
| `third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h` | Chinese or English interface, from the Mac's first preferred language | totk, oot, odyssey |
| `runtime/hud_box.h` | The corner box and FPS graph that frontend.m shows | totk, oot, odyssey |
| `runtime/settings_panel.h` | The settings panel (key 1); each game's rows and pages are in its `runtime/settings_game.h` | totk, oot, odyssey |
| `runtime/screenshot.h` | The capture button, P and a held controller button: the game's next frame saved as a PNG; the folder's checks | totk, oot, odyssey |
| `runtime/board_upload.h`, `runtime/board_tab.h` | The frame-rate board: its rounds and upload (我要上榜), and the panel's 性能排行榜 page | totk, oot, odyssey |
| `runtime/shot_upload.h`, `runtime/shot_tab.h` | Screenshot sharing (分享我的截图) and the panel's 截图分享 page | totk, oot, odyssey |
| `runtime/nick_row.h` | The player's nickname row on those two pages | totk, oot, odyssey |
| `runtime/users.h`, `runtime/account.c` | Local users, one save area each (the User page); Odyssey's own account entries behind `GAME_ODYSSEY` | totk, oot, odyssey |
| `runtime/dsu_client.h`, `runtime/dsu_protocol.h`, `runtime/dsu_settings_rows.h` | DSU (cemuhook) motion and input over UDP, and its input-page rows | totk, odyssey |
| `third_party/d2r_gfx/gfx/nvn/fps_board.{c,h}` | The board's frame record: frame times after the world started | totk, oot, odyssey |
| `third_party/d2r_gfx/gfx/nvn/nvn_capture_region.h` | The present's crop as a capture region (the screenshot's game frame) | totk, oot, odyssey |

When you change a file here, build and test every game that links it: `git grep` for the file name under `games/`, or
run `find games -lname '*shared/<path>'`. Game-specific behaviour belongs in the game's own files, or behind its
`GAME_*` switch when it cannot be separated.

Note: `git mv` keeps a file's old modification time. After moving a file here, `touch` it, or make will keep
the game's old object file.
