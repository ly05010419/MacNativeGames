/* The Witcher 3: Wild Hunt 4.04b (update of the American release 0100E67012924000, CD PROJEKT RED's REDengine 3, Saber
   Interactive's Switch port): main build ID b151a224..., sdk d5a1029a..., pinned in assets/expected-4.04b.json. Modules
   main and sdk only (as Red Dead Redemption). No game-specific address is defined until it is verified. */
#pragma once
#define GAME_W3 1
#define GAME_FOLDER "W3"
#define GAME_NAME "The Witcher 3"   /* the screenshots' file names and note (runtime/screenshot.h), the window title */
#define GAME_LOG "w3"
#define TOTK_GAME_VERSION "4.04b"
#define TOTK_GAME_DIR "local"
#define GAME_TITLE_ID 0x0100E67012924000ULL
/* The game's id for the shader inbox, the frame-rate board and screenshot sharing (totk_contrib_upload.h). */
#define GAME_INBOX_ID "w3"
