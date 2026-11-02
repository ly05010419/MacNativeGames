/* Version-specific main offsets matched against original NSO code.
   Instruction guards remain mandatory; 1.4.3 also has new SDK modules.
   A new version's block: scripts/map_version_sites.py --from v112 --to <version> --header. */
#pragma once
#define GAME_ADDR_JOIN(x) GAME_ADDR_##x
#define GAME_ADDR(x) GAME_ADDR_JOIN(x)
#if defined(GAME_BOTW160)
/* The Legend of Zelda: Breath of the Wild 1.6.0 (botw.md). The TOTK patch sites do not exist here: game_patches_botw.c replaces
   game_patches.c, and the shared code that uses TOTK addresses is compiled out with GAME_BOTW. */
#ifndef GAME_BOTW
#define GAME_BOTW 1
#endif
#define TOTK_GAME_VERSION "1.6.0"
#define GAME_NAME "Breath of the Wild"
#define GAME_FOLDER "BotW"   /* ~/Library/Application Support/MacNativeGames/<GAME_FOLDER> (app/launcher.m) */
#define GAME_LOG "botw"      /* ~/Library/Logs/MacNativeGames/<GAME_LOG>.log */
#define GAME_CONTRIB_TEMPLATE "shader-list-botw.yml"   /* the issue form for this game's shared shader list (totk_shader_contrib.h) */
#define GAME_CONTRIB_TITLE "Shader list (Breath of the Wild):"   /* the title prefix of an automatically submitted contribution */
#define TOTK_GAME_DIR "botw160"
#define GAME_ADDR_0xe995c4 0xe53f8c   /* return address of nnMain's only call to nn::oe::SetFocusHandlingMode */
#define GAME_ADDR_0x2aa4fa0 0x2128690 /* nn::swkbd::ShowKeyboard: main's PLT stub */
#define GAME_ADDR_0x453f648 0x2c7ef90 /* nn::swkbd::ShowKeyboard: main's GOT slot */
#elif defined(GAME_BOTW190)
/* Breath of the Wild 1.9.0 (docs/botw-v1.9.0-port.md): the same game code as 1.6.0 with the SDK 20.5 of a later build;
   game_patches_botw.c holds its 1.9.0 sites (the #else branch of GAME_BOTW160). */
#ifndef GAME_BOTW
#define GAME_BOTW 1
#endif
#define TOTK_GAME_VERSION "1.9.0"
#define GAME_NAME "Breath of the Wild"
#define GAME_FOLDER "BotW"
#define GAME_LOG "botw"
#define GAME_CONTRIB_TEMPLATE "shader-list-botw.yml"
#define GAME_CONTRIB_TITLE "Shader list (Breath of the Wild):"
#define TOTK_GAME_DIR "botw190"
#define GAME_ADDR_0xe995c4 0xc617e8   /* return address of nnMain's only call to nn::oe::SetFocusHandlingMode */
#define GAME_ADDR_0x2aa4fa0 0x15a01a0 /* nn::swkbd::ShowKeyboard: main's PLT stub */
#define GAME_ADDR_0x453f648 0x1d78198 /* nn::swkbd::ShowKeyboard: main's GOT slot */
#elif defined(GAME_WONDER)
/* Super Mario Bros. Wonder 1.0.0, or 1.2.1 with WONDER_V121 (games/wonder): the TOTK host with the game's own patch file
   (runtime/game_patches_wonder.c). TOTK's patch sites are never patched in it: these values are outside
   main, so they never match a PC. */
#define GAME_TITLE_ID 0x010015100b514000ULL
#define GAME_NAME "Super Mario Bros. Wonder"
#define GAME_FOLDER "Wonder"   /* ~/Library/Application Support/MacNativeGames/<this>, Logs/MacNativeGames/<lowercase>.log */
#define GAME_LOG "wonder"
#ifdef WONDER_V121
#define TOTK_GAME_VERSION "1.2.1"
#define TOTK_GAME_DIR "wonder121"
#else
#define TOTK_GAME_VERSION "1.0.0"
#define TOTK_GAME_DIR "wonder"
#endif
#define GAME_ADDR_0x15ecb0c 0xffffffffu
#define GAME_ADDR_0xe995c4 0xffffffffu
#define GAME_ADDR_0x2aa4fa0 0xffffffffu
#define GAME_ADDR_0x453f648 0xffffffffu
#define GAME_ADDR_0x8f6cbc 0xffffffffu
#elif defined(TOTK_GAME_V143)
#define TOTK_GAME_VERSION "1.4.3"
#define TOTK_GAME_DIR "v143"
/* Survey only (world_survey.c): forceSetMatrix found through three 1.2.1 callers that match, the scene slot through its
   singleton getter (same 0x490-byte object); setTransform writes +0x2b4 / +0x2c0 as in 1.2.1. */
#define GAME_ACTOR_NAME 0x218
#define GAME_ACTOR_POS 0x2b4
#define GAME_ACTOR_ROT 0x2c0
#define GAME_ADDR_0x69a18c 0x45ce70
#define GAME_ADDR_0x4663f08 0x3add238
#define GAME_ADDR_0x15ecb0c 0x1bd01c4
#define GAME_ADDR_0x8f6cbc 0x2db2ec
#define GAME_ADDR_0xe995c4 0xbdb48c
#define GAME_ADDR_0x2aa4fa0 0x2bb79f0
#define GAME_ADDR_0x453f648 0x398b348
#elif defined(TOTK_GAME_V112)
#define TOTK_GAME_VERSION "1.1.2"
#define TOTK_GAME_DIR "v112"
#define GAME_ACTOR_NAME 0x218
#define GAME_ACTOR_POS 0x2b4
#define GAME_ACTOR_ROT 0x2c0
#define GAME_ADDR_0x8f6c60 0x8f66e4
#define GAME_ADDR_0x8f6c64 0x8f66e8
#define GAME_ADDR_0x8f6c88 0x8f670c
#define GAME_ADDR_0x8f6d18 0x8f679c
#define GAME_ADDR_0x8f6d28 0x8f67ac
#define GAME_ADDR_0x8f6d7c 0x8f6800
#define GAME_ADDR_0x8f6d98 0x8f681c
#define GAME_ADDR_0x7a83f4 0x7f7a38
#define GAME_ADDR_0x7a83f8 0x7f7a3c
#define GAME_ADDR_0x7a83fc 0x7f7a40
#define GAME_ADDR_0x7a8400 0x7f7a44
#define GAME_ADDR_0x7a840c 0x7f7a50
#define GAME_ADDR_0x7a8410 0x7f7a54
#define GAME_ADDR_0x8f68b8 0x8f633c
#define GAME_ADDR_0x8f68bc 0x8f6340
#define GAME_ADDR_0x8f68cc 0x8f6350
#define GAME_ADDR_0x8f68d0 0x8f6354
#define GAME_ADDR_0xc630bc 0xcdd3e8
#define GAME_ADDR_0xc630c4 0xcdd3f0
#define GAME_ADDR_0xc630c8 0xcdd3f4
#define GAME_ADDR_0xe996ac 0xea7efc
#define GAME_ADDR_0xe996b0 0xea7f00
#define GAME_ADDR_0xc630a4 0xcdd3d0
#define GAME_ADDR_0xc630a8 0xcdd3d4
#define GAME_ADDR_0xc630ac 0xcdd3d8
#define GAME_ADDR_0x69a18c 0x70376c
#define GAME_ADDR_0x15ecb0c 0x1627f10
#define GAME_ADDR_0xe995c4 0xea7e14
#define GAME_ADDR_0x4663f08 0x4725058
#define GAME_ADDR_0x2aa4fa0 0x2b16100
#define GAME_ADDR_0x453f648 0x46129e0
#define GAME_ADDR_0x8f6cbc 0x8f6740
#define GAME_ADDR_0xce3fec 0xbbb4c0   /* trampoline slow-motion check (game_patches.c patch_slowmo_bounce) */
#define GAME_OUTPUT_CALL_WORD 0x94000083u
#elif defined(TOTK_GAME_V121)
/* Found from 1.1.2 by scripts/map_version_sites.py --from v112 --to v121 (every original word unchanged). */
#define TOTK_GAME_VERSION "1.2.1"
#define TOTK_GAME_DIR "v121"
#define GAME_ACTOR_NAME 0x218
#define GAME_ACTOR_POS 0x2b4
#define GAME_ACTOR_ROT 0x2c0
#define GAME_ADDR_0x8f6c60 0x8f273c
#define GAME_ADDR_0x8f6c64 0x8f2740
#define GAME_ADDR_0x8f6c88 0x8f2764
#define GAME_ADDR_0x8f6d18 0x8f27f4
#define GAME_ADDR_0x8f6d28 0x8f2804
#define GAME_ADDR_0x8f6d7c 0x8f2858
#define GAME_ADDR_0x8f6d98 0x8f2874
#define GAME_ADDR_0x7a83f4 0x7ede7c
#define GAME_ADDR_0x7a83f8 0x7ede80
#define GAME_ADDR_0x7a83fc 0x7ede84
#define GAME_ADDR_0x7a8400 0x7ede88
#define GAME_ADDR_0x7a840c 0x7ede94
#define GAME_ADDR_0x7a8410 0x7ede98
#define GAME_ADDR_0x8f68b8 0x8f2394
#define GAME_ADDR_0x8f68bc 0x8f2398
#define GAME_ADDR_0x8f68cc 0x8f23a8
#define GAME_ADDR_0x8f68d0 0x8f23ac
#define GAME_ADDR_0xc630bc 0xcc3f68
#define GAME_ADDR_0xc630c4 0xcc3f70
#define GAME_ADDR_0xc630c8 0xcc3f74
#define GAME_ADDR_0xe996ac 0xe7aa3c
#define GAME_ADDR_0xe996b0 0xe7aa40
#define GAME_ADDR_0xc630a4 0xcc3f50
#define GAME_ADDR_0xc630a8 0xcc3f54
#define GAME_ADDR_0xc630ac 0xcc3f58
#define GAME_ADDR_0x69a18c 0x6ba86c
#define GAME_ADDR_0x15ecb0c 0x1622bf0
#define GAME_ADDR_0xe995c4 0xe7a954
#define GAME_ADDR_0x4663f08 0x4728538
#define GAME_ADDR_0x2aa4fa0 0x2b18000
#define GAME_ADDR_0x453f648 0x4615d78
#define GAME_ADDR_0x8f6cbc 0x8f2798
#define GAME_ADDR_0xce3fec 0xbda798
#define GAME_OUTPUT_CALL_WORD 0x94000083u
#else
#define TOTK_GAME_VERSION "1.0.0"
#define TOTK_GAME_DIR "v0"
#define GAME_ACTOR_NAME 0x210
#define GAME_ACTOR_POS 0x2ac
#define GAME_ACTOR_ROT 0x2b8
#define GAME_ADDR_0x8f6c60 0x8f6c60
#define GAME_ADDR_0x8f6c64 0x8f6c64
#define GAME_ADDR_0x8f6c88 0x8f6c88
#define GAME_ADDR_0x8f6d18 0x8f6d18
#define GAME_ADDR_0x8f6d28 0x8f6d28
#define GAME_ADDR_0x8f6d7c 0x8f6d7c
#define GAME_ADDR_0x8f6d98 0x8f6d98
#define GAME_ADDR_0x7a83f4 0x7a83f4
#define GAME_ADDR_0x7a83f8 0x7a83f8
#define GAME_ADDR_0x7a83fc 0x7a83fc
#define GAME_ADDR_0x7a8400 0x7a8400
#define GAME_ADDR_0x7a840c 0x7a840c
#define GAME_ADDR_0x7a8410 0x7a8410
#define GAME_ADDR_0x8f68b8 0x8f68b8
#define GAME_ADDR_0x8f68bc 0x8f68bc
#define GAME_ADDR_0x8f68cc 0x8f68cc
#define GAME_ADDR_0x8f68d0 0x8f68d0
#define GAME_ADDR_0xc630bc 0xc630bc
#define GAME_ADDR_0xc630c4 0xc630c4
#define GAME_ADDR_0xc630c8 0xc630c8
#define GAME_ADDR_0xe996ac 0xe996ac
#define GAME_ADDR_0xe996b0 0xe996b0
#define GAME_ADDR_0xc630a4 0xc630a4
#define GAME_ADDR_0xc630a8 0xc630a8
#define GAME_ADDR_0xc630ac 0xc630ac
#define GAME_ADDR_0x69a18c 0x69a18c
#define GAME_ADDR_0x15ecb0c 0x15ecb0c
#define GAME_ADDR_0xe995c4 0xe995c4
#define GAME_ADDR_0x4663f08 0x4663f08
#define GAME_ADDR_0x2aa4fa0 0x2aa4fa0
#define GAME_ADDR_0x453f648 0x453f648
#define GAME_ADDR_0x8f6cbc 0x8f6cbc
#define GAME_ADDR_0xce3fec 0xce3fec
#define GAME_OUTPUT_CALL_WORD 0x94000082u
#endif
#ifndef GAME_NAME
#define GAME_NAME "Tears of the Kingdom"   /* window title and menu (game_version.h is per binary) */
#endif
#ifndef GAME_FOLDER
#define GAME_FOLDER "TOTK"   /* the player's data folder under ~/Library/Application Support/MacNativeGames (app/launcher.m) */
#define GAME_LOG "totk"
#endif
