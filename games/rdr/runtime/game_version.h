/* Red Dead Redemption 1.0.4 (update v262144, Rockstar's RAGE in Double Eleven's port): main build ID 37531419...,
   sdk 86188952... (SDK 15.3.2), pinned in assets/expected-1.0.4.json. Modules main and sdk only (no subsdk; the Bink
   movies are decoded by main). No game-specific address is defined until it is verified. */
#pragma once
#define GAME_RDR 1
#define GAME_FOLDER "RDR"
#define GAME_NAME "Red Dead Redemption"   /* the screenshots' file names and note (runtime/screenshot.h) */
#define GAME_LOG "rdr"
#define TOTK_GAME_VERSION "1.0.4"
#define TOTK_GAME_DIR "local"
#define GAME_TITLE_ID 0x01007820196A6000ULL
/* The game's id for the shader inbox, the frame-rate board and screenshot sharing (totk_contrib_upload.h; it is
   "odyssey" there by default, which this game must not report as). */
#define GAME_INBOX_ID "rdr"
