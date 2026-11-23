/* Persona 5 Royal 1.0.2 (update v65536 of the Asian release 01004B10157F2000, Atlus's GFD / Needle engine with CRI
   middleware): main build ID d4b150b2..., subsdk0 a8cb9780... (NintendoSDK movie), sdk 357efc19... (SDK 13.3.2), pinned in
   assets/expected-1.0.2.json. The same main as the American release 01005CA01580E000. No game-specific address is defined
   until it is verified. */
#pragma once
#define GAME_P5R 1
#define GAME_FOLDER "P5R"
#define GAME_NAME "Persona 5 Royal"   /* the screenshots' file names and note (runtime/screenshot.h) */
#define GAME_LOG "p5r"
#define TOTK_GAME_VERSION "1.0.2"
#define TOTK_GAME_DIR "local"
#define GAME_TITLE_ID 0x01004B10157F2000ULL
/* The game's id for the shader inbox, the frame-rate board and screenshot sharing (totk_contrib_upload.h; it is
   "odyssey" there by default, which this game must not report as). */
#define GAME_INBOX_ID "p5r"
