/* Metroid Dread 2.1.0 (main build ID 646761F643AFEBB379EDD5E6A5151AF2CEF93DC1; sdk and subsdk0 are 1.0.0's).
   No game-code address is defined on purpose: the TotK GAME_ADDR maps do not apply, so any leftover use is a
   compile error. Metroid Dread addresses get named defines here when a patch needs one. */
#pragma once
#define GAME_DREAD 1
#ifdef DREAD_V100
#define TOTK_GAME_VERSION "1.0.0"
#define TOTK_GAME_DIR "v100"
#else
#define TOTK_GAME_VERSION "2.1.0"
#define TOTK_GAME_DIR "v210"
#endif
#define GAME_TITLE_ID 0x010093801237c000ULL
