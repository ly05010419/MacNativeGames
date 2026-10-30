/* Super Smash Bros. Ultimate 13.0.2 (main build ID CBD5A9B56EA859BA11CA069B19B666101AE56F5A), SDK 8.2.1. The host is a fork
   of games/smo (Super Mario Odyssey 1.4.1, SDK 7.3.2 = Breath of the Wild 1.6.0's SDK), the nearest SDK of a game that runs
   (CHANGELOG.md, stage 2). No game-code address is defined on purpose: neither the TotK and BotW GAME_ADDR maps nor Mario's
   addresses apply, so any leftover use is a compile error. Smash addresses get named defines here when a patch needs one.
   13.0.5 (SSBU_GAME_V1305, main build ID 21450C647B8C5940ED5B68218EA128FD3C5D597D; the Makefile's VERSION=v1305, chosen by
   scripts/build_ssbu.py from the update's verified ExeFS): rtld, sdk and subsdk0 are the 13.0.2 modules unchanged and its
   NRR lists the same 98 NRO modules; only main differs (game_patches.c RES_SHIFT). Without a define the runtime is 13.0.2. */
#pragma once
#define GAME_SSBU 1
#if defined(SSBU_GAME_V1305)
#define TOTK_GAME_VERSION "13.0.5"
#define TOTK_GAME_DIR "v1305"
#else
#define TOTK_GAME_VERSION "13.0.2"
#define TOTK_GAME_DIR "v1302"
#endif
#define GAME_TITLE_ID 0x01006A800016E000ULL
#define GAME_NAME "Super Smash Bros. Ultimate"
#define GAME_FOLDER "SSBU"   /* ~/Library/Application Support/MacNativeGames/<GAME_FOLDER> */
#define GAME_LOG "ssbu"      /* ~/Library/Logs/MacNativeGames/<GAME_LOG>.log */
/* Smash's page allocator reserves 62 GiB of address space at startup (main+0x37c90b0, AllocateAddressRegion 0xf80000000)
   and maps pages into it on demand. No free hole below 2^39 is that big on macOS 27, so the alias region goes above 2^39,
   where its low 39 bits fall into 0x10_0000_0000..0x20_0000_0000, a range Darwin keeps for itself: an IPC address the SDK
   cut to 39 bits is then unambiguous (services.c runtime_ipc_address). */
#define GUEST_ALIAS_HIGH 1
