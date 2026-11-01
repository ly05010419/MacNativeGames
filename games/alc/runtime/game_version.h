/* Astral Chain 1.0.1 (main build ID 4B159F0F7A360669738390EE8657B4F000000000000000000000000000000000,
   from the card's update Program NCA; the base NCA holds 1.0.0 code the build does not use).
   No game-code address is defined on purpose: any leftover by3/TotK GAME_ADDR use is a compile error.
   Astral Chain addresses get named defines here when a patch needs one. */
#pragma once
#define GAME_ALC 1
#define TOTK_GAME_VERSION "1.0.1"
#define TOTK_GAME_DIR "v101"
#define GAME_NAME "Astral Chain"
#define GAME_FOLDER "ALC"   /* ~/Library/Application Support/MacNativeGames/<GAME_FOLDER> (app/launcher.m) */
#define GAME_LOG "alc"      /* ~/Library/Logs/MacNativeGames/<GAME_LOG>.log */
#define GAME_CONTRIB_TEMPLATE ""   /* no shared shader list for this game: the panel's contribution button is hidden */
#define GAME_CONTRIB_TITLE "Shader list (Astral Chain):"   /* the title prefix of a shader-list contribution (contrib_submit.h) */
#define GAME_TITLE_ID 0x01007300020fa000ULL
