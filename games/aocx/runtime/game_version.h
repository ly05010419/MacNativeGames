/* Hyrule Warriors: Age of Calamity 1.4.1 (update v458752, Koei Tecmo's KTGL on NVN): main build ID e5f275b5...,
   subsdk0 3ab92c34... (the SDK's movie library), sdk 95832210... (SDK 9.6.0), pinned in assets/expected-1.4.1.json.
   No game-specific address is defined until it is verified. */
#pragma once
#define GAME_AOCX 1
#define GAME_FOLDER "AOCX"
#define GAME_NAME "Hyrule Warriors Age of Calamity"   /* the screenshots' file names and note (runtime/screenshot.h) */
#define GAME_LOG "aocx"
#define TOTK_GAME_VERSION "1.4.1"
#define TOTK_GAME_DIR "local"
#define GAME_TITLE_ID 0x01002B00111A2000ULL
/* The game's id for the shader inbox, the frame-rate board and screenshot sharing (totk_contrib_upload.h; it is
   "odyssey" there by default, which this game must not report as). */
#define GAME_INBOX_ID "aocx"
