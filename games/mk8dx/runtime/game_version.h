/* Mario Kart 8 Deluxe 4.0.1 (update v1507328) executables: main build ID 158c0c73..., sdk ba520906... (SDK 4.4.0), subsdk0
   d9de423e... (the SDK's movie library), pinned in assets/expected-4.0.1.json. No game-specific address is defined until
   it is verified. */
#pragma once
#define GAME_MK8DX 1
#define GAME_FOLDER "MK8DX"
#define GAME_NAME "Mario Kart 8 Deluxe"   /* the screenshots' file names and note (runtime/screenshot.h) */
#define GAME_LOG "mk8dx"
#define TOTK_GAME_VERSION "4.0.1"
#define TOTK_GAME_DIR "local"
#define GAME_TITLE_ID 0x0100152000022000ULL
/* The add-on content this host can mount: the Booster Course Pass (0100152000023001, index 1). Its data is only
   contents.dat and version.txt (the courses are in the update), extracted to extracted/aoc/<index>/. */
#define GAME_AOC_BASE_ID 0x0100152000023000ULL
/* The game's id for the shader inbox, the frame-rate board and screenshot sharing (totk_contrib_upload.h names it
   too, since 2026-11-22: its copy came from Odyssey's and said "odyssey"; the inbox lists this id). */
#define GAME_INBOX_ID "mk8dx"
